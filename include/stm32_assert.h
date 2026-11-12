/**
 * @file      stm32_assert.h
 * @brief     Assert macro used by the STM32Cube HAL/LL drivers when
 *            USE_FULL_ASSERT is defined.
 *
 * The Cube G4 package ships Drivers/STM32G4xx_HAL_Driver/Inc/stm32_assert_template.h
 * and expects the project to provide this header. Defining USE_FULL_ASSERT
 * without it breaks the build (the LL drivers include it unconditionally).
 *
 * Keep this in include/ so the assert is available to the whole firmware.
 */

#ifndef __STM32_ASSERT_H
#define __STM32_ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Exported by src/main.c. Traps the CPU so a debugger can recover the call
 * site; in a release build the watchdog turns the trap into a reset. */
void assert_failed(uint8_t *file, uint32_t line);

#ifdef USE_FULL_ASSERT
/**
 * @brief Assert function used by the STM32Cube HAL/LL drivers.
 *
 * @param expr  Expression to evaluate.
 * @param file  Source file name, from __FILE__.
 * @param line  Source line number, from __LINE__.
 */
#define assert_param(expr) \
    ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
#else
#define assert_param(expr) ((void)0U)
#endif /* USE_FULL_ASSERT */

#ifdef __cplusplus
}
#endif

#endif /* __STM32_ASSERT_H */