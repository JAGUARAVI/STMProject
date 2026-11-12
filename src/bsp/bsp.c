/**
 * @file      bsp.c
 * @brief     Board support for the ESWCap STM32G474CEU6 build: clock tree,
 *            time base, DWT cycle counter and independent watchdog.
 *
 * @section clock_tree 170 MHz clock arithmetic
 *
 * The board has an 8 MHz crystal on PF0/PF1 (HSE). The main PLL is used as
 * follows (STM32G4 PLL: VCO = HSE / PLLM * PLLN, SYSCLK = VCO / PLLR):
 *
 *     PLL input = HSE / PLLM = 8 MHz / 2  =   4 MHz   (legal: 2.66..8 MHz)
 *     VCO       = 4 MHz * PLLN = 4 MHz * 85 = 340 MHz (legal: 96..344 MHz)
 *     SYSCLK    = VCO / PLLR = 340 MHz / 2 = 170 MHz
 *     PLLP      = VCO / 2 = 170 MHz  (ADC kernel output; unused here)
 *     PLLQ      = VCO / 2 = 170 MHz  (cannot produce 48 MHz; USB uses HSI48)
 *
 * Note the naming trap on G4: PLLP is an ADC-clocking output, not the system
 * clock. Unlike F-series parts, the CPU clock is PLLR, so PLLR must be 2.
 * PLLP and PLLQ are still programmed to legal values because the HAL rejects
 * zero/invalid dividers, and leaving them at 170 MHz keeps every output
 * within the 170 MHz limit.
 *
 * 170 MHz is only legal with the internal regulator in Range 1 *boost*
 * (1.28 V). In plain Range 1 (1.2 V) the maximum is 150 MHz, and in Range 2
 * (1.0 V) only 26 MHz. The boost bit (PWR_CR5.R1MODE = 0) must be set before
 * SYSCLK is raised, hence the ordering in bsp_init().
 *
 * At 170 MHz the flash needs 4 wait states in boost mode. The HAL programs
 * the latency before it switches SYSCLK, so the core never runs faster than
 * the flash can deliver instructions. Prefetch plus the instruction/data
 * caches then hide most of the 4-wait-state penalty; this build starts with
 * PREFETCH_ENABLE == 0, so all three are enabled explicitly here.
 *
 * @section usb_clock Why USB does not use the PLL
 *
 * With a 4 MHz PLL input and a 340 MHz VCO, the closest integer PLLQ divider
 * to 48 MHz is 7, which gives 48.571 MHz — outside the USB full-speed
 * tolerance. There is no combination in this clock plan that yields exactly
 * 48 MHz without disturbing the 170 MHz SYSCLK, so USB is clocked from the
 * dedicated HSI48 oscillator instead (bsp_usb_clock_enable()).
 */

#include "bsp.h"
#include "config.h"

/* ------------------------------------------------------------------------- */
/* Private constants                                                         */
/* ------------------------------------------------------------------------- */

/** Expected SYSCLK after bsp_init(), used as a post-condition check. */
#define BSP_SYSCLK_HZ               170000000U

/** Bounded wait for HSI48 to settle (nominal startup is a few microseconds). */
#define BSP_HSI48_READY_TIMEOUT_MS  2U

/**
 * HSI48 smooth-trimming starting value.
 *
 * The CRS TRIM field is 7 bits wide and 64 (0x40) is mid-scale, but this
 * CubeG4 HAL's IS_RCC_CRS_HSI48CALIBRATION() assert only accepts 0..0x3F.
 * Starting one LSB below mid-scale keeps the assert-enabled debug build
 * working; the CRS auto-trim loop removes the residual error once USB SOF
 * events arrive.
 */
#define BSP_CRS_HSI48_TRIM          0x3FU

/* ------------------------------------------------------------------------- */
/* Private state                                                             */
/* ------------------------------------------------------------------------- */

/** Watchdog handle, refreshed by bsp_iwdg_refresh(). */
static IWDG_HandleTypeDef s_iwdg;

/* ------------------------------------------------------------------------- */
/* Private helpers                                                           */
/* ------------------------------------------------------------------------- */

/**
 * @brief Configure HSE, the main PLL, the bus dividers and the flash wait
 *        states for 170 MHz SYSCLK.
 *
 * Split out of bsp_init() so the clock path has a single failure exit and the
 * ordering (oscillator -> flash latency -> SYSCLK switch -> caches) is
 * visible in one place.
 *
 * @return true if SYSCLK is 170 MHz with 4 flash wait states, false otherwise.
 */
static bool bsp_clock_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    /* HSE from the 8 MHz crystal, then the PLL as described in the file
     * header. 4 MHz PLL input, 340 MHz VCO, 170 MHz on every PLL output. */
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = RCC_PLLM_DIV2;   /* 8 MHz / 2 = 4 MHz */
    osc.PLL.PLLN = 85U;             /* 4 MHz * 85 = 340 MHz VCO */
    osc.PLL.PLLP = RCC_PLLP_DIV2;   /* 340 MHz / 2 = 170 MHz (ADC output) */
    osc.PLL.PLLQ = RCC_PLLQ_DIV2;   /* 340 MHz / 2 = 170 MHz, not 48 MHz */
    osc.PLL.PLLR = RCC_PLLR_DIV2;   /* 340 MHz / 2 = 170 MHz SYSCLK */

    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        return false;
    }

    /* AHB, APB1 and APB2 all run at 170 MHz. The measurement front end needs
     * the full timer resolution (5.88 ns per TIM tick), and the ADC kernel
     * clock is derived from SYSCLK (see bsp_adc_clock_init()). */
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;

    /* 4 wait states in Range 1 boost at 170 MHz. HAL_RCC_ClockConfig() raises
     * the latency before switching SYSCLK and restarts SysTick afterwards, so
     * the 1 ms tick is based on the final 170 MHz HCLK. */
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK)
    {
        return false;
    }

    if (__HAL_FLASH_GET_LATENCY() != FLASH_LATENCY_4)
    {
        return false;
    }

    /* Caches are free performance at 170 MHz: they absorb most instruction
     * fetches that would otherwise stall for the 4 programmed wait states. */
    __HAL_FLASH_INSTRUCTION_CACHE_ENABLE();
    __HAL_FLASH_DATA_CACHE_ENABLE();
    __HAL_FLASH_PREFETCH_BUFFER_ENABLE();

    /* Post-condition: catch a PLL that silently came up on the wrong source
     * or divider instead of running the whole application with bad timing. */
    if (HAL_RCC_GetSysClockFreq() != BSP_SYSCLK_HZ)
    {
        return false;
    }

    return true;
}

/**
 * @brief Start the independent watchdog.
 *
 * The IWDG runs from the LSI (~32 kHz), not from SYSCLK, so it still resets
 * the MCU if the PLL or HSE fails. With /32 prescaling the counter ticks at
 * about 1 kHz; a 4095 reload therefore gives (4095 + 1) / 1000 = 4.096 s.
 * LSI tolerance makes that approximate — the watchdog is a safety net, not a
 * timebase.
 *
 * Window mode is explicitly disabled: the zero-initialised HAL handle would
 * otherwise leave WINR at 0, which forbids refreshing before the counter
 * reaches zero and would reset the board on the first kick.
 *
 * @return true if the watchdog started and was refreshed once, false on a
 *         register-update timeout.
 */
static bool bsp_iwdg_start(void)
{
    s_iwdg.Instance = IWDG;
    s_iwdg.Init.Prescaler = IWDG_PRESCALER;
    s_iwdg.Init.Reload = IWDG_RELOAD;
    s_iwdg.Init.Window = IWDG_WINDOW_DISABLE;

    if (HAL_IWDG_Init(&s_iwdg) != HAL_OK)
    {
        return false;
    }

    /* Start the application with the full timeout ahead of it. */
    return HAL_IWDG_Refresh(&s_iwdg) == HAL_OK;
}

/**
 * @brief Route SYSCLK to the ADC12 and ADC345 kernel clock inputs.
 *
 * The ADC's own asynchronous prescaler is ADC_CLOCK_ASYNC_DIV4 (config.h),
 * so a 170 MHz kernel clock becomes 42.5 MHz. That is the fastest legal
 * single-ADC rate on this part (60 MHz limit; 52 MHz with concurrent ADCs).
 *
 * The ADC peripheral clocks themselves are deliberately not enabled here:
 * the ADC driver owns them and may keep them off while the front end is idle.
 *
 * @return true if the RCC extended-clock registers accepted the selection.
 */
static bool bsp_adc_clock_init(void)
{
    RCC_PeriphCLKInitTypeDef pclk = {0};

    pclk.PeriphClockSelection = RCC_PERIPHCLK_ADC12;
    pclk.Adc12ClockSelection = RCC_ADC12CLKSOURCE_SYSCLK;

    /* ADC3/4/5 exist on the G474; keep the guard so the same BSP still
     * compiles on a G4 part without the second ADC cluster. */
#if defined(ADC345_COMMON) && defined(RCC_PERIPHCLK_ADC345)
    pclk.PeriphClockSelection |= RCC_PERIPHCLK_ADC345;
    pclk.Adc345ClockSelection = RCC_ADC345CLKSOURCE_SYSCLK;
#endif

    return HAL_RCCEx_PeriphCLKConfig(&pclk) == HAL_OK;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief Bring the board up: clocks, timebase, DWT and watchdog.
 *
 * Call once from main() before any other BSP or peripheral function. The
 * order matters:
 *
 *   1. HAL_Init()                    HAL state + 1 ms tick on HSI
 *   2. boost regulator               Range 1 boost before exceeding 150 MHz
 *   3. HSE + PLL + flash latency     170 MHz SYSCLK, 4 wait states
 *   4. HAL_InitTick()                1 ms SysTick at the new HCLK
 *   5. DWT                           free-running cycle counter for us delays
 *   6. IWDG                          ~4 s safety net
 *   7. board clocks                  GPIOA/B/C/F, SYSCFG, PWR
 *   8. ADC kernel clock              SYSCLK -> ADC prescaler /4 = 42.5 MHz
 *
 * @return true on success. On any HAL failure the function returns false
 *         instead of hanging, leaving the caller to report or retry.
 */
bool bsp_init(void)
{
    if (HAL_Init() != HAL_OK)
    {
        return false;
    }

    /* The regulator must already be in boost mode before the PLL is allowed
     * anywhere near 170 MHz. PWR_CR1/PWR_CR5 are in the PWR clock domain, so
     * enable that clock first (HAL_MspInit() also does this, but the clock
     * path should not depend on MSP ordering). HAL_PWREx_ControlVoltageScaling()
     * clears PWR_CR5.R1MODE and waits for PWR_SR2.VOSF to clear, which is the
     * asynchronous regulator transition the reference manual requires. */
    __HAL_RCC_PWR_CLK_ENABLE();
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST) != HAL_OK)
    {
        return false;
    }

    if (!bsp_clock_init())
    {
        return false;
    }

    /* HAL_RCC_ClockConfig() has already restarted the tick at the new HCLK,
     * but configuring it explicitly documents the 1 ms contract and does not
     * rely on that side effect. TICK_INT_PRIORITY is 15 in this build, so a
     * future measurement ISR can preempt the tick. */
    if (HAL_InitTick(TICK_INT_PRIORITY) != HAL_OK)
    {
        return false;
    }

    /* DWT cycle counter: the only timebase accurate enough for microsecond
     * delays at 170 MHz (5.88 ns per cycle). TRCENA must be set before the
     * counter enable bit is written. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    if (!bsp_iwdg_start())
    {
        return false;
    }

    /* Board-level clocks only. USART/TIM/ADC/DAC/OPAMP drivers enable their
     * own peripheral clocks so they can also power them down; doing it here
     * would couple the BSP to every module's power policy. GPIOF carries the
     * HSE crystal, SYSCFG is needed for COMP/EXTI routing, and PWR is used
     * for the boost regulator above. */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();

    if (!bsp_adc_clock_init())
    {
        return false;
    }

    return true;
}

/**
 * @brief Milliseconds since boot.
 *
 * Backed by the SysTick interrupt installed in step 4 of bsp_init(). Wraps
 * naturally at 2^32 ms (~49.7 days); all interval arithmetic must use
 * unsigned subtraction so the wrap is transparent.
 */
uint32_t bsp_millis(void)
{
    return HAL_GetTick();
}

/**
 * @brief Blocking millisecond delay.
 *
 * HAL_Delay() is interrupt-driven by SysTick, so this yields to interrupts
 * and keeps the watchdog serviceable from an ISR or main loop. For the
 * microsecond-scale settling times used by the analogue front end, use
 * bsp_delay_us() instead.
 */
/**
 * @brief Blocking millisecond delay that keeps the watchdog fed.
 *
 * Long front-end holds (pre-charge and discharge are 2 s each) would otherwise
 * trip the ~4 s IWDG. The delay is served in 20 ms slices so the refresh is
 * frequent enough that the sum of several consecutive holds cannot reset the
 * board, while still leaving the watchdog meaningful for the measurement path:
 * a genuine hang outside a delay still trips it.
 */
void bsp_delay_ms(uint32_t ms)
{
    while (ms > 0U) {
        uint32_t slice = (ms > 20U) ? 20U : ms;
        HAL_Delay(slice);
        bsp_iwdg_refresh();
        ms -= slice;
    }
}

/**
 * @brief Blocking microsecond delay using the DWT cycle counter.
 *
 * At 170 MHz one microsecond is 170 CPU cycles, far too fine for the 1 ms
 * SysTick, hence the DWT. The loop compares unsigned differences, which makes
 * it correct across the 32-bit CYCCNT wrap. A zero delay returns immediately.
 *
 * The product is saturated instead of allowed to wrap: a wrapped count would
 * turn a long delay into an unexpectedly short one.
 */
void bsp_delay_us(uint32_t us)
{
    uint32_t cycles_per_us = SystemCoreClock / 1000000U;
    uint32_t cycles;
    uint32_t start;

    if ((us == 0U) || (cycles_per_us == 0U))
    {
        return;
    }

    if (us > (UINT32_MAX / cycles_per_us))
    {
        cycles = UINT32_MAX;
    }
    else
    {
        cycles = us * cycles_per_us;
    }

    /* bsp_init() arms the DWT. If this is called before init (or a debugger
     * cleared DEMCR), arm it locally rather than spin forever on a stopped
     * counter. */
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
    {
        CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
        DWT->CYCCNT = 0U;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }

    start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles)
    {
        /* Busy-wait: cycle accuracy is the requirement here, not low power. */
    }
}

/**
 * @brief Kick the independent watchdog.
 *
 * Must be called well inside the ~4 s window, including while the front end
 * is waiting for a long precharge or discharge. HAL_IWDG_Refresh() only writes
 * the reload key; the return value is ignored because there is no recovery
 * path if the watchdog has already started its reset sequence.
 */
void bsp_iwdg_refresh(void)
{
    /* s_iwdg.Instance is NULL until bsp_init() has run; HAL_IWDG_Refresh()
     * does not null-check, so a stray early call would fault. */
    if (s_iwdg.Instance != NULL)
    {
        (void)HAL_IWDG_Refresh(&s_iwdg);
    }
}

/**
 * @brief Free-running CPU cycle count.
 *
 * Wraps every 2^32 / 170e6 = 25.3 s at full speed. Use unsigned subtraction
 * for intervals, exactly as bsp_delay_us() does.
 */
uint32_t bsp_cycles(void)
{
    return DWT->CYCCNT;
}

/**
 * @brief CPU cycles per microsecond (170 at 170 MHz).
 *
 * Callers converting cycle counts to microseconds should divide by this value
 * rather than hard-code 170 so the code stays correct if the clock plan
 * changes.
 */
uint32_t bsp_cycles_per_us(void)
{
    return SystemCoreClock / 1000000U;
}

/**
 * @brief Best-effort 48 MHz USB clock: HSI48 routed through the CRS.
 *
 * The application does not need USB, but the DFU bootloader and any future
 * USB console do. Because the 170 MHz PLL cannot divide to exactly 48 MHz
 * (340/7 = 48.57 MHz), USB must use HSI48.
 *
 * This function has no return value by contract, so every failure is silent:
 * if HSI48 never becomes ready or RCC rejects the route, USB simply has no
 * clock. Nothing else in the firmware depends on this call.
 *
 * The CRS then trims HSI48 against the 1 kHz USB start-of-frame packets. With
 * no USB host attached there are no sync events; the oscillator keeps running
 * on its free-running trim, which is fine for a best-effort clock.
 */
void bsp_usb_clock_enable(void)
{
    RCC_PeriphCLKInitTypeDef pclk = {0};
    RCC_CRSInitTypeDef crs = {0};
    uint32_t tickstart;

    __HAL_RCC_HSI48_ENABLE();

    tickstart = HAL_GetTick();
    while (__HAL_RCC_GET_FLAG(RCC_FLAG_HSI48RDY) == 0U)
    {
        if ((HAL_GetTick() - tickstart) > BSP_HSI48_READY_TIMEOUT_MS)
        {
            return;
        }
    }

    pclk.PeriphClockSelection = RCC_PERIPHCLK_USB;
    pclk.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK)
    {
        return;
    }

    /* CRS registers live on APB1 and must be clocked before configuration. */
    __HAL_RCC_CRS_CLK_ENABLE();

    crs.Prescaler = RCC_CRS_SYNC_DIV1;
    crs.Source = RCC_CRS_SYNC_SOURCE_USB;
    crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue = RCC_CRS_RELOADVALUE_DEFAULT; /* 48000 - 1 for 1 kHz SOF */
    crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
    crs.HSI48CalibrationValue = BSP_CRS_HSI48_TRIM;
    HAL_RCCEx_CRSConfig(&crs);
}
