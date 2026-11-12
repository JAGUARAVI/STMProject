/**
 * @file  rc_fit.h
 * @brief Voltage-domain exponential fit:  V(t) = Vinf - (Vinf - V0)*exp(-(t-t0)/tau)
 *
 * Ported from the ESP32 firmware, where the joint solve of Vinf, tau and t0
 * removed a ~7 % tau bias that the old log-linearisation produced when the
 * asymptote estimate was off.
 *
 * Pure arithmetic: no HAL, host-testable.
 */

#ifndef ESWCAP_RC_FIT_H
#define ESWCAP_RC_FIT_H

#include <stdbool.h>
#include "eswcap.h"

/** Fit sampled points.
 *  @param t_us     sample times, microseconds, monotonic (origin arbitrary)
 *  @param v_mv     calibrated sample voltages, millivolts
 *  @param n        number of points
 *  @param v0_mv    node voltage at the step edge
 *  @param v_inf_hint measured asymptote hint in mV, or <=0 if unknown
 *  @return rc_fit_t; ok is false when the fit is unusable. */
rc_fit_t rc_fit_samples(const double *t_us, const double *v_mv, int n,
                        double v0_mv, double v_inf_hint);

/** Convenience wrapper for hardware-timed uniform sampling: point i is at
 *  i * dt_us. */
rc_fit_t rc_fit_uniform(const double *v_mv, int n, double dt_us,
                        double v0_mv, double v_inf_hint);

/** Solve a 3x3 system by Gauss-Jordan with partial pivoting. Exposed for
 *  tests. Returns false if singular. */
bool rc_solve3(double A[3][3], const double b[3], double x[3]);

#endif /* ESWCAP_RC_FIT_H */
