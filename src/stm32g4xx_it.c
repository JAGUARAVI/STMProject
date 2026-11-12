/**
 * @file      stm32g4xx_it.c
 * @brief     Interrupt service routines for the ESWCap STM32G474 build.
 *
 * SCAFFOLD. Contains only the Cortex-M core handlers required to boot plus the
 * SysTick handler STM32Cube HAL expects. Peripheral ISRs (notably the LM393
 * edge capture, which replaces the ESP32 ISR + PCNT oscillator loop) are
 * deliberately absent until the measurement code exists.
 */

#include "stm32g4xx_hal.h"

/* ------------------------------------------------------------------------- */
/* Cortex-M4 core handlers                                                   */
/* ------------------------------------------------------------------------- */

/**
 * @brief Non-maskable interrupt.
 */
void NMI_Handler(void)
{
}

/**
 * @brief Hard fault.
 *
 * Trap in a loop so a debugger can recover the faulting context. A production
 * build should capture the stacked registers and reset, or latch a fault code
 * the console can report.
 */
void HardFault_Handler(void)
{
    while (1)
    {
    }
}

/**
 * @brief Memory management fault.
 */
void MemManage_Handler(void)
{
    while (1)
    {
    }
}

/**
 * @brief Bus fault.
 */
void BusFault_Handler(void)
{
    while (1)
    {
    }
}

/**
 * @brief Usage fault.
 */
void UsageFault_Handler(void)
{
    while (1)
    {
    }
}

/**
 * @brief Supervisor call.
 */
void SVC_Handler(void)
{
}

/**
 * @brief Debug monitor.
 */
void DebugMon_Handler(void)
{
}

/**
 * @brief PendSV.
 *
 * Reserved for a future RTOS port; unused in the scaffold.
 */
void PendSV_Handler(void)
{
}

/**
 * @brief System tick.
 *
 * Drives HAL_Delay() and HAL_GetTick(). The real firmware will use this as the
 * coarse timebase while hardware timers provide the measurement timing.
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}