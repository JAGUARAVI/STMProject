/**
 * @file  osc_measure.h
 * @brief Turns a measured oscillator period into a scored sample.
 *
 * Pure arithmetic: no HAL, host-testable.
 */

#ifndef ESWCAP_OSC_MEASURE_H
#define ESWCAP_OSC_MEASURE_H

#include "eswcap.h"

/** Build a sample from a hardware-captured period.
 *  @param range_idx  range used
 *  @param period_us  mean full period
 *  @param n_periods  periods averaged
 *  @param k          geometry constant for this range
 *  @param t0_us      open-node tare (ignored when has_t0 is false)
 *  @param has_t0     whether the tare is valid
 *  @param delay_us   legacy per-cycle delay when no tare exists
 *  @param r_ohm      range resistance
 *  @param c_block_f  DC-blocking capacitance
 */
sample_t osc_measure_sample(uint8_t range_idx, double period_us,
                            uint32_t n_periods,
                            double k, double t0_us, bool has_t0,
                            double delay_us, double r_ohm, double c_block_f);

#endif /* ESWCAP_OSC_MEASURE_H */
