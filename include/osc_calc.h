/**
 * @file  osc_calc.h
 * @brief Relaxation-oscillator model:  T = K * R * C_dut + T0
 *
 * T0 is a *measured* open-node tare that absorbs the node parasitic
 * capacitance and any fixed loop latency. Because the G474 captures periods in
 * hardware, T0 no longer has to absorb CPU jitter, so it should be smaller and
 * more repeatable than on the ESP32.
 *
 * Pure arithmetic: no HAL, host-testable.
 */

#ifndef ESWCAP_OSC_CALC_H
#define ESWCAP_OSC_CALC_H

#include <stdbool.h>
#include "eswcap.h"

/** Series-equivalent capacitance of the DUT through the DC-blocking cap.
 *  (Declared in eswcap.h; repeated here for discoverability.) */

/** C_eq for a measured period, given the tare and geometry. */
double osc_c_eq(double period_us, double t0_us, double k, double r_ohm);

/** DUT capacitance from C_eq after inverting the C_block series combination.
 *  Returns a negative value when non-physical. */
double osc_c_dut(double c_eq_f, double c_block_f, bool tared);

/** Solve K from a single known reference (with a valid tare). */
double osc_k_from_ref(double period_us, double t0_us, double r_ohm,
                      double c_ref_f, double c_block_f);

/** Solve K and T0 from two known references.
 *  @return false if the two points are too close or produce an unphysical K. */
bool osc_solve_two_point(double p1_us, double c1_f,
                         double p2_us, double c2_f,
                         double r_ohm, double c_block_f,
                         double *k_out, double *t0_out);

/** Quality score for an oscillator sample.
 *  @param freq_hz    measured frequency
 *  @param n_periods  periods averaged
 *  @param tared      true when a tare T0 exists
 *  @param c_f        reconstructed capacitance
 */
double osc_quality(double freq_hz, uint32_t n_periods, bool tared, double c_f);

#endif /* ESWCAP_OSC_CALC_H */
