/**
 * @file  bsp_comp.h
 * @brief Comparator oscillator: COMP1 + DAC3 threshold + TIM2 input capture.
 *
 * Replaces the external LM393 and the ESP32 software ISR/PCNT loop.
 *
 *  - Threshold: DAC3_CH1 drives COMP1's inverting input internally.
 *    DAC3 has no pin on this package, so the threshold costs no I/O.
 *  - Capture: COMP1_OUT is routed *internally* to TIM2_CH1; TIM2 is 32-bit,
 *    so at 170 MHz it gives 5.88 ns resolution with a 25.3 s wrap and needs no
 *    overflow bookkeeping across the 10 Hz .. 1 MHz band.
 *  - Loop closure: the oscillator is a relaxation loop, so the comparator
 *    output must drive the excitation buffer. PA0 is both COMP1_OUT (AF8) and
 *    the buffer input (PA8 cannot be: its AF8 is COMP7_OUT), so the loop is closed in hardware with no CPU in it.
 *    The same pin is switched to GPIO for the RC-step method.
 */

#ifndef ESWCAP_BSP_COMP_H
#define ESWCAP_BSP_COMP_H

#include <stdint.h>
#include <stdbool.h>

/** Initialise COMP1, DAC3_CH1 and the TIM2 capture channel (stopped). */
bool comp_init(void);

/** Set the comparator threshold in millivolts (0..VREF). */
void comp_set_threshold_mv(uint16_t mv);
uint16_t comp_threshold_mv(void);

/** Enable/disable the comparator (disabled = low power). */
void comp_enable(bool enable);
bool comp_enabled(void);

/** Instantaneous comparator output level. */
bool comp_output(void);

/* ------------------------------------------------------------------------- */
/* Relaxation-loop drive                                                     */
/* ------------------------------------------------------------------------- */

/** Start mirroring COMP1 to the drive pin and begin capturing periods. */
void comp_loop_start(void);
/** Stop the mirror and the capture. */
void comp_loop_stop(void);

/* ------------------------------------------------------------------------- */
/* Period measurement                                                        */
/* ------------------------------------------------------------------------- */

/** Capture periods until n_target edges are seen or timeout expires.
 *  @param n_target    desired number of complete periods (capped)
 *  @param timeout_ms  give up after this long
 *  @param period_us   receives the mean period
 *  @param n_periods   receives the number of periods averaged
 *  @param freq_hz     receives the mean frequency
 *  @return true on success. */
bool comp_measure_period(uint32_t n_target, uint32_t timeout_ms,
                         double *period_us, uint32_t *n_periods,
                         double *freq_hz);

/** Number of edges dropped because capture could not keep up. Diagnostics. */
uint32_t comp_capture_overruns(void);

/* ------------------------------------------------------------------------- */
/* COMP4: R-mode V_N overvoltage guard                                       */
/* ------------------------------------------------------------------------- */

/** Configure COMP4 (INP = PB0/V_N, INM = DAC1_CH1) and its EXTI line. */
bool bsp_comp4_init(void);

/** Arm/disarm the overvoltage guard. The ISR calls fe_r_fault(). */
void bsp_comp4_enable(bool enable);
bool bsp_comp4_enabled(void);

#endif /* ESWCAP_BSP_COMP_H */
