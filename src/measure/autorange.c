/**
 * @file  autorange.c
 * @brief PROBE -> SWEEP -> FUSION: the measurement orchestrator.
 *
 * The shape of the pipeline is inherited from the ESP32 firmware because it
 * encodes hard-won behaviour, not implementation detail:
 *
 *  1. PROBE with the oscillator. It is the only method that works across the
 *     whole range, so it is used to place the ADC sweep. Probing with the ADC
 *     would be useless: a 10 pF DUT is invisible to a 250 us tau gate.
 *  2. SWEEP the ADC ranges whose predicted tau lands in the window where the
 *     fit is well conditioned, plus the oscillator range nearest its
 *     best-signal band.
 *  3. FUSE with a quality-weighted log-domain mean and a median gate.
 *
 * What changed on the G474 is entirely below this layer: acquisition is
 * hardware-timed, so there is no decimation, no timestamp bookkeeping and no
 * ISR rate limiter to work around. The *decisions* are the same because they
 * describe the instrument, not the CPU.
 */

#include "autorange.h"

#include <math.h>
#include <string.h>

#include "adc_measure.h"
#include "bsp.h"
#include "bsp_adc.h"
#include "bsp_comp.h"
#include "bsp_console.h"
#include "bsp_frontend.h"
#include "calib.h"
#include "config.h"
#include "events.h"
#include "fusion.h"
#include "instrument.h"
#include "osc_measure.h"

/* Current pipeline phase, reported in `sample` telemetry. */
static const char *s_phase = "idle";

/* Remembered probe range: seeded at 100 kohm, which is the range where the
 * oscillator's signal band is widest and where an unknown DUT is most likely
 * to land. Updated to whichever range produced the best-quality sample. */
static uint8_t s_preferred = 2;

static volatile bool s_abort;

/* Asymptote cache per range. A droopy asymptote (parasitic pull-down) is a
 * property of the range and the leakage path, not of the DUT, so it is
 * measured once and reused. 0 means "unknown". */
static int s_v_inf_cache[RANGE_COUNT];

/** Decision trace for the dashboard's ranging matrix. Declared in eswcap.h and
 *  shared with the telemetry layer. */
sweep_info_t g_sweep;

/* Cached VDDA in millivolts, measured once. */
static uint32_t s_vref_mv;

void autorange_abort(void)
{
    s_abort = true;
}

bool autorange_aborting(void)
{
    return s_abort;
}

void autorange_clear_abort(void)
{
    s_abort = false;
}

uint8_t autorange_preferred_range(void)
{
    return s_preferred;
}

void autorange_set_preferred(uint8_t range_idx)
{
    if (range_idx < RANGE_COUNT) {
        s_preferred = range_idx;
    }
}

static uint32_t vref_mv(void)
{
    if (s_vref_mv == 0U) {
        uint32_t v = adc_vdda_mv();
        s_vref_mv = (v >= 1800U && v <= 3600U) ? v : 3300U;
    }
    return s_vref_mv;
}

/** Report a sample to the console and the telemetry stream. */
static void report_sample(sample_t *s)
{
    evt_sample(s_phase, s);

#if ESWCAP_VERBOSE_LOG
    if (!s->valid) {
        console_printf("  [%s] %-3s %-7s | no reading\n", s_phase,
                       s->method == METHOD_ADC_STEP ? "adc" : "osc",
                       g_ranges[s->range_idx].label);
        return;
    }
    if (s->method == METHOD_ADC_STEP) {
        console_printf("  [%s] adc %-7s | tau %8.1f us  C %8.3f nF  q %.3f%s\n",
                       s_phase, g_ranges[s->range_idx].label, s->tau_us,
                       s->capacitance_f * 1e9, s->quality,
                       s->plausible ? "" : "  (rejected)");
    } else {
        console_printf("  [%s] osc %-7s | f %9.1f Hz   C %8.3f nF  q %.3f%s\n",
                       s_phase, g_ranges[s->range_idx].label, s->freq_hz,
                       s->capacitance_f * 1e9, s->quality,
                       s->plausible ? "" : "  (rejected)");
    }
#else
    (void)s;
#endif
}

/* ------------------------------------------------------------------------- */
/* Oscillator sample                                                         */
/* ------------------------------------------------------------------------- */

/**
 * @brief Measure one range with the comparator oscillator.
 *
 * The threshold is held at mid-rail, matching the LM393's 10k/10k divider, so
 * the oscillation condition and therefore the calibrated K remain comparable
 * with the values measured on the old board.
 */
sample_t autorange_measure_osc(uint8_t range_idx)
{
    sample_t s;
    memset(&s, 0, sizeof(s));
    s.method = METHOD_OSC;
    s.range_idx = range_idx;

    if (range_idx >= RANGE_COUNT) {
        return s;
    }

    fe_select_range(range_idx);
    bsp_delay_us(5);
    fe_prepare_measurement(g_inst.auto_precharge);

    comp_set_threshold_mv(1650);
    /* comp_loop_start() enables the comparator and starts TIM2; do not call
     * comp_enable() separately or HAL_COMP_Start() is issued twice. */
    comp_loop_start();
    bsp_delay_us(50);

    double period_us = 0.0;
    double freq_hz = 0.0;
    uint32_t n_periods = 0;

    bool ok = comp_measure_period(OSC_TARGET_PERIODS, 3000, &period_us,
                                  &n_periods, &freq_hz);

    comp_loop_stop();
    fe_set_drive(false);

    if (!ok || period_us <= 0.0) {
        return s;
    }

    s = osc_measure_sample(range_idx, period_us, n_periods,
                           cal_osc_k(range_idx), cal_osc_t0(range_idx),
                           cal_osc_has_t0(range_idx),
                           cal_osc_delay(range_idx),
                           g_ranges[range_idx].resistance_ohms,
                           board_c_block_f());

    report_sample(&s);
    return s;
}

/* ------------------------------------------------------------------------- */
/* ADC sample                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief Measure (or recall) the true charge asymptote for a range.
 *
 * The node does not always charge to the rail: a parasitic pull-down forms a
 * divider with the range resistor, so the plateau is below VREF. Every tau
 * threshold must reference the *measured* plateau, not an assumed 3.3 V, or
 * high-resistance ranges never cross the stop threshold at all.
 */
static int adc_asymptote_for_range(uint8_t range_idx, double tau_est_us)
{
    if (s_v_inf_cache[range_idx] > 0) {
        return s_v_inf_cache[range_idx];
    }

    /* Settle time scales with the estimated time constant. A fixed 20 ms is
     * enough for a small DUT but samples a 360 uF DUT on 100 ohm (tau ~36 ms)
     * nowhere near its asymptote, which would bias both V_inf and the
     * thresholds derived from it. 8 tau is three time constants past "fully
     * settled" with room to spare. */
    uint32_t settle_ms = 20U;
    if (tau_est_us > 0.0) {
        double ms = 8.0 * tau_est_us / 1000.0;
        if (ms > (double)settle_ms) {
            settle_ms = (uint32_t)ms;
        }
    }
    if (settle_ms > 15000U) {
        settle_ms = 15000U;
    }

    fe_set_drive(true);
    bsp_delay_ms(settle_ms);

    int32_t code = adc_read(ADC_CH_VCAP, 16);
    fe_set_drive(false);

    if (code < 0) {
        return 0;
    }

    int mv = adc_code_to_mv((uint16_t)code, 16, vref_mv());
    if (mv > 100) {
        s_v_inf_cache[range_idx] = mv;
    }
    return mv;
}

/**
 * @brief Wait for the node to fall back to ~0 V before the timed charge.
 *
 * The return path is through the range resistor, so a large DUT discharges
 * slowly for exactly the same reason it charges slowly. Starting the capture
 * before the node is actually at the floor would make V0 wrong and bend the
 * fitted curve.
 */
static bool wait_node_below(double tau_est_us, int v_inf)
{
    uint32_t timeout_ms = 20U;
    if (tau_est_us > 0.0) {
        double ms = 8.0 * tau_est_us / 1000.0;
        if (ms > (double)timeout_ms) {
            timeout_ms = (uint32_t)ms;
        }
    }
    if (timeout_ms > 15000U) {
        timeout_ms = 15000U;
    }

    int target = (int)(0.02 * (double)v_inf) + 25;
    uint32_t t0 = bsp_millis();
    uint32_t last_feed = t0;

    for (;;) {
        int32_t code = adc_read(ADC_CH_VCAP, 16);
        if (code >= 0) {
            int mv = adc_code_to_mv((uint16_t)code, 16, vref_mv());
            if (mv <= target) {
                return true;
            }
        }

        uint32_t now = bsp_millis();
        if ((now - t0) > timeout_ms) {
            return false;
        }
        if ((now - last_feed) > 500U) {
            bsp_iwdg_refresh();
            last_feed = now;
        }
        bsp_delay_ms(1);
    }
}

/**
 * @brief Capture and score one ADC RC-step measurement.
 *
 * The sample period is derived from the oscillator probe's estimate whenever it
 * is available: `period = tau_est / 64` gives roughly 64 points per time
 * constant, which is far more than the fit needs and guarantees the capture
 * window spans the whole curve instead of filling up on the near-linear start.
 *
 * Without an estimate the per-range defaults are used. Those are deliberately
 * conservative — they cover the DUT band each range is *good for*, not the
 * extreme ends.
 */
sample_t autorange_measure_adc(uint8_t range_idx)
{
    sample_t s;
    memset(&s, 0, sizeof(s));
    s.method = METHOD_ADC_STEP;
    s.range_idx = range_idx;

    if (range_idx >= RANGE_COUNT) {
        return s;
    }

    static double v_mv[RC_FIT_MAX_PTS];
    static uint16_t raw[RC_FIT_MAX_PTS];

    fe_select_range(range_idx);
    fe_prepare_measurement(g_inst.auto_precharge);

    /* Estimated time constant from the oscillator probe. Every settle below is
     * timed relative to it, so large DUTs are not sampled mid-transient. */
    double tau_est_us = 0.0;
    if (g_sweep.have_rough && g_sweep.rough_f > 0.0) {
        tau_est_us = g_sweep.rough_f * g_ranges[range_idx].resistance_ohms * 1e6;
    }

    /* ---- Asymptote ------------------------------------------------------ */
    int v_inf = adc_asymptote_for_range(range_idx, tau_est_us);
    if (v_inf < 900) {
        /* Heavy pull-down: the node never reaches a usable plateau, so any tau
         * derived from it would be meaningless. */
        report_sample(&s);
        return s;
    }

    /* ---- Reset the node and record the start level ---------------------- */
    fe_set_drive(false);
    if (!wait_node_below(tau_est_us, v_inf)) {
        report_sample(&s);
        return s;
    }

    int32_t c0 = adc_read(ADC_CH_VCAP, 16);
    if (c0 < 0) {
        report_sample(&s);
        return s;
    }
    int v0 = adc_code_to_mv((uint16_t)c0, 16, vref_mv());

    uint32_t period_ns;
    if (tau_est_us > 0.0) {
        double p_ns = (tau_est_us * 1000.0) / 64.0;
        if (p_ns < 1000.0) {
            p_ns = 1000.0;
        }
        if (p_ns > 2000000.0) {
            p_ns = 2000000.0;
        }
        period_ns = (uint32_t)p_ns;
    } else {
        static const uint32_t def_ns[RANGE_COUNT] = {
            10000U, 20000U, 200000U, 2000000U
        };
        period_ns = def_ns[range_idx];
    }

    static const uint8_t os_ratio[RANGE_COUNT] = {
        ADC_OS_RATIO_100R, ADC_OS_RATIO_1K, ADC_OS_RATIO_100K, ADC_OS_RATIO_1M
    };

    /* The requested period may be shorter than an oversampled frame can fit
     * (e.g. a 250 us tau estimate implies a 3.9 us period, but 4x oversampling
     * needs 5.7 us). Raise the period until the trigger can keep up rather than
     * discarding an otherwise valid range. */
    bool started = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (adc_stream_start(os_ratio[range_idx], period_ns)) {
            started = true;
            break;
        }
        period_ns *= 2U;
        if (period_ns > 2000000U) {
            period_ns = 2000000U;
        }
    }

    if (!started) {
        report_sample(&s);
        return s;
    }

    /* Use the period the timer actually programmed, not the request: the BSP
     * rounds to whole ticks and may have applied a prescaler. The fit depends
     * on dt being exact. */
    double dt_us = (double)adc_stream_info()->sample_period_ns / 1000.0;

    /* ---- Capture -------------------------------------------------------- */
    adc_stream_flush();
    adc_stream_sync();          /* TIM6 counter = 0: sample 0 is the step edge */
    fe_set_drive(true);

    int v_stop = v0 + (int)(0.90 * (double)(v_inf - v0));
    int got = 0;
    int peak = 0;
    bool stopped_early = false;

    uint32_t poll_us = period_ns / 4000U;
    if (poll_us < 1U) {
        poll_us = 1U;
    }
    if (poll_us > 100U) {
        poll_us = 100U;
    }

    uint32_t timeout_us = (tau_est_us > 0.0)
                              ? (uint32_t)(6.0 * tau_est_us) + 100000U
                              : 15000000U;

    uint32_t elapsed_us = 0;
    uint32_t last_feed_ms = bsp_millis();
    while (got < RC_FIT_MAX_PTS) {
        /* Service the console receiver but never dispatch from here: a complete
         * line just aborts the capture so the main loop can act on it. This
         * keeps long captures from locking out `stop` or a calibration command
         * without introducing re-entrant command handling. */
        console_poll();
        if (console_line_ready()) {
            s_abort = true;
        }

        if (s_abort) {
            stopped_early = true;
            break;
        }

        int n = adc_stream_take(&raw[got], (uint32_t)(RC_FIT_MAX_PTS - got));
        if (n < 0) {
            /* Overrun: the consumer fell behind. Whatever was copied is still
             * coherent, so keep it rather than discarding the whole capture. */
            stopped_early = true;
            break;
        }

        for (int i = 0; i < n; ++i) {
            int mv = adc_code_to_mv(raw[got + i], os_ratio[range_idx],
                                    vref_mv());
            v_mv[got + i] = (double)mv;
            if (mv > peak) {
                peak = mv;
            }
        }
        got += n;

        if (peak >= v_stop) {
            break;      /* captured well past tau */
        }

        bsp_delay_us(poll_us);
        elapsed_us += poll_us;
        if (elapsed_us > timeout_us) {
            stopped_early = true;
            break;
        }

        /* The ADC timeout can legitimately reach 15 s on a very large DUT, well
         * beyond the ~4 s watchdog window. bsp_delay_us() deliberately does not
         * refresh the watchdog (that would defeat it for tight loops), so do it
         * here on a coarser cadence. */
        uint32_t now_ms = bsp_millis();
        if ((now_ms - last_feed_ms) > 500U) {
            bsp_iwdg_refresh();
            last_feed_ms = now_ms;
        }
    }

    fe_set_drive(false);
    adc_stream_stop();

    if (got < RC_FIT_MIN_PTS) {
        report_sample(&s);
        return s;
    }

    /* ---- Score ---------------------------------------------------------- */
    adc_capture_t cap;
    cap.v_mv = v_mv;
    cap.n = got;
    cap.dt_us = dt_us;
    cap.v0_mv = (double)v0;
    cap.v_inf_hint_mv = (double)v_inf;
    cap.range_idx = range_idx;
    cap.fit_ok = false;

    double v_inf_out = 0.0;
    s = adc_measure_sample(&cap, &v_inf_out);

    if (v_inf_out > 900.0 && v_inf_out < 3400.0) {
        s_v_inf_cache[range_idx] = (int)(v_inf_out + 0.5);
    }

    if (g_inst.curve) {
        evt_curve(range_idx, (int)(v_inf_out > 0.0 ? v_inf_out : v_inf), v0,
                  s.fit_r2 >= 0.0 ? 0.0 : 0.0,
                  s.fit_r2 >= 0.0 ? s.tau_us : -1.0, s.fit_r2,
                  v_mv, got, cap.dt_us);
    }

    (void)stopped_early;
    report_sample(&s);
    return s;
}

/* ------------------------------------------------------------------------- */
/* Pipeline                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Full autoranging cycle.
 *
 * The `sweep` telemetry is emitted before fusion so the dashboard's decision
 * matrix can be rendered even when the cycle ultimately produces no valid
 * value.
 */
fusion_t autorange_measure(void)
{
    static sample_t samples[MAX_SAMPLES];

    /* The asymptote depends on the leakage path to the bias rail, so a change
     * in the node's charge state invalidates every cached plateau. Tracking the
     * transition here catches precharge, discharge and auto-precharge without
     * every caller having to remember. */
    static bool s_last_charged;
    if (fe_charged() != s_last_charged) {
        memset(s_v_inf_cache, 0, sizeof(s_v_inf_cache));
        s_last_charged = fe_charged();
    }

    memset(&g_sweep, 0, sizeof(g_sweep));
    g_sweep.valid = true;
    g_sweep.locked = false;
    g_sweep.lock_range = -1;
    g_sweep.osc_best_range = -1;

    int n = 0;

    /* ---- Phase 1: oscillator probe -------------------------------------- */
    s_phase = "probe";
    sample_t probe = autorange_measure_osc(s_preferred);

    double rough_c = 0.0;
    bool have_rough = false;

    if (probe.valid && probe.plausible && probe.capacitance_f > 0.0) {
        /* Plan with the series-equivalent the circuit actually sees, not the
         * reconstructed DUT value: they diverge as C_dut approaches C_block. */
        rough_c = (probe.c_eq_f > 0.0) ? probe.c_eq_f : probe.capacitance_f;
        have_rough = true;
    }

    if (!have_rough) {
        /* Linear hunt from the slowest range up. A DUT that the probe range
         * cannot see is usually either open or far outside that range's band. */
        for (int r = RANGE_COUNT - 1; r >= 0 && !have_rough; --r) {
            if (s_abort) {
                break;
            }
            s_phase = "hunt";
            sample_t p = autorange_measure_osc((uint8_t)r);
            if (p.valid && p.plausible && p.capacitance_f > 0.0) {
                rough_c = (p.c_eq_f > 0.0) ? p.c_eq_f : p.capacitance_f;
                have_rough = true;
                s_preferred = (uint8_t)r;
                probe = p;
            }
        }
    }

    /* ---- Phase 2: choose the sweep -------------------------------------- */
    bool adc_tried[RANGE_COUNT] = {false, false, false, false};
    int osc_best_range = -1;
    double osc_best_cost = 1e30;

    /* The ADC cannot resolve sub-nF DUTs: a 10 pF change on 1 Mohm is only
     * ~10 us of tau, far below the fit floor. When the probe already says the
     * DUT is small, skip the ADC entirely and let the tared oscillator own the
     * measurement. */
    bool sub_nf = have_rough && rough_c > 0.0 && rough_c < ADC_SUBNF_GATE_F;

    if (have_rough && rough_c > 0.0) {
        for (int r = 0; r < RANGE_COUNT; ++r) {
            double tau_us = rough_c * g_ranges[r].resistance_ohms * 1e6;
            if (!sub_nf && tau_us >= ADC_STEP_THRESHOLD_US && tau_us <= 4.0e6) {
                adc_tried[r] = true;
            }

            /* Predict the *measured* period including the tare, otherwise a
             * small DUT is wildly over-predicted and no range ever qualifies. */
            double t_pred_us = cal_osc_k((uint8_t)r) *
                               g_ranges[r].resistance_ohms * rough_c * 1e6;
            if (cal_osc_has_t0((uint8_t)r)) {
                t_pred_us += cal_osc_t0((uint8_t)r);
            }
            double f_pred = (t_pred_us > 0.0) ? 1e6 / t_pred_us : 0.0;

            if (f_pred >= 100.0 && f_pred <= 15000.0) {
                /* Prefer the range closest to the middle of the oscillator's
                 * useful band. Log distance makes the choice scale-invariant. */
                double cost = fabs(log(f_pred / 2000.0));
                if (cost < osc_best_cost) {
                    osc_best_cost = cost;
                    osc_best_range = r;
                }
            }
        }
    } else {
        for (int r = 0; r < RANGE_COUNT; ++r) {
            adc_tried[r] = true;
        }
        osc_best_range = 2;
    }

    g_sweep.have_rough = have_rough;
    g_sweep.rough_f = rough_c;
    g_sweep.sub_nf = sub_nf;
    g_sweep.osc_best_range = osc_best_range;
    for (int r = 0; r < RANGE_COUNT; ++r) {
        g_sweep.adc_tried[r] = adc_tried[r];
    }

    /* ---- Phase 3: gather ------------------------------------------------ */
    bool saturated = false;
    double best_q = -1.0;

    s_phase = "adc";
    for (int r = 0; r < RANGE_COUNT && n < MAX_SAMPLES; ++r) {
        if (!adc_tried[r] || s_abort) {
            continue;
        }
        sample_t s = autorange_measure_adc((uint8_t)r);
        if (s.valid) {
            samples[n++] = s;
        }
        if (s.valid && s.plausible && s.quality > best_q) {
            best_q = s.quality;
            s_preferred = (uint8_t)r;
        }
        if (r == 0 && s.valid && !s.plausible) {
            /* Even 100 ohm was too fast to time cleanly. Confirm on 100 kohm,
             * which slows the edge ~1000x into the accurate window. */
            saturated = true;
        }
    }

    if (saturated && !adc_tried[2] && n < MAX_SAMPLES && !s_abort) {
        s_phase = "adc-sub";
        g_sweep.adc_sub = true;
        sample_t s = autorange_measure_adc(2);
        if (s.valid) {
            samples[n++] = s;
        }
        if (s.valid && s.plausible) {
            s_preferred = 2;
        }
    }

    bool osc_added = false;

    if (osc_best_range >= 0 && n < MAX_SAMPLES && !s_abort) {
        if (probe.valid && (int)probe.range_idx == osc_best_range && probe.plausible) {
            samples[n++] = probe;
            osc_added = true;
        } else {
            s_phase = "osc";
            sample_t o = autorange_measure_osc((uint8_t)osc_best_range);
            if (o.valid) {
                samples[n++] = o;
                osc_added = true;
                if (o.plausible && o.quality > best_q) {
                    best_q = o.quality;
                    s_preferred = (uint8_t)osc_best_range;
                }
            }
        }
    }

    /* A valid probe is a real measurement. Never discard it merely because the
     * predictive selection failed to nominate its range. */
    if (!osc_added && probe.valid && probe.plausible &&
        probe.capacitance_f > 0.0 && n < MAX_SAMPLES) {
        samples[n++] = probe;
        osc_added = true;
        osc_best_range = (int)probe.range_idx;
    }

    /* Neighbouring-range oscillator run for cross-validation when there is
     * time and the predicted frequency still lands in a useful band. */
    if (osc_added && osc_best_range >= 0 && n < MAX_SAMPLES && !s_abort) {
        int alt = (osc_best_range > 0) ? osc_best_range - 1 : osc_best_range + 1;
        if (alt >= 0 && alt < RANGE_COUNT) {
            double f_alt = 0.0;
            if (have_rough && rough_c > 0.0) {
                double t_alt_us = cal_osc_k((uint8_t)alt) *
                                  g_ranges[alt].resistance_ohms * rough_c * 1e6;
                if (cal_osc_has_t0((uint8_t)alt)) {
                    t_alt_us += cal_osc_t0((uint8_t)alt);
                }
                if (t_alt_us > 0.0) {
                    f_alt = 1e6 / t_alt_us;
                }
            }
            if (f_alt >= 100.0 && f_alt <= 15000.0) {
                s_phase = "osc-x";
                sample_t o2 = autorange_measure_osc((uint8_t)alt);
                if (o2.valid) {
                    samples[n++] = o2;
                }
            }
        }
    }

    s_phase = "fuse";
    g_sweep.saturated = saturated;
    evt_sweep();

    return fuse_samples(samples, n);
}

/**
 * @brief Locked-range measurement (`range <n>`).
 *
 * Both methods are attempted on the one range. The ADC's own tau gate still
 * rejects sub-nF DUTs, so a locked pF range naturally ends up
 * oscillator-only — no special case needed.
 */
fusion_t autorange_measure_range(uint8_t range_idx)
{
    static sample_t samples[MAX_SAMPLES];

    memset(&g_sweep, 0, sizeof(g_sweep));
    g_sweep.valid = true;
    g_sweep.locked = true;
    g_sweep.lock_range = (int)range_idx;
    g_sweep.have_rough = false;
    g_sweep.osc_best_range = (int)range_idx;

    if (range_idx >= RANGE_COUNT) {
        fusion_t empty;
        memset(&empty, 0, sizeof(empty));
        return empty;
    }

    g_sweep.adc_tried[range_idx] = true;

    int n = 0;

    s_phase = "lock-osc";
    sample_t o = autorange_measure_osc(range_idx);
    if (o.valid) {
        samples[n++] = o;
    }

    if (!s_abort && n < MAX_SAMPLES) {
        s_phase = "lock-adc";
        sample_t a = autorange_measure_adc(range_idx);
        if (a.valid) {
            samples[n++] = a;
        }
    }

    s_phase = "fuse";
    evt_sweep();
    return fuse_samples(samples, n);
}
