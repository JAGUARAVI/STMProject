/**
 * @file  bsp_frontend.h
 * @brief Front-end safety state machine, relay/mux control and mode interlock.
 *
 * Two different front ends share this module:
 *
 *  C mode (always available)
 *      V_cap -> C_block -> N_DUT -> DUT -> B -> GND
 *      SSR1 precharges N_DUT, SSR2 discharges it, SSR3 clamps V_cap.  SSR1
 *      and SSR3 share one pin; that is the hardware interlock and is not a
 *      bug.  A C-mode measurement NEVER closes SSR2: the DUT stays biased
 *      through R_bias while only the MCU-side node is reset.
 *
 *  R mode (only when ESWCAP_HAS_RMODE = 1)
 *      +3.3 V -> Q1 -> N_DUT -> DUT -> B -> K2A -> R mux -> RSENSE -> GND
 *      K1 lifts B and isolates C_block. K2 connects B to the R mux and opens
 *      the TIA feedback. The LMC6482 A is the TIA; OPAMP1 is the Kelvin error
 *      amplifier.
 *
 * Interlocks:
 *   1. K2 must never be energised unless K1 is energised.
 *   2. K1 must never be energised while B is driven; Q1 off first.
 *   3. K1B is normally closed, so never DE-energise K1 while N_DUT is biased.
 *   4. The R mux must be disabled in TIA; K2A is open.
 *   5. The C mux must be disabled in R mode.
 *   6. SSR2 is never closed during a C-mode measurement. It is the discharge
 *      path and is used only outside a measurement.
 */

#ifndef ESWCAP_BSP_FRONTEND_H
#define ESWCAP_BSP_FRONTEND_H

#include <stdint.h>
#include <stdbool.h>
#include "eswcap.h"

typedef enum {
    OP_MODE_CAP = 0,
    OP_MODE_RES = 1,
} op_mode_t;

/** Configure all front-end GPIO as safe outputs (all off). */
bool fe_init(void);

/** Raw C-mode control. `fe_set_drive` is only valid when the comparator loop
 *  is stopped: PA8 must be a GPIO at that point (bsp_comp.c owns the switch). */
void fe_set_drive(bool high);
void fe_select_range(uint8_t range_idx);
uint8_t fe_selected_range(void);

/* C-mode lifecycle. */
void fe_idle(void);
void fe_precharge(void);
void fe_discharge(void);
void fe_prepare_measurement(bool auto_precharge);
void fe_finish(bool auto_discharge, bool charged);

/* State and telemetry. */
fe_state_t fe_state(void);
void       fe_set_state(fe_state_t s);
bool       fe_charged(void);
bool       fe_discharged(void);
void       fe_set_charged(bool charged);

/** Bitmask of front-end levels for telemetry:
 *  bit0 SSR13, bit1 SSR2, bit2 drive, bit3 K1, bit4 K2,
 *  bit5 R mux enabled, bit6 C mux enabled, bit7 R_FORCE low. */
uint8_t fe_pin_bits(void);

/* Mode interlock. */
bool      fe_set_mode(op_mode_t mode);
op_mode_t fe_mode(void);

/* ------------------------------------------------------------------------- */
/* R-mode control (only when ESWCAP_HAS_RMODE = 1)                           */
/* ------------------------------------------------------------------------- */

/** Enter R mode: discharge, lift B (K1), isolate C_block, start the V_N
 *  buffer. Refuses if the node is charged or the hardware is not fitted. */
bool fe_r_enter(void);

/** Select the Kelvin sense element and close the loop. K2 on, R mux on,
 *  OPAMP1 on. Caller must already be in R mode. */
bool fe_r_kelvin(uint8_t element);

/** Leave the Kelvin loop without leaving R mode (OPAMP1 off, K2 off, R mux
 *  off). Used before the TIA measurement. */
void fe_r_kelvin_off(void);

/** Switch to the TIA: K2 off (feedback closed), R mux off, Q1 fully on. */
bool fe_r_tia(void);

/** Leave R mode and return to the safe C-mode state. Discharges N_DUT. */
void fe_r_exit(void);

/** Emergency stop from the COMP4 overvoltage ISR: remove the force path
 *  immediately. The main loop must call fe_r_exit() afterwards. */
void fe_r_fault(void);

/** True while an R session is active (between enter and exit). */
bool fe_r_active(void);

/** True if the COMP4 overvoltage fault fired. */
bool fe_r_faulted(void);

#endif /* ESWCAP_BSP_FRONTEND_H */
