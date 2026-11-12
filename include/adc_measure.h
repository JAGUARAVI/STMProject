/**
 * @file  adc_measure.h
 * @brief Turns a captured RC charge curve into a scored sample.
 *
 * Pure arithmetic: no HAL, host-testable. The caller supplies the curve; this
 * module applies the fit, the physics plausibility gates and the quality
 * model.
 */

#ifndef ESWCAP_ADC_MEASURE_H
#define ESWCAP_ADC_MEASURE_H

#include "eswcap.h"

typedef struct {
    const double *v_mv;      /**< sample voltages */
    int           n;         /**< sample count */
    double        dt_us;     /**< uniform sample interval */
    double        v0_mv;     /**< node voltage at the step edge */
    double        v_inf_hint_mv; /**< asymptote hint, <=0 if unknown */
    uint8_t       range_idx;
    bool          fit_ok;    /**< out: which estimator was used */
} adc_capture_t;

/** Score a capture. On success fills sample_t (valid/plausible/quality) and
 *  returns it. Also reports the fitted asymptote through *v_inf_out so the
 *  caller can cache it per range. */
sample_t adc_measure_sample(const adc_capture_t *cap, double *v_inf_out);

/** Fold the leakage path implied by an asymptote droop into the chain
 *  resistance. When V_inf is at the rail there is no droop and R_chain is
 *  returned unchanged; otherwise the parasitic pull-down R_leak =
 *  V_inf*R_chain/(V_nom - V_inf) appears in parallel.
 *  Exposed because the calibration solver must use the identical formula. */
double adc_effective_r(double r_chain, double v_inf_mv);

/** Exact inverse of adc_effective_r(). */
double adc_chain_from_eff(double r_eff, double v_inf_mv);

#endif /* ESWCAP_ADC_MEASURE_H */
