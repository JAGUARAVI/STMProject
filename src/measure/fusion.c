/**
 * @file  fusion.c
 * @brief Median gating and log-domain weighted fusion of measurement samples.
 *
 * Fusion answers one question: given samples from several ranges and both
 * measurement methods, what single capacitance should be reported? Two design
 * choices dominate the answer.
 *
 * 1. The centre is a **median**, not a mean. One wildly wrong sample (a
 *    mis-ranged reading, a comparator glitch, a disconnected probe) can move a
 *    mean arbitrarily far, while a median is immune to it. The median is used
 *    only as a gate, so the reported value can still use all survivors rather
 *    than throwing away their resolution.
 *
 * 2. The survivors are averaged in the **log domain**. Capacitance spans six
 *    decades in this instrument, so arithmetic averaging would let a 1 uF
 *    sample dominate a cluster of 1 pF samples completely. In log space the
 *    fusion is scale-invariant: a factor-of-two error carries the same weight
 *    at 1 pF and at 1 uF, and the result is the weighted geometric mean, which
 *    is the natural centre for a multiplicative quantity.
 *
 * The ADC-vs-oscillator mismatch check is computed on the **raw pre-gate
 * population** (still after validity/plausibility/quality filtering, but
 * before the median gate). If it were computed after gating, a genuine
 * systematic disagreement could be masked: the median gate would discard
 * exactly the minority method's samples and the two method estimates would
 * then artificially agree. Reporting on the raw population preserves the
 * diagnostic even when one method is subsequently rejected.
 *
 * Pure arithmetic: no HAL calls and no HAL includes here, so the file can be
 * compiled and unit-tested on a host.
 */

#include "fusion.h"

#include <math.h>
#include <string.h>

/* Values at or below zero cannot enter a logarithm. This floor is far below
 * any physical capacitance the instrument can resolve (1e-15 F = 1 fF) and
 * only exists so that a zero reading contributes a very small value instead
 * of -inf/NaN. */
#define FUSION_LOG_FLOOR 1e-15

/* Fusion telemetry, read by the protocol layer. They are globals because the
 * console needs the per-contributor breakdown after the call returns. */
fusion_contrib_t g_fusion_contrib[FUSION_MAX_CONTRIB];
int              g_fusion_n_contrib;

/**
 * @brief Median of an array of doubles.
 *
 * The caller's array is never modified: fusion often receives a pointer into
 * live telemetry state, and a helper that silently reorders it would be a
 * nasty side effect. A static copy is used for the same reason rc_fit uses
 * static working storage: the sort is small, only one measurement runs at a
 * time, and it keeps the measurement stack free of a 96-byte array.
 *
 * n > FUSION_MAX_CONTRIB is capped to the first FUSION_MAX_CONTRIB entries;
 * the fusion caller never supplies more than that, and a median of a truncated
 * window is better than an out-of-bounds write.
 *
 * @param v input values
 * @param n count; values <= 0 return 0
 * @return median (mean of the two middles for even n), or 0 for empty input
 */
double median_of(const double *v, int n)
{
    static double buf[FUSION_MAX_CONTRIB];

    if (v == NULL || n <= 0) {
        return 0.0;
    }

    int m = (n > FUSION_MAX_CONTRIB) ? FUSION_MAX_CONTRIB : n;
    for (int i = 0; i < m; ++i) {
        buf[i] = v[i];
    }

    /* Insertion sort: m is at most 12, so the asymptotic difference from a
     * library qsort is irrelevant and this avoids a libc dependency in the
     * maths layer. */
    for (int i = 1; i < m; ++i) {
        double key = buf[i];
        int j = i - 1;
        while (j >= 0 && buf[j] > key) {
            buf[j + 1] = buf[j];
            --j;
        }
        buf[j + 1] = key;
    }

    if ((m & 1) != 0) {
        return buf[m / 2];
    }
    return 0.5 * (buf[m / 2 - 1] + buf[m / 2]);
}

/** log(c) with a floor for non-positive values, keeping every arithmetic path
 *  finite. The caller has already discarded non-finite capacitances. */
static inline double fusion_log_c(double c)
{
    return log(c > 0.0 ? c : FUSION_LOG_FLOOR);
}

/**
 * @brief Fuse a set of scored samples into one estimate.
 *
 * Pipeline:
 *  1. Drop invalid, implausible and below-noise-floor samples.
 *  2. Compute per-method estimates on that raw population.
 *  3. Median-gate at FUSION_GATE_REL and keep survivors for the final value.
 *  4. Quality-weighted geometric mean of the survivors, plus the log-domain
 *     relative spread as an uncertainty indicator.
 *
 * @param samples sample array, may be NULL when n <= 0
 * @param n       number of samples
 * @return fusion result. valid is false when no sample survived.
 */
fusion_t fuse_samples(const sample_t *samples, int n)
{
    fusion_t f;
    memset(&f, 0, sizeof(f));

    /* Documented NAN sentinels: these three fields are explicitly allowed to
     * be NaN, and only when there is no estimate to report. */
    f.adc_estimate = NAN;
    f.osc_estimate = NAN;
    f.method_mismatch_ratio = NAN;
    f.raw_samples = n;

    g_fusion_n_contrib = 0;

    if (samples == NULL || n <= 0) {
        return f;
    }

    /* Never write past the contributor array even if a caller passes a larger
     * n. raw_samples above still reports what was actually offered. */
    int n_scan = (n > FUSION_MAX_CONTRIB) ? FUSION_MAX_CONTRIB : n;

    /* Static for the same reason the reference firmware used file-scope
     * arrays: the measurement task's stack is small and only one fusion runs
     * at a time. Entries [0, k) are always rewritten before use. */
    static double  cvals[FUSION_MAX_CONTRIB];
    static double  weights[FUSION_MAX_CONTRIB];
    static uint8_t methods[FUSION_MAX_CONTRIB];

    int k = 0;
    for (int i = 0; i < n_scan; ++i) {
        const sample_t *sm = &samples[i];

        if (!sm->valid || !sm->plausible) {
            continue;
        }
        /* The 0.02 floor is the noise floor carried over from the ESP32
         * firmware: below it a sample carries less information than the cost
         * of including it. NaN/Inf quality is treated as unusable rather than
         * being allowed to poison the weights. */
        if (!isfinite(sm->quality) || sm->quality < 0.02) {
            continue;
        }
        /* A non-finite capacitance cannot be sorted or logged. Zero is allowed
         * and handled by the log floor, because a tared open socket really can
         * report 0 F. */
        if (!isfinite(sm->capacitance_f)) {
            continue;
        }

        cvals[k]   = sm->capacitance_f;
        weights[k] = sm->quality;
        methods[k] = (uint8_t)sm->method;

        fusion_contrib_t *fc = &g_fusion_contrib[k];
        fc->range_idx = sm->range_idx;
        fc->method    = (uint8_t)sm->method;
        fc->c_f       = sm->capacitance_f;
        fc->c_eq_f    = sm->c_eq_f;
        fc->q         = sm->quality;
        fc->w         = sm->quality;
        fc->r2        = (sm->method == METHOD_ADC_STEP) ? sm->fit_r2 : 0.0;
        fc->kept      = false;  /* set by the median gate below */

        ++k;
    }

    g_fusion_n_contrib = k;
    f.n_kept = k;

    if (k == 0) {
        return f;
    }

    /* ---- Per-method estimates on the raw population --------------------- */
    double adc_sum = 0.0, adc_w = 0.0;
    double osc_sum = 0.0, osc_w = 0.0;

    for (int i = 0; i < k; ++i) {
        double lc = fusion_log_c(cvals[i]);
        if (methods[i] == (uint8_t)METHOD_ADC_STEP) {
            adc_sum += weights[i] * lc;
            adc_w   += weights[i];
        } else if (methods[i] == (uint8_t)METHOD_OSC) {
            osc_sum += weights[i] * lc;
            osc_w   += weights[i];
        }
    }

    if (adc_w > 0.0) {
        f.adc_estimate = exp(adc_sum / adc_w);
    }
    if (osc_w > 0.0) {
        f.osc_estimate = exp(osc_sum / osc_w);
    }

    if (isfinite(f.adc_estimate) && isfinite(f.osc_estimate) &&
        f.adc_estimate > 0.0 && f.osc_estimate > 0.0) {
        double hi = fmax(f.adc_estimate, f.osc_estimate);
        double lo = fmin(f.adc_estimate, f.osc_estimate);
        f.method_mismatch_ratio = hi / lo;
        f.method_mismatch = (f.method_mismatch_ratio > METHOD_MISMATCH_RATIO);
    }

    /* ---- Median gate ---------------------------------------------------- */
    double med = median_of(cvals, k);
    f.median_f = med;

    double lw = 0.0;
    double lw_sum = 0.0;
    bool   have_kept = false;
    int    n_adc = 0;
    int    n_osc = 0;

    for (int i = 0; i < k; ++i) {
        if (med > 0.0 &&
            fabs(cvals[i] - med) / med > FUSION_GATE_REL) {
            g_fusion_contrib[i].kept = false;
            ++f.n_gated;
            continue;
        }

        g_fusion_contrib[i].kept = true;

        if (!have_kept) {
            f.c_min_f = cvals[i];
            f.c_max_f = cvals[i];
            have_kept = true;
        } else {
            if (cvals[i] < f.c_min_f) {
                f.c_min_f = cvals[i];
            }
            if (cvals[i] > f.c_max_f) {
                f.c_max_f = cvals[i];
            }
        }

        lw_sum += weights[i] * fusion_log_c(cvals[i]);
        lw     += weights[i];

        if (methods[i] == (uint8_t)METHOD_ADC_STEP) {
            ++n_adc;
        } else if (methods[i] == (uint8_t)METHOD_OSC) {
            ++n_osc;
        }
    }

    f.n_adc = n_adc;
    f.n_osc = n_osc;

    if (lw <= 0.0 || !have_kept) {
        return f;
    }

    f.capacitance_f = exp(lw_sum / lw);
    f.total_weight  = lw;

    int n_kept_gate = n_adc + n_osc;
    f.quality = (n_kept_gate > 0)
                    ? lw / (double)n_kept_gate
                    : 0.0;

    f.valid = true;

    /* Weighted relative spread in log space. This is the natural error bar for
     * a geometric mean and is invariant to the absolute capacitance scale.
     * The same log floor as above keeps a zero reading from producing -inf. */
    double var = 0.0;
    for (int i = 0; i < k; ++i) {
        if (!g_fusion_contrib[i].kept) {
            continue;
        }
        double ci = cvals[i] > 0.0 ? cvals[i] : FUSION_LOG_FLOOR;
        double d = log(ci / f.capacitance_f);
        var += weights[i] * d * d;
    }
    f.rel_spread = sqrt(var / lw);

    return f;
}
