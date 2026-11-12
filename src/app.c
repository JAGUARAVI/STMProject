/**
 * @file  app.c
 * @brief Cooperative scheduler: the only place that decides when to measure.
 *
 * There is no RTOS. Once acquisition was moved into hardware (timer-triggered
 * ADC, comparator capture), the only concurrency the firmware needs is
 * "console in, measurement out", and a superloop expresses that without the
 * stack sizing, task watchdog and core-pinning complexity the ESP32 build
 * carried.
 */

#include "app.h"

#include <string.h>

#include "autorange.h"
#include "bsp.h"
#include "bsp_adc.h"
#include "bsp_comp.h"
#include "bsp_console.h"
#include "bsp_dac.h"
#include "bsp_frontend.h"
#include "bsp_led.h"
#include "calib.h"
#include "config.h"
#include "console.h"
#include "eswcap.h"
#include "events.h"
#include "instrument.h"
#include "rmode.h"

instrument_t g_inst;

static uint32_t s_last_led_tick;

void instrument_init(void)
{
    memset(&g_inst, 0, sizeof(g_inst));
    g_inst.run = false;             /* safe boot: nothing measures */
    g_inst.single = false;
    g_inst.locked = false;
    g_inst.lock_range = -1;
    g_inst.auto_precharge = false;
    g_inst.auto_discharge = false;
    g_inst.stream = false;
    g_inst.curve = false;
    g_inst.cycles = 0;
    g_inst.last_cycle_ms = 0;
    g_inst.mode = OP_MODE_CAP;
}

/** Initialise a subsystem, trapping with a distinguishable blink pattern if it
 *  fails. A silent failure here would produce a board that appears alive but
 *  cannot measure, which is worse than an obvious blink. */
static void require(bool ok, uint8_t blinks)
{
    if (ok) {
        return;
    }

    led_init();
    __disable_irq();
    for (;;) {
        for (uint8_t i = 0; i < blinks; ++i) {
            led_manual(255);
            for (volatile uint32_t d = 0; d < 800000U; ++d) {
            }
            led_manual(0);
            for (volatile uint32_t d = 0; d < 400000U; ++d) {
            }
        }
        for (volatile uint32_t d = 0; d < 1200000U; ++d) {
        }
    }
}

void app_init(void)
{
    instrument_init();

    require(bsp_init(), 1);          /* clocks, tick, watchdog */

    /* Console first so failures below can be reported to a human. */
    require(console_init(), 2);

    console_printf("\r\n%s %s on %s\r\n", ESWCAP_FW_NAME, ESWCAP_FW_VERSION,
                   ESWCAP_FW_TARGET);

    require(fe_init(), 3);
    require(adc_init(), 4);
    require(adc_read_init(), 5);
    require(bsp_dac3_init(), 6);
    require(comp_init(), 7);
    require(rmode_init(), 8);
    led_init();

    cal_load();

    console_printf("clocks: SYSCLK %lu Hz, VDDA %lu mV\r\n",
                   (unsigned long)HAL_RCC_GetHCLKFreq(),
                   (unsigned long)adc_vdda_mv());
    console_printf("cal:    %s\r\n",
                   cal_board_overridden() ? "per-unit record loaded"
                                          : "compiled defaults");
    console_printf("type 'h' for commands\r\n\r\n");

    evt_boot();
    evt_set_stream(false);
}

/** Human-readable result line, independent of the telemetry stream. */
static void report_result(const fusion_t *f)
{
    if (!f->valid) {
        console_printf(">> no valid reading (see the per-sample lines above)\r\n");
        return;
    }

    double c = f->capacitance_f;
    if (c < 1e-9) {
        console_printf(">> C = %.2f pF", c * 1e12);
    } else if (c < 1e-6) {
        console_printf(">> C = %.2f nF", c * 1e9);
    } else {
        console_printf(">> C = %.3f uF", c * 1e6);
    }

    console_printf("   spread %.2f%%  quality %.2f  (%d adc, %d osc)\r\n",
                   f->rel_spread * 100.0, f->quality, f->n_adc, f->n_osc);

    if (f->method_mismatch) {
        console_printf(">> WARNING: ADC and oscillator disagree by %.2fx\r\n",
                       f->method_mismatch_ratio);
    }
}

/** Run exactly one measurement cycle and publish it. */
static void run_cycle(void)
{
    autorange_clear_abort();

    uint32_t t0 = bsp_millis();
    fusion_t f;

    if (g_inst.locked && g_inst.lock_range >= 0 &&
        g_inst.lock_range < RANGE_COUNT) {
        f = autorange_measure_range((uint8_t)g_inst.lock_range);
    } else {
        f = autorange_measure();
    }

    uint32_t dt = bsp_millis() - t0;

    /* If the cycle was interrupted, do not count it and do not change the
     * automation state: the operator's command is still pending. */
    if (autorange_aborting()) {
        console_printf(">> cycle aborted\r\n");
        autorange_clear_abort();
        return;
    }

    g_inst.cycles++;
    g_inst.last_cycle_ms = dt;

    evt_cycle(&f, dt);
    evt_fuse(&f);
    report_result(&f);

    /* Pace a failing loop. With no DUT a cycle can fail in microseconds, and
     * spinning on it would flood the console without producing new
     * information. */
    if (!f.valid) {
        bsp_delay_ms(100);
    }

    led_report_result(f.valid && !f.method_mismatch && f.quality >= 0.5,
                      f.valid && (f.method_mismatch || f.quality < 0.5),
                      !f.valid);

    /* End-of-session front-end policy is shared with the console commands: with
     * auto-discharge off the node stays biased and is reported as such. */
    fe_finish(g_inst.auto_discharge, fe_charged());
    evt_fe();

    if (g_inst.single) {
        g_inst.single = false;
        g_inst.run = false;
        console_printf(">> single cycle complete; stopped\r\n");
    }
}

/** Human-readable resistance line for the R-mode scheduler. */
static void report_res(const res_sample_t *s)
{
    if (!s->valid) {
        console_printf(">> R: no valid reading\r\n");
        return;
    }

    console_printf(">> R = ");
    if (s->r_ohm < 1.0e3) {
        console_printf("%.3f ohm", s->r_ohm);
    } else if (s->r_ohm < 1.0e6) {
        console_printf("%.4f kohm", s->r_ohm / 1.0e3);
    } else {
        console_printf("%.4f Mohm", s->r_ohm / 1.0e6);
    }

    if (s->method == RMODE_TIA) {
        console_printf("  TIA  V_N=%.1f mV  V_TIA=%.1f mV  I=%.3f uA  "
                       "q=%.3f\r\n",
                       s->v_n_mv, s->v_tia_mv, s->current_ua, s->quality);
    } else {
        console_printf("  Kelvin e%d  V_N=%.1f mV  V_B=%.1f mV  I=%.3f uA  "
                       "q=%.3f\r\n",
                       s->element, s->v_n_mv, s->v_b_mv, s->current_ua,
                       s->quality);
    }
}

/** Run exactly one R-mode cycle. The R session stays open between cycles. */
static void run_res_cycle(void)
{
    res_sample_t s;
    memset(&s, 0, sizeof(s));

    uint32_t t0 = bsp_millis();
    bool ok = rmode_measure_auto(&s);
    uint32_t dt = bsp_millis() - t0;

    if (fe_r_faulted()) {
        console_printf(">> R-mode overvoltage fault: force path removed\r\n");
        rmode_leave();
        g_inst.mode = OP_MODE_CAP;
        g_inst.run = false;
        g_inst.single = false;
        evt_fe();
        return;
    }

    g_inst.cycles++;
    g_inst.last_cycle_ms = dt;

    evt_res(&s);
    report_res(&s);
    evt_fe();

    led_report_result(ok && s.valid && s.quality >= 0.5,
                      ok && s.valid && s.quality < 0.5,
                      !ok || !s.valid);

    if (!ok || !s.valid) {
        bsp_delay_ms(100);
    }

    if (g_inst.single) {
        g_inst.single = false;
        g_inst.run = false;
        console_printf(">> single R cycle complete; stopped\r\n");
    }
}

void app_run(void)
{
    char line[128];

    for (;;) {
        uint32_t now = bsp_millis();

        /* Animation runs on its own clock so a long measurement cannot make it
         * stutter, and it never blocks. */
        if ((now - s_last_led_tick) >= 1U) {
            s_last_led_tick = now;
            led_tick(now);
        }

        console_poll();

        if (console_line_ready()) {
            if (console_read_line(line, sizeof(line))) {
                /* A command arriving mid-cycle already aborted the capture
                 * inside autorange; make sure the flag is clear before
                 * dispatching so the command itself is not refused. */
                autorange_clear_abort();
                (void)console_dispatch(line);
            }
            bsp_iwdg_refresh();
            continue;
        }

        if (g_inst.run) {
            if (g_inst.mode == OP_MODE_RES) {
                run_res_cycle();
            } else {
                run_cycle();
            }
        } else {
            bsp_iwdg_refresh();
        }
    }
}
