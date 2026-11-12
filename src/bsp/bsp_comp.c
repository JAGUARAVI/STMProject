/**
 * @file  bsp_comp.c
 * @brief COMP1 relaxation oscillator and hardware period capture.
 *
 * This replaces the external LM393 and the ESP32's software oscillator loop.
 * The exchange is:
 *
 *   ESP32                                  G474
 *   LM393 + 10k/10k divider + 100k hyst    COMP1 + DAC3_CH1 threshold
 *   GPIO ISR mirrors comparator to drive   COMP1_OUT on PA0 closes the loop
 *   PCNT / reciprocal edge timing          TIM2 32-bit input capture via DMA
 *   ISR latency inside the measurement     no CPU in the loop at all
 *
 * ## Why the loop is fully hardware
 *
 * PA0 is both COMP1_OUT (AF8) and the input of the SN74LVC1G34 drive buffer.
 * (Not PA8: on this package PA8's AF8 is COMP7_OUT. COMP1_OUT exists only on
 * PA0, PA6, PA11 and PB8; see config.h.)
 * The comparator therefore drives the range network directly: when V_cap is
 * below the threshold the buffer drives high and charges it; when V_cap crosses
 * the threshold the buffer drives low and discharges it. No interrupt runs at
 * the oscillation rate, so the period is limited only by the comparator,
 * buffer and RC network.
 *
 * The **output polarity must be inverted** for this to oscillate. With
 * INP = V_cap and INM = DAC3_CH1, the raw comparator is high while
 * V_cap > threshold. Driving high in that state would keep charging the node
 * and the loop would latch. `COMP_OUTPUTPOL_INVERTED` makes the pin high while
 * V_cap is below the threshold, which is the relaxation-loop condition.
 *
 * ## Why the timing is genuinely better
 *
 * TIM2 latches the edge instant in hardware. Any remaining loop delay is a
 * constant offset that the T0 tare absorbs; there is no per-edge CPU jitter.
 *
 * TIM2 is chosen deliberately: it is one of only two 32-bit timers on the part
 * (with TIM5, whose pins are not bonded here). At 170 MHz it gives 5.88 ns
 * resolution and a 25.3 s wrap, covering the whole 10 Hz .. 1 MHz band with no
 * overflow bookkeeping and no prescaler.
 *
 * ## Comparator-to-timer routing
 *
 * `comp1_out -> tim2_ti1_in` is an internal interconnect path, so no GPIO is
 * involved. The selector is on the **timer** side, not the comparator:
 * `TIM2->TISEL.TI1SEL = 0b0001` selects `tim_ti1_in1`, which RM0440 Table 272
 * defines as `comp1_out`. `CCMR1.CC1S = 0b01` then maps `tim_ic1` onto
 * `tim_ti1`. G4 has no output-routing field in the comparator register (unlike
 * STM32F3, whose `COMP_CSR.OUTSEL` served this purpose), which is why this can
 * look impossible if you only read the COMP chapter.
 *
 * The same node is also available on PA0 for a scope, and that is what closes
 * the oscillator loop. The timer does not use the pin.
 */

#include "bsp_comp.h"

#include "bsp.h"
#include "bsp_dac.h"
#include "bsp_frontend.h"
#include "config.h"

/* Default capture buffer depth. 512 edges at 1 MHz is ~0.5 ms of data; at
 * 10 Hz it is 51 s, which is why the capture length is chosen adaptively
 * instead of always filling the buffer. */
#ifndef ESWCAP_OSC_CAP_DEPTH
#define ESWCAP_OSC_CAP_DEPTH 512U
#endif

/* Minimum and maximum number of periods to average in one measurement. */
#define OSC_MIN_PERIODS 4U
#define OSC_MAX_PERIODS 256U

/* Target averaging window. Long enough to average down capture quantisation,
 * short enough that the operator does not wait. */
#define OSC_WINDOW_US 200000U

static COMP_HandleTypeDef s_hcomp1;
static TIM_HandleTypeDef  s_htim2;
static DMA_HandleTypeDef  s_hdma_tim2_ch1;

static uint32_t s_cap[ESWCAP_OSC_CAP_DEPTH];
static volatile uint32_t s_cap_len;
static volatile bool     s_dma_done;
static volatile uint32_t s_overruns;

static bool s_osc_running;
static bool s_comp_on;
static uint16_t s_threshold_mv;

/* ------------------------------------------------------------------------- */
/* GPIO                                                                      */
/* ------------------------------------------------------------------------- */

/**
 * @brief Put the drive pin (PA0) under COMP1's control.
 *
 * AF8 on PA0 is COMP1_OUT. The pin then carries the (polarity-corrected) comparator
 * output straight into the drive buffer, which is the whole point of the
 * hardware loop.
 */
static void drive_pin_comparator(void)
{
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin       = PIN_DRIVE;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = PIN_DRIVE_AF;
    HAL_GPIO_Init(PIN_DRIVE_PORT, &gpio);
}

/**
 * @brief Return the drive pin (PA0) to a plain GPIO output, driven low.
 *
 * Used for the RC-step method, where the firmware issues the excitation edge
 * itself. The external 10k pull-down on the buffer input keeps the drive low
 * while the pin is being reconfigured.
 */
static void drive_pin_gpio(void)
{
    GPIO_InitTypeDef gpio = {0};

    HAL_GPIO_WritePin(PIN_DRIVE_PORT, PIN_DRIVE, GPIO_PIN_RESET);

    gpio.Pin   = PIN_DRIVE;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(PIN_DRIVE_PORT, &gpio);
}

/** PA1 is shared by the ADC and the comparator, so it must be analog (not an
 *  alternate function) for both to see the true node voltage. */
static void comp_pins_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin  = PIN_VCAP;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(PIN_VCAP_PORT, &gpio);

    drive_pin_gpio();
}

/* ------------------------------------------------------------------------- */
/* DMA / timer MSP                                                           */
/* ------------------------------------------------------------------------- */

/**
 * @brief HAL MSP hook for the capture timer.
 *
 * Only TIM2 is served here; the ADC trigger timer has its own handle and does
 * not use DMA, so nothing else can reach this path.
 */
void HAL_TIM_IC_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM2) {
        return;
    }

    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    /* DMA1_Channel1 is taken by the ADC stream; use channel 2 here so the two
     * acquisitions never contend for a stream or an IRQ. */
    s_hdma_tim2_ch1.Instance = DMA1_Channel2;
    s_hdma_tim2_ch1.Init.Request = DMA_REQUEST_TIM2_CH1;
    s_hdma_tim2_ch1.Init.Direction = DMA_PERIPH_TO_MEMORY;
    s_hdma_tim2_ch1.Init.PeriphInc = DMA_PINC_DISABLE;
    s_hdma_tim2_ch1.Init.MemInc = DMA_MINC_ENABLE;
    s_hdma_tim2_ch1.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    s_hdma_tim2_ch1.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
    s_hdma_tim2_ch1.Init.Mode = DMA_NORMAL;
    s_hdma_tim2_ch1.Init.Priority = DMA_PRIORITY_HIGH;

    if (HAL_DMA_Init(&s_hdma_tim2_ch1) != HAL_OK) {
        return;
    }

    __HAL_LINKDMA(htim, hdma[TIM_DMA_ID_CC1], s_hdma_tim2_ch1);

    HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 5U, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);
}

void DMA1_Channel2_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hdma_tim2_ch1);
}

/**
 * @brief Capture-DMA completion.
 *
 * HAL reports a completed capture transfer here. The measurement itself has
 * already been stored by DMA, so the callback only has to raise a flag.
 */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2 && htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1) {
        s_dma_done = true;
    }
}

/* ------------------------------------------------------------------------- */
/* Comparator                                                                */
/* ------------------------------------------------------------------------- */

bool comp_init(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    comp_pins_init();

    if (!bsp_dac3_init()) {
        return false;
    }

    s_hcomp1.Instance = COMP1;
    s_hcomp1.Init.InputPlus = COMP_INPUT_PLUS_IO1;      /* PA1 = V_cap */
    s_hcomp1.Init.InputMinus = COMP_INPUT_MINUS_DAC3_CH1;
    /* The hysteresis replaces the LM393's 100k positive-feedback resistor and
     * sets the oscillator swing, hence K. 70 mV is the largest G4 setting
     * (K ~= 0.085); 20 mV gave K ~= 0.024, which the calibration solver
     * rejects and which leaves the swing buried in noise. See config.h. */
    s_hcomp1.Init.Hysteresis = OSC_COMP_HYSTERESIS;
    /* INVERTED is mandatory: the drive buffer is non-inverting, so the loop
     * needs "drive high while V_cap is below the threshold". See the file
     * header. */
    s_hcomp1.Init.OutputPol = COMP_OUTPUTPOL_INVERTED;
    s_hcomp1.Init.BlankingSrce = COMP_BLANKINGSRC_NONE;
    s_hcomp1.Init.TriggerMode = COMP_TRIGGERMODE_NONE;

    if (HAL_COMP_Init(&s_hcomp1) != HAL_OK) {
        return false;
    }

    /* TIM2 free-running at 170 MHz, 32-bit. */
    s_htim2.Instance = TIM2;
    s_htim2.Init.Prescaler = 0U;
    s_htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    s_htim2.Init.Period = 0xFFFFFFFFUL;
    s_htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    s_htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_IC_Init(&s_htim2) != HAL_OK) {
        return false;
    }

    TIM_IC_InitTypeDef ic;
    ic.ICPolarity = TIM_ICPOLARITY_RISING;
    ic.ICSelection = TIM_ICSELECTION_DIRECTTI;
    ic.ICPrescaler = TIM_ICPSC_DIV1;
    ic.ICFilter = 0U;

    if (HAL_TIM_IC_ConfigChannel(&s_htim2, &ic, TIM_CHANNEL_1) != HAL_OK) {
        return false;
    }

    /* Point TIM2's TI1 multiplexer at the internal comparator instead of the
     * default pin. This is the step that makes the whole design work without a
     * jumper: RM0440 Table 272 lists tim_ti1_in1 = comp1_out for TIM2, selected
     * by TISEL.TI1SEL = 0b0001. CC1S above maps tim_ic1 onto tim_ti1, so the
     * capture now sees COMP1 with no GPIO involved.
     *
     * Note the mux populations differ per timer: TIM2 TI1 accepts COMP1..COMP5
     * only. COMP1 is all this design needs. */
    if (HAL_TIMEx_TISelection(&s_htim2, TIM_TIM2_TI1_COMP1,
                              TIM_CHANNEL_1) != HAL_OK) {
        return false;
    }

    s_threshold_mv = 1650;
    bsp_dac3_set_mv(s_threshold_mv);
    return true;
}

void comp_set_threshold_mv(uint16_t mv)
{
    s_threshold_mv = mv;
    bsp_dac3_set_mv(mv);
}

uint16_t comp_threshold_mv(void)
{
    return s_threshold_mv;
}

void comp_enable(bool enable)
{
    if (enable) {
        if (HAL_COMP_Start(&s_hcomp1) == HAL_OK) {
            s_comp_on = true;
        }
    } else {
        (void)HAL_COMP_Stop(&s_hcomp1);
        s_comp_on = false;
    }
}

bool comp_enabled(void)
{
    /* Track this ourselves: HAL_COMP_Start leaves the handle in BUSY, so
     * comparing State against READY would report "off" for a running
     * comparator. */
    return s_comp_on;
}

/**
 * @brief Read the effective comparator output.
 *
 * COMP_CSR.VALUE is the raw analogue comparison, *before* polarity. The
 * hardware loop and the timer see the inverted output, so this function
 * applies the same inversion. It is used for diagnostics and for priming the
 * drive in a software-fallback build; the real loop does not read it.
 */
bool comp_output(void)
{
    bool raw = (READ_BIT(COMP1->CSR, COMP_CSR_VALUE) != 0U);
    return !raw;   /* COMP_OUTPUTPOL_INVERTED is fixed in this design */
}

/* ------------------------------------------------------------------------- */
/* Loop closure                                                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Start the relaxation loop.
 *
 * PA0 becomes COMP1_OUT, COMP1 is enabled, and TIM2 begins capturing. There is
 * no CPU in the loop: the comparator drives the buffer, the buffer drives the
 * range network, and TIM2 timestamps the edges.
 */
void comp_loop_start(void)
{
    drive_pin_comparator();

    /* The comparator's inverting input is DAC3_CH1; the channel must actually
     * be started for the threshold to appear. bsp_dac3_init() only configures
     * it. */
    bsp_dac3_enable(true);

    comp_enable(true);
    if (!s_comp_on) {
        bsp_dac3_enable(false);
        drive_pin_gpio();
        return;
    }

    __HAL_TIM_SET_COUNTER(&s_htim2, 0U);
    __HAL_TIM_ENABLE(&s_htim2);
    s_osc_running = true;
}

void comp_loop_stop(void)
{
    s_osc_running = false;

    __HAL_TIM_DISABLE(&s_htim2);

    /* Disable the comparator before handing PA0 back to GPIO, so the pin does
     * not glitch high through the AF path. */
    comp_enable(false);
    bsp_dac3_enable(false);
    drive_pin_gpio();
}

uint32_t comp_capture_overruns(void)
{
    return s_overruns;
}

/* ------------------------------------------------------------------------- */
/* Period measurement                                                        */
/* ------------------------------------------------------------------------- */

/** Capture exactly n CCR1 values into s_cap. */
static bool capture_edges(uint32_t n, uint32_t timeout_ms)
{
    if (n < 2U) {
        return false;
    }
    if (n > ESWCAP_OSC_CAP_DEPTH) {
        n = ESWCAP_OSC_CAP_DEPTH;
    }

    s_cap_len = n;
    s_dma_done = false;

    /* Clear any stale over-capture before arming. */
    __HAL_TIM_CLEAR_FLAG(&s_htim2, TIM_FLAG_CC1 | TIM_FLAG_CC1OF);

    if (HAL_TIM_IC_Start_DMA(&s_htim2, TIM_CHANNEL_1, s_cap, n) != HAL_OK) {
        return false;
    }

    uint32_t t0 = bsp_millis();
    bool ok = false;

    while (!s_dma_done) {
        if ((bsp_millis() - t0) > timeout_ms) {
            break;
        }
        bsp_iwdg_refresh();
    }

    if (s_dma_done) {
        ok = true;
    }

    (void)HAL_TIM_IC_Stop_DMA(&s_htim2, TIM_CHANNEL_1);

    if (__HAL_TIM_GET_FLAG(&s_htim2, TIM_FLAG_CC1OF)) {
        __HAL_TIM_CLEAR_FLAG(&s_htim2, TIM_FLAG_CC1OF);
        ++s_overruns;
        ok = false;
    }

    return ok;
}

/** Mean period in microseconds over the captured edges. */
static double mean_period_us(uint32_t n)
{
    if (n < 2U || n > s_cap_len) {
        return 0.0;
    }

    /* Unsigned 32-bit subtraction is correct across the counter wrap: the
     * difference of two captures is the elapsed ticks modulo 2^32, which is
     * exactly the interval as long as the interval itself is < 25.3 s. */
    uint64_t sum = 0;
    for (uint32_t i = 1; i < n; ++i) {
        sum += (uint32_t)(s_cap[i] - s_cap[i - 1]);
    }

    double ticks = (double)sum / (double)(n - 1U);
    return ticks * 1e6 / 170000000.0;
}

/**
 * @brief Measure the oscillation period.
 *
 * Two stages, because the oscillator band spans five decades:
 *
 *  1. Capture a single period to learn roughly where we are.
 *  2. Choose how many periods fit in a ~200 ms window and average those.
 *
 * A fixed count cannot work: 256 periods at 10 Hz would take 25 s, while one
 * period at 1 MHz is far too short to average quantisation out.
 */
bool comp_measure_period(uint32_t n_target, uint32_t timeout_ms,
                         double *period_us, uint32_t *n_periods,
                         double *freq_hz)
{
    if (period_us == NULL || n_periods == NULL || freq_hz == NULL) {
        return false;
    }
    if (!s_osc_running) {
        return false;
    }

    /* Stage 1: one period. A quarter of the caller's timeout is enough for even
     * the slowest supported DUT (10 Hz -> 100 ms). */
    uint32_t coarse_timeout = timeout_ms / 4U;
    if (coarse_timeout < 200U) {
        coarse_timeout = 200U;
    }

    if (!capture_edges(2U, coarse_timeout)) {
        return false;
    }

    double coarse_us = mean_period_us(2U);
    if (coarse_us <= 0.0) {
        return false;
    }

    /* Stage 2: average over the target window. */
    double want = (double)OSC_WINDOW_US / coarse_us;
    uint32_t n = (uint32_t)(want + 0.5);

    if (n < OSC_MIN_PERIODS) {
        n = OSC_MIN_PERIODS;
    }
    if (n > OSC_MAX_PERIODS) {
        n = OSC_MAX_PERIODS;
    }
    if (n_target > 0U && n > n_target) {
        n = n_target;
    }
    if (n < OSC_MIN_PERIODS) {
        n = OSC_MIN_PERIODS;
    }

    /* Capturing n edges yields n-1 complete periods. */
    uint32_t edges = n + 1U;
    if (edges > ESWCAP_OSC_CAP_DEPTH) {
        edges = ESWCAP_OSC_CAP_DEPTH;
    }

    if (!capture_edges(edges, timeout_ms)) {
        /* Fall back to the coarse single period rather than failing outright:
         * a slightly noisy reading beats discarding a working oscillator. */
        *period_us = coarse_us;
        *n_periods = 1U;
        *freq_hz = 1e6 / coarse_us;
        return true;
    }

    double p = mean_period_us(edges);
    if (p <= 0.0) {
        return false;
    }

    *period_us = p;
    *n_periods = edges - 1U;
    *freq_hz = 1e6 / p;
    return true;
}

/* ------------------------------------------------------------------------- */
/* COMP4: R-mode V_N overvoltage guard                                       */
/* ------------------------------------------------------------------------- */

static COMP_HandleTypeDef s_hcomp4;
static bool s_comp4_on;
static bool s_comp4_ready;

/**
 * @brief Configure COMP4 as the R-mode V_N overvoltage guard.
 *
 *   INP = PB0 = V_N_SENSE
 *   INM = DAC1_CH1 = 2.6 V (internal, no pin)
 *   OUT -> EXTI line 24 (rising)
 *
 * A valid Kelvin reading keeps V_N below RMODE_VN_MAX_MV (2.3 V). If the
 * current source loses regulation and V_N runs away, COMP4 trips at 2.6 V and
 * the ISR removes the force path before the DUT or the ADC input is damaged.
 *
 * The comparator output is not routed to a pin; the EXTI line is enough.
 */
bool bsp_comp4_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    if (s_comp4_ready) {
        return true;
    }

    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* PB0 = V_N_SENSE is the comparator input. OPAMP3 also configures it, but
     * do it here so the guard is valid even if COMP4 is initialised alone. */
    gpio.Pin  = PIN_VN;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(PIN_VN_PORT, &gpio);

    if (!bsp_dac1_init()) {
        return false;
    }
    bsp_dac1_enable(true);

    s_hcomp4.Instance = COMP4;
    s_hcomp4.Init.InputPlus = COMP_INPUT_PLUS_IO1;      /* PB0 = V_N */
    s_hcomp4.Init.InputMinus = COMP_INPUT_MINUS_DAC1_CH1;
    s_hcomp4.Init.Hysteresis = COMP_HYSTERESIS_20MV;
    s_hcomp4.Init.OutputPol = COMP_OUTPUTPOL_NONINVERTED;
    s_hcomp4.Init.BlankingSrce = COMP_BLANKINGSRC_NONE;
    s_hcomp4.Init.TriggerMode = COMP_TRIGGERMODE_IT_RISING;

    if (HAL_COMP_Init(&s_hcomp4) != HAL_OK) {
        return false;
    }

    __HAL_COMP_COMP4_EXTI_ENABLE_RISING_EDGE();
    __HAL_COMP_COMP4_EXTI_ENABLE_IT();

    HAL_NVIC_SetPriority(COMP4_5_6_IRQn, 3U, 0U);
    HAL_NVIC_EnableIRQ(COMP4_5_6_IRQn);

    s_comp4_ready = true;
    return true;
}

void bsp_comp4_enable(bool enable)
{
    /* Clear any stale EXTI flag first. Without this, a flag raised while the
     * comparator was being configured fires the handler the instant the
     * interrupt is enabled, which would latch FE_FAULT before a measurement
     * ever starts. */
    __HAL_COMP_COMP4_EXTI_CLEAR_FLAG();

    if (enable) {
        if (HAL_COMP_Start(&s_hcomp4) == HAL_OK) {
            __HAL_COMP_COMP4_EXTI_CLEAR_FLAG();
            s_comp4_on = true;
            __HAL_COMP_COMP4_EXTI_ENABLE_RISING_EDGE();
            __HAL_COMP_COMP4_EXTI_ENABLE_IT();
        }
    } else {
        __HAL_COMP_COMP4_EXTI_DISABLE_IT();
        __HAL_COMP_COMP4_EXTI_DISABLE_RISING_EDGE();
        (void)HAL_COMP_Stop(&s_hcomp4);
        __HAL_COMP_COMP4_EXTI_CLEAR_FLAG();
        s_comp4_on = false;
    }
}

bool bsp_comp4_enabled(void)
{
    return s_comp4_on;
}

/**
 * @brief COMP4 overvoltage interrupt.
 *
 * This is a safety path, not a measurement: remove the gate drive and the R
 * mux immediately, then let the main loop finish the shutdown with
 * fe_r_exit(). The handler does not block or wait for anything.
 */
void COMP4_5_6_IRQHandler(void)
{
    if (__HAL_COMP_COMP4_EXTI_GET_FLAG()) {
        __HAL_COMP_COMP4_EXTI_CLEAR_FLAG();
        fe_r_fault();
    }
}
