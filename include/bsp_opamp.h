/**
 * @file  bsp_opamp.h
 * @brief OPAMP1 as the R-mode current-source error amplifier.
 *
 * Routing (all internal except the output):
 *
 *   VINP <- PA7 = B        (Kelvin sense node)
 *   VINM <- PA3 = VREF     (0.405 V setpoint)
 *   VOUT -> PA2            (drives the external P-FET gate)
 *
 * Why this polarity: Q1 is a high-side P-FET whose gate is pulled up to +3.3 V
 * (off) by R13 and driven low (on) by the op-amp. If B rises, the op-amp output
 * must rise, turning Q1 off and pulling B back down. That is negative feedback
 * only when B is on the **non-inverting** input and VREF on the inverting one.
 * The earlier VINP=DAC / VINM=B arrangement was positive feedback.
 *
 * OPAMP2 cannot be used here: its VINP is fixed at PA7 (B) and its VINM at PA5
 * (VBIAS), which is the opposite polarity, and it has no DAC path on G4.
 *
 * Disabling the OPAMP drives its output high-Z, which is what the mode
 * interlock relies on to keep the C-mode front end unloaded.
 */

#ifndef ESWCAP_BSP_OPAMP_H
#define ESWCAP_BSP_OPAMP_H

#include <stdbool.h>

/** Configure OPAMP1 in standalone mode with B on VINP and VREF on VINM. */
bool bsp_opamp1_init(void);

/** Enable (drives PA2) or disable (high-Z) the op-amp. */
void bsp_opamp1_enable(bool enable);

bool bsp_opamp1_enabled(void);

/** Run the offset self-calibration. Must be called with the output enabled and
 *  no external load driving the pin. Returns false if it times out. */
bool bsp_opamp1_calibrate(void);

#endif /* ESWCAP_BSP_OPAMP_H */
