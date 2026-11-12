/**
 * @file  bsp.h
 * @brief System services: clocks, time base, delays, watchdog.
 */

#ifndef ESWCAP_BSP_H
#define ESWCAP_BSP_H

#include <stdint.h>
#include <stdbool.h>

/** Configure 170 MHz SYSCLK from the 8 MHz HSE, SysTick at 1 kHz, DWT cycle
 *  counter, and start the independent watchdog. Call once from main. */
bool bsp_init(void);

/** Milliseconds since boot (SysTick, wraps at 2^32). */
uint32_t bsp_millis(void);

void bsp_delay_ms(uint32_t ms);
void bsp_delay_us(uint32_t us);

/** Kick the independent watchdog. */
void bsp_iwdg_refresh(void);

/** Free-running CPU cycle counter (DWT). Wraps at 2^32. */
uint32_t bsp_cycles(void);
uint32_t bsp_cycles_per_us(void);

/** 48 MHz USB clock is only required by the DFU bootloader, not the app.
 *  Provided so main can optionally bring USB up. */
void bsp_usb_clock_enable(void);

#endif /* ESWCAP_BSP_H */
