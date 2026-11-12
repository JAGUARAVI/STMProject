/**
 * @file  osc_calc.c
 * @brief The relaxation-oscillator model and its calibration solvers.
 *
 * The oscillator is modelled as
 *
 *     T = K * R * C_eq + T0
 *
 * where C_eq is the series combination of the DUT with the DC-blocking
 * capacitor, K is a dimensionless geometry constant for the range, and T0 is a
 * *measured* open-node period offset.
 *
 * T0 is the important part. On the ESP32 it absorbed both the node parasitic
 * capacitance and the software loop latency, so it was large and drifted with
 * ISR timing. On the G474 the period is captured by TIM2 in hardware, so the
 * latency contribution essentially vanishes and T0 becomes a cleaner measure of
 * the stray capacitance alone. The model is unchanged; only its ingredients
 * got better. That is why the existing two-point cal1/cal2 procedure still
 * applies and why old calibration values are meaningful, if not optimal.
 *
 * Pure arithmetic: no HAL, host-testable.
 */

#include "osc_calc.h"

#include <stddef.h>

#include "config.h"

/* A solved K outside these bounds means a bad reference value or a saturated
 * capture, not real physics. The bounds are deliberately generous: the exact
 * constant depends on the comparator thresholds and the drive topology, and
 * rejecting a legitimate value is worse than accepting a marginal one that the
 * quality score will down-weight anyway.
 *
 * The lower bound must sit well below OSC_K_NOMINAL (~0.085 with 70 mV of
 * COMP1 hysteresis): the hysteresis has a wide datasheet spread, so a real
 * board can land at roughly half the nominal K. 0.05 was sized for the ESP32
 * LM393 (K ~= 0.19) and would refuse a legitimate G474 calibration. */
#define OSC_K_MIN 0.02
#define OSC_K_MAX 50.0

/**
 * @brief Series-equivalent capacitance implied by a measured period.
 *
 * Returns a negative value when the period is shorter than the tare, which is
 * non-physical (it would imply a negative RC time).
 */
double osc_c_eq(double period_us, double t0_us, double k, double r_ohm)
{
    if (k <= 0.0 || r_ohm <= 0.0) {
        return -1.0;
    }
    double eff_us = period_us - t0_us;
    if (eff_us <= 0.0) {
        return -1.0;
    }
    return (eff_us * 1e-6) / (k * r_ohm);
}

/**
 * @brief Recover the DUT capacitance from the series equivalent.
 *
 * With a valid tare the stray is already inside T0, so C_eq inverts straight
 * to the DUT. Without one, the legacy path still subtracts the nominal
 * stray constant — that is less accurate but keeps an un-calibrated board
 * behaving the way it did before.
 */
double osc_c_dut(double c_eq_f, double c_block_f, bool tared)
{
    (void)c_block_f;

    if (c_eq_f < 0.0) {
        return -1.0;
    }

    double c_total = cblock_invert(c_eq_f);
    if (c_total < 0.0) {
        return -1.0;
    }

    if (tared) {
        return c_total;
    }

    double c = c_total - STRAY_CAPACITANCE_F;
    return (c < 0.0) ? 0.0 : c;
}

/**
 * @brief Solve K from a single known reference.
 *
 * Uses the same C_eq model as the measurement path, which is essential: an
 * earlier ESP32 bug added the stray capacitance here *and* relied on the tare,
 * which double-counted it and pushed the solved K about 2.1x too large. Keep
 * this consistent with osc_c_eq() / osc_c_dut().
 */
double osc_k_from_ref(double period_us, double t0_us, double r_ohm,
                      double c_ref_f, double c_block_f)
{
    (void)c_block_f;

    if (r_ohm <= 0.0 || c_ref_f <= 0.0) {
        return -1.0;
    }

    /* The tare already removes the stray, so the reference is the DUT alone. */
    double c_eq = cblock_forward(c_ref_f);
    if (c_eq <= 0.0) {
        return -1.0;
    }

    double eff_us = period_us - t0_us;
    if (eff_us <= 0.0) {
        return -1.0;
    }

    double k = (eff_us * 1e-6) / (r_ohm * c_eq);
    if (k < OSC_K_MIN || k > OSC_K_MAX) {
        return -1.0;
    }
    return k;
}

/**
 * @brief Solve K and T0 from two known references on the same range.
 *
 *     T1 = K*R*Ceq1 + T0
 *     T2 = K*R*Ceq2 + T0
 *
 * Subtracting removes T0, which is exactly the point: the parasitic
 * capacitance cancels instead of having to be known. The two references must
 * differ enough that Ceq1-Ceq2 is well above the measurement noise, otherwise
 * the solve is ill-conditioned and is rejected.
 */
bool osc_solve_two_point(double p1_us, double c1_f,
                         double p2_us, double c2_f,
                         double r_ohm, double c_block_f,
                         double *k_out, double *t0_out)
{
    (void)c_block_f;

    if (k_out == NULL || t0_out == NULL || r_ohm <= 0.0) {
        return false;
    }
    if (c1_f <= 0.0 || c2_f <= 0.0) {
        return false;
    }

    double ceq1 = cblock_forward(c1_f);
    double ceq2 = cblock_forward(c2_f);
    if (ceq1 <= 0.0 || ceq2 <= 0.0) {
        return false;
    }

    double dc = ceq1 - ceq2;
    double dp = p1_us - p2_us;

    /* Require the references to be separated by at least 5 % in C_eq. Closer
     * than that and the subtraction amplifies noise into a meaningless K. */
    double scale = (ceq1 > ceq2) ? ceq1 : ceq2;
    if (fabs(dc) < 0.05 * scale) {
        return false;
    }

    double k = (dp * 1e-6) / (r_ohm * dc);
    if (k < OSC_K_MIN || k > OSC_K_MAX) {
        return false;
    }

    double t0 = p1_us - (k * r_ohm * ceq1 * 1e6);
    if (!isfinite(t0)) {
        return false;
    }

    *k_out = k;
    *t0_out = t0;
    return true;
}

/**
 * @brief Quality score for an oscillator sample.
 *
 * Carried over unchanged from the ESP32 firmware so that the fusion weights
 * keep the same meaning:
 *
 *  - frequency: full confidence over a plateau, Gaussian roll-off outside.
 *    The plateau is now conservative rather than optimistic: on the ESP32 the
 *    upper edge was set by software-loop latency, which the G474 hardware
 *    capture removes. Widening it is a future tuning exercise, not a porting
 *    change, so behaviour is preserved deliberately.
 *  - period count: more averaged periods means less quantisation noise.
 *  - stray: without a tare, small readings are dominated by parasitic
 *    capacitance and are down-weighted; with a tare that term is disabled
 *    because a legitimately open socket reads ~0 pF.
 */
double osc_quality(double freq_hz, uint32_t n_periods, bool tared, double c_f)
{
    if (freq_hz <= 0.0) {
        return 0.0;
    }

    double log_f = log(freq_hz);
    double q_freq;

    if (freq_hz >= 500.0 && freq_hz <= 10000.0) {
        q_freq = 1.0;
    } else if (freq_hz < 500.0) {
        q_freq = exp(-pow((log_f - log(500.0)) / 1.2, 2.0));
    } else {
        q_freq = exp(-pow((log_f - log(10000.0)) / 0.9, 2.0));
    }

    double q_count = (n_periods >= OSC_TARGET_PERIODS)
                         ? 1.0
                         : 0.6 + 0.4 * ((double)n_periods / (double)OSC_TARGET_PERIODS);

    double q_stray = tared ? 1.0 : (c_f / (c_f + 3.0 * STRAY_CAPACITANCE_F));

    return q_freq * q_count * q_stray;
}
