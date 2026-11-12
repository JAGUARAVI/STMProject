/**
 * @file  bsp_dac.h
 * @brief Internal DAC references for COMP1 and COMP4.
 *
 * DAC3_CH1 -> COMP1_INM : C-mode relaxation-oscillator threshold (1.65 V)
 * DAC1_CH1 -> COMP4_INM : R-mode V_N overvoltage threshold (~2.6 V)
 *
 * Both channels are internal-only and unbuffered, so they cost no I/O pins and
 * are not exposed to board noise. DAC3 has no output pin on this package;
 * DAC1_OUT1 is on PA4 but is deliberately not connected.
 */

#ifndef ESWCAP_BSP_DAC_H
#define ESWCAP_BSP_DAC_H

#include <stdint.h>
#include <stdbool.h>

/* DAC3 channel 1: COMP1 threshold. */
bool bsp_dac3_init(void);
void bsp_dac3_set_mv(uint16_t mv);
uint16_t bsp_dac3_get_mv(void);
void bsp_dac3_enable(bool enable);

/* DAC1 channel 1: COMP4 V_N overvoltage threshold. */
bool bsp_dac1_init(void);
void bsp_dac1_set_mv(uint16_t mv);
void bsp_dac1_enable(bool enable);

/** Reference used for scaling. The DAC and the ADC share VREF+, so this tracks
 *  the measured VDDA when available and falls back to 3300 mV. */
uint32_t bsp_dac3_vref_mv(void);

#endif /* ESWCAP_BSP_DAC_H */
