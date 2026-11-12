/**
 * @file      main.c
 * @brief     ESWCap entry point.
 *
 * Target : STM32G474CEU6, UFQFPN48, Cortex-M4F @ 170 MHz, 512 KB flash, 128 KB RAM
 * Board  : WeAct Studio STM32G474CEU6 "Long"
 * Console: USART1 115200 8N1, PA9 = TX, PA10 = RX
 *
 * There is deliberately almost nothing here. Clock setup, peripheral bring-up
 * and the measurement loop live in the BSP and application modules so they can
 * be read and tested independently; `main` only hands control to them.
 */

#include "app.h"

#include "stm32g4xx_hal.h"
#include "bsp_console.h"

int main(void)
{
    app_init();
    app_run();

    /* app_run() never returns. */
    return 0;
}

#ifdef USE_FULL_ASSERT
/**
 * @brief STM32Cube assertion hook, active in debug builds.
 *
 * Report the failing site on the console before trapping: a bare infinite loop
 * on an assertion is indistinguishable from a hang, and the console is the only
 * diagnostic channel this board has without a probe attached.
 */
void assert_failed(uint8_t *file, uint32_t line)
{
    console_printf("\r\nASSERT FAILED: %s:%lu\r\n", (const char *)file,
                   (unsigned long)line);
    __disable_irq();
    for (;;)
    {
    }
}
#endif
