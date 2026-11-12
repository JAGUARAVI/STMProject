/**
 * @file  bsp_opamp3.h
 * @brief OPAMP3 as the unity-gain V_N buffer.
 *
 * The Kelvin top node is sensed through 1 kohm + 1 Mohm and a BAT54S clamp.
 * That source impedance is far too high for the ADC sample-and-hold, so
 * OPAMP3 buffers it:
 *
 *   VINP <- PB0 = V_N_SENSE
 *   VOUT -> PB1 (also ADC3_IN1)
 *   VINM -> internally tied to VOUT in follower mode; the PB2 pin is NOT
 *           used because it is the R-mux enable output
 *
 * The output is taken from the pin, not the internal ADC channel. PB1 is
 * bonded and ADC3_IN1 is documented, so this avoids relying on an internal
 * OPAMP-to-ADC route that the HAL does not describe.
 */

#ifndef ESWCAP_BSP_OPAMP3_H
#define ESWCAP_BSP_OPAMP3_H

#include <stdbool.h>

/** Configure OPAMP3 as a unity-gain follower on PB0/PB1. */
bool bsp_opamp3_init(void);

/** Enable (drives PB1) or disable (high-Z) the buffer. */
void bsp_opamp3_enable(bool enable);

bool bsp_opamp3_enabled(void);

#endif /* ESWCAP_BSP_OPAMP3_H */
