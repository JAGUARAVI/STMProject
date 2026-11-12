/**
 * @file  events.c
 * @brief @@EVT single-line JSON telemetry emitters.
 *
 * The host (host/server.py, host/static/app.js and precision.js) parses these
 * lines byte-for-byte, so the field names, field order and printf precision
 * are part of the firmware contract. The event set is the ESP32 set plus the
 * additive R-mode `res` event; nothing may be renamed or removed.
 *
 * Every emitter obeys three rules:
 *
 *  - It returns immediately when g_inst.stream is false. This includes `ack`:
 *    the original firmware suppressed acknowledgements as well, and the host
 *    treats a missing ack as "streaming is off" rather than as a failure.
 *  - It writes exactly one line, prefixed with "@@EVT " and terminated by a
 *    single '\n'. Long events are split across several console_printf() calls
 *    so that no call can run into the 256-byte console staging buffer; the
 *    concatenated bytes are still one line.
 *  - It performs no allocation and no HAL access. Only bounded, small
 *    buffers are used, and no emitter calls another emitter.
 *
 * All functions here are main-loop only. console_printf() is backed by a
 * static buffer and is explicitly not reentrant, so these must never be
 * called from an ISR.
 */

#include "events.h"

#include <math.h>
#include <stdio.h>

#include "bsp.h"
#include "bsp_console.h"
#include "calib.h"
#include "config.h"
#include "instrument.h"

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

/** Hard ceiling on the number of points in one curve event. The host keeps
 *  only the latest curve, and an unbounded n would make one telemetry line
 *  grow without limit. */
#define EVT_CURVE_MAX_PTS       512

/** Size of one formatted JSON number. Enough for "%.9g" of a double,
 *  including a sign, exponent and terminator, plus headroom. */
#define EVT_NUM_BUF_SIZE        32

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

/**
 * @brief JSON boolean literal.
 *
 * @param b Value to render.
 * @return "true" or "false".
 */
static const char *jt(bool b)
{
    return b ? "true" : "false";
}

/**
 * @brief Render a double as a JSON number.
 *
 * JSON has no NaN or infinity literals, so non-finite values become `null`,
 * which the host maps back to a missing estimate. Finite values use "%.9g",
 * preserving full double precision without the trailing zeros of fixed-point
 * formatting.
 *
 * @param buf Destination buffer.
 * @param len Size of @p buf in bytes.
 * @param v   Value to render.
 */
static void json_num(char *buf, size_t len, double v)
{
    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    if (!isfinite(v)) {
        (void)snprintf(buf, len, "null");
        return;
    }

    (void)snprintf(buf, len, "%.9g", v);
}

/**
 * @brief Range label with a bounded lookup.
 *
 * @param range_idx Range index, 0..RANGE_COUNT-1.
 * @return Configured label, or "?" for a corrupt index.
 */
static const char *range_label(int range_idx)
{
    if ((range_idx < 0) || (range_idx >= RANGE_COUNT)) {
        return "?";
    }
    return g_ranges[range_idx].label;
}

/**
 * @brief Range resistance with a bounded lookup.
 *
 * @param range_idx Range index, 0..RANGE_COUNT-1.
 * @return Configured resistance in ohms, or 0 for a corrupt index.
 */
static double range_resistance(int range_idx)
{
    if ((range_idx < 0) || (range_idx >= RANGE_COUNT)) {
        return 0.0;
    }
    return g_ranges[range_idx].resistance_ohms;
}

/**
 * @brief Event spelling of a capacitance measurement method.
 *
 * @param method Measurement method.
 * @return "adc" or "osc".
 */
static const char *method_name(method_t method)
{
    return (method == METHOD_OSC) ? "osc" : "adc";
}

/**
 * @brief Event spelling of an R-mode measurement method.
 *
 * @param method R-mode method.
 * @return "kelvin" or "tia".
 */
static const char *rmode_name(rmode_method_t method)
{
    return (method == RMODE_TIA) ? "tia" : "kelvin";
}

/**
 * @brief Event spelling of a front-end state.
 *
 * @param state Current front-end state.
 * @return Stable lowercase state name.
 */
static const char *fe_state_name(fe_state_t state)
{
    switch (state) {
    case FE_PRECHARGE:
        return "precharge";
    case FE_PRECHARGED:
        return "precharged";
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
    case FE_IDLE:
    default:
        return "idle";
    }
}

/**
 * @brief Derive the charge string from the independent charge flags.
 *
 * The two flags are maintained separately by the front end and can both be
 * true (or both false) after an aborted sequence, so the ambiguous cases are
 * reported as "unknown" rather than guessing.
 *
 * @param charged    Node is charged.
 * @param discharged Node is discharged.
 * @return "charged", "discharged" or "unknown".
 */
static const char *charge_name(bool charged, bool discharged)
{
    if (charged && !discharged) {
        return "charged";
    }
    if (discharged && !charged) {
        return "discharged";
    }
    return "unknown";
}

/* ------------------------------------------------------------------------- */
/* Stream control                                                            */
/* ------------------------------------------------------------------------- */

/**
 * @brief Enable or disable @@EVT output.
 *
 * @param on True to emit telemetry.
 */
void evt_set_stream(bool on)
{
    g_inst.stream = on;
}

/**
 * @brief Report the current telemetry gate.
 *
 * @return true when events are being emitted.
 */
bool evt_stream_enabled(void)
{
    return g_inst.stream;
}

/**
 * @brief Emit a preformatted JSON body.
 *
 * The body is passed through verbatim after the sentinel and newline. Like
 * every other emitter it is gated by the stream flag.
 *
 * @param json JSON object text; NULL is ignored.
 */
void evt_emit_json(const char *json)
{
    if (!g_inst.stream || (json == NULL)) {
        return;
    }

    console_printf("@@EVT %s\n", json);
}

/* ------------------------------------------------------------------------- */
/* Boot and periodic events                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Emit the boot descriptor with every configured range and calibration
 *        record.
 *
 * The host uses this to label ranges and to show whether the stored oscillator
 * and ADC calibration were found. The per-range object is one of the longest
 * single event fragments, so each range is formatted separately.
 */
void evt_boot(void)
{
    if (!g_inst.stream) {
        return;
    }

    console_printf("@@EVT {\"t\":\"boot\",\"fw\":\"%s\",\"ranges\":[",
                   ESWCAP_FW_NAME);

    for (int i = 0; i < RANGE_COUNT; ++i) {
        uint8_t r = (uint8_t)i;

        console_printf("%s{\"i\":%d,\"label\":\"%s\",\"r\":%.6g,\"mux\":%d,"
                       "\"k\":%.6g,\"t0_us\":%.6g,\"has_t0\":%s,"
                       "\"valid\":%s,\"adc_r_eff\":%.6g,\"adc_c0_f\":%.6g,"
                       "\"adc_valid\":%s}",
                       (i == 0) ? "" : ",",
                       i,
                       g_ranges[i].label,
                       g_ranges[i].resistance_ohms,
                       (int)g_ranges[i].mux_channel,
                       cal_osc_k(r),
                       cal_osc_t0(r),
                       jt(cal_osc_has_t0(r)),
                       jt(cal_osc_valid(r)),
                       cal_adc_r_eff(r),
                       cal_adc_c0(r),
                       jt(cal_adc_valid(r)));
    }

    console_printf("]}\n");
}

/**
 * @brief Emit the result of one complete measurement cycle.
 *
 * `ts_ms` is wall time since boot, not the cycle duration. The duration is
 * carried in g_inst.last_cycle_ms and reported by evt_stat(); the cycle event
 * has no duration field in the host contract, so @p cycle_ms is accepted for
 * API compatibility and intentionally not printed.
 *
 * @param f         Fusion result for the cycle.
 * @param cycle_ms  Duration of the cycle in milliseconds (not part of the
 *                  event schema).
 */
void evt_cycle(const fusion_t *f, uint32_t cycle_ms)
{
    static char mismatch[EVT_NUM_BUF_SIZE];
    static char adc[EVT_NUM_BUF_SIZE];
    static char osc[EVT_NUM_BUF_SIZE];

    (void)cycle_ms;

    if (!g_inst.stream || (f == NULL)) {
        return;
    }

    json_num(mismatch, sizeof(mismatch), f->method_mismatch_ratio);
    json_num(adc, sizeof(adc), f->adc_estimate);
    json_num(osc, sizeof(osc), f->osc_estimate);

    console_printf("@@EVT {\"t\":\"cycle\",\"ts_ms\":%lu,\"valid\":%s,"
                   "\"c\":%.9g,\"spread\":%.6g,\"quality\":%.6g,"
                   "\"weight\":%.6g,\"mismatch_ratio\":%s,",
                   (unsigned long)bsp_millis(),
                   jt(f->valid),
                   f->capacitance_f,
                   f->rel_spread,
                   f->quality,
                   f->total_weight,
                   mismatch);

    console_printf("\"n_adc\":%d,\"n_osc\":%d,\"raw\":%d,\"adc\":%s,"
                   "\"osc\":%s,\"mismatch\":%s}\n",
                   f->n_adc,
                   f->n_osc,
                   f->raw_samples,
                   adc,
                   osc,
                   jt(f->method_mismatch));
}

/**
 * @brief Emit one scored sample.
 *
 * `vinf` is only meaningful for an ADC fit; oscillator samples have no
 * asymptote and an ADC sample with an unset (zero) asymptote reports -1. The
 * host applies the same rule when it displays the marker.
 *
 * @param phase Calibration or acquisition phase name.
 * @param s     Sample to report.
 */
void evt_sample(const char *phase, const sample_t *s)
{
    double vinf = -1.0;

    if (!g_inst.stream || (s == NULL)) {
        return;
    }

    if ((s->method == METHOD_ADC_STEP) && (s->v_inf_mv != 0.0)) {
        vinf = s->v_inf_mv;
    }

    console_printf("@@EVT {\"t\":\"sample\",\"phase\":\"%s\","
                   "\"method\":\"%s\",\"range\":%d,\"label\":\"%s\","
                   "\"valid\":%s,\"plausible\":%s,",
                   (phase != NULL) ? phase : "",
                   method_name(s->method),
                   (int)s->range_idx,
                   range_label((int)s->range_idx),
                   jt(s->valid),
                   jt(s->plausible));

    console_printf("\"c\":%.9g,\"c_eq\":%.9g,\"q\":%.6g,\"tau_us\":%.6g,"
                   "\"freq\":%.6g,\"r2\":%.6g,\"vinf\":%.0f,"
                   "\"rmse_mv\":%.6g,\"tau_unc_us\":%.6g}\n",
                   s->capacitance_f,
                   s->c_eq_f,
                   s->quality,
                   s->tau_us,
                   s->freq_hz,
                   s->fit_r2,
                   vinf,
                   s->fit_rmse_mv,
                   s->tau_unc_us);
}

/**
 * @brief Emit the fusion decision and its per-contributor breakdown.
 *
 * The header is split into two console_printf() calls because the numeric
 * fields can each be up to 24 characters at "%.9g"; the contributor list is
 * then streamed one object at a time so the line length is independent of
 * MAX_SAMPLES.
 *
 * @param f Fusion result to report.
 */
void evt_fuse(const fusion_t *f)
{
    static char mismatch[EVT_NUM_BUF_SIZE];
    int n;

    if (!g_inst.stream || (f == NULL)) {
        return;
    }

    json_num(mismatch, sizeof(mismatch), f->method_mismatch_ratio);

    console_printf("@@EVT {\"t\":\"fuse\",\"valid\":%s,\"median\":%.9g,"
                   "\"c\":%.9g,\"c_min\":%.9g,\"c_max\":%.9g,"
                   "\"spread\":%.6g,\"quality\":%.6g,"
                   "\"mismatch_ratio\":%s,",
                   jt(f->valid),
                   f->median_f,
                   f->capacitance_f,
                   f->c_min_f,
                   f->c_max_f,
                   f->rel_spread,
                   f->quality,
                   mismatch);

    console_printf("\"gate_rel\":%.6g,\"n_kept\":%d,\"n_gated\":%d,"
                   "\"w_total\":%.6g,\"contrib\":[",
                   (double)FUSION_GATE_REL,
                   f->n_kept,
                   f->n_gated,
                   f->total_weight);

    n = g_fusion_n_contrib;
    if (n < 0) {
        n = 0;
    }
    if (n > FUSION_MAX_CONTRIB) {
        n = FUSION_MAX_CONTRIB;
    }

    for (int i = 0; i < n; ++i) {
        const fusion_contrib_t *fc = &g_fusion_contrib[i];
        int range_idx = (int)fc->range_idx;

        console_printf("%s{\"r\":%d,\"label\":\"%s\",\"m\":\"%s\","
                       "\"c\":%.9g,\"c_eq\":%.9g,\"q\":%.6g,"
                       "\"w\":%.6g,\"r2\":%.6g,\"kept\":%s}",
                       (i == 0) ? "" : ",",
                       range_idx,
                       range_label(range_idx),
                       method_name((method_t)fc->method),
                       fc->c_f,
                       fc->c_eq_f,
                       fc->q,
                       fc->w,
                       fc->r2,
                       jt(fc->kept));
    }

    console_printf("]}\n");
}

/**
 * @brief Emit the autoranging decision trace.
 *
 * Nothing is emitted when no sweep has produced a trace yet. The mode and lock
 * range come from the trace itself (g_sweep), so the event describes the
 * decision that was actually taken rather than a later console setting.
 */
void evt_sweep(void)
{
    int lock_range;

    if (!g_inst.stream || !g_sweep.valid) {
        return;
    }

    lock_range = g_sweep.locked ? g_sweep.lock_range : -1;

    console_printf("@@EVT {\"t\":\"sweep\",\"mode\":\"%s\","
                   "\"have_rough\":%s,\"rough\":%.9g,\"sub_nf\":%s,"
                   "\"saturated\":%s,\"adc_sub\":%s,\"osc_best\":%d,"
                   "\"lock\":%d,\"adc_tried\":[",
                   g_sweep.locked ? "locked" : "auto",
                   jt(g_sweep.have_rough),
                   g_sweep.rough_f,
                   jt(g_sweep.sub_nf),
                   jt(g_sweep.saturated),
                   jt(g_sweep.adc_sub),
                   g_sweep.osc_best_range,
                   lock_range);

    for (int i = 0; i < RANGE_COUNT; ++i) {
        console_printf("%s%s", (i == 0) ? "" : ",", jt(g_sweep.adc_tried[i]));
    }

    console_printf("]}\n");
}

/**
 * @brief Emit periodic health counters.
 *
 * heap_free and heap_min are retained with literal zeroes purely for
 * dashboard compatibility: the STM32G474 build uses static storage only and
 * has no heap to report.
 */
void evt_stat(void)
{
    if (!g_inst.stream) {
        return;
    }

    console_printf("@@EVT {\"t\":\"stat\",\"uptime_ms\":%lu,\"cycles\":%u,"
                   "\"cycle_ms\":%u,\"heap_free\":0,\"heap_min\":0}\n",
                   (unsigned long)bsp_millis(),
                   (unsigned)g_inst.cycles,
                   (unsigned)g_inst.last_cycle_ms);
}

/**
 * @brief Emit the front-end state, pin levels and automation flags.
 *
 * The three pin bits come from fe_pin_bits(): bit0 SSR13, bit1 SSR2 and bit2
 * drive. The charge string is derived from the independent charged and
 * discharged latches, not from the momentary state machine state.
 */
void evt_fe(void)
{
    fe_state_t state;
    uint8_t bits;
    bool charged;
    bool discharged;

    if (!g_inst.stream) {
        return;
    }

    state      = fe_state();
    bits       = fe_pin_bits();
    charged    = fe_charged();
    discharged = fe_discharged();

    console_printf("@@EVT {\"t\":\"fe\",\"state\":\"%s\",\"ssr13\":%d,"
                   "\"ssr2\":%d,\"drive\":%d,\"biased\":%s,\"charged\":%s,"
                   "\"discharged\":%s,\"charge\":\"%s\",\"auto_pre\":%s,"
                   "\"auto_dis\":%s,\"run\":%s,\"single\":%s,\"lock\":%d}\n",
                   fe_state_name(state),
                   ((bits & 0x01U) != 0U) ? 1 : 0,
                   ((bits & 0x02U) != 0U) ? 1 : 0,
                   ((bits & 0x04U) != 0U) ? 1 : 0,
                   jt(state == FE_PRECHARGED || state == FE_R_ISOLATE ||
                      state == FE_R_KELVIN || state == FE_R_TIA),
                   jt(charged),
                   jt(discharged),
                   charge_name(charged, discharged),
                   jt(g_inst.auto_precharge),
                   jt(g_inst.auto_discharge),
                   jt(g_inst.run),
                   jt(g_inst.single),
                   g_inst.lock_range);
}

/* ------------------------------------------------------------------------- */
/* Calibration and console events                                            */
/* ------------------------------------------------------------------------- */

/**
 * @brief Emit the start of an oscillator calibration point acquisition.
 *
 * @param range_idx Range being calibrated.
 * @param op        Operation name ("cal1", "cal2", ...).
 * @param ref_pf    Reference capacitance in picofarads.
 * @param freq      Measured oscillator frequency in hertz.
 */
void evt_calpt(int range_idx, const char *op, double ref_pf, double freq)
{
    double period_us;

    if (!g_inst.stream) {
        return;
    }

    period_us = (freq > 0.0) ? (1e6 / freq) : 0.0;

    console_printf("@@EVT {\"t\":\"calpt\",\"op\":\"%s\",\"range\":%d,"
                   "\"ref_pf\":%.6g,\"freq\":%.6g,\"period_us\":%.6g}\n",
                   (op != NULL) ? op : "",
                   range_idx,
                   ref_pf,
                   freq,
                   period_us);
}

/**
 * @brief Emit the storage result of an oscillator calibration.
 *
 * @param range_idx Range calibrated.
 * @param op        Operation name.
 * @param k         Solved oscillator geometry constant.
 * @param t0        Solved open-node period offset in microseconds.
 * @param ok        Non-zero when the solve and store succeeded.
 */
void evt_calres(int range_idx, const char *op, double k, double t0, int ok)
{
    if (!g_inst.stream) {
        return;
    }

    console_printf("@@EVT {\"t\":\"calres\",\"op\":\"%s\",\"range\":%d,"
                   "\"k\":%.6g,\"t0_us\":%.6g,\"ok\":%s}\n",
                   (op != NULL) ? op : "",
                   range_idx,
                   k,
                   t0,
                   jt(ok != 0));
}

/**
 * @brief Emit the start of an ADC calibration point acquisition.
 *
 * @param range_idx Range being calibrated.
 * @param op        Operation name.
 * @param ref_pf    Reference capacitance in picofarads.
 * @param tau_us    Fitted RC time constant in microseconds.
 */
void evt_adccalpt(int range_idx, const char *op, double ref_pf, double tau_us)
{
    if (!g_inst.stream) {
        return;
    }

    console_printf("@@EVT {\"t\":\"adccalpt\",\"op\":\"%s\",\"range\":%d,"
                   "\"ref_pf\":%.6g,\"tau_us\":%.6g}\n",
                   (op != NULL) ? op : "",
                   range_idx,
                   ref_pf,
                   tau_us);
}

/**
 * @brief Emit the storage result of an ADC calibration.
 *
 * @param range_idx Range calibrated.
 * @param op        Operation name.
 * @param r_eff     Solved effective series resistance in ohms.
 * @param c0_f      Solved node offset capacitance in farads.
 * @param ok        Non-zero when the solve and store succeeded.
 */
void evt_adccalres(int range_idx, const char *op, double r_eff, double c0_f,
                   int ok)
{
    if (!g_inst.stream) {
        return;
    }

    console_printf("@@EVT {\"t\":\"adccalres\",\"op\":\"%s\",\"range\":%d,"
                   "\"r_eff\":%.6g,\"c0_f\":%.6g,\"r_nom\":%.6g,"
                   "\"ok\":%s}\n",
                   (op != NULL) ? op : "",
                   range_idx,
                   r_eff,
                   c0_f,
                   range_resistance(range_idx),
                   jt(ok != 0));
}

/**
 * @brief Emit one tare sample and the accumulated result.
 *
 * @param range_idx Range being tared.
 * @param idx       Zero-based tare sample index.
 * @param total     Total number of tare samples.
 * @param freq      Measured oscillator frequency in hertz.
 * @param t0_us     Accumulated open-node period offset in microseconds.
 * @param stray_pf  Accumulated stray capacitance in picofarads.
 * @param done      Non-zero when the tare finished.
 */
void evt_tare(int range_idx, int idx, int total, double freq, double t0_us,
              double stray_pf, int done)
{
    double period_us;

    if (!g_inst.stream) {
        return;
    }

    period_us = (freq > 0.0) ? (1e6 / freq) : 0.0;

    console_printf("@@EVT {\"t\":\"tare\",\"range\":%d,\"idx\":%d,"
                   "\"total\":%d,\"freq\":%.6g,\"period_us\":%.6g,"
                   "\"t0_us\":%.6g,\"stray_pf\":%.6g,\"done\":%s}\n",
                   range_idx,
                   idx,
                   total,
                   freq,
                   period_us,
                   t0_us,
                   stray_pf,
                   jt(done != 0));
}

/**
 * @brief Emit a command acknowledgement.
 *
 * Also gated by g_inst.stream, matching the original firmware: with streaming
 * off the console is silent so a host that has not enabled telemetry does not
 * see half a protocol.
 *
 * @param cmd Command name.
 * @param ok  Non-zero on success.
 * @param msg Human-readable result message.
 */
void evt_ack(const char *cmd, int ok, const char *msg)
{
    if (!g_inst.stream) {
        return;
    }

    console_printf("@@EVT {\"t\":\"ack\",\"cmd\":\"%s\",\"ok\":%s,"
                   "\"msg\":\"%s\"}\n",
                   (cmd != NULL) ? cmd : "",
                   jt(ok != 0),
                   (msg != NULL) ? msg : "");
}

/* ------------------------------------------------------------------------- */
/* Curve and R-mode events                                                   */
/* ------------------------------------------------------------------------- */

/**
 * @brief Emit a full ADC charge curve for the host's curve replay.
 *
 * Only emitted when curve streaming is enabled. Point i is placed at
 * i * dt_us rounded to the nearest integer microsecond, matching the
 * hardware-timed uniform capture. The voltage is rounded to an integer
 * millivolt. n is clamped to EVT_CURVE_MAX_PTS so a corrupt capture cannot
 * turn one telemetry line into an unbounded write.
 *
 * The opening fragment, each point and the closing bracket are separate
 * console_printf() calls: one line in total, but no call approaches the
 * console's 256-byte staging buffer.
 *
 * @param range_idx Range the curve came from.
 * @param v_inf     Fitted asymptote in millivolts.
 * @param v0        Initial voltage in millivolts.
 * @param t0_us     Fitted time origin in microseconds.
 * @param tau_us    Fitted time constant in microseconds.
 * @param r2        Fit coefficient of determination.
 * @param v_mv      Sample voltages in millivolts.
 * @param n         Number of points; clamped to EVT_CURVE_MAX_PTS.
 * @param dt_us     Uniform sample interval in microseconds.
 */
void evt_curve(int range_idx, int v_inf, int v0, double t0_us, double tau_us,
               double r2, const double *v_mv, int n, double dt_us)
{
    int count;

    if (!g_inst.stream || !g_inst.curve) {
        return;
    }

    console_printf("@@EVT {\"t\":\"curve\",\"range\":%d,\"vinf\":%d,"
                   "\"v0\":%d,\"t0_us\":%.6g,\"tau_us\":%.6g,"
                   "\"r2\":%.6g,\"pts\":[",
                   range_idx,
                   v_inf,
                   v0,
                   t0_us,
                   tau_us,
                   r2);

    count = (v_mv == NULL) ? 0 : n;
    if (count < 0) {
        count = 0;
    }
    if (count > EVT_CURVE_MAX_PTS) {
        count = EVT_CURVE_MAX_PTS;
    }

    for (int i = 0; i < count; ++i) {
        long t_point = lround((double)i * dt_us);
        long mv      = lround(v_mv[i]);

        console_printf("%s[%ld,%ld]",
                       (i == 0) ? "" : ",",
                       t_point,
                       mv);
    }

    console_printf("]}\n");
}

/**
 * @brief Emit one R-mode resistance result.
 *
 * Schema (additive to the C-mode event set; no C-mode field changes):
 *
 *   @@EVT {"t":"res","valid":true,"rx":999.9,"method":"kelvin","element":1,
 *          "vn":405.1,"vb":405.0,"vtia":null,"i":407.6,"q":0.93,
 *          "flags":["uncal"]}
 *
 * `rx` is the measured resistance in ohms. `vn` is N_DUT, `vb` is B (or the
 * VBIAS reference in TIA mode) and `vtia` is the TIA output, null in Kelvin.
 * `flags` is a JSON array of short strings so a host can badge a reading
 * without re-deriving the firmware's quality rules:
 *
 *   "saturated"  the node hit the compliance limit
 *   "uncal"      the sense element has no stored calibration
 *   "tia_uncal"  the TIA has no stored calibration (confidence capped)
 *   "invalid"    no usable reading
 *
 * The envelope is `"t"` like every other emitter in this file; the plan's
 * sketch called it `"ev"`, but one protocol is worth more than one letter.
 *
 * @param r Result to publish; NULL is ignored.
 */
void evt_res(const res_sample_t *r)
{
    char flags[64];
    size_t used = 0U;

    if (!g_inst.stream || (r == NULL)) {
        return;
    }

    flags[0] = '\0';

#define RES_FLAG(name, cond)                                                  \
    do {                                                                      \
        if (cond) {                                                           \
            int n = snprintf(flags + used, sizeof(flags) - used, "%s\"%s\"",  \
                             (used == 0U) ? "" : ",", (name));                \
            if (n > 0) {                                                      \
                used += (size_t)n;                                            \
                if (used >= sizeof(flags)) {                                  \
                    used = sizeof(flags) - 1U;                                \
                }                                                             \
            }                                                                 \
        }                                                                     \
    } while (0)

    RES_FLAG("invalid", !r->valid);
    RES_FLAG("saturated", r->saturated);
    RES_FLAG("uncal", (r->method == RMODE_KELVIN) &&
                      ((r->element < 0) ||
                       (r->element >= RMODE_ELEMENTS) ||
                       !cal_rm_valid((uint8_t)r->element)));
    RES_FLAG("tia_uncal", (r->method == RMODE_TIA) && !cal_rm_tia_valid());

#undef RES_FLAG

    flags[used] = '\0';

    console_printf("@@EVT {\"t\":\"res\",\"valid\":%s,\"rx\":%.9g,"
                   "\"method\":\"%s\",\"element\":%d,\"vn\":%.6g,"
                   "\"vb\":%.6g,\"vtia\":",
                   jt(r->valid),
                   r->r_ohm,
                   rmode_name(r->method),
                   r->element,
                   r->v_n_mv,
                   r->v_b_mv);

    if (r->method == RMODE_TIA) {
        console_printf("%.6g", r->v_tia_mv);
    } else {
        console_printf("null");
    }

    console_printf(",\"i\":%.6g,\"q\":%.6g,\"flags\":[%s]}\n",
                   r->current_ua, r->quality, flags);
}
