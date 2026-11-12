/**
 * @file  osc_measure.c
 * @brief Turns a hardware-captured oscillator period into a scored sample.
 *
 * The relaxation oscillator obeys
 *
 *     T = K * R * C_eq + T0
 *
 * where C_eq is the DUT in series with C_block and T0 is the measured
 * open-node tare. The measurement path therefore has to subtract the tare
 * (or the legacy per-cycle delay when no tare exists), scale the remainder by
 * K*R, undo the C_block series combination and, only in the un-tared case,
 * remove the nominal stray capacitance.
 *
 * The distinction between the two offset paths matters: with a tare the stray
 * is already inside T0, so subtracting STRAY_CAPACITANCE_F again would double
 * count it. Without one, the legacy path still needs the nominal constant to
 * keep an un-calibrated board in the right ballpark.
 *
 * A small negative (period - tare) excursion is tolerated when a tare exists.
 * Around an open socket the true period is essentially T0, so the measured
 * period can land a few tens of nanoseconds on the wrong side purely from
 * noise. Treating that as a negative RC time would make an open socket an
 * error instead of a valid ~0 pF reading.
 *
 * Pure arithmetic: no HAL calls and no HAL includes here, so the file can be
 * compiled and unit-tested on a host.
 */

#include "osc_measure.h"

#include <math.h>
#include <string.h>

#include "config.h"
#include "osc_calc.h"

/* Largest negative (period - tare) fraction still considered noise around the
 * tare baseline. Larger than this is a real timing fault, not dither. */
#define OSC_TARE_NOISE_FRACTION 0.05

/**
 * @brief Build a measured-capacitance sample from a captured period.
 *
 * valid means "a finite capacitance was recovered". plausible is the
 * independent physics gate and can be false on a valid sample: for example a
 * 2 MHz oscillation still yields a number, but it is outside the oscillator's
 * usable band. Keeping the two flags separate lets telemetry show both the
 * attempt and why it was rejected.
 *
 * @param range_idx  range used
 * @param period_us  mean full period, microseconds
 * @param n_periods  number of periods averaged
 * @param k          geometry constant for this range
 * @param t0_us      measured open-node tare; ignored unless has_t0
 * @param has_t0     whether t0_us is valid
 * @param delay_us   legacy per-cycle delay used when no tare exists
 * @param r_ohm      range resistance, ohms
 * @param c_block_f  DC-blocking capacitance (the series inversion itself uses
 *                   the current board constant through cblock_invert())
 * @return scored sample
 */
sample_t osc_measure_sample(uint8_t range_idx, double period_us,
                            uint32_t n_periods,
                            double k, double t0_us, bool has_t0,
                            double delay_us, double r_ohm, double c_block_f)
{
    sample_t s;
    memset(&s, 0, sizeof(s));
    s.method = METHOD_OSC;
    s.range_idx = range_idx;

    /* cblock_invert() uses the live board C_block so that a flash override
     * applies consistently to both measurement methods. The parameter is part
     * of the public contract and is kept for callers that already have the
     * value, but it must not silently diverge from the board state. */
    (void)c_block_f;

    /* The !(x > 0) form rejects NaN as well as zero and negative values. */
    if (!(period_us > 0.0) || !(k > 0.0) || !(r_ohm > 0.0)) {
        return s;
    }
    if (!isfinite(period_us) || !isfinite(k) || !isfinite(r_ohm)) {
        return s;
    }

    double freq = 1e6 / period_us;
    bool use_t0 = has_t0;
    double off = use_t0 ? t0_us : delay_us;

    if (!isfinite(off)) {
        s.freq_hz = freq;
        s.tau_us = period_us;
        return s;
    }

    double eff = period_us - off;

    if (eff <= 0.0) {
        /* A tared open node legitimately sits at ~0 effective time. Allow a
         * small negative excursion; anything larger means the capture is
         * shorter than the tare and cannot describe a positive RC time. */
        if (use_t0 && off > 0.0 &&
            eff > -OSC_TARE_NOISE_FRACTION * off) {
            eff = 0.0;
        } else {
            s.freq_hz = freq;
            s.tau_us = period_us;
            return s;
        }
    }

    double c_eq = eff * 1e-6 / (k * r_ohm);
    double c_total = cblock_invert(c_eq);

    if (!isfinite(c_eq) || c_total < 0.0 || !isfinite(c_total)) {
        s.freq_hz = freq;
        s.tau_us = period_us;
        return s;
    }

    /* With a tare the stray is inside T0. Without one, subtract the compiled
     * nominal as the cold-start approximation. A negative result is clamped
     * to zero rather than reported as negative capacitance. */
    double c = use_t0 ? c_total : (c_total - STRAY_CAPACITANCE_F);
    if (c < 0.0) {
        c = 0.0;
    }

    /* tau_us is the raw measured full period, not the tare-corrected one:
     * the field is a timing diagnostic, and the tare is already reflected in
     * c_eq_f. */
    s.tau_us        = period_us;
    s.freq_hz       = freq;
    s.c_eq_f        = c_eq;
    s.capacitance_f = c;
    s.valid         = true;

    bool plausible = true;
    if (freq < OSC_MIN_F_HZ) {
        plausible = false;
    }
    if (freq > OSC_MAX_F_HZ) {
        plausible = false;
    }
    if (n_periods < 4U) {
        plausible = false;
    }
    if (c < (use_t0 ? 0.0 : 0.5 * STRAY_CAPACITANCE_F)) {
        plausible = false;
    }
    s.plausible = plausible;

    /* Quality is reported even for an implausible sample so the caller can
     * distinguish "rejected but nearly in band" from "wildly out of band". */
    s.quality = osc_quality(freq, n_periods, use_t0, c);
    s.suspicious = false;

    return s;
}
