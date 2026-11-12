/**
 * @file  bsp_adc.h
 * @brief ADC acquisition.
 *
 * Two independent paths, deliberately on different ADC instances so C-mode
 * streaming and R-mode single reads can never disturb each other:
 *
 *  - Stream (C-mode): ADC1, single channel V_cap, TIM6-triggered, circular
 *    DMA, hardware oversampling. The timer counter is reset at the excitation
 *    edge, so sample n occurs at exactly n * period; no timestamps are
 *    recorded.
 *  - Read (R-mode): ADC2 for V_TIA/V_BIAS, ADC3 for V_N, ADC4 for V_B. Each is
 *    a software-triggered single conversion with a dummy-conversion discard
 *    (errata ES0430 2.7.2 / 2.7.3). ADC3 also reads VREFINT for VDDA.
 */

#ifndef ESWCAP_BSP_ADC_H
#define ESWCAP_BSP_ADC_H

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------------- */
/* Streaming path (ADC1)                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    const uint16_t *buf;          /**< ring buffer, ADC_STREAM_CAPACITY entries */
    uint32_t        capacity;
    volatile uint32_t total;      /**< samples written since start (free-running) */
    volatile uint32_t overruns;
    uint32_t        sample_period_ns;
    uint8_t         os_ratio;
} adc_stream_info_t;

/** Initialise ADC1 + TIM6 + DMA in the stopped state. */
bool adc_init(void);

/** Start a continuous, timer-paced stream.
 *  @param os_ratio  hardware oversampling ratio (1,2,4,...,256)
 *  @param period_ns output sample period in nanoseconds (>= 1 us)
 *  @return false if the requested period cannot fit the conversion time. */
bool adc_stream_start(uint8_t os_ratio, uint32_t period_ns);
void adc_stream_stop(void);

const adc_stream_info_t *adc_stream_info(void);

/** Reset the trigger phase so that sample 0 is taken at the next trigger
 *  after this call. Used at the excitation edge. */
void adc_stream_sync(void);

/** Copy up to max samples that have arrived since the last call.
 *  @return number copied, or -1 on overrun. */
int adc_stream_take(uint16_t *dst, uint32_t max);

/** Discard everything currently buffered. */
void adc_stream_flush(void);

/** Convert a raw oversampled code to millivolts using VREF (mV) and the
 *  configured ratio/shift. Purely arithmetic; no hardware access. */
int adc_code_to_mv(uint16_t code, uint8_t os_ratio, uint32_t vref_mv);

/** Measured VDDA in millivolts, derived from VREFINT against the factory
 *  calibration word. Returns 0 if unavailable. */
uint32_t adc_vdda_mv(void);

/* ------------------------------------------------------------------------- */
/* Single-read paths (ADC2 / ADC3 / ADC4)                                    */
/* ------------------------------------------------------------------------- */

/** Averaged single-channel read on ADC2 (V_TIA / V_BIAS / B).
 *  @param channel  ADC_CHANNEL_x (must be ADC2-capable)
 *  @param os_ratio 1..256
 *  @return oversampled code, or -1 on failure/timeout. */
int32_t adc_read(uint32_t channel, uint8_t os_ratio);

/** Single-channel read on ADC3 (V_N, and VREFINT internally). */
int32_t adc_read_adc3(uint32_t channel, uint8_t os_ratio);

/** Single-channel read on ADC4 (V_B). */
int32_t adc_read_adc4(uint32_t channel, uint8_t os_ratio);

/** Initialise the ADC2 and ADC4 single-read paths. */
bool adc_read_init(void);

#endif /* ESWCAP_BSP_ADC_H */
