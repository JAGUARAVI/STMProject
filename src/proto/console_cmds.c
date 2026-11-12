/**
 * @file  console_cmds.c
 * @brief Interactive command dispatcher for the ESWCap STM32G474 port.
 *
 * The command names, the human-readable replies and the `@@EVT` events are
 * preserved from the ESP32 firmware so the existing host dashboard and the
 * project documentation remain valid.  The dispatcher is deliberately a thin
 * layer: it validates user input, calls the same public measurement entry
 * points the scheduler uses, and reports through console_printf() plus the
 * event emitters.
 *
 * @section calpath Calibration captures use the runtime estimator
 *
 * Every calibration command calls autorange_measure_osc() or
 * autorange_measure_adc() instead of issuing its own comparator/ADC capture.
 * That is intentional and safety-relevant for the calibration itself: the
 * stored K, T0, R_eff and C0 are later consumed by exactly those functions and
 * by their estimators (comp_measure_period()/osc_measure_sample() and the
 * rc_fit path).  A separate "calibration capture" would inevitably differ in
 * averaging, fit rejection or asymptote selection, and the resulting constants
 * would be biased by the difference.  Such a bias cannot be removed by any
 * later calibration because calibration and measurement would keep
 * disagreeing.  One path means one estimator, so a calibrated unit measures
 * what it was calibrated against.
 *
 * @section precharge_safety Precharge safety warning
 *
 * `precharge` leaves N_DUT and C_block at V_BIAS through R_bias even after the
 * front end is isolated (FE_PRECHARGED).  Removing a DUT in that state exposes
 * the bias node; `discharge` is the only command that bleeds it.  The warning
 * is printed and sent in the acknowledgement because the dashboard can issue
 * this command remotely, with nobody looking at the board.
 *
 * @section rmode_rcal R-mode calibration
 *
 * The R-mode sense-resistor constants live in the shared calibration record,
 * so `rcal?`, `rcal`, `rcalclear` and the TIA values all go through the
 * rmode_* accessors that read and write that record.  Operating on a private
 * copy would let the display and the measurement path disagree about which
 * RREF is in use.
 *
 * `rcal <element> <rref_ohm> [gain]` and `rcal tia <rf_ohm> <vbias_mv>
 * [gain]` are how a bench measurement becomes persistent: they validate the
 * value, write the record and save it to flash.  Without them the R-mode
 * calibration was read-only and the instrument could never be calibrated.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "console.h"
#include "instrument.h"
#include "eswcap.h"
#include "config.h"
#include "autorange.h"
#include "calib.h"
#include "rmode.h"
#include "events.h"
#include "bsp_frontend.h"
#include "bsp_adc.h"
#include "bsp_led.h"
#include "bsp_console.h"
#include "bsp.h"
#include "osc_calc.h"
#include "adc_measure.h"

/* ------------------------------------------------------------------------- */
/* Private state                                                             */
/* ------------------------------------------------------------------------- */

/** Range selected by the `0`..`3` commands.  Default 2 is the 100 kR range,
 *  the widest-signal oscillator range and therefore the one most likely to be
 *  calibrated first. */
static uint8_t s_selected_range = 2U;

/** One captured point of a two-point calibration. */
typedef struct {
    bool    valid;
    uint8_t range;
    double  period_us;   /**< oscillator period, or ADC tau */
    double  ref_f;       /**< reference capacitance in farads */
} cal_point_t;

/** Pending first point of the oscillator two-point procedure. */
static cal_point_t s_osc_cal1;

/** Pending first point of the ADC two-point procedure. */
static cal_point_t s_adc_cal1;

/** Safety text repeated in the precharge acknowledgement. */
#define PRECHARGE_WARNING "N_DUT biased; discharge before removing DUT"

/** Top R-mode sense resistor.  It is the 4.7 MR part added for the high end of
 *  the R-mode range, not the 1.01 MR C-mode range resistor, so it cannot come
 *  from g_ranges[3]. */

/* ------------------------------------------------------------------------- */
/* Small parsing and formatting helpers                                      */
/* ------------------------------------------------------------------------- */

/**
 * @brief True for the whitespace the dispatcher treats as a token separator.
 *
 * CR/LF are included because a caller other than console_read_line() may hand
 * over a line that still has its terminator.
 */
static bool is_space(char c)
{
    return (c == ' ') || (c == '\t') || (c == '\r') || (c == '\n');
}

/**
 * @brief Skip leading whitespace.
 */
static const char *skip_ws(const char *s)
{
    while ((s != NULL) && is_space(*s)) {
        ++s;
    }
    return s;
}

/**
 * @brief Length of the first whitespace-delimited token.
 */
static size_t token_len(const char *s)
{
    size_t n = 0U;

    if (s == NULL) {
        return 0U;
    }
    while ((s[n] != '\0') && !is_space(s[n])) {
        ++n;
    }
    return n;
}

/**
 * @brief True when the first token of @p s is exactly @p word.
 *
 * This is deliberately an exact-token comparison rather than a prefix test.
 * It keeps `cal` from swallowing `cal1`, `cal2` or `calclear`, and keeps
 * `adccal` from swallowing `adccal1`, `adccal2` or `adccalclear`.
 */
static bool token_is(const char *s, const char *word)
{
    size_t n = token_len(s);
    size_t w = strlen(word);

    return (n == w) && (strncmp(s, word, n) == 0);
}

/**
 * @brief True when @p s contains exactly one token and it equals @p word.
 *
 * Used where an argument must be the whole remainder of the line, such as
 * `mode c` or `led auto`, so a stray trailing word is rejected rather than
 * silently ignored.
 */
static bool only_token_is(const char *s, const char *word)
{
    return token_is(s, word) && (*skip_ws(s + token_len(s)) == '\0');
}

/**
 * @brief True when the command word at the start of @p s is @p cmd.
 *
 * The character after the word must be whitespace or NUL, so a command never
 * matches a longer word that merely starts with it.
 */
static bool cmd_is(const char *s, const char *cmd)
{
    size_t n = strlen(cmd);

    if (strncmp(s, cmd, n) != 0) {
        return false;
    }
    return (s[n] == '\0') || is_space(s[n]);
}

/**
 * @brief Return the argument text after the first token.
 */
static const char *cmd_arg(const char *s)
{
    return skip_ws(s + token_len(s));
}

/**
 * @brief Parse one decimal integer argument and reject trailing junk.
 *
 * @param s   Argument text.
 * @param out Receives the value on success.
 * @return true only when exactly one integer was present.
 */
static bool parse_int_arg(const char *s, int *out)
{
    char extra = '\0';

    if ((s == NULL) || (out == NULL)) {
        return false;
    }
    return (sscanf(s, "%d %c", out, &extra) == 1);
}

/**
 * @brief Parse one finite floating-point argument and reject trailing junk.
 */
static bool parse_double_arg(const char *s, double *out)
{
    char extra = '\0';

    if ((s == NULL) || (out == NULL)) {
        return false;
    }
    if (sscanf(s, "%lf %c", out, &extra) != 1) {
        return false;
    }
    return isfinite(*out);
}

/**
 * @brief Parse an on/off argument.
 *
 * @param s Argument text.
 * @return 1 for on/1, 0 for off/0, -1 for anything else.
 */
static int parse_onoff(const char *s)
{
    size_t n;

    if (s == NULL) {
        return -1;
    }

    s = skip_ws(s);
    n = token_len(s);
    if (*skip_ws(s + n) != '\0') {
        return -1;
    }

    if ((n == 2U) && (strncmp(s, "on", 2U) == 0)) {
        return 1;
    }
    if ((n == 3U) && (strncmp(s, "off", 3U) == 0)) {
        return 0;
    }
    return -1;
}

/**
 * @brief Return the range selected for calibration and probe commands.
 */
static uint8_t cur_range(void)
{
    return console_selected_range();
}

/**
 * @brief Human-readable name of a front-end phase.
 */
static const char *fe_state_name(fe_state_t st)
{
    switch (st) {
    case FE_IDLE:
        return "idle";
    case FE_PRECHARGE:
        return "precharge";
    case FE_PRECHARGED:
        return "precharged (biased)";
    case FE_MEASURING:
        return "measuring";
    case FE_DISCHARGE:
        return "discharge";
    case FE_R_ISOLATE:
        return "r_isolate";
    case FE_R_KELVIN:
        return "r_kelvin";
    case FE_R_TIA:
        return "r_tia";
    case FE_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

/**
 * @brief Print a resistance in engineering units without a newline.
 *
 * The console is also used stand-alone, so 1.01e6 is far less readable than
 * 1.010 MR.  The values here span 1 R to 1 GR.
 */
static void print_resistance(double ohm)
{
    double magnitude;

    if (!isfinite(ohm)) {
        console_printf("---");
        return;
    }

    magnitude = fabs(ohm);
    if (magnitude >= 1.0e9) {
        console_printf("%.4f GR", ohm / 1.0e9);
    } else if (magnitude >= 1.0e6) {
        console_printf("%.4f MR", ohm / 1.0e6);
    } else if (magnitude >= 1.0e3) {
        console_printf("%.3f kR", ohm / 1.0e3);
    } else {
        console_printf("%.3f R", ohm);
    }
}

/**
 * @brief Persist the calibration record and report a flash failure.
 *
 * RAM is already updated by the caller; a failure here means the new value
 * will not survive a reset.  Say so explicitly rather than claiming success.
 *
 * @return true when the record reached flash.
 */
static bool save_cal(void)
{
    if (cal_save()) {
        return true;
    }
    console_printf(">> WARNING: calibration could not be written to flash\r\n");
    return false;
}

/**
 * @brief Compiled default for one R-mode sense element.
 *
 * Elements 0..2 are the C-mode range resistors reused as the R-mode sense
 * bank; element 3 is the extra 4.7 MR part that extends the Kelvin range to
 * the tens of megohms.  These are the same defaults the calibration store
 * installs at boot.
 *
 * @param element Sense element index (0..3).
 * @return Default resistance in ohms, or 0 for an invalid index.
 */
static double rmode_default_rref(uint8_t element)
{
    switch (element) {
    case 0U:
        return RSENSE_100_OHM;
    case 1U:
        return RSENSE_1K_OHM;
    case 2U:
        return RSENSE_100K_OHM;
    case 3U:
        return RSENSE_4M7_OHM;
    default:
        return 0.0;
    }
}

/* ------------------------------------------------------------------------- */
/* Help and status                                                           */
/* ------------------------------------------------------------------------- */

/**
 * @brief Print the command list.
 *
 * Mirrors the ESP32 help text, with the G474-specific additions (`mode`,
 * `res`, `rcal?`, `rcal`, `rcalclear`, `reboot`) integrated in place.
 */
void console_help(void)
{
    console_printf("\r\n%s %s - %s\r\n",
                   ESWCAP_FW_NAME, ESWCAP_FW_VERSION, ESWCAP_FW_TARGET);
    console_printf("Commands:\r\n");
    console_printf("  help | h                     this list\r\n");
    console_printf("  status | p                   instrument and calibration status\r\n");
    console_printf("  0 | 1 | 2 | 3                select range for cal/probe\r\n");
    console_printf("  start | stop                 start / stop autoranging\r\n");
    console_printf("  single                       one autoranging cycle\r\n");
    console_printf("  precharge | discharge        manual front-end bias control\r\n");
    console_printf("  idle                         end session (auto-discharge if on)\r\n");
    console_printf("  autoprecharge on|off         alias: autocharge on|off\r\n");
    console_printf("  autodischarge on|off         end-of-cycle discharge policy\r\n");
    console_printf("  auto?                        show the automation flags\r\n");
    console_printf("  auto | range <0-3>           autorange / lock one range\r\n");
    console_printf("  stream on|off                @@EVT telemetry\r\n");
    console_printf("  curve on|off                 include charge curves\r\n");
    console_printf("  led auto|off|brightness      or: led <r> <g> <b>\r\n");
    console_printf("  mode c|r                     capacitance / resistance mode\r\n");
    console_printf("  res                          one autoranged resistance read\r\n");
    console_printf("  rcal? | rcalclear            show / reset R-mode calibration\r\n");
    console_printf("  rcal <0-3> <ohm> [gain]      store a sense-element calibration\r\n");
    console_printf("  rcal tia <rf> <mv> [gain]    store the TIA calibration\r\n");
    console_printf("  zero | zeroall               tare oscillator T0 (zeroall: 2 and 3)\r\n");
    console_printf("  cal <pF>                     one-point oscillator K\r\n");
    console_printf("  cal1 <pF> | cal2 <pF>        two-point oscillator K/T0\r\n");
    console_printf("  cal? | calclear [0-3]        show / clear oscillator calibration\r\n");
    console_printf("  adccal <pF>                  one-point ADC R_eff/C0\r\n");
    console_printf("  adccal1 <pF> | adccal2 <pF>  two-point ADC calibration\r\n");
    console_printf("  adccal? | adccalclear [0-3]  show / clear ADC calibration\r\n");
    console_printf("  board?                       board constants\r\n");
    console_printf("  boardset cblock <uF>         set DC-blocking capacitor\r\n");
    console_printf("  boardset rbias <ohm>         set bias resistor\r\n");
    console_printf("  boardset r0..r3 <ohm>        set a range resistor\r\n");
    console_printf("  boardclear                   restore compiled board defaults\r\n");
    console_printf("  probe <0-3>                  front-end connectivity probe\r\n");
    console_printf("  stat                         uptime and cycle counters\r\n");
    console_printf("  reset | reboot               NVIC system reset\r\n");
}

/**
 * @brief `status` / `p`: a one-screen snapshot of the instrument.
 */
static void cmd_status(void)
{
    uint8_t bits = fe_pin_bits();
    uint8_t selected = cur_range();

    console_printf(">> status\r\n");
    console_printf("   mode        : %c (%s)\r\n",
                   (g_inst.mode == OP_MODE_RES) ? 'r' : 'c',
                   (g_inst.mode == OP_MODE_RES) ? "resistance"
                                                : "capacitance");
    console_printf("   run         : %s%s\r\n",
                   g_inst.run ? "on" : "off",
                   g_inst.single ? " (single)" : "");
    console_printf("   fe state    : %s\r\n", fe_state_name(fe_state()));
    console_printf("   charge      : %s (charged=%d discharged=%d)\r\n",
                   fe_charged() ? "charged / N_DUT biased" : "not charged",
                   fe_charged() ? 1 : 0,
                   fe_discharged() ? 1 : 0);
    console_printf("   switches    : 0x%02X (ssr13=%u ssr2=%u k1=%u k2=%u "
                   "rmux=%u cmux=%u rforce_low=%u)\r\n",
                   (unsigned)bits,
                   (unsigned)((bits >> 0) & 0x01U),
                   (unsigned)((bits >> 1) & 0x01U),
                   (unsigned)((bits >> 3) & 0x01U),
                   (unsigned)((bits >> 4) & 0x01U),
                   (unsigned)((bits >> 5) & 0x01U),
                   (unsigned)((bits >> 6) & 0x01U),
                   (unsigned)((bits >> 7) & 0x01U));
    console_printf("   drive       : %s\r\n",
                   ((bits & 0x04U) != 0U) ? "high" : "low");
    console_printf("   range sel   : %u (%s)\r\n",
                   (unsigned)selected, g_ranges[selected].label);
    console_printf("   auto        : precharge=%s discharge=%s %s\r\n",
                   g_inst.auto_precharge ? "on" : "off",
                   g_inst.auto_discharge ? "on" : "off",
                   g_inst.locked ? "locked" : "autorange");
    console_printf("   stream      : %s (curve=%s)\r\n",
                   g_inst.stream ? "on" : "off",
                   g_inst.curve ? "on" : "off");
    console_printf("   uptime      : %lu ms\r\n",
                   (unsigned long)bsp_millis());
    console_printf("   cycles      : %lu (last %lu ms)\r\n",
                   (unsigned long)g_inst.cycles,
                   (unsigned long)g_inst.last_cycle_ms);

    console_printf("   oscillator calibration:\r\n");
    for (int i = 0; i < RANGE_COUNT; ++i) {
        uint8_t r = (uint8_t)i;

        console_printf("     [%d] %-7s K=%10.6f T0=%10.3f us %-8s %s\r\n",
                       i,
                       g_ranges[i].label,
                       cal_osc_k(r),
                       cal_osc_t0(r),
                       cal_osc_has_t0(r) ? "tared" : "untared",
                       cal_osc_valid(r) ? "cal" : "uncal");
    }

    console_printf("   ADC calibration:\r\n");
    for (int i = 0; i < RANGE_COUNT; ++i) {
        uint8_t r = (uint8_t)i;

        console_printf("     [%d] %-7s R_eff=", i, g_ranges[i].label);
        print_resistance(cal_adc_r_eff(r));
        console_printf(" C0=%.3f pF %s\r\n",
                       cal_adc_c0(r) * 1.0e12,
                       cal_adc_valid(r) ? "valid" : "invalid");
    }
}

/* ------------------------------------------------------------------------- */
/* Front-end and acquisition commands                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief `precharge`: bias N_DUT and leave the node isolated.
 */
static void cmd_precharge(void)
{
    g_inst.run = false;

    fe_precharge();
    /* fe_precharge() already does both, but the console contract explicitly
     * names them.  Keeping the calls makes the state transition audible in
     * this file even if the BSP implementation changes. */
    fe_set_charged(true);
    fe_set_state(FE_PRECHARGED);

    evt_fe();
    console_printf(">> precharged (%s)\r\n", PRECHARGE_WARNING);
    evt_ack("precharge", 1, PRECHARGE_WARNING);
}

/**
 * @brief `discharge`: bleed N_DUT and C_block to ground.
 */
static void cmd_discharge(void)
{
    g_inst.run = false;

    fe_set_state(FE_DISCHARGE);
    fe_discharge();
    fe_set_charged(false);
    fe_set_state(FE_IDLE);

    evt_fe();
    console_printf(">> discharged: node is safe to touch\r\n");
    evt_ack("discharge", 1, "node discharged");
}

/**
 * @brief `idle`: end a session, honouring the auto-discharge policy.
 */
static void cmd_idle(void)
{
    g_inst.run = false;

    fe_finish(g_inst.auto_discharge, fe_charged());
    evt_fe();

    if (fe_charged()) {
        console_printf(">> idle (%s)\r\n", PRECHARGE_WARNING);
        evt_ack("idle", 1, PRECHARGE_WARNING);
    } else {
        console_printf(">> idle: node discharged\r\n");
        evt_ack("idle", 1, "node discharged");
    }
}

/**
 * @brief `autoprecharge on|off` and its `autocharge` alias.
 */
static void cmd_autoprecharge(const char *arg)
{
    int value = parse_onoff(arg);

    if (value < 0) {
        console_printf(">> usage: autoprecharge on|off  (alias autocharge)\r\n");
        evt_ack("autoprecharge", 0, "expected on|off");
        return;
    }

    g_inst.auto_precharge = (value != 0);
    console_printf(">> autoprecharge %s\r\n",
                   g_inst.auto_precharge ? "on" : "off");
    evt_fe();
    evt_ack("autoprecharge", 1,
            g_inst.auto_precharge ? "auto precharge on"
                                  : "auto precharge off");
}

/**
 * @brief `autodischarge on|off`.
 */
static void cmd_autodischarge(const char *arg)
{
    int value = parse_onoff(arg);

    if (value < 0) {
        console_printf(">> usage: autodischarge on|off\r\n");
        evt_ack("autodischarge", 0, "expected on|off");
        return;
    }

    g_inst.auto_discharge = (value != 0);
    console_printf(">> autodischarge %s\r\n",
                   g_inst.auto_discharge ? "on" : "off");
    evt_fe();
    evt_ack("autodischarge", 1,
            g_inst.auto_discharge ? "auto discharge on"
                                  : "auto discharge off");
}

/**
 * @brief `stream on|off`.
 *
 * evt_set_stream() is also the owner of g_inst.stream, so there is no second
 * copy of the flag to keep in sync here.
 */
static void cmd_stream(const char *arg)
{
    int value = parse_onoff(arg);

    if (value < 0) {
        console_printf(">> usage: stream on|off\r\n");
        evt_ack("stream", 0, "expected on|off");
        return;
    }

    evt_set_stream(value != 0);
    console_printf(">> stream %s\r\n", evt_stream_enabled() ? "on" : "off");
    evt_ack("stream", 1, evt_stream_enabled() ? "stream on" : "stream off");
}

/**
 * @brief `curve on|off`.
 */
static void cmd_curve(const char *arg)
{
    int value = parse_onoff(arg);

    if (value < 0) {
        console_printf(">> usage: curve on|off\r\n");
        evt_ack("curve", 0, "expected on|off");
        return;
    }

    g_inst.curve = (value != 0);
    console_printf(">> curve %s\r\n", g_inst.curve ? "on" : "off");
    evt_ack("curve", 1, g_inst.curve ? "curve data on" : "curve data off");
}

/**
 * @brief `led` command.
 *
 * The G474 board has one monochrome LED, so the ESP32 RGB command is mapped to
 * brightness: `max(r,g,b)` for the three-number form and the number itself for
 * `led <brightness>`.
 */
static void cmd_led(const char *arg)
{
    int r_value;
    int g_value;
    int b_value;
    int brightness;
    char extra = '\0';

    if (only_token_is(arg, "auto")) {
        led_set_auto();
        console_printf(">> led auto\r\n");
        return;
    }
    if (only_token_is(arg, "off")) {
        led_off();
        console_printf(">> led off\r\n");
        return;
    }

    /* Three-number form first: a trailing token makes the single-value parse
     * fail, so the order does not need a special case. */
    if (sscanf(arg, "%d %d %d %c", &r_value, &g_value, &b_value, &extra) == 3) {
        if ((r_value < 0) || (r_value > 255) ||
            (g_value < 0) || (g_value > 255) ||
            (b_value < 0) || (b_value > 255)) {
            console_printf(">> led: each RGB component must be 0..255\r\n");
            return;
        }
        brightness = r_value;
        if (g_value > brightness) {
            brightness = g_value;
        }
        if (b_value > brightness) {
            brightness = b_value;
        }
        led_manual((uint8_t)brightness);
        console_printf(">> led manual brightness %d (from %d,%d,%d)\r\n",
                       brightness, r_value, g_value, b_value);
        return;
    }

    if (sscanf(arg, "%d %c", &brightness, &extra) == 1) {
        if ((brightness < 0) || (brightness > 255)) {
            console_printf(">> led: brightness must be 0..255\r\n");
            return;
        }
        led_manual((uint8_t)brightness);
        console_printf(">> led manual brightness %d\r\n", brightness);
        return;
    }

    console_printf(">> usage: led auto|off|<brightness>|<r> <g> <b>\r\n");
}

/**
 * @brief `mode c|r`: switch acquisition mode.
 *
 * R-mode requires a discharged node because fe_set_mode() refuses to re-route
 * while the C-mode bias may still be present.  The failure is reported as an
 * instruction to discharge, not as a generic error, because that is the only
 * action that can make the command succeed.
 */
static void cmd_mode(const char *arg)
{
    bool to_res = only_token_is(arg, "r");

    if (!to_res && !only_token_is(arg, "c")) {
        console_printf(">> usage: mode c|r\r\n");
        evt_ack("mode", 0, "expected c|r");
        return;
    }

    /* Repeating the current mode must not run the teardown path: `mode c` in
     * C mode would otherwise call rmode_leave() -> fe_r_exit(), which forces
     * the front end to IDLE without changing the C-mode charge state. */
    if ((to_res && (g_inst.mode == OP_MODE_RES)) ||
        (!to_res && (g_inst.mode == OP_MODE_CAP))) {
        console_printf(">> already in %c mode\r\n", to_res ? 'r' : 'c');
        evt_ack("mode", 1, to_res ? "already R-mode" : "already C-mode");
        return;
    }

    if (to_res && !ESWCAP_HAS_RMODE) {
        console_printf(">> mode r unavailable: R-mode hardware is not fitted "
                       "(build with -DESWCAP_HAS_RMODE=1)\r\n");
        evt_ack("mode", 0, "R-mode hardware not fitted");
        return;
    }

    if (!to_res) {
        /* rmode_leave() also returns the interlock to C-mode; the explicit
         * fe_set_mode() call is the one whose result the console reports. */
        rmode_leave();
        if (!fe_set_mode(OP_MODE_CAP)) {
            console_printf(">> mode c refused: discharge required first\r\n");
            evt_ack("mode", 0, "discharge required before C-mode");
            return;
        }

        /* Switching the analogue front end while the scheduler is still in
         * autorange mode would let the C-mode pipeline re-enter on the next
         * pass. Stop only after the interlock accepted the new mode; `start`
         * resumes explicitly. The ESP32 had no mode command, so this is new
         * with the port. */
        g_inst.run = false;
        g_inst.single = false;
        g_inst.mode = OP_MODE_CAP;
        evt_fe();
        console_printf(">> mode c (capacitance)\r\n");
        evt_ack("mode", 1, "C-mode");
        return;
    }

    if (!fe_set_mode(OP_MODE_RES)) {
        console_printf(">> mode r refused: discharge required first\r\n");
        evt_ack("mode", 0, "discharge required before R-mode");
        return;
    }

    /* See the C-mode branch: the scheduler must not re-enter the C pipeline
     * after the front end has moved to R-mode. */
    g_inst.run = false;
    g_inst.single = false;

    if (!rmode_enter()) {
        /* The mode switch itself succeeded, but the analogue blocks did not
         * come up.  Put the interlock back where it was rather than leaving a
         * half-configured R front end. */
        (void)fe_set_mode(OP_MODE_CAP);
        console_printf(">> mode r failed: R-mode could not be entered\r\n");
        evt_fe();
        evt_ack("mode", 0, "R-mode entry failed");
        return;
    }
    g_inst.mode = OP_MODE_RES;
    evt_fe();
    console_printf(">> mode r (resistance)\r\n");
    evt_ack("mode", 1, "R-mode");
}

/**
 * @brief `res`: one autoranged four-wire resistance measurement.
 */
static void cmd_res(void)
{
    res_sample_t s;

    memset(&s, 0, sizeof(s));

    /* Say this explicitly: without an R session there is no force path at all,
     * so the generic failure text below would be misleading. */
    if (g_inst.mode != OP_MODE_RES) {
        console_printf(">> res: R mode is not active; send `mode r` first\r\n");
        evt_ack("res", 0, "not in R mode");
        return;
    }

    bool ok = rmode_measure_auto(&s);

    if (fe_r_faulted()) {
        console_printf(">> res aborted: V_N overvoltage; force path "
                       "removed\r\n");
        rmode_leave();
        g_inst.mode = OP_MODE_CAP;
        evt_fe();
        evt_ack("res", 0, "overvoltage fault");
        return;
    }

    if (!ok || !s.valid) {
        /* Report which wall was hit. The three cases need different actions:
         * too large a DUT needs the TIA, too small is below the 100 ohm
         * element, and no reading at all is a hardware problem. */
        if (s.saturated || (s.v_n_mv >= (double)RMODE_VN_MAX_MV)) {
            console_printf(">> res: out of compliance (V_N=%.1f mV, "
                           "V_B=%.1f mV)\r\n",
                           s.v_n_mv, s.v_b_mv);
            evt_ack("res", 0, "out of compliance");
        } else if ((s.v_b_mv > 0.0) && (s.v_n_mv > 0.0) &&
                   ((s.v_n_mv - s.v_b_mv) < RMODE_MIN_DV_MV)) {
            console_printf(">> res: below the resolution of the 100 ohm "
                           "element (dV=%.2f mV)\r\n",
                           s.v_n_mv - s.v_b_mv);
            evt_ack("res", 0, "below resolution");
        } else {
            console_printf(">> res failed: no usable R-mode reading\r\n");
            evt_ack("res", 0, "no usable R-mode measurement");
        }
        return;
    }

    console_printf(">> R = ");
    print_resistance(s.r_ohm);
    if (s.method == RMODE_TIA) {
        console_printf("  TIA  V_N=%.1f mV V_TIA=%.1f mV I=%.3f uA q=%.3f\r\n",
                       s.v_n_mv, s.v_tia_mv, s.current_ua, s.quality);
    } else {
        console_printf("  Kelvin e%d  V_N=%.1f mV V_B=%.1f mV I=%.3f uA "
                       "q=%.3f\r\n",
                       s.element, s.v_n_mv, s.v_b_mv, s.current_ua,
                       s.quality);
    }
    evt_res(&s);
}

/* ------------------------------------------------------------------------- */
/* Autorange / range-selection commands                                      */
/* ------------------------------------------------------------------------- */

/**
 * @brief `auto`: return to autoranging.
 */
static void cmd_auto(void)
{
    g_inst.locked = false;
    g_inst.lock_range = -1;
    evt_fe();
    console_printf(">> autoranging\r\n");
    evt_ack("auto", 1, "autoranging");
}

/**
 * @brief `range <0-3>`: lock the autoranger to one range.
 */
static void cmd_range(const char *arg)
{
    int range;

    if (!parse_int_arg(arg, &range) ||
        (range < 0) || (range >= RANGE_COUNT)) {
        console_printf(">> usage: range <0-3>\r\n");
        evt_ack("range", 0, "range must be 0..3");
        return;
    }

    g_inst.locked = true;
    g_inst.lock_range = range;
    evt_fe();
    console_printf(">> range locked to %d (%s)\r\n",
                   range, g_ranges[range].label);
    evt_ack("range", 1, "range locked");
}

/* ------------------------------------------------------------------------- */
/* Oscillator calibration: zero / zeroall / cal*                             */
/* ------------------------------------------------------------------------- */

/**
 * @brief `zero`: tare the oscillator T0 on the selected range.
 *
 * Range 0 or 1 is allowed but warned about: an empty socket on the 100 R or
 * 1 kR range can oscillate above OSC_MAX_F_HZ, so the resulting sample is
 * marked implausible by the measurement path.  The tare can still be attempted
 * because a fixture or probe may load the node enough to bring it in band.
 */
static void cmd_zero(void)
{
    uint8_t range = cur_range();
    sample_t s;

    if (range < 2U) {
        console_printf(">> WARNING: empty socket on range %u oscillates too "
                       "fast to tare; the reading will be rejected\r\n",
                       (unsigned)range);
    }

    s = autorange_measure_osc(range);
    if (!s.valid || !(s.tau_us > 0.0) || !isfinite(s.tau_us)) {
        console_printf(">> zero failed on range %u (%s): no valid oscillator "
                       "capture\r\n",
                       (unsigned)range, g_ranges[range].label);
        evt_ack("zero", 0, "invalid oscillator capture");
        return;
    }

    cal_osc_set(range, cal_osc_k(range), s.tau_us, true,
                cal_osc_valid(range));
    bool saved = save_cal();

    console_printf(">> range %u (%s) T0 = %.3f us at %.3f Hz\r\n",
                   (unsigned)range, g_ranges[range].label,
                   s.tau_us, s.freq_hz);
    evt_tare(range, 0, 1, s.freq_hz, s.tau_us, 0.0, 1);
    evt_ack("zero", saved ? 1 : 0,
            saved ? "tare stored" : "tare in RAM only (flash save failed)");
}

/**
 * @brief `zeroall`: tare the two oscillator ranges that can run open.
 *
 * Ranges 0 and 1 are deliberately excluded: with nothing in the socket their
 * period is too short to be a useful tare.  Ranges 2 and 3 are measured in one
 * pass and the record is saved once, avoiding two flash erase/write cycles.
 */
static void cmd_zeroall(void)
{
    int tared = 0;

    for (int i = 0; i < 2; ++i) {
        uint8_t range = (uint8_t)(2 + i);
        sample_t s;

        console_printf(">> zeroall [%d/2] range %u (%s)...\r\n",
                       i + 1, (unsigned)range, g_ranges[range].label);

        s = autorange_measure_osc(range);
        if (!s.valid || !(s.tau_us > 0.0) || !isfinite(s.tau_us)) {
            console_printf("   FAILED: no valid oscillator capture\r\n");
            evt_tare(range, i, 2, 0.0, 0.0, 0.0, 0);
            continue;
        }

        cal_osc_set(range, cal_osc_k(range), s.tau_us, true,
                    cal_osc_valid(range));
        console_printf("   T0 = %.3f us at %.3f Hz\r\n", s.tau_us, s.freq_hz);
        evt_tare(range, i, 2, s.freq_hz, s.tau_us, 0.0, 1);
        ++tared;
    }

    bool saved = save_cal();
    const char *message;

    if (!saved) {
        message = "tared in RAM; flash save failed";
    } else if (tared == 2) {
        message = "both oscillator ranges tared";
    } else {
        message = "one or more tare attempts failed";
    }

    console_printf(">> zeroall: %d/2 ranges tared%s\r\n",
                   tared, saved ? "" : " (flash save failed)");
    evt_ack("zeroall", (saved && (tared == 2)) ? 1 : 0, message);
}

/**
 * @brief `cal <pF>`: one-point oscillator calibration.
 *
 * The existing tare is used when present.  Without one, the legacy per-cycle
 * delay is subtracted, matching osc_measure_sample(): both routes have to use
 * the same offset or the solved K would not reproduce the runtime reading.
 */
static void cmd_cal(const char *arg)
{
    double ref_pf;

    if (!parse_double_arg(arg, &ref_pf) || (ref_pf <= 0.0)) {
        console_printf(">> usage: cal <pF>\r\n");
        evt_ack("cal", 0, "invalid reference capacitance");
        return;
    }

    uint8_t range = cur_range();
    sample_t s = autorange_measure_osc(range);

    if (!s.valid || !(s.tau_us > 0.0)) {
        console_printf(">> cal failed: no valid oscillator capture on range "
                       "%u (%s)\r\n",
                       (unsigned)range, g_ranges[range].label);
        evt_ack("cal", 0, "invalid oscillator capture");
        return;
    }

    double offset_us = cal_osc_has_t0(range) ? cal_osc_t0(range)
                                             : cal_osc_delay(range);
    double k = osc_k_from_ref(s.tau_us, offset_us,
                              g_ranges[range].resistance_ohms,
                              ref_pf * 1.0e-12, board_c_block_f());

    bool saved = false;
    if (k > 0.0) {
        cal_osc_set(range, k, cal_osc_t0(range), cal_osc_has_t0(range), true);
        saved = save_cal();
    }

    evt_calpt(range, "cal", ref_pf, s.freq_hz);
    evt_calres(range, "cal", k, cal_osc_t0(range), (k > 0.0) ? 1 : 0);

    if (k > 0.0) {
        console_printf(">> range %u (%s) K = %.6f (ref %.3f pF, %.3f Hz)\r\n",
                       (unsigned)range, g_ranges[range].label,
                       k, ref_pf, s.freq_hz);
        evt_ack("cal", saved ? 1 : 0,
                saved ? "one-point calibration stored"
                      : "calibration in RAM only (flash save failed)");
    } else {
        console_printf(">> cal rejected: solved K is outside the physical "
                       "range\r\n");
        evt_ack("cal", 0, "solved K out of range");
    }
}

/**
 * @brief `cal1 <pF>` / `cal2 <pF>`: two-point oscillator calibration.
 *
 * The two points must come from the same range, because K and T0 are
 * per-range constants.  A mismatched range is rejected without consuming the
 * pending first point, so the operator can select the right range and retry
 * `cal2` without re-running `cal1`.
 *
 * @param arg    Reference capacitance in pF.
 * @param second false for cal1, true for cal2.
 */
static void cmd_cal_two_point(const char *arg, bool second)
{
    double ref_pf;
    const char *name = second ? "cal2" : "cal1";

    if (!parse_double_arg(arg, &ref_pf) || (ref_pf <= 0.0)) {
        console_printf(">> usage: %s <pF>\r\n", name);
        evt_ack(name, 0, "invalid reference capacitance");
        return;
    }

    uint8_t range = cur_range();

    if (second) {
        if (!s_osc_cal1.valid) {
            console_printf(">> cal2 rejected: no cal1 point pending; run "
                           "cal1 <pF> first\r\n");
            evt_ack("cal2", 0, "no pending cal1 point");
            return;
        }
        if (s_osc_cal1.range != range) {
            console_printf(">> cal2 rejected: pending cal1 is on range %u, "
                           "but range %u is selected\r\n",
                           (unsigned)s_osc_cal1.range, (unsigned)range);
            evt_ack("cal2", 0, "cal1 and cal2 must use the same range");
            return;
        }
    } else {
        /* A failed new cal1 must not leave an older point eligible. */
        s_osc_cal1.valid = false;
    }

    sample_t s = autorange_measure_osc(range);
    if (!s.valid || !(s.tau_us > 0.0)) {
        console_printf(">> %s failed: no valid oscillator capture on range "
                       "%u (%s)\r\n",
                       name, (unsigned)range, g_ranges[range].label);
        evt_ack(name, 0, "invalid oscillator capture");
        return;
    }

    if (!second) {
        s_osc_cal1.valid = true;
        s_osc_cal1.range = range;
        s_osc_cal1.period_us = s.tau_us;
        s_osc_cal1.ref_f = ref_pf * 1.0e-12;

        evt_calpt(range, "cal1", ref_pf, s.freq_hz);
        console_printf(">> cal1 stored: range %u (%s), %.3f pF at %.3f Hz\r\n",
                       (unsigned)range, g_ranges[range].label,
                       ref_pf, s.freq_hz);
        console_printf(">> now run cal2 <pF> on the same range\r\n");
        evt_ack("cal1", 1, "first calibration point stored");
        return;
    }

    double p1_us = s_osc_cal1.period_us;
    double c1_f = s_osc_cal1.ref_f;
    double c2_f = ref_pf * 1.0e-12;

    /* The pair is consumed as soon as the second capture succeeds, even if
     * the solve rejects it.  Keeping it would make a retry silently reuse a
     * stale first point. */
    s_osc_cal1.valid = false;

    double k = 0.0;
    double t0 = 0.0;
    bool ok = osc_solve_two_point(p1_us, c1_f, s.tau_us, c2_f,
                                  g_ranges[range].resistance_ohms,
                                  board_c_block_f(), &k, &t0);

    evt_calpt(range, "cal2", ref_pf, s.freq_hz);

    if (!ok) {
        evt_calres(range, "two", 0.0, 0.0, 0);
        console_printf(">> cal2 rejected: the two points are too close or "
                       "produce an unphysical K\r\n");
        evt_ack("cal2", 0, "two-point solve rejected");
        return;
    }

    cal_osc_set(range, k, t0, true, true);
    bool saved = save_cal();

    evt_calres(range, "two", k, t0, 1);
    console_printf(">> range %u (%s) two-point K = %.6f, T0 = %.3f us\r\n",
                   (unsigned)range, g_ranges[range].label, k, t0);
    evt_ack("cal2", saved ? 1 : 0,
            saved ? "two-point calibration stored"
                  : "calibration in RAM only (flash save failed)");
}

/**
 * @brief `cal?`: show the oscillator calibration table.
 */
static void cmd_cal_show(void)
{
    console_printf(">> oscillator calibration\r\n");
    for (int i = 0; i < RANGE_COUNT; ++i) {
        uint8_t r = (uint8_t)i;

        console_printf("   %c[%d] %-7s K=%10.6f T0=%10.3f us %-8s %s\r\n",
                       (r == cur_range()) ? '*' : ' ',
                       i, g_ranges[i].label,
                       cal_osc_k(r),
                       cal_osc_t0(r),
                       cal_osc_has_t0(r) ? "tared" : "untared",
                       cal_osc_valid(r) ? "cal" : "uncal");
    }
}

/**
 * @brief `calclear [0-3]`: clear one oscillator calibration entry.
 */
static void cmd_cal_clear(const char *arg)
{
    int range = (int)cur_range();

    if (*arg != '\0') {
        if (!parse_int_arg(arg, &range) ||
            (range < 0) || (range >= RANGE_COUNT)) {
            console_printf(">> usage: calclear [0-3]\r\n");
            evt_ack("calclear", 0, "range must be 0..3");
            return;
        }
    }

    cal_osc_clear((uint8_t)range);
    bool saved = save_cal();

    console_printf(">> oscillator calibration cleared on range %d (%s)\r\n",
                   range, g_ranges[range].label);
    evt_ack("calclear", saved ? 1 : 0,
            saved ? "oscillator calibration cleared"
                  : "cleared in RAM; flash save failed");
}

/* ------------------------------------------------------------------------- */
/* ADC calibration: adccal*                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief `adccal <pF>`: one-point ADC calibration.
 *
 * C0 is taken from an existing valid calibration when available; otherwise
 * the compiled stray value is used as the cold-start guess.  The solved chain
 * resistance is reported together with its offset from the nominal range
 * resistor, which makes mux and buffer contributions visible to the operator.
 */
static void cmd_adccal(const char *arg)
{
    double ref_pf;

    if (!parse_double_arg(arg, &ref_pf) || (ref_pf <= 0.0)) {
        console_printf(">> usage: adccal <pF>\r\n");
        evt_ack("adccal", 0, "invalid reference capacitance");
        return;
    }

    uint8_t range = cur_range();
    sample_t s = autorange_measure_adc(range);

    if (!s.valid || !(s.tau_us > 0.0)) {
        console_printf(">> adccal failed: no valid ADC capture on range %u "
                       "(%s)\r\n",
                       (unsigned)range, g_ranges[range].label);
        evt_ack("adccal", 0, "invalid ADC capture");
        return;
    }

    double c0_f = cal_adc_valid(range) ? cal_adc_c0(range)
                                       : STRAY_CAPACITANCE_F;
    double r_eff = 0.0;

    evt_adccalpt(range, "cal", ref_pf, s.tau_us);

    if (!cal_solve_adc_one_point(s.tau_us, ref_pf * 1.0e-12,
                                 board_c_block_f(), c0_f, &r_eff)) {
        evt_adccalres(range, "cal", 0.0, c0_f, 0);
        console_printf(">> adccal rejected: solver could not form a physical "
                       "R_eff\r\n");
        evt_ack("adccal", 0, "ADC one-point solve rejected");
        return;
    }

    double r_chain = adc_chain_from_eff(r_eff, s.v_inf_mv);

    if (!isfinite(r_chain) || !(r_chain > 1.0) || (r_chain > 1.0e7)) {
        evt_adccalres(range, "cal", r_chain, c0_f, 0);
        console_printf(">> adccal rejected: R_chain = ");
        print_resistance(r_chain);
        console_printf(" is outside the plausible 1 R .. 10 MR window\r\n");
        evt_ack("adccal", 0, "R_chain outside plausible window");
        return;
    }

    cal_adc_set(range, r_chain, c0_f, true);
    bool saved = save_cal();

    evt_adccalres(range, "cal", r_chain, c0_f, 1);
    console_printf(">> range %u (%s) R_eff = ",
                   (unsigned)range, g_ranges[range].label);
    print_resistance(r_chain);
    console_printf(", series offset = ");
    print_resistance(r_chain - g_ranges[range].resistance_ohms);
    console_printf(", C0 = %.3f pF\r\n", c0_f * 1.0e12);
    evt_ack("adccal", saved ? 1 : 0,
            saved ? "ADC one-point calibration stored"
                  : "calibration in RAM only (flash save failed)");
}

/**
 * @brief `adccal1 <pF>` / `adccal2 <pF>`: two-point ADC calibration.
 *
 * Both points must be taken on the same range, and the second point's
 * asymptote is the one used to fold the effective resistance back into the
 * chain resistance, matching the runtime adc_effective_r() path.
 *
 * @param arg    Reference capacitance in pF.
 * @param second false for adccal1, true for adccal2.
 */
static void cmd_adccal_two_point(const char *arg, bool second)
{
    double ref_pf;
    const char *name = second ? "adccal2" : "adccal1";

    if (!parse_double_arg(arg, &ref_pf) || (ref_pf <= 0.0)) {
        console_printf(">> usage: %s <pF>\r\n", name);
        evt_ack(name, 0, "invalid reference capacitance");
        return;
    }

    uint8_t range = cur_range();

    if (second) {
        if (!s_adc_cal1.valid) {
            console_printf(">> adccal2 rejected: no adccal1 point pending; "
                           "run adccal1 <pF> first\r\n");
            evt_ack("adccal2", 0, "no pending adccal1 point");
            return;
        }
        if (s_adc_cal1.range != range) {
            console_printf(">> adccal2 rejected: pending adccal1 is on range "
                           "%u, but range %u is selected\r\n",
                           (unsigned)s_adc_cal1.range, (unsigned)range);
            evt_ack("adccal2", 0, "adccal1 and adccal2 must use the same range");
            return;
        }
    } else {
        s_adc_cal1.valid = false;
    }

    sample_t s = autorange_measure_adc(range);
    if (!s.valid || !(s.tau_us > 0.0)) {
        console_printf(">> %s failed: no valid ADC capture on range %u "
                       "(%s)\r\n",
                       name, (unsigned)range, g_ranges[range].label);
        evt_ack(name, 0, "invalid ADC capture");
        return;
    }

    evt_adccalpt(range, second ? "adccal2" : "adccal1", ref_pf, s.tau_us);

    if (!second) {
        s_adc_cal1.valid = true;
        s_adc_cal1.range = range;
        s_adc_cal1.period_us = s.tau_us;
        s_adc_cal1.ref_f = ref_pf * 1.0e-12;

        console_printf(">> adccal1 stored: range %u (%s), %.3f pF, "
                       "tau = %.3f us\r\n",
                       (unsigned)range, g_ranges[range].label,
                       ref_pf, s.tau_us);
        console_printf(">> now run adccal2 <pF> on the same range\r\n");
        evt_ack("adccal1", 1, "first ADC calibration point stored");
        return;
    }

    double tau1_us = s_adc_cal1.period_us;
    double c1_f = s_adc_cal1.ref_f;
    double c2_f = ref_pf * 1.0e-12;

    s_adc_cal1.valid = false;

    double r_eff = 0.0;
    double c0_f = 0.0;

    if (!cal_solve_adc_two_point(tau1_us, c1_f, s.tau_us, c2_f,
                                 board_c_block_f(), &r_eff, &c0_f)) {
        evt_adccalres(range, "two", 0.0, 0.0, 0);
        console_printf(">> adccal2 rejected: solver could not form a physical "
                       "R_eff/C0 pair\r\n");
        evt_ack("adccal2", 0, "ADC two-point solve rejected");
        return;
    }

    double r_chain = adc_chain_from_eff(r_eff, s.v_inf_mv);

    if (!isfinite(r_chain) || !(r_chain > 1.0) || (r_chain > 1.0e7) ||
        !isfinite(c0_f) || (c0_f < 0.0)) {
        evt_adccalres(range, "two", r_chain, c0_f, 0);
        console_printf(">> adccal2 rejected: R_chain = ");
        print_resistance(r_chain);
        console_printf(" or C0 = %.3f pF is outside the plausible window\r\n",
                       c0_f * 1.0e12);
        evt_ack("adccal2", 0, "R_chain/C0 outside plausible window");
        return;
    }

    cal_adc_set(range, r_chain, c0_f, true);
    bool saved = save_cal();

    evt_adccalres(range, "two", r_chain, c0_f, 1);
    console_printf(">> range %u (%s) two-point R_eff = ",
                   (unsigned)range, g_ranges[range].label);
    print_resistance(r_chain);
    console_printf(", series offset = ");
    print_resistance(r_chain - g_ranges[range].resistance_ohms);
    console_printf(", C0 = %.3f pF\r\n", c0_f * 1.0e12);
    evt_ack("adccal2", saved ? 1 : 0,
            saved ? "ADC two-point calibration stored"
                  : "calibration in RAM only (flash save failed)");
}

/**
 * @brief `adccal?`: show the ADC calibration table.
 */
static void cmd_adccal_show(void)
{
    console_printf(">> ADC calibration\r\n");
    for (int i = 0; i < RANGE_COUNT; ++i) {
        uint8_t r = (uint8_t)i;
        double r_eff = cal_adc_r_eff(r);

        console_printf("   %c[%d] %-7s R_eff=",
                       (r == cur_range()) ? '*' : ' ',
                       i, g_ranges[i].label);
        print_resistance(r_eff);
        console_printf(" offset=");
        print_resistance(r_eff - g_ranges[i].resistance_ohms);
        console_printf(" C0=%.3f pF %s\r\n",
                       cal_adc_c0(r) * 1.0e12,
                       cal_adc_valid(r) ? "valid" : "invalid");
    }
}

/**
 * @brief `adccalclear [0-3]`: clear one ADC calibration entry.
 */
static void cmd_adccal_clear(const char *arg)
{
    int range = (int)cur_range();

    if (*arg != '\0') {
        if (!parse_int_arg(arg, &range) ||
            (range < 0) || (range >= RANGE_COUNT)) {
            console_printf(">> usage: adccalclear [0-3]\r\n");
            evt_ack("adccalclear", 0, "range must be 0..3");
            return;
        }
    }

    cal_adc_clear((uint8_t)range);
    bool saved = save_cal();

    console_printf(">> ADC calibration cleared on range %d (%s)\r\n",
                   range, g_ranges[range].label);
    evt_ack("adccalclear", saved ? 1 : 0,
            saved ? "ADC calibration cleared"
                  : "cleared in RAM; flash save failed");
}

/* ------------------------------------------------------------------------- */
/* Board constants                                                           */
/* ------------------------------------------------------------------------- */

/**
 * @brief `board?`: show the effective board constants.
 */
static void cmd_board_show(void)
{
    console_printf(">> board constants\r\n");
    console_printf("   C_block : %.4f uF\r\n", board_c_block_f() * 1.0e6);
    console_printf("   R_bias  : ");
    print_resistance(board_r_bias_ohm());
    console_printf("\r\n");

    for (int i = 0; i < RANGE_COUNT; ++i) {
        console_printf("   r%d %-7s: ", i, g_ranges[i].label);
        print_resistance(g_ranges[i].resistance_ohms);
        console_printf("\r\n");
    }

    console_printf("   overrides: %s\r\n",
                   board_cal_overridden() ? "yes (RAM/flash override)"
                                          : "no (compiled defaults)");
}

/**
 * @brief `boardset cblock|rbias|r0..r3 <value>`.
 */
static void cmd_board_set(const char *arg)
{
    char key[16];
    char extra = '\0';
    double value;
    bool valid = true;

    if (sscanf(arg, "%15s %lf %c", key, &value, &extra) != 2) {
        console_printf(">> usage: boardset cblock <uF> | rbias <ohm> | "
                       "r0..r3 <ohm>\r\n");
        evt_ack("boardset", 0, "expected <key> <value>");
        return;
    }

    if (!isfinite(value) || (value <= 0.0)) {
        console_printf(">> boardset: value must be finite and positive\r\n");
        evt_ack("boardset", 0, "value must be finite and positive");
        return;
    }

    if (strcmp(key, "cblock") == 0) {
        /* The argument is microfarads, not farads.  Ten farads is already far
         * beyond any plausible DC-blocking capacitor, so it is a safe upper
         * sanity bound while still accepting the fitted 936 uF part. */
        if (value > 1.0e7) {
            valid = false;
        } else {
            cal_board_set_c_block(value * 1.0e-6);
            console_printf(">> C_block = %.4f uF\r\n", value);
        }
    } else if (strcmp(key, "rbias") == 0) {
        if (value > 1.0e9) {
            valid = false;
        } else {
            cal_board_set_r_bias(value);
            console_printf(">> R_bias = ");
            print_resistance(value);
            console_printf("\r\n");
        }
    } else if ((key[0] == 'r') &&
               (key[1] >= '0') && (key[1] <= '3') &&
               (key[2] == '\0')) {
        int range = key[1] - '0';

        if (value > 1.0e9) {
            valid = false;
        } else {
            cal_board_set_range_r((uint8_t)range, value);
            console_printf(">> r%d (%s) = ", range, g_ranges[range].label);
            print_resistance(value);
            console_printf("\r\n");
        }
    } else {
        valid = false;
    }

    if (!valid) {
        console_printf(">> boardset: unknown key or implausible value\r\n");
        evt_ack("boardset", 0, "unknown key or implausible value");
        return;
    }

    bool saved = save_cal();
    evt_ack("boardset", saved ? 1 : 0,
            saved ? "board constant stored"
                  : "board constant in RAM only (flash save failed)");
}

/**
 * @brief `boardclear`: restore compiled board constants.
 */
static void cmd_board_clear(void)
{
    cal_board_clear();
    bool saved = save_cal();

    console_printf(">> board constants restored to compiled defaults\r\n");
    evt_ack("boardclear", saved ? 1 : 0,
            saved ? "board constants reset"
                  : "reset in RAM; flash save failed");
}

/* ------------------------------------------------------------------------- */
/* R-mode calibration display / reset                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief `rcal?`: show the sense-resistor calibration.
 */
static void cmd_rcal_show(void)
{
    console_printf(">> R-mode calibration (per sense element)\r\n");
    for (uint8_t i = 0U; i < RMODE_ELEMENTS; ++i) {
        console_printf("   e%u RREF=%.6f R %s\r\n",
                       (unsigned)i,
                       rmode_rref(i),
                       rmode_cal_valid(i) ? "calibrated" : "uncalibrated");
    }
    console_printf("   TIA Rf=%.6f R  VBIAS=%.1f mV  %s\r\n",
                   rmode_tia_rf(),
                   cal_rm_tia_vbias_mv(),
                   rmode_tia_valid() ? "calibrated" : "uncalibrated");
}

/**
 * @brief `rcalclear`: reset every R-mode sense element to its default.
 *
 * The R-mode API has no bulk reset, so each element is restored with
 * rmode_set_cal() using the compiled sense-resistor values.  Unity gain and
 * zero offset are the uncalibrated defaults; `valid = false` tells the
 * measurement path to use the nominal values.  Saving afterwards keeps the
 * reset across a power cycle.
 */
static void cmd_rcal_clear(void)
{
    for (uint8_t i = 0U; i < RMODE_ELEMENTS; ++i) {
        rmode_set_cal(i, rmode_default_rref(i), 1.0, 0.0, false);
    }
    cal_rm_tia_clear();
    bool saved = save_cal();

    console_printf(">> R-mode calibration reset to defaults\r\n");
    evt_ack("rcalclear", saved ? 1 : 0,
            saved ? "R-mode calibration cleared"
                  : "cleared in RAM; flash save failed");
}

/**
 * @brief `rcal <element> <rref_ohm> [gain]` / `rcal tia <rf> <vbias_mv> [gain]`.
 *
 * This is the only way an R-mode calibration measured on the bench can get
 * into the flash record. The sense elements are calibrated as an effective
 * resistance because RSENSE_eff folds in the MAX4619 Ron and the K2A contact
 * resistance, which are neither stable nor known well enough to model.
 *
 * The gain term corrects the residual ratiometric error measured against a
 * reference resistor. It defaults to 1.0, which is what the uncalibrated maths
 * uses anyway.
 *
 * Bounds are plausibility gates, not accuracy claims: anything outside them is
 * almost certainly a typo (for example 98.9 entered in kohm).
 */
static void cmd_rcal_set(const char *arg)
{
    char extra = '\0';
    double value = 0.0;
    double second = 0.0;
    double gain = 1.0;
    int fields;

    arg = skip_ws(arg);

    /* `rcal tia ...` is checked first: the integer parse below cannot match
     * "tia" and would report a usage error for a valid command. */
    if (token_is(arg, "tia")) {
        const char *rest = skip_ws(arg + token_len(arg));

        fields = sscanf(rest, "%lf %lf %lf %c", &value, &second, &gain, &extra);
        if ((fields < 2) || (fields > 3)) {
            console_printf(">> usage: rcal tia <rf_ohm> <vbias_mv> [gain]\r\n");
            evt_ack("rcal", 0, "expected tia <rf_ohm> <vbias_mv> [gain]");
            return;
        }

        if (!isfinite(value) || (value <= 0.0) || (value > 1.0e12)) {
            console_printf(">> rcal: Rf out of range (0 < Rf <= 1e12 ohm)\r\n");
            evt_ack("rcal", 0, "Rf out of range");
            return;
        }
        if (!isfinite(second) || (second < 100.0) || (second > 3300.0)) {
            console_printf(">> rcal: VBIAS out of range (100 .. 3300 mV)\r\n");
            evt_ack("rcal", 0, "VBIAS out of range");
            return;
        }
        if (!isfinite(gain) || (gain <= 0.0) || (gain > 100.0)) {
            console_printf(">> rcal: gain out of range (0 < gain <= 100)\r\n");
            evt_ack("rcal", 0, "gain out of range");
            return;
        }

        rmode_set_tia(value, second, gain, true);
        bool saved = save_cal();

        console_printf(">> TIA calibrated: Rf = ");
        print_resistance(value);
        console_printf("  VBIAS = %.1f mV  gain = %.6f\r\n", second, gain);
        evt_ack("rcal", saved ? 1 : 0,
                saved ? "TIA calibration stored"
                      : "TIA calibration in RAM only (flash save failed)");
        return;
    }

    /* `rcal <element> <rref_ohm> [gain]` */
    size_t n0 = token_len(arg);
    int element;

    if ((n0 != 1U) || (arg[0] < '0') || (arg[0] > '9')) {
        console_printf(">> usage: rcal <0-3> <rref_ohm> [gain] | "
                       "rcal tia <rf_ohm> <vbias_mv> [gain]\r\n");
        evt_ack("rcal", 0, "expected <element> <rref_ohm> [gain]");
        return;
    }
    element = arg[0] - '0';

    const char *rest = skip_ws(arg + n0);

    fields = sscanf(rest, "%lf %lf %c", &value, &gain, &extra);
    if ((fields < 1) || (fields > 2)) {
        console_printf(">> usage: rcal <0-3> <rref_ohm> [gain]\r\n");
        evt_ack("rcal", 0, "expected <element> <rref_ohm> [gain]");
        return;
    }
    if (fields == 1) {
        gain = 1.0;
    }

    if ((element < 0) || (element >= RMODE_ELEMENTS)) {
        console_printf(">> rcal: element must be 0..%d\r\n",
                       RMODE_ELEMENTS - 1);
        evt_ack("rcal", 0, "element out of range");
        return;
    }
    if (!isfinite(value) || (value <= 0.0) || (value > 1.0e12)) {
        console_printf(">> rcal: RREF out of range (0 < RREF <= 1e12 ohm)\r\n");
        evt_ack("rcal", 0, "RREF out of range");
        return;
    }
    if (!isfinite(gain) || (gain <= 0.0) || (gain > 100.0)) {
        console_printf(">> rcal: gain out of range (0 < gain <= 100)\r\n");
        evt_ack("rcal", 0, "gain out of range");
        return;
    }

    rmode_set_cal((uint8_t)element, value, gain, 0.0, true);
    bool saved = save_cal();

    console_printf(">> e%d calibrated: RREF = ", element);
    print_resistance(value);
    console_printf("  gain = %.6f\r\n", gain);
    evt_ack("rcal", saved ? 1 : 0,
            saved ? "R-mode element stored"
                  : "R-mode element in RAM only (flash save failed)");
}

/* ------------------------------------------------------------------------- */
/* Connectivity probe                                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief `probe <0-3>`: charge the node and check that it responds.
 *
 * The drive is held low for 5 ms and then high for 20 ms, with the ADC read
 * at each plateau.  This is a wiring/continuity check, not a measurement: a
 * shorted or disconnected node cannot produce a meaningful delta.
 */
static void cmd_probe(const char *arg)
{
    int range;

    if (!parse_int_arg(arg, &range) ||
        (range < 0) || (range >= RANGE_COUNT)) {
        console_printf(">> usage: probe <0-3>\r\n");
        evt_ack("probe", 0, "range must be 0..3");
        return;
    }

    fe_select_range((uint8_t)range);
    fe_prepare_measurement(false);
    fe_set_drive(false);
    bsp_delay_ms(5);

    int32_t low_code = adc_read(ADC_CH_VCAP, 16);
    fe_set_drive(true);
    bsp_delay_ms(20);

    int32_t high_code = adc_read(ADC_CH_VCAP, 16);
    fe_set_drive(false);

    if ((low_code < 0) || (high_code < 0)) {
        console_printf(">> probe failed: ADC read error (low=%ld high=%ld)\r\n",
                       (long)low_code, (long)high_code);
        evt_ack("probe", 0, "ADC read failed");
        return;
    }

    int low_mv = adc_code_to_mv((uint16_t)low_code, 16, 0);
    int high_mv = adc_code_to_mv((uint16_t)high_code, 16, 0);
    int delta_mv = high_mv - low_mv;
    const char *verdict;
    int ok;

    if (delta_mv < 100) {
        verdict = "no charge: open drive / shorted node";
        ok = 0;
    } else if (high_mv < 900) {
        verdict = "heavy pull-down";
        ok = 0;
    } else {
        verdict = "node responds";
        ok = 1;
    }

    console_printf(">> probe range %d (%s): V_low=%d mV V_high=%d mV "
                   "delta=%d mV\r\n",
                   range, g_ranges[range].label,
                   low_mv, high_mv, delta_mv);
    console_printf("   verdict: %s\r\n", verdict);
    evt_ack("probe", ok, verdict);
}

/* ------------------------------------------------------------------------- */
/* Counters and reset                                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief `stat`: uptime, completed cycles and last cycle duration.
 */
static void cmd_stat(void)
{
    console_printf(">> uptime=%lu ms cycles=%lu last_cycle=%lu ms\r\n",
                   (unsigned long)bsp_millis(),
                   (unsigned long)g_inst.cycles,
                   (unsigned long)g_inst.last_cycle_ms);
    evt_stat();
}

/**
 * @brief `reset` / `reboot`: immediate NVIC system reset.
 *
 * Deliberately has no confirmation prompt: a remote operator needs a way to
 * recover a wedged board without physical access.  The console write is
 * blocking, so the acknowledgement leaves the UART before the reset.
 */
static void cmd_reset(void)
{
    console_printf(">> resetting\r\n");
    NVIC_SystemReset();
}

/* ------------------------------------------------------------------------- */
/* Public dispatcher                                                         */
/* ------------------------------------------------------------------------- */

/**
 * @brief Range selected for calibration and probe commands.
 *
 * @return 0..3, defaulting to 2 (100 kR).
 */
uint8_t console_selected_range(void)
{
    return s_selected_range;
}

/**
 * @brief Dispatch one console line.
 *
 * Leading whitespace is ignored.  The return value says only whether the
 * command word was recognised; a recognised command with bad arguments prints
 * its own usage text, emits a negative ack where applicable and still returns
 * true.  That distinction lets the caller report "unknown command" without
 * duplicating the argument validation.
 *
 * Command words are matched as exact tokens, not prefixes.  In particular the
 * longer forms are checked before the shorter ones they contain:
 * `adccal1`/`adccal2` before `adccal`, `cal1`/`cal2` before `cal`, and
 * `autodischarge` before the `auto` family.  The ordering is kept explicit
 * even though token_is() already rejects longer words, because it documents
 * the command set to the next reader.
 *
 * @param line NUL-terminated command line, with or without CR/LF.
 * @return true when the command was recognised.
 */
bool console_dispatch(const char *line)
{
    if (line == NULL) {
        return false;
    }

    const char *s = skip_ws(line);
    if (*s == '\0') {
        return false;
    }

    /* Help and status */
    if (cmd_is(s, "help") || cmd_is(s, "h")) {
        console_help();
        return true;
    }
    if (cmd_is(s, "status") || cmd_is(s, "p")) {
        cmd_status();
        return true;
    }

    /* Range selection for calibration/probe commands. */
    if (cmd_is(s, "0") || cmd_is(s, "1") ||
        cmd_is(s, "2") || cmd_is(s, "3")) {
        s_selected_range = (uint8_t)(s[0] - '0');
        console_printf(">> range %u (%s)\r\n",
                       (unsigned)s_selected_range,
                       g_ranges[s_selected_range].label);
        return true;
    }

    /* Acquisition run control */
    if (cmd_is(s, "start")) {
        g_inst.run = true;
        g_inst.single = false;
        evt_fe();
        console_printf(">> running\r\n");
        return true;
    }
    if (cmd_is(s, "stop")) {
        g_inst.run = false;
        g_inst.single = false;
        evt_fe();
        console_printf(">> stopped\r\n");
        return true;
    }
    if (cmd_is(s, "single")) {
        g_inst.single = true;
        g_inst.run = true;
        evt_fe();
        console_printf(">> single cycle\r\n");
        return true;
    }

    /* Front-end lifecycle */
    if (cmd_is(s, "precharge")) {
        cmd_precharge();
        return true;
    }
    if (cmd_is(s, "discharge")) {
        cmd_discharge();
        return true;
    }
    if (cmd_is(s, "idle")) {
        cmd_idle();
        return true;
    }

    /* Automation flags.  autodischarge is 13 characters, so it is checked
     * before the shorter auto/autoprecharge family to keep the intent clear. */
    if (cmd_is(s, "autodischarge")) {
        cmd_autodischarge(cmd_arg(s));
        return true;
    }
    if (cmd_is(s, "autoprecharge") || cmd_is(s, "autocharge")) {
        cmd_autoprecharge(cmd_arg(s));
        return true;
    }
    if (cmd_is(s, "auto?")) {
        console_printf(">> autoprecharge=%s autodischarge=%s\r\n",
                       g_inst.auto_precharge ? "on" : "off",
                       g_inst.auto_discharge ? "on" : "off");
        return true;
    }
    if (cmd_is(s, "auto")) {
        cmd_auto();
        return true;
    }
    if (cmd_is(s, "range")) {
        cmd_range(cmd_arg(s));
        return true;
    }

    /* Telemetry */
    if (cmd_is(s, "stream")) {
        cmd_stream(cmd_arg(s));
        return true;
    }
    if (cmd_is(s, "curve")) {
        cmd_curve(cmd_arg(s));
        return true;
    }
    if (cmd_is(s, "led")) {
        cmd_led(cmd_arg(s));
        return true;
    }

    /* Mode and R-mode */
    if (cmd_is(s, "mode")) {
        cmd_mode(cmd_arg(s));
        return true;
    }
    if (cmd_is(s, "res")) {
        cmd_res();
        return true;
    }
    if (cmd_is(s, "rcal?")) {
        cmd_rcal_show();
        return true;
    }
    if (cmd_is(s, "rcalclear")) {
        cmd_rcal_clear();
        return true;
    }
    if (cmd_is(s, "rcal")) {
        cmd_rcal_set(cmd_arg(s));
        return true;
    }

    /* Oscillator calibration.  cal1/cal2 are checked before cal; token_is()
     * makes the ordering a documentation aid rather than a correctness
     * requirement. */
    if (cmd_is(s, "zeroall")) {
        cmd_zeroall();
        return true;
    }
    if (cmd_is(s, "zero")) {
        cmd_zero();
        return true;
    }
    if (cmd_is(s, "cal1")) {
        cmd_cal_two_point(cmd_arg(s), false);
        return true;
    }
    if (cmd_is(s, "cal2")) {
        cmd_cal_two_point(cmd_arg(s), true);
        return true;
    }
    if (cmd_is(s, "cal?")) {
        cmd_cal_show();
        return true;
    }
    if (cmd_is(s, "calclear")) {
        cmd_cal_clear(cmd_arg(s));
        return true;
    }
    if (cmd_is(s, "cal")) {
        cmd_cal(cmd_arg(s));
        return true;
    }

    /* ADC calibration.  adccal1/adccal2 must be checked before adccal. */
    if (cmd_is(s, "adccal1")) {
        cmd_adccal_two_point(cmd_arg(s), false);
        return true;
    }
    if (cmd_is(s, "adccal2")) {
        cmd_adccal_two_point(cmd_arg(s), true);
        return true;
    }
    if (cmd_is(s, "adccal?")) {
        cmd_adccal_show();
        return true;
    }
    if (cmd_is(s, "adccalclear")) {
        cmd_adccal_clear(cmd_arg(s));
        return true;
    }
    if (cmd_is(s, "adccal")) {
        cmd_adccal(cmd_arg(s));
        return true;
    }

    /* Board constants */
    if (cmd_is(s, "boardset")) {
        cmd_board_set(cmd_arg(s));
        return true;
    }
    if (cmd_is(s, "boardclear")) {
        cmd_board_clear();
        return true;
    }
    if (cmd_is(s, "board?")) {
        cmd_board_show();
        return true;
    }

    /* Diagnostics and reset */
    if (cmd_is(s, "probe")) {
        cmd_probe(cmd_arg(s));
        return true;
    }
    if (cmd_is(s, "stat")) {
        cmd_stat();
        return true;
    }
    if (cmd_is(s, "reset") || cmd_is(s, "reboot")) {
        cmd_reset();
        return true;
    }

    return false;
}

/**
 * @brief True for commands that must interrupt an in-flight measurement.
 *
 * Only the first token is inspected.  The list contains every command that
 * either blocks for a long time or reconfigures the analogue front end, so the
 * scheduler can abort a cycle before it is dispatched.  Normal start/stop
 * controls and the status/help queries are intentionally not exclusive: they
 * are safe to run between cycles.
 *
 * @param line Command line.
 * @return true when the command requires the scheduler to abort first.
 */
bool console_is_exclusive(const char *line)
{
    static const char *const exclusive[] = {
        "zero",
        "zeroall",
        "cal",
        "cal1",
        "cal2",
        "calclear",
        "adccal",
        "adccal1",
        "adccal2",
        "adccalclear",
        "probe",
        "boardclear",
        "boardset",
        "rcalclear",
        "rcal",
        "res",
        "mode",
        "precharge",
        "discharge",
        "single",
    };

    if (line == NULL) {
        return false;
    }

    const char *s = skip_ws(line);
    if (*s == '\0') {
        return false;
    }

    for (size_t i = 0U; i < (sizeof(exclusive) / sizeof(exclusive[0])); ++i) {
        if (token_is(s, exclusive[i])) {
            return true;
        }
    }

    return false;
}
