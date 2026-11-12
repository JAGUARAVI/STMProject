/**
 * @file      stm32g4xx_hal_msp.c
 * @brief     HAL MSP (MCU Support Package) callbacks for the ESWCap G474 build.
 *
 * SCAFFOLD. Provides the minimal per-peripheral initialisation HAL requires —
 * in particular SysTick, which HAL_Init() configures through this file. Pin
 * muxing for the measurement front-end lives here once it exists.
 */

#include "stm32g4xx_hal.h"

/**
 * @brief Initialise the MCU support package.
 *
 * Called once by HAL_Init(). Enables the system clocks HAL needs and sets the
 * SysTick priority and 1 ms tick.
 */
void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();

    /* Remap the SysTick interrupt priority to the lowest (15) so a future
     * measurement ISR can preempt it, and give HAL a 1 ms tick. */
    HAL_NVIC_SetPriority(SysTick_IRQn, 15U, 0U);
}