/**
 * @file  rmode.c
 * @brief 4-wire Kelvin / TIA resistance measurement.
 *
 * ## Why this file was rewritten
 *
 * The first version assumed a DUT between V_cap and a low-side sense resistor.
 * That is not the ESWCap board: C_block sits between V_cap and N_DUT, so a DC
 * force applied to V_cap only charges C_block. The real DUT is N_DUT -> B, and
 * the force must be applied to N_DUT. See docs/handoff-verdict.md.
 *
 * ## Kelvin
 *
 * OPAMP1 servos B to VREF:
 *
 *   VINP = B (PA7), VINM = VREF (PA3), VOUT = Q1 gate (PA2)
 *
 * Q1 is a high-side P-FET: B rises -> VOUT rises -> gate rises -> Q1 conducts
 * less -> B falls. Negative feedback. The loop current is
 *
 *   I = VREF / RSENSE
 *
 * and the two sensed nodes are
 *
 *   V_N = I * (R_dut + RSENSE)
 *   V_B = I * RSENSE
 *
 * so
 *
 *   R_dut = RSENSE_eff * (V_N - V_B) / V_B
 *
 * The setpoint, DAC gain and op-amp offset cancel in the ratio. RSENSE_eff
 * (including the mux and relay Ron) and the V_N/V_B channel gains do not, so
 * both are calibrated.
 *
 * ## Autoranging
 *
 * The Kelvin element must be chosen from the compliance, not from a predicted
 * resistance: a 100 ohm DUT on the 4.7 Mohm element produces a V_N-V_B of only
 * ~9 uV, which is invisible. The ladder starts on the 1 kohm element and
 * moves **up** when V_B falls below regulation (too much DUT drop) and
 * **down** when V_N-V_B is too small (too little resolution). If the top
 * element cannot regulate, the DUT is above ~22 Mohm and the TIA takes over.
 *
 * ## TIA
 *
 * The external LMC6482 A holds B at VBIAS and converts the DUT current:
 *
 *   R_dut = Rf * (V_N - VBIAS) / (VBIAS - V_TIA)
 *
 * OPAMP2 cannot do this: its VINP is PA7 (B) and VINM is PA5 (VBIAS), the
 * opposite polarity, and it has no DAC path on G4.
 */

#include "rmode.h"

#include <math.h>
#include <string.h>

#include "bsp.h"
#include "bsp_adc.h"
#include "bsp_comp.h"
#include "bsp_dac.h"
#include "bsp_frontend.h"
#include "bsp_opamp.h"
#include "bsp_opamp3.h"
#include "calib.h"
#include "config.h"

/* Oversampling on the single-read paths. R-mode is slow, so 16x is free. */
#define RMODE_OS_RATIO      16U
#define RMODE_AVG           4

static bool s_active;
static uint8_t s_element = 1;   /* 1 kohm is the middle starting point */

/* ------------------------------------------------------------------------- */
/* ADC helpers                                                               */
/* ------------------------------------------------------------------------- */

static double avg_adc2(uint32_t channel)
{
    double sum = 0.0;
    int got = 0;

    for (int i = 0; i < RMODE_AVG; ++i) {
        int32_t code = adc_read(channel, RMODE_OS_RATIO);
        if (code >= 0) {
            sum += (double)adc_code_to_mv((uint16_t)code, RMODE_OS_RATIO, 0);
            ++got;
        }
    }
    return (got > 0) ? (sum / (double)got) : -1.0;
}

static double avg_adc3(uint32_t channel)
{
    double sum = 0.0;
    int got = 0;

    for (int i = 0; i < RMODE_AVG; ++i) {
        int32_t code = adc_read_adc3(channel, RMODE_OS_RATIO);
        if (code >= 0) {
            sum += (double)adc_code_to_mv((uint16_t)code, RMODE_OS_RATIO, 0);
            ++got;
        }
    }
    return (got > 0) ? (sum / (double)got) : -1.0;
}

static double avg_adc4(uint32_t channel)
{
    double sum = 0.0;
    int got = 0;

    for (int i = 0; i < RMODE_AVG; ++i) {
        int32_t code = adc_read_adc4(channel, RMODE_OS_RATIO);
        if (code >= 0) {
            sum += (double)adc_code_to_mv((uint16_t)code, RMODE_OS_RATIO, 0);
            ++got;
        }
    }
    return (got > 0) ? (sum / (double)got) : -1.0;
}

/* ------------------------------------------------------------------------- */
/* Kelvin                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * @brief One Kelvin measurement on one sense element.
 *
 * On failure the diagnostic fields (v_n_mv/v_b_mv/saturated) are still
 * filled when the ADC reads succeeded, so the autorange ladder can decide which
 * direction to move.
 *
 * The relay/loop settling is done by fe_r_kelvin(); this function just reads.
 */
static bool kelvin_measure(uint8_t element, res_sample_t *out)
{
    memset(out, 0, sizeof(*out));
    out->method = RMODE_KELVIN;
    out->element = (int)element;

    double rref = cal_rm_rref(element);
    if (rref <= 0.0) {
        return false;
    }

    if (!fe_r_kelvin(element)) {
        out->saturated = true;
        return false;
    }

    double vn = avg_adc3(ADC_CH_VN);
    double vb = avg_adc4(ADC_CH_VB);

    /* The Kelvin loop is finished before any analysis: do not leave Q1 driven
     * while the caller thinks about the result. */
    fe_r_kelvin_off();

    if (vn < 0.0 || vb < 0.0) {
        return false;
    }

    out->v_n_mv = vn;
    out->v_b_mv = vb;
    out->v_tia_mv = -1.0;
    out->current_ua = rmode_current_ua(rref, vb);

    /* 2.3 V is the Kelvin operating limit, not the ADC limit: above it the
     * current source is out of compliance and the TIA is the right path. */
    if (vn > RMODE_VN_MAX_MV) {
        out->saturated = true;
        return false;
    }
    if (vb < RMODE_VB_MIN_MV || vn <= vb) {
        out->saturated = (vb < RMODE_VB_MIN_MV);
        return false;
    }

    double r = rmode_resistance(rref, vn, vb);
    if (r < 0.0) {
        out->saturated = true;
        return false;
    }

    out->r_ohm = r * cal_rm_gain(element);

    /* Quality: regulation, resolution and headroom. All three matter, because
     * a reading can be arithmetically valid while the loop is out of
     * regulation or the drop is only a few LSB. */
    double dv = vn - vb;
    double q_reg = exp(-pow((vb - (double)RMODE_VREF_MV) /
                            (0.25 * (double)RMODE_VREF_MV), 2.0));
    double q_res = dv / (dv + 5.0);
    double q_head = 1.0;
    if (vn > 0.90 * (double)RMODE_VN_MAX_MV) {
        double d = (vn - 0.90 * (double)RMODE_VN_MAX_MV) /
                   (0.10 * (double)RMODE_VN_MAX_MV);
        q_head = exp(-d * d);
    }

    out->quality = clampd(q_reg * q_res * q_head, 0.0, 1.0);
    out->saturated = (vn > 0.95 * (double)RMODE_VN_MAX_MV);
    out->valid = true;
    return true;
}

/* ------------------------------------------------------------------------- */
/* TIA                                                                       */
/* ------------------------------------------------------------------------- */

static bool tia_measure(res_sample_t *out)
{
    memset(out, 0, sizeof(*out));
    out->method = RMODE_TIA;
    out->element = -1;

    double rf = cal_rm_tia_rf();
    if (rf <= 0.0) {
        return false;
    }

    if (!fe_r_tia()) {
        return false;
    }

    double vn = avg_adc3(ADC_CH_VN);
    double vtia = avg_adc2(ADC_CH_TIA);
    double vbias_mv = avg_adc2(ADC_CH_VBIAS);

    /* Return to the isolated state: Q1 off, K2 off, R mux off. */
    fe_r_kelvin_off();

    if (vn < 0.0 || vtia < 0.0) {
        return false;
    }

    if (vbias_mv < 1200.0 || vbias_mv > 2100.0) {
        /* The divider is nominal 1.65 V; a reading outside this window means
         * the ADC or the divider is broken. Fall back to the calibrated value
         * rather than computing with nonsense. */
        vbias_mv = cal_rm_tia_vbias_mv();
    }

    out->v_n_mv = vn;
    out->v_b_mv = vbias_mv;
    out->v_tia_mv = vtia;
    out->current_ua = ((vn - vbias_mv) * 1e-3) / rf * 1e6;

    if (vn > RMODE_VN_ABS_MAX_MV) {
        out->saturated = true;
        return false;
    }

    double r = rmode_tia_resistance(rf, vn, vbias_mv, vtia);
    if (r < 0.0) {
        out->saturated = true;
        return false;
    }

    out->r_ohm = r * cal_rm_tia_gain();

    double dev = vbias_mv - vtia;
    double exc = vn - vbias_mv;
    double q_dev = dev / (dev + 20.0);
    double q_exc = exc / (exc + 50.0);
    double q = clampd(q_dev * q_exc, 0.0, 1.0);

    /* Until the TIA has been bench-calibrated, cap its confidence. The path
     * is physically sound but its offset/gain are unverified. */
    out->quality = cal_rm_tia_valid() ? (0.9 * q) : (0.5 * q);
    out->valid = true;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Setup                                                                     */
/* ------------------------------------------------------------------------- */

bool rmode_init(void)
{
#if !ESWCAP_HAS_RMODE
    /* The feature is compiled out; this is not a boot failure. rmode_enter()
     * refuses with a clear message. */
    s_active = false;
    return true;
#else
    if (!bsp_opamp1_init()) {
        return false;
    }
    if (!bsp_opamp3_init()) {
        return false;
    }
    if (!bsp_comp4_init()) {
        return false;
    }
    bsp_comp4_enable(false);

    s_active = false;
    s_element = 1;
    return true;
#endif
}

bool rmode_enter(void)
{
#if !ESWCAP_HAS_RMODE
    return false;
#else
    if (s_active) {
        return true;
    }
    if (fe_charged()) {
        return false;
    }

    /* C-mode oscillator off; PA0 back to GPIO. */
    comp_loop_stop();
    bsp_dac3_enable(false);

    if (!fe_set_mode(OP_MODE_RES)) {
        return false;
    }

    if (!bsp_comp4_init()) {
        return false;
    }
    bsp_comp4_enable(true);

    if (!fe_r_enter()) {
        bsp_comp4_enable(false);
        (void)fe_set_mode(OP_MODE_CAP);
        return false;
    }

    s_active = true;
    return true;
#endif
}

void rmode_leave(void)
{
#if ESWCAP_HAS_RMODE
    bsp_comp4_enable(false);
    fe_r_exit();
#endif

    s_active = false;
    (void)fe_set_mode(OP_MODE_CAP);
    bsp_opamp1_enable(false);
    bsp_opamp3_enable(false);
    bsp_dac3_enable(false);
}

bool rmode_active(void)
{
    return s_active;
}

bool rmode_select_element(uint8_t element)
{
    if (!s_active || element >= RMODE_ELEMENTS) {
        return false;
    }
    s_element = element;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Measurement                                                               */
/* ------------------------------------------------------------------------- */

bool rmode_measure(res_sample_t *out)
{
    if (out == NULL || !s_active) {
        return false;
    }
    return kelvin_measure(s_element, out);
}

/**
 * @brief Up/down Kelvin ladder, then TIA.
 *
 * Start on the 1 kohm element. The direction is decided from the measured
 * compliance, not from a predicted resistance:
 *
 *   V_B < 0.9*VREF or V_N > VN_MAX -> too much DUT drop -> larger RSENSE
 *   V_N - V_B < 3 mV               -> too little resolution -> smaller RSENSE
 *
 * If the top element cannot regulate, the DUT is above ~22 Mohm and the TIA is
 * the only path with usable signal.
 */
bool rmode_measure_auto(res_sample_t *out)
{
    if (out == NULL || !s_active) {
        return false;
    }
    if (fe_r_faulted()) {
        return false;
    }

    res_sample_t s;
    memset(&s, 0, sizeof(s));

    int e = 1;   /* the 1 kohm element is the middle of the bank */

    for (int guard = 0; guard < RMODE_ELEMENTS + 3; ++guard) {
        if (kelvin_measure((uint8_t)e, &s)) {
            /* One refinement step: a small drop means the next lower element
             * would give more signal; a node close to the compliance limit
             * means the next higher element would give more headroom. Keep
             * whichever sample scores better. */
            double dv = s.v_n_mv - s.v_b_mv;
            int alt = -1;

            if (dv < 0.15 * (double)RMODE_VREF_MV && e > 0) {
                alt = e - 1;
            } else if (s.v_n_mv > 0.80 * (double)RMODE_VN_MAX_MV &&
                       e < RMODE_ELEMENTS - 1) {
                alt = e + 1;
            }

            if (alt >= 0) {
                res_sample_t t;
                if (kelvin_measure((uint8_t)alt, &t) && t.valid &&
                    t.quality > s.quality) {
                    s = t;
                }
            }

            *out = s;
            return true;
        }

        /* A failed ADC read leaves both nodes at zero, which cannot be
         * classified; retrying on another element would fail identically. */
        if ((s.v_n_mv <= 0.0) && (s.v_b_mv <= 0.0)) {
            break;
        }

        /* Out of regulation: either Q1 is saturated at the top rail (V_N at
         * the limit) or the loop cannot force B up to VREF, which is what a
         * DUT too large for this element looks like (V_B well below VREF).
         * Both need a LARGER RSENSE: less current, so less drop in the DUT.
         *
         * This has to include V_B ~ 0, not just "a bit low". A 100 Mohm DUT on
         * the 1 kohm element gives V_B = 3.3 V * 1k/100M, i.e. microvolts, and
         * that is exactly the case the TIA exists for. */
        bool too_much_drop = (s.v_n_mv >= (double)RMODE_VN_MAX_MV) ||
                             (s.v_b_mv < 0.90 * (double)RMODE_VREF_MV);

        if (too_much_drop) {
            if (e < RMODE_ELEMENTS - 1) {
                ++e;
                continue;
            }
            if (tia_measure(out)) {
                return true;
            }
            *out = s;
            return false;
        }

        /* In regulation but the DUT drop is below the resolution of this
         * element: a smaller RSENSE raises the current and the drop. */
        if ((s.v_n_mv - s.v_b_mv) < RMODE_MIN_DV_MV) {
            if (e > 0) {
                --e;
                continue;
            }
            *out = s;   /* already on the smallest element */
            return false;
        }

        /* Rejected on a quality gate without a clear ladder direction. */
        break;
    }

    *out = s;
    return false;
}

/* ------------------------------------------------------------------------- */
/* Calibration accessors                                                     */
/* ------------------------------------------------------------------------- */

double rmode_rref(uint8_t element)
{
    return cal_rm_rref(element);
}

void rmode_set_rref(uint8_t element, double ohm)
{
    cal_rm_set(element, ohm, cal_rm_gain(element), cal_rm_valid(element));
}

bool rmode_cal_valid(uint8_t element)
{
    return cal_rm_valid(element);
}

void rmode_set_cal(uint8_t element, double rref, double gain, double offset,
                   bool valid)
{
    (void)offset;   /* folded into gain in this topology */
    cal_rm_set(element, rref, gain, valid);
}

double rmode_tia_rf(void)
{
    return cal_rm_tia_rf();
}

void rmode_set_tia(double rf, double vbias_mv, double gain, bool valid)
{
    cal_rm_tia_set(rf, vbias_mv, gain, valid);
}

bool rmode_tia_valid(void)
{
    return cal_rm_tia_valid();
}
