/**
 * @file  adc_measure.c
 * @brief Turns a captured RC charge curve into a scored capacitance sample.
 *
 * Acquisition hands this module a uniformly sampled voltage curve. The module
 * fits V(t) = Vinf - (Vinf - V0)*exp(-(t-t0)/tau), converts tau to capacitance
 * through the range resistor, removes the C_block series combination and the
 * node offset, then applies the physics gates and the quality model.
 *
 * Two details in here are deliberately non-obvious:
 *
 *  - A fitted asymptote below the 3.3 V rail means a leakage path is pulling
 *    the node down. The RC time constant then belongs to R_chain || R_leak,
 *    not R_chain, so the chain resistance has to be folded down before tau is
 *    converted to capacitance. adc_effective_r() is the exact fold and
 *    adc_chain_from_eff() its exact inverse, so calibration can store the
 *    chain value that the runtime fold will turn back into the solved R_eff.
 *
 *  - The Gauss-Newton fit can fail for legitimate reasons: a fast range may
 *    charge completely within the first handful of points, and a shallow
 *    curve gives the three-parameter fit very little to work with. Rather than
 *    throwing the capture away, a two-threshold estimator recovers tau from
 *    the 10 % and 63.2 % crossings. It is less precise, so it is tagged with
 *    fit_r2 = -1 and its quality term is capped below a good fit; telemetry
 *    still reports the attempt instead of a silent hole.
 *
 * Pure arithmetic: no HAL calls and no HAL includes here, so the file can be
 * compiled and unit-tested on a host.
 */

#include "adc_measure.h"

#include <math.h>
#include <string.h>

#include "calib.h"
#include "config.h"
#include "rc_fit.h"

/* ADC full scale in millivolts. The part has no external reference on this
 * board, so the rail is the reference. */
#define ADC_VREF_MV          3300.0

/* Within this distance of the rail the asymptote is considered "at the rail":
 * the droop is inside the fit's uncertainty and dividing by it would fold a
 * meaningless (possibly negative) leakage resistance into R_eff. */
#define ADC_RAIL_MARGIN_MV   50.0

/* Hard physical bounds for a plausible DUT. The upper bound is well past any
 * real capacitor this instrument targets; it exists to reject a fit that ran
 * away. */
#define ADC_TAU_MAX_US       15.0e6
#define ADC_C_MAX_F          1.0

/* Fallback-estimator thresholds, as fractions of the asymptote excursion. */
#define ADC_LOW_FRACTION     0.10
#define ADC_TAU_FRACTION     0.632

/* Smallest correction exponent accepted from the two-threshold estimator.
 * If the crossings are closer than this the extrapolated tau is dominated by
 * one ADC step, so the estimate is clamped rather than allowed to explode. */
#define ADC_MIN_K_CORR       0.05

/* The curve copy is static for the same reason rc_fit uses static storage: a
 * 256-point curve is ~4 KB, which is not comfortable on a measurement stack,
 * and only one measurement runs at a time. */
static double s_xs[RC_FIT_MAX_PTS];
static double s_ys[RC_FIT_MAX_PTS];

/**
 * @brief Fold the leakage implied by an asymptote droop into R_chain.
 *
 * A real RC charge driven from a 3.3 V logic output normally settles at the
 * rail. When the fitted asymptote sits below it, the missing voltage is being
 * lost through a parasitic pull-down. That path sees the same node voltage as
 * the range resistor, so it appears in parallel:
 *
 *     R_leak = Vinf * R_chain / (Vrail - Vinf)
 *     R_eff  = R_chain * R_leak / (R_chain + R_leak)
 *
 * Returning R_chain unchanged near the rail is not a shortcut: as Vinf
 * approaches Vrail, R_leak tends to infinity and the parallel combination
 * tends to R_chain. The explicit branch just avoids evaluating the singular
 * expression there.
 *
 * @param r_chain   nominal (or calibrated) chain resistance, ohms
 * @param v_inf_mv  asymptote used by the fit, millivolts
 * @return effective resistance seen by the node, ohms
 */
double adc_effective_r(double r_chain, double v_inf_mv)
{
    if (!(r_chain > 0.0)) {
        return r_chain;
    }

    if (v_inf_mv >= ADC_VREF_MV - ADC_RAIL_MARGIN_MV) {
        return r_chain;
    }

    double r_leak = v_inf_mv * r_chain /
                    (ADC_VREF_MV - v_inf_mv + 1e-9);

    /* A zero/negative leak means the asymptote itself was non-physical
     * (negative, or NaN); leave the chain value alone rather than producing a
     * negative resistance. The !(x > 0) form also catches NaN. */
    if (!(r_leak > 0.0)) {
        return r_chain;
    }

    double r_eff = (r_chain * r_leak) / (r_chain + r_leak);

    /* An overflowing leak is electrically an open circuit, so the parallel
     * combination is just R_chain. Returning the finite value keeps a
     * pathological input from turning into inf/inf = NaN. */
    if (!isfinite(r_eff)) {
        return r_chain;
    }

    return r_eff;
}

/**
 * @brief Exact inverse of adc_effective_r().
 *
 * With a = Vinf/(Vrail - Vinf), adc_effective_r() gives
 * R_eff = R_chain * a/(1+a), hence
 *
 *     R_chain = R_eff * (1 + a) / a.
 *
 * The calibration solver must use this inverse so the R_eff it stores, once
 * folded forward again at measurement time, reproduces the curve the
 * calibration saw. Keeping the two directions in one file stops them from
 * drifting apart.
 *
 * @param r_eff     effective resistance, ohms
 * @param v_inf_mv  asymptote used by the fit, millivolts
 * @return chain resistance, ohms
 */
double adc_chain_from_eff(double r_eff, double v_inf_mv)
{
    if (!(r_eff > 0.0)) {
        return r_eff;
    }

    if (v_inf_mv >= ADC_VREF_MV - ADC_RAIL_MARGIN_MV) {
        return r_eff;
    }

    double denom = ADC_VREF_MV - v_inf_mv + 1e-9;
    if (!(denom > 0.0)) {
        return r_eff;
    }

    double a = v_inf_mv / denom;
    if (!(a > 0.0)) {
        return r_eff;
    }

    double r_chain = r_eff * (1.0 + a) / a;

    /* An overflow here means the input was not physical. Returning the
     * effective value is the only finite, unit-correct fallback. */
    if (!isfinite(r_chain)) {
        return r_eff;
    }

    return r_chain;
}

/**
 * @brief Score one RC-step capture.
 *
 * The estimator choice is made before any resistance conversion because the
 * asymptote is needed to choose the resistance. A fit is accepted only when
 * it is numerically healthy *and* its asymptote clears the observed peak by a
 * sane margin; otherwise the two-threshold fallback takes over.
 *
 * @param cap         captured curve and range metadata
 * @param v_inf_out   optional; receives the asymptote actually used
 * @return scored sample. valid is false when no finite capacitance could be
 *         recovered; plausible is the independent physics gate.
 */
sample_t adc_measure_sample(const adc_capture_t *cap, double *v_inf_out)
{
    sample_t s;
    memset(&s, 0, sizeof(s));
    s.method = METHOD_ADC_STEP;

    if (cap == NULL || cap->v_mv == NULL || cap->n <= 0) {
        return s;
    }

    s.range_idx = cap->range_idx;
    if (cap->range_idx >= RANGE_COUNT) {
        /* Every lookup below is indexed by range; treating an out-of-range
         * code as an invalid capture is safer than clamping to a different
         * range and reporting a believable but wrong capacitance. */
        return s;
    }

    int n = cap->n;
    if (n > RC_FIT_MAX_PTS) {
        n = RC_FIT_MAX_PTS;
    }

    /* Copy the curve once. The fallback scan and the peak search then work on
     * a stable, bounded snapshot even if the caller's buffer is shorter than
     * the timestamp array rc_fit_uniform() would otherwise build. */
    double v_peak = cap->v_mv[0];
    for (int i = 0; i < n; ++i) {
        s_xs[i] = (double)i * cap->dt_us;
        s_ys[i] = cap->v_mv[i];
        if (s_ys[i] > v_peak) {
            v_peak = s_ys[i];
        }
    }

    rc_fit_t fit = rc_fit_uniform(cap->v_mv, n, cap->dt_us, cap->v0_mv,
                                  cap->v_inf_hint_mv);

    bool   used_fit  = false;
    double tau_eff   = 0.0;
    double v_inf_eff = 0.0;
    double fit_r2    = -1.0;
    double fit_rmse  = -1.0;
    double tau_unc   = -1.0;

    /* A fit whose asymptote is far below the observed peak is not describing
     * this curve; the peak itself is the cheapest independent cross-check. */
    bool fit_ok = fit.ok &&
                  fit.r2 > 0.90 &&
                  fit.tau_us > 0.0 &&
                  fit.v_inf_mv >= v_peak - 20.0;

    if (fit_ok) {
        used_fit  = true;
        tau_eff   = fit.tau_us;
        v_inf_eff = fit.v_inf_mv;
        fit_r2    = fit.r2;
        fit_rmse  = fit.rmse_mv;
        tau_unc   = fit.tau_se_us;

        if (v_inf_out != NULL) {
            *v_inf_out = fit.v_inf_mv;
        }
    } else {
        /* ---- Two-threshold fallback -------------------------------------
         * Solving tau from two crossings needs no iteration: for a single
         * exponential the ratio of the gaps to the asymptote is fixed, so
         *
         *     tau = (t_63 - t_10) / ln((Vinf - V10) / (Vinf - V63)).
         *
         * The 10 % crossing is floored 40 mV above V0 so that noise around a
         * flat start cannot define t_start. The 63.2 % crossing is the
         * classic single-time-constant point and is far enough from both ends
         * to be well conditioned. The result is deliberately flagged with
         * r2 = -1: it is a rescue estimate, not a fit. */
        double v_inf = (cap->v_inf_hint_mv > 0.0)
                           ? cap->v_inf_hint_mv
                           : v_peak;
        double v0 = cap->v0_mv;

        double v_low = v0 + ADC_LOW_FRACTION * (v_inf - v0);
        double v_floor = v0 + 40.0;
        if (v_low < v_floor) {
            v_low = v_floor;
        }
        double v_tau = v0 + ADC_TAU_FRACTION * (v_inf - v0);

        double t_start = 0.0;
        double t_tau   = 0.0;
        bool   have_start = false;
        bool   have_tau   = false;

        for (int i = 0; i < n; ++i) {
            if (!have_start && s_ys[i] >= v_low) {
                t_start = s_xs[i];
                have_start = true;
            }
            if (!have_tau && s_ys[i] >= v_tau) {
                t_tau = s_xs[i];
                have_tau = true;
            }
            if (have_start && have_tau) {
                break;
            }
        }

        /* A capture that never reaches both thresholds carries no usable
         * timing information. */
        if (!have_start || !have_tau) {
            return s;
        }

        double gap_low = v_inf - v_low;
        double gap_tau = v_inf - v_tau;
        if (!(gap_low > 0.0) || !(gap_tau > 0.0)) {
            return s;
        }

        double k_corr = log(gap_low / gap_tau);
        if (!isfinite(k_corr)) {
            return s;
        }
        if (k_corr < ADC_MIN_K_CORR) {
            k_corr = ADC_MIN_K_CORR;
        }

        tau_eff = (t_tau - t_start) / k_corr;

        /* If the floor pushed v_low above v_tau, the crossings can appear in
         * the wrong order and tau goes negative. That is a bad capture, not a
         * negative time constant. */
        if (!(tau_eff >= 0.0) || !isfinite(tau_eff)) {
            return s;
        }

        fit_r2   = -1.0;  /* marks the fallback path */
        fit_rmse = -1.0;
        tau_unc  = -1.0;
        v_inf_eff = v_inf;

        if (v_inf_out != NULL) {
            *v_inf_out = v_inf;
        }
    }

    /* ---- Resistance and capacitance ------------------------------------- */
    bool adc_cal = cal_adc_valid(cap->range_idx);

    double r_chain;
    if (adc_cal) {
        r_chain = cal_adc_r_eff(cap->range_idx);
        if (!(r_chain > 0.0)) {
            r_chain = g_ranges[cap->range_idx].resistance_ohms;
        }
    } else {
        r_chain = g_ranges[cap->range_idx].resistance_ohms;
    }

    double r_eff = adc_effective_r(r_chain, v_inf_eff);
    if (!(r_eff > 0.0) || !isfinite(r_eff)) {
        return s;
    }

    double c_eq = tau_eff * 1e-6 / r_eff;
    if (!isfinite(c_eq) || c_eq < 0.0) {
        return s;
    }

    double c_total = cblock_invert(c_eq);
    if (!isfinite(c_total) || c_total < 0.0) {
        return s;
    }

    double c_off = adc_cal ? cal_adc_c0(cap->range_idx)
                           : (double)STRAY_CAPACITANCE_F;
    if (!isfinite(c_off)) {
        c_off = (double)STRAY_CAPACITANCE_F;
    }

    double c = c_total - c_off;
    if (c < 0.0) {
        c = 0.0;
    }

    s.tau_us        = tau_eff;
    s.c_eq_f        = c_eq;
    s.capacitance_f = c;
    s.v_inf_mv      = v_inf_eff;
    s.fit_r2      = fit_r2;
    s.fit_rmse_mv = fit_rmse;
    s.tau_unc_us  = tau_unc;
    s.valid       = true;

    /* ---- Physics gates ---------------------------------------------------
     * These are independent of estimator quality. A short tau cannot be
     * resolved by ADC_STEP_THRESHOLD_US, an absurdly long one is a runaway
     * fit, and a result buried in the stray capacitance (or above the
     * instrument's range) is not a DUT measurement. Fields stay filled so
     * telemetry can report the attempt. */
    bool plausible = true;
    if (tau_eff < ADC_STEP_THRESHOLD_US) {
        plausible = false;
    }
    if (tau_eff > ADC_TAU_MAX_US) {
        plausible = false;
    }
    if (c < 3.0 * STRAY_CAPACITANCE_F) {
        plausible = false;
    }
    if (c > ADC_C_MAX_F) {
        plausible = false;
    }
    s.plausible = plausible;

    /* ---- Quality ---------------------------------------------------------
     * The terms multiply, so any one bad physical condition dominates. The
     * time term is a log-domain plateau: tau is meaningful over a wide band,
     * and the roll-off is symmetric in decades rather than in microseconds.
     * The fit term penalises the fallback path without discarding it, and the
     * uncertainty term collapses when the fit covariance says tau is poorly
     * determined. */
    double q_time;
    if (!(tau_eff > 0.0)) {
        q_time = 0.0;
    } else {
        double log_tau = log(tau_eff);
        if (tau_eff >= 200.0 && tau_eff <= 20000.0) {
            q_time = 1.0;
        } else if (tau_eff < 200.0) {
            q_time = exp(-pow((log_tau - log(200.0)) / 0.8, 2.0));
        } else {
            q_time = exp(-pow((log_tau - log(20000.0)) / 2.0, 2.0));
        }
    }

    double q_stray = c / (c + 3.0 * STRAY_CAPACITANCE_F);
    double q_range = (cap->range_idx <= 1) ? 0.85 : 1.0;

    double q_fit;
    if (fit_r2 < 0.0) {
        q_fit = 0.6;
    } else if (fit_r2 >= 0.99) {
        q_fit = 1.0;
    } else {
        q_fit = 0.4 + 0.6 * fit_r2;
    }

    double q_unc = 1.0;
    if (used_fit && tau_unc > 0.0 && tau_eff > 0.0) {
        q_unc = exp(-pow((tau_unc / tau_eff) / 0.05, 2.0));
    }

    s.quality = q_time * q_stray * q_range * q_fit * q_unc;
    s.suspicious = false;

    return s;
}
