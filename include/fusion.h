/**
 * @file  fusion.h
 * @brief Scoring, median gating and log-domain weighted fusion.
 *
 * Pure arithmetic: no HAL, host-testable.
 */

#ifndef ESWCAP_FUSION_H
#define ESWCAP_FUSION_H

#include "eswcap.h"

/** Fuse up to n samples. Preserves the ESP32 semantics exactly:
 *  - quality below 0.02 is a noise floor and is dropped
 *  - per-method estimates are computed on the raw pre-gate population so a
 *    genuine ADC-vs-oscillator disagreement is still reported
 *  - a relative median gate of FUSION_GATE_REL rejects outliers
 *  - survivors combine as a log-domain quality-weighted mean
 */
fusion_t fuse_samples(const sample_t *samples, int n);

/** Median of n doubles. Returns 0 for n <= 0. Exposed for tests. */
double median_of(const double *v, int n);

#endif /* ESWCAP_FUSION_H */
