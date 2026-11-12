/**
 * @file  rmode.h
 * @brief 4-wire Kelvin / TIA resistance measurement (R-mode).
 *
 * Topology (docs/resistance-autoranging-plan.md, confirmed against the ESP32
 * combined-circuit netlist):
 *
 *   +3.3 V -> Q1 -> N_DUT -> DUT -> B -> K2A -> R mux -> RSENSE -> GND
 *
 * N_DUT is the force node; B is the sense node. C_block is between V_cap and
 * N_DUT and is isolated by K1B in R mode, because it is an open circuit at DC
 * and would otherwise sit in series with the force path.
 *
 * Kelvin:
 *   V_B is servoed to VREF by OPAMP1 (VINP = B, VINM = VREF, VOUT = Q1 gate).
 *   I = VREF / RSENSE, so
 *       R_dut = RSENSE_eff * (V_N - V_B) / V_B
 *   The setpoint, DAC gain and op-amp offset cancel in the ratio. RSENSE_eff
 *   (including mux/relay Ron) and the V_N/V_B channel gains do not.
 *
 * TIA (LMC6482 A, external):
 *   IN+ = VBIAS, IN- = B, OUT = V_TIA, feedback Rf/Cf.
 *       R_dut = Rf * (V_N - VBIAS) / (VBIAS - V_TIA)
 *   Used above the top Kelvin element (~22 Mohm).
 */

#ifndef ESWCAP_RMODE_H
#define ESWCAP_RMODE_H

#include "eswcap.h"

/** Configure the R-mode analog blocks (OPAMP1, OPAMP3, COMP4). Returns true
 *  when the feature is compiled out; rmode_enter() still refuses. */
bool rmode_init(void);

/** Enter/leave R-mode. Entering requires a discharged node. */
bool rmode_enter(void);
void rmode_leave(void);
bool rmode_active(void);

/** Select the sense-resistor element (0..3) used by rmode_measure(). */
bool rmode_select_element(uint8_t element);

/** Measure once on the selected element (Kelvin). */
bool rmode_measure(res_sample_t *out);

/** Autoranged measurement: up/down Kelvin ladder, then TIA above ~22 Mohm. */
bool rmode_measure_auto(res_sample_t *out);

/* Per-element Kelvin calibration. */
double rmode_rref(uint8_t element);
void   rmode_set_rref(uint8_t element, double ohm);
bool   rmode_cal_valid(uint8_t element);
void   rmode_set_cal(uint8_t element, double rref, double gain, double offset,
                     bool valid);

/* TIA calibration. */
double rmode_tia_rf(void);
void   rmode_set_tia(double rf, double vbias_mv, double gain, bool valid);
bool   rmode_tia_valid(void);

/* Pure maths, exposed for host tests. */
double rmode_resistance(double rref_ohm, double vn_mv, double vb_mv);
double rmode_tia_resistance(double rf_ohm, double vn_mv, double vbias_mv,
                            double vtia_mv);
double rmode_current_ua(double rref_ohm, double vb_mv);

#endif /* ESWCAP_RMODE_H */
