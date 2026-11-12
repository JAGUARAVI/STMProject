/**
 * @file  bsp_led.h
 * @brief Status LED.
 *
 * The WeAct board has a single monochrome LED on PC6 (active high), not the
 * ESP32 board's WS2812. The RGB/range-colour semantics of the original
 * firmware are mapped onto brightness and pattern so the front-panel language
 * survives:
 *
 *   idle / safe to touch   -> slow breathe, dim
 *   precharge              -> medium pulse
 *   biased (do not touch)  -> steady bright
 *   measuring              -> fast blink, brightness = range index
 *   discharge              -> short pulse
 *   result flash           -> 1-2 quick flashes
 *   fault                  -> rapid blink
 */

#ifndef ESWCAP_BSP_LED_H
#define ESWCAP_BSP_LED_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_IDLE,
    LED_MODE_PRECHARGE,
    LED_MODE_BIASED,
    LED_MODE_MEASURING,
    LED_MODE_DISCHARGE,
    LED_MODE_FAULT,
} led_mode_t;

/** Configure PC6 as a push-pull output, LED off. */
void led_init(void);

/** Advance the animation. Call at ~1 kHz (or as often as convenient) with the
 *  current millisecond tick. */
void led_tick(uint32_t now_ms);

/** Select an animated mode. */
void led_set_mode(led_mode_t mode, uint8_t range_idx);

/** Hold a fixed brightness (0..255). */
void led_manual(uint8_t brightness);

/** Return to state-driven animation. */
void led_set_auto(void);

/** Blank the LED. */
void led_off(void);

/** Flash the result of a cycle: clean / low-confidence / rejected. */
void led_report_result(bool clean, bool low_confidence, bool rejected);

#endif /* ESWCAP_BSP_LED_H */
