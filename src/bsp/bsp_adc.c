/**
 * @file  bsp_adc.c
 * @brief ADC acquisition for the ESWCap STM32G474CEU6 build.
 *
 * @section paths Two independent acquisition paths
 *
 * The instrument has two jobs whose rate requirements are incompatible:
 *
 *  - C-mode streams V_cap at the excitation rate (kilohertz to hundreds of
 *    kilohertz). It must be paced by hardware and must never pause.
 *  - R-mode reads V_N, V_B, V_TIA and V_BIAS occasionally; its rate is
 *    irrelevant.
 *
 * Errata ES0430 section 2.7.2 says that switching the channel multiplexer on
 * an ADC disturbs conversions already in flight on that ADC. Putting both jobs
 * on one instance would therefore make an R-mode read corrupt a chunk of the
 * C-mode waveform, and a fast C-mode stream would make an R-mode reading
 * impossible. The jobs are consequently split across instances:
 *
 *      C-mode stream   : ADC1 + TIM6 + DMA1 channel 1 (circular)
 *      V_N             : ADC3 (PB1, OPAMP3 output)
 *      V_B             : ADC4 (PB12, LMC6482 B output)
 *      V_TIA / V_BIAS  : ADC2, software-triggered single conversions
 *
 * None of the single-read paths ever reconfigures the ADC1 stream, so they
 * cannot disturb the C-mode waveform. ADC2/3/4 are independent instances.
 *
 * VREFINT is *not* available on ADC2: the G4 datasheet and the ST HAL both
 * confirm VREFINT is connected to ADCx_IN18 for x = 1, 3, 4 and 5, but not
 * ADC2 (see ADC_VREFINT_INSTANCE() in stm32g4xx_hal_adc_ex.h). Reading it on
 * ADC2 would sample an unrelated pin and produce a wrong VDDA. It is read on
 * ADC3, time-multiplexed with V_N; the dummy-conversion discard after each
 * channel switch handles the change. The result is cached after the first
 * successful read.
 *
 * @section oversampling Mean-preserving hardware oversampling
 *
 * The stream and single-read paths use the G4 hardware oversampler with:
 *
 *      ratio N in {2,4,...,256}, right shift M = log2(N)
 *
 * so the output is the *mean* of N conversions and stays on the native
 * 0..4095 scale. The tempting alternative, M = log2(N) - 4, gives four extra
 * bits (up to 16 effective bits), but it changes the scale whenever the ratio
 * changes and it is not a true gain in accuracy: a 12-bit SAR is already close
 * to its ENOB limit, and the extra bits are dominated by noise and INL. What
 * actually matters here is ratiometric cancellation: the signal channel and
 * the VREFINT reference channel must both be normalised by the same full-scale
 * code. With the mean-preserving shift both stay in 0..4095, so
 *
 *      voltage = code * VDDA / 4095
 *
 * is exact for every ratio. The sub-LSB resolution is deliberately not
 * exploited.
 *
 * With ratio == 1 the oversampler is switched off entirely: there is no
 * ADC_OVERSAMPLING_RATIO_1 constant in this HAL (the ratio field only encodes
 * 2..256), and a disabled oversampler is exactly a 1x mean.
 *
 * @section timing Timer pacing and the legal period window
 *
 * ADC clock = SYSCLK 170 MHz / ADC_CLOCK_ASYNC_DIV4 = 42.5 MHz (config.h).
 * A 12-bit conversion with a 47.5-cycle sample time takes 47.5 + 12.5 = 60
 * ADC clocks:
 *
 *      t_conv = 60 / 42.5e6 s = 1.411765 us = 240 TIM6 ticks
 *
 * because TIM6 runs at 170 MHz, exactly 4x the ADC clock. One complete
 * oversampled frame takes ratio * 240 ticks. The trigger period must leave
 * room for that frame plus acquisition jitter and synchroniser delay, so the
 * driver enforces 20 % margin:
 *
 *      period_ticks >= ratio * 240 * 1.2 = ratio * 288
 *
 * This is checked in *timer ticks*, not nanoseconds, so the check is exact at
 * 5.88 ns resolution. TIM6 is a 16-bit timer, so at most 65536 ticks
 * (385.5 us) can be programmed. The largest ratio that still fits is 128
 * (36864 ticks, 216.8 us); 256 would need 73728 ticks and is therefore
 * rejected by adc_stream_start(). That is a hard limit of a 16-bit TIM6 at
 * 47.5-cycle sampling, not a software restriction.
 *
 * @section errata Errata ES0430 mitigations
 *
 *  - 2.7.2 (channel switch disturbs conversions): ADC1 is configured with one
 *    channel only and is never reconfigured while streaming. ADC2 changes
 *    channels, but it owns no other conversion.
 *  - 2.7.3 (conversion more than 1 ms after the previous one is wrong): the
 *    stream is continuous and timer-triggered, so the gap between triggers is
 *    the programmed period and never exceeds TIM6's 16-bit maximum (385.5 us),
 *    comfortably below 1 ms.
 *  - 2.7.4 (first conversion after enable/stop can be zero): adc_stream_start()
 *    performs one complete dummy conversion through the real TIM6 trigger path
 *    and discards it *before* starting DMA. HAL_ADC_Start_DMA() re-enables the
 *    ADC, so the first DMA sample is discarded as well. adc_read() does a
 *    dummy conversion after every channel switch for the same reason.
 *
 * @section ring Circular DMA bookkeeping
 *
 * DMA1 channel 1 writes ADC1 results into a free-running circular buffer. The
 * producer position is (laps * capacity + (capacity - CNDTR)). A transfer
 * complete interrupt per lap keeps the lap count exact without a per-sample
 * interrupt. The consumer keeps an absolute "consumed" index and snapshots the
 * producer position before and after each copy; a copy that the producer
 * overwrote while it was in progress is trimmed from the front (the oldest
 * samples) and reported as an overrun. All index arithmetic is unsigned and
 * wraps naturally.
 */

#include "bsp_adc.h"
#include "config.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Private constants                                                         */
/* ------------------------------------------------------------------------- */

/** TIM6 clock: PCLK1 with APB1 prescaler /1, i.e. SYSCLK = 170 MHz. */
#define ADC_TRIGGER_TIMER_HZ            170000000U

/** ADC clocks consumed by one 12-bit conversion (47.5 sample + 12.5 SAR). */
#define ADC_CONV_ADC_CLOCKS             60U

/**
 * TIM6 ticks per ADC clock. 170 MHz / 42.5 MHz = 4 exactly, so a conversion
 * is 240 timer ticks and all period arithmetic stays integral.
 */
#define ADC_TIMER_TICKS_PER_ADC_CLK     (ADC_TRIGGER_TIMER_HZ / ADC_CLOCK_HZ)

/** Required headroom over the pure conversion time (20 %). */
#define ADC_PERIOD_MARGIN_NUM           6U
#define ADC_PERIOD_MARGIN_DEN           5U

/** ARR is 16 bits, so the reload value (ticks) ranges from 1 to 65536. */
#define ADC_TIMER_MAX_TICKS             65536U

/** Continuous-conversion floor from bsp_adc.h. */
#define ADC_MIN_REQUEST_NS              1000U

/** Bounded waits. Every conversion here completes in well under 1 ms. */
#define ADC_READ_TIMEOUT_MS             10U
#define ADC_FIRST_SAMPLE_TIMEOUT_MS     10U

/** VREFINT buffer settling after the internal path is enabled. */
#define ADC_VREFINT_SETTLE_MS           1U

/** NVIC priority for the DMA lap counter. Bookkeeping only, below the tick. */
#define ADC_DMA_IRQ_PRIORITY            10U

/* The period maths assumes the TIM6 clock is an exact multiple of the ADC
 * clock. bsp_init() produces 170 MHz / 4 = 42.5 MHz; fail the build rather
 * than silently truncating if that ever changes. */
#if (ADC_TRIGGER_TIMER_HZ % ADC_CLOCK_HZ) != 0U
#error "TIM6 clock is not an integral multiple of the ADC kernel clock"
#endif

#if (ADC_STREAM_CAPACITY > 65535U)
#error "ADC_STREAM_CAPACITY does not fit the DMA 16-bit transfer counter"
#endif

/* ------------------------------------------------------------------------- */
/* Private state                                                             */
/* ------------------------------------------------------------------------- */

/** Stream ring buffer shared with DMA1 channel 1. Halfword aligned.
 *  The 4-byte alignment is only for the uint32_t* cast HAL_ADC_Start_DMA()
 *  requires; the DMA itself is configured for halfword transfers. */
static uint16_t s_stream_ring[ADC_STREAM_CAPACITY] __attribute__((aligned(4)));

/** Public view of the stream. buf/capacity are constant after init. */
static adc_stream_info_t s_stream_info = {
    .buf      = s_stream_ring,
    .capacity = ADC_STREAM_CAPACITY,
    .total    = 0U,
    .overruns = 0U,
    .sample_period_ns = 0U,
    .os_ratio = 0U,
};

static ADC_HandleTypeDef s_hadc1;       /**< C-mode stream instance.        */
static ADC_HandleTypeDef s_hadc2;       /**< R-mode read instance.          */
static ADC_HandleTypeDef s_hadc3;       /**< VREFINT and V_N.               */
static ADC_HandleTypeDef s_hadc4;       /**< V_B read instance.             */
static DMA_HandleTypeDef s_hdma_adc1;   /**< ADC1 circular DMA.             */
static TIM_HandleTypeDef s_htim6;       /**< ADC1 trigger timebase.         */

static bool     s_adc1_ready;           /**< ADC1/TIM6/DMA initialised.     */
static bool     s_streaming;            /**< DMA stream currently running.  */
static bool     s_adc2_ready;           /**< ADC2 initialised and calibrated. */
static uint8_t  s_adc2_ratio;           /**< Oversampling ratio ADC2 is set to. */
static bool     s_adc3_ready;           /**< ADC3 initialised and calibrated. */
static uint8_t  s_adc3_ratio;           /**< Oversampling ratio ADC3 is set to. */
static bool     s_adc4_ready;           /**< ADC4 initialised and calibrated. */
static uint8_t  s_adc4_ratio;           /**< Oversampling ratio ADC4 is set to. */

/**
 * Number of complete DMA laps since the stream started. Incremented in the
 * transfer-complete callback, i.e. once per ADC_STREAM_CAPACITY samples.
 */
static volatile uint32_t s_dma_laps;

/** Absolute index of the next sample to be copied out of the ring. */
static volatile uint32_t s_consumed;

/** Mirror of s_consumed % ADC_STREAM_CAPACITY, kept for readability. */
static volatile uint32_t s_read_index;

/** Cached VDDA measurement; zero until a successful VREFINT read. */
static uint32_t s_vdda_mv;

/* ------------------------------------------------------------------------- */
/* Private helpers: oversampling and timing                                  */
/* ------------------------------------------------------------------------- */

/** True for the ratios the hardware oversampler and this API support. */
static bool adc_ratio_is_valid(uint8_t ratio)
{
    switch ((uint32_t)ratio)
    {
        case 1U:
        case 2U:
        case 4U:
        case 8U:
        case 16U:
        case 32U:
        case 64U:
        case 128U:
        case 256U:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Map a ratio to the HAL oversampling-ratio constant.
 *
 * Ratio 1 has no representable constant (the field encodes 2..256); callers
 * must disable OversamplingMode for 1. The returned value is then ignored.
 */
static uint32_t adc_ovs_ratio_sel(uint8_t ratio)
{
    switch ((uint32_t)ratio)
    {
        case 2U:   return ADC_OVERSAMPLING_RATIO_2;
        case 4U:   return ADC_OVERSAMPLING_RATIO_4;
        case 8U:   return ADC_OVERSAMPLING_RATIO_8;
        case 16U:  return ADC_OVERSAMPLING_RATIO_16;
        case 32U:  return ADC_OVERSAMPLING_RATIO_32;
        case 64U:  return ADC_OVERSAMPLING_RATIO_64;
        case 128U: return ADC_OVERSAMPLING_RATIO_128;
        case 256U: return ADC_OVERSAMPLING_RATIO_256;
        default:   return ADC_OVERSAMPLING_RATIO_2;
    }
}

/**
 * @brief Map a ratio to the mean-preserving right shift, log2(ratio).
 *
 * This is the whole point of the oversampling configuration. Keeping the
 * shift at log2(N) makes the result an average on the 0..4095 scale. The
 * constants in this HAL are named ADC_RIGHTBITSHIFT_n, not
 * ADC_OVERSAMPLING_SHIFT_n.
 */
static uint32_t adc_ovs_shift_sel(uint8_t ratio)
{
    switch ((uint32_t)ratio)
    {
        case 1U:   return ADC_RIGHTBITSHIFT_NONE;
        case 2U:   return ADC_RIGHTBITSHIFT_1;
        case 4U:   return ADC_RIGHTBITSHIFT_2;
        case 8U:   return ADC_RIGHTBITSHIFT_3;
        case 16U:  return ADC_RIGHTBITSHIFT_4;
        case 32U:  return ADC_RIGHTBITSHIFT_5;
        case 64U:  return ADC_RIGHTBITSHIFT_6;
        case 128U: return ADC_RIGHTBITSHIFT_7;
        case 256U: return ADC_RIGHTBITSHIFT_8;
        default:   return ADC_RIGHTBITSHIFT_NONE;
    }
}

/**
 * @brief Timer ticks occupied by one complete oversampled frame.
 *
 * 60 ADC clocks = 240 TIM6 ticks; ratio frames are accumulated before the
 * output register is updated.
 */
static uint32_t adc_frame_ticks(uint8_t ratio)
{
    return (uint32_t)ratio * ADC_CONV_ADC_CLOCKS * ADC_TIMER_TICKS_PER_ADC_CLK;
}

/**
 * @brief Smallest legal trigger period in TIM6 ticks for a ratio.
 *
 * frame * 1.2. With TIM6 at 170 MHz this is ratio * 288 exactly.
 */
static uint32_t adc_min_period_ticks(uint8_t ratio)
{
    return (adc_frame_ticks(ratio) * ADC_PERIOD_MARGIN_NUM) / ADC_PERIOD_MARGIN_DEN;
}

/**
 * @brief Convert a requested period in nanoseconds to whole TIM6 ticks.
 *
 * Rounds to the nearest tick. 64-bit intermediate so a full 32-bit period
 * cannot overflow; the caller rejects anything above the 16-bit ARR range.
 */
static uint32_t adc_ns_to_ticks(uint32_t period_ns)
{
    uint64_t ticks = ((uint64_t)period_ns * ADC_TRIGGER_TIMER_HZ) / 1000000000ULL;

    /* Round to nearest tick: the remainder is period_ns*170 mod 1e9. */
    if ((((uint64_t)period_ns * ADC_TRIGGER_TIMER_HZ) % 1000000000ULL) >= 500000000ULL)
    {
        ticks++;
    }

    if (ticks > 0xFFFFFFFFULL)
    {
        return 0xFFFFFFFFU;
    }
    return (uint32_t)ticks;
}

/** @brief Convert timer ticks back to nanoseconds (the *actual* period). */
static uint32_t adc_ticks_to_ns(uint32_t ticks)
{
    uint64_t ns = ((uint64_t)ticks * 1000000000ULL + (ADC_TRIGGER_TIMER_HZ / 2U)) /
                  ADC_TRIGGER_TIMER_HZ;

    return (ns > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)ns;
}

/** @brief Configure a GPIO pin as a high-impedance analogue input. */
static void adc_gpio_analog(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin = pin;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(port, &gpio);
}

/* ------------------------------------------------------------------------- */
/* Private helpers: producer position                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief Producer sample count, with interrupts masked for a coherent read.
 *
 * CNDTR and the lap counter are two pieces of one 32-bit quantity. If the DMA
 * wraps between reading them the result can appear to jump backwards by one
 * buffer, so the pair is read inside a short critical section. The section is
 * only a handful of instructions; the caller's copy runs with interrupts on.
 *
 * Total is (laps * capacity) + samples written in the current lap.
 *
 * A transfer-complete event has a small window in which the hardware has
 * reloaded CNDTR and set the TC flag, but the interrupt has not yet run and so
 * s_dma_laps is still one short. HAL_DMA_IRQHandler() clears the TC flag
 * *before* invoking the callback that increments s_dma_laps, so a set flag
 * inside this masked section means exactly one lap is pending: count it here.
 * Without that correction the producer position would appear to jump
 * backwards by a full buffer at every wrap.
 */
static uint32_t adc_producer_total_unlocked(void)
{
    uint32_t laps = s_dma_laps;
    uint32_t idx  = ADC_STREAM_CAPACITY - __HAL_DMA_GET_COUNTER(&s_hdma_adc1);

    /* idx == capacity is CNDTR == 0: the lap boundary is already represented
     * by the index itself, so a pending TC must not be counted twice. */
    if ((idx < ADC_STREAM_CAPACITY) &&
        (__HAL_DMA_GET_FLAG(&s_hdma_adc1, DMA_FLAG_TC1) != 0U))
    {
        laps++;
    }

    return (laps * ADC_STREAM_CAPACITY) + idx;
}

static uint32_t adc_producer_total(void)
{
    uint32_t primask = __get_PRIMASK();
    uint32_t total;

    __disable_irq();
    total = adc_producer_total_unlocked();
    __set_PRIMASK(primask);

    return total;
}

/** @brief Set the consumer origin to "the next sample the DMA writes". */
static void adc_consume_to(uint32_t total)
{
    s_consumed   = total;
    s_read_index = total % ADC_STREAM_CAPACITY;
    s_stream_info.total = total;
}

/* ------------------------------------------------------------------------- */
/* Private helpers: ADC configuration                                        */
/* ------------------------------------------------------------------------- */

/** @brief Apply the common 12-bit single-ended register settings. */
static void adc_fill_common_init(ADC_InitTypeDef *init)
{
    init->ClockPrescaler        = ADC_ASYNC_DIV; /* 170 MHz / 4 = 42.5 MHz */
    init->Resolution            = ADC_RESOLUTION_12B;
    init->DataAlign             = ADC_DATAALIGN_RIGHT;
    init->GainCompensation      = 0U;
    init->ScanConvMode          = ADC_SCAN_DISABLE;
    init->EOCSelection          = ADC_EOC_SINGLE_CONV;
    init->LowPowerAutoWait      = DISABLE;
    init->ContinuousConvMode    = DISABLE;       /* trigger/DMA paces it */
    init->NbrOfConversion       = 1U;
    init->DiscontinuousConvMode = DISABLE;
    init->NbrOfDiscConversion   = 1U;
    init->SamplingMode          = ADC_SAMPLING_MODE_NORMAL;
    init->DMAContinuousRequests = DISABLE;
    init->Overrun               = ADC_OVR_DATA_PRESERVED;
    init->OversamplingMode      = DISABLE;
    init->Oversampling.Ratio             = ADC_OVERSAMPLING_RATIO_2;
    init->Oversampling.RightBitShift     = ADC_RIGHTBITSHIFT_NONE;
    init->Oversampling.TriggeredMode     = ADC_TRIGGEREDMODE_SINGLE_TRIGGER;
    init->Oversampling.OversamplingStopReset = ADC_REGOVERSAMPLING_RESUMED_MODE;
}

/**
 * @brief Configure the hardware oversampler for a ratio.
 *
 * One trigger releases all N conversions (SINGLE_TRIGGER), because TIM6 paces
 * *frames*, not individual samples. The shift is log2(N): see the file header
 * for why the resolution-maximising shift is deliberately not used.
 */
static void adc_fill_oversampling(ADC_InitTypeDef *init, uint8_t ratio)
{
    if (ratio <= 1U)
    {
        init->OversamplingMode = DISABLE;
        return;
    }

    init->OversamplingMode = ENABLE;
    init->Oversampling.Ratio         = adc_ovs_ratio_sel(ratio);
    init->Oversampling.RightBitShift = adc_ovs_shift_sel(ratio);
    init->Oversampling.TriggeredMode = ADC_TRIGGEREDMODE_SINGLE_TRIGGER;
    init->Oversampling.OversamplingStopReset = ADC_REGOVERSAMPLING_RESUMED_MODE;
}

/** @brief Fill the one-channel regular sequence description. */
static void adc_fill_channel(ADC_ChannelConfTypeDef *ch, uint32_t channel,
                             uint32_t sampling_time)
{
    ch->Channel         = channel;
    ch->Rank            = ADC_REGULAR_RANK_1;
    ch->SamplingTime    = sampling_time;
    ch->SingleDiff      = ADC_SINGLE_ENDED;
    ch->OffsetNumber    = ADC_OFFSET_NONE;
    ch->Offset          = 0U;
    ch->OffsetSign      = ADC_OFFSET_SIGN_NEGATIVE;
    ch->OffsetSaturation = DISABLE;
}

/**
 * @brief Program ADC1 for timer-triggered, DMA-backed streaming.
 *
 * Called with the ADC stopped. Re-applying HAL_ADC_Init() is how the
 * oversampling ratio is changed between captures; the peripheral must be
 * disabled, which adc_stream_stop() guarantees.
 */
static bool adc1_configure(uint8_t ratio)
{
    ADC_ChannelConfTypeDef ch = {0};

    s_hadc1.Instance = ADC1;
    adc_fill_common_init(&s_hadc1.Init);
    s_hadc1.Init.ExternalTrigConv      = ADC_EXTERNALTRIG_T6_TRGO;
    s_hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_RISING;
    s_hadc1.Init.DMAContinuousRequests = ENABLE;
    s_hadc1.Init.Overrun               = ADC_OVR_DATA_OVERWRITTEN;
    adc_fill_oversampling(&s_hadc1.Init, ratio);

    __HAL_LINKDMA(&s_hadc1, DMA_Handle, s_hdma_adc1);

    if (HAL_ADC_Init(&s_hadc1) != HAL_OK)
    {
        return false;
    }

    adc_fill_channel(&ch, ADC_CH_VCAP, ADC_SAMPLETIME_47CYCLES_5);
    if (HAL_ADC_ConfigChannel(&s_hadc1, &ch) != HAL_OK)
    {
        return false;
    }

    /* Offset-only self-calibration; the ADC is disabled here, as required by
     * HAL_ADCEx_Calibration_Start(). */
    return HAL_ADCEx_Calibration_Start(&s_hadc1, ADC_SINGLE_ENDED) == HAL_OK;
}

/**
 * @brief Perform and discard one conversion through the real trigger path.
 *
 * This is the ES0430 2.7.4 mitigation: the first conversion after the ADC is
 * enabled (or after a stop) may be zero or wrong. Running it here, through
 * TIM6 and the external-trigger configuration that the stream will actually
 * use, exercises the complete path rather than a special software-start mode.
 *
 * The timer is stopped and its counter cleared afterwards so the subsequent
 * HAL_ADC_Start_DMA() + HAL_TIM_Base_Start() begin from a known phase.
 */
static bool adc1_dummy_conversion(void)
{
    HAL_StatusTypeDef status;

    if (HAL_ADC_Start(&s_hadc1) != HAL_OK)
    {
        return false;
    }

    if (HAL_TIM_Base_Start(&s_htim6) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc1);
        return false;
    }

    status = HAL_ADC_PollForConversion(&s_hadc1, ADC_READ_TIMEOUT_MS);

    (void)HAL_ADC_GetValue(&s_hadc1);   /* deliberately discarded */
    (void)HAL_ADC_Stop(&s_hadc1);
    (void)HAL_TIM_Base_Stop(&s_htim6);

    __HAL_TIM_SET_COUNTER(&s_htim6, 0U);
    __HAL_TIM_CLEAR_FLAG(&s_htim6, TIM_FLAG_UPDATE);

    return status == HAL_OK;
}

/**
 * @brief Program ADC2 for software-triggered single reads.
 *
 * Changing the oversampling ratio requires disabling the ADC. Re-initialising
 * the instance on a ratio change is acceptable because this path belongs to
 * R-mode, where rate is irrelevant; doing it any more cleverly would mean
 * touching CFGR2 mid-conversion, which is exactly what ES0430 2.7.2 warns
 * about. The currently applied ratio is cached in s_adc2_ratio.
 */
static bool adc2_configure(uint8_t ratio)
{
    if (s_adc2_ready && (s_adc2_ratio == ratio))
    {
        return true;
    }

    if (s_adc2_ready)
    {
        (void)HAL_ADC_Stop(&s_hadc2);
        if (HAL_ADC_DeInit(&s_hadc2) != HAL_OK)
        {
            s_adc2_ready = false;
            return false;
        }
        s_adc2_ready = false;
    }

    s_hadc2.Instance = ADC2;
    adc_fill_common_init(&s_hadc2.Init);
    s_hadc2.Init.ExternalTrigConv     = ADC_SOFTWARE_START;
    s_hadc2.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    adc_fill_oversampling(&s_hadc2.Init, ratio);

    if (HAL_ADC_Init(&s_hadc2) != HAL_OK)
    {
        return false;
    }

    if (HAL_ADCEx_Calibration_Start(&s_hadc2, ADC_SINGLE_ENDED) != HAL_OK)
    {
        (void)HAL_ADC_DeInit(&s_hadc2);
        return false;
    }

    s_adc2_ratio = ratio;
    s_adc2_ready = true;
    return true;
}

/**
 * @brief Convert one channel on ADC2 and return its oversampled code.
 *
 * Performs the errata dummy conversion after configuring the channel, then one
 * real conversion. Because the hardware oversampler is used, the single real
 * conversion already produces the mean of `ratio` samples with shift
 * log2(ratio); no software accumulation is needed and the result remains on the
 * 0..4095 scale.
 *
 * Both waits are bounded by HAL_ADC_PollForConversion(), which uses the HAL
 * millisecond tick; there is no unbounded loop anywhere in this path.
 *
 * @return raw code 0..4095, or -1 on timeout/configuration failure.
 */
static int32_t adc2_read_channel(uint32_t channel, uint8_t ratio,
                                 uint32_t sampling_time)
{
    ADC_ChannelConfTypeDef ch = {0};
    uint32_t value;

    if (!adc_ratio_is_valid(ratio))
    {
        return -1;
    }

    if (!adc2_configure(ratio))
    {
        return -1;
    }

    adc_fill_channel(&ch, channel, sampling_time);
    if (HAL_ADC_ConfigChannel(&s_hadc2, &ch) != HAL_OK)
    {
        return -1;
    }

    /* Dummy conversion: the channel mux was just switched, and the ADC may
     * have been re-enabled by a ratio change. Discard this result. */
    if (HAL_ADC_Start(&s_hadc2) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc2);
        return -1;
    }
    if (HAL_ADC_PollForConversion(&s_hadc2, ADC_READ_TIMEOUT_MS) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc2);
        return -1;
    }
    (void)HAL_ADC_GetValue(&s_hadc2);

    /* Real conversion, same enabled ADC and same channel. */
    if (HAL_ADC_Start(&s_hadc2) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc2);
        return -1;
    }
    if (HAL_ADC_PollForConversion(&s_hadc2, ADC_READ_TIMEOUT_MS) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc2);
        return -1;
    }

    value = HAL_ADC_GetValue(&s_hadc2);
    (void)HAL_ADC_Stop(&s_hadc2);

    return (int32_t)value;
}

/**
 * @brief Configure ADC3 with the requested hardware-oversampling ratio.
 *
 * ADC3 serves two masters: VREFINT/VDDA (ratio 1, long sample time) and the
 * R-mode V_N buffer (oversampled). Both need the instance re-initialised when
 * the ratio changes; the path is slow but is only used for R-mode and a cached
 * VDDA reading.
 *
 * VREFINT is not routed to ADC2 on this part; see the file header. ADC3 is
 * independent of ADC1 and ADC2, so reconfiguring it cannot disturb the C-mode
 * stream or the ADC2/ADC4 Kelvin reads.
 */
static bool adc3_configure(uint8_t ratio)
{
    if (s_adc3_ready && (s_adc3_ratio == ratio))
    {
        return true;
    }

    if (s_adc3_ready)
    {
        (void)HAL_ADC_Stop(&s_hadc3);
        if (HAL_ADC_DeInit(&s_hadc3) != HAL_OK)
        {
            s_adc3_ready = false;
            return false;
        }
        s_adc3_ready = false;
    }

    __HAL_RCC_ADC345_CLK_ENABLE();

    s_hadc3.Instance = ADC3;
    adc_fill_common_init(&s_hadc3.Init);
    s_hadc3.Init.ExternalTrigConv     = ADC_SOFTWARE_START;
    s_hadc3.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    adc_fill_oversampling(&s_hadc3.Init, ratio);

    if (HAL_ADC_Init(&s_hadc3) != HAL_OK)
    {
        return false;
    }

    if (HAL_ADCEx_Calibration_Start(&s_hadc3, ADC_SINGLE_ENDED) != HAL_OK)
    {
        (void)HAL_ADC_DeInit(&s_hadc3);
        return false;
    }

    s_adc3_ratio = ratio;
    s_adc3_ready = true;
    return true;
}

/**
 * @brief Convert one channel on ADC3 and return its oversampled code.
 *
 * Dummy-then-real after every channel switch, for ES0430 2.7.2/2.7.4.
 * Waits are bounded by HAL_ADC_PollForConversion().
 */
static int32_t adc3_read_channel(uint32_t channel, uint8_t ratio,
                                 uint32_t sampling_time)
{
    ADC_ChannelConfTypeDef ch = {0};
    uint32_t value;

    if (!adc_ratio_is_valid(ratio))
    {
        return -1;
    }
    if (!adc3_configure(ratio))
    {
        return -1;
    }

    adc_fill_channel(&ch, channel, sampling_time);
    if (HAL_ADC_ConfigChannel(&s_hadc3, &ch) != HAL_OK)
    {
        return -1;
    }

    if (HAL_ADC_Start(&s_hadc3) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc3);
        return -1;
    }
    if (HAL_ADC_PollForConversion(&s_hadc3, ADC_READ_TIMEOUT_MS) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc3);
        return -1;
    }
    (void)HAL_ADC_GetValue(&s_hadc3);

    if (HAL_ADC_Start(&s_hadc3) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc3);
        return -1;
    }
    if (HAL_ADC_PollForConversion(&s_hadc3, ADC_READ_TIMEOUT_MS) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc3);
        return -1;
    }

    value = HAL_ADC_GetValue(&s_hadc3);
    (void)HAL_ADC_Stop(&s_hadc3);

    return (int32_t)value;
}

/**
 * @brief Read VREFINT on ADC3 at ratio 1 with the long sample time.
 *
 * The internal reference buffer needs a short settling time after VREFEN is
 * set. One millisecond is far more than the 12 us the LL header specifies,
 * and the result is cached by adc_vdda_mv(), so this happens once.
 *
 * @return true and *raw on success; false on any timeout.
 */
static bool adc3_read_vrefint(uint16_t *raw)
{
    ADC_ChannelConfTypeDef ch = {0};
    uint32_t value;

    if (!adc3_configure(1U))
    {
        return false;
    }

    adc_fill_channel(&ch, ADC_CHANNEL_VREFINT, ADC_SAMPLETIME_247CYCLES_5);
    if (HAL_ADC_ConfigChannel(&s_hadc3, &ch) != HAL_OK)
    {
        return false;
    }

    HAL_Delay(ADC_VREFINT_SETTLE_MS);

    /* Dummy conversion after the channel/VREFEN switch, per ES0430 2.7.4. */
    if (HAL_ADC_Start(&s_hadc3) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc3);
        return false;
    }
    if (HAL_ADC_PollForConversion(&s_hadc3, ADC_READ_TIMEOUT_MS) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc3);
        return false;
    }
    (void)HAL_ADC_GetValue(&s_hadc3);

    if (HAL_ADC_Start(&s_hadc3) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc3);
        return false;
    }
    if (HAL_ADC_PollForConversion(&s_hadc3, ADC_READ_TIMEOUT_MS) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc3);
        return false;
    }

    value = HAL_ADC_GetValue(&s_hadc3);
    (void)HAL_ADC_Stop(&s_hadc3);

    if (value == 0U)
    {
        return false;
    }

    *raw = (uint16_t)value;
    return true;
}

/**
 * @brief Configure ADC4 for the V_B Kelvin read.
 *
 * ADC4 is independent of ADC1/2/3. PB12 (ADC4_IN3) is the LMC6482 B output;
 * it is the only channel this instance needs.
 */
static bool adc4_configure(uint8_t ratio)
{
    if (s_adc4_ready && (s_adc4_ratio == ratio))
    {
        return true;
    }

    if (s_adc4_ready)
    {
        (void)HAL_ADC_Stop(&s_hadc4);
        if (HAL_ADC_DeInit(&s_hadc4) != HAL_OK)
        {
            s_adc4_ready = false;
            return false;
        }
        s_adc4_ready = false;
    }

    __HAL_RCC_ADC345_CLK_ENABLE();

    s_hadc4.Instance = ADC4;
    adc_fill_common_init(&s_hadc4.Init);
    s_hadc4.Init.ExternalTrigConv     = ADC_SOFTWARE_START;
    s_hadc4.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    adc_fill_oversampling(&s_hadc4.Init, ratio);

    if (HAL_ADC_Init(&s_hadc4) != HAL_OK)
    {
        return false;
    }

    if (HAL_ADCEx_Calibration_Start(&s_hadc4, ADC_SINGLE_ENDED) != HAL_OK)
    {
        (void)HAL_ADC_DeInit(&s_hadc4);
        return false;
    }

    s_adc4_ratio = ratio;
    s_adc4_ready = true;
    return true;
}

/**
 * @brief Convert one channel on ADC4, dummy-then-real with bounded waits.
 */
static int32_t adc4_read_channel(uint32_t channel, uint8_t ratio,
                                 uint32_t sampling_time)
{
    ADC_ChannelConfTypeDef ch = {0};
    uint32_t value;

    if (!adc_ratio_is_valid(ratio))
    {
        return -1;
    }
    if (!adc4_configure(ratio))
    {
        return -1;
    }

    adc_fill_channel(&ch, channel, sampling_time);
    if (HAL_ADC_ConfigChannel(&s_hadc4, &ch) != HAL_OK)
    {
        return -1;
    }

    if (HAL_ADC_Start(&s_hadc4) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc4);
        return -1;
    }
    if (HAL_ADC_PollForConversion(&s_hadc4, ADC_READ_TIMEOUT_MS) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc4);
        return -1;
    }
    (void)HAL_ADC_GetValue(&s_hadc4);

    if (HAL_ADC_Start(&s_hadc4) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc4);
        return -1;
    }
    if (HAL_ADC_PollForConversion(&s_hadc4, ADC_READ_TIMEOUT_MS) != HAL_OK)
    {
        (void)HAL_ADC_Stop(&s_hadc4);
        return -1;
    }

    value = HAL_ADC_GetValue(&s_hadc4);
    (void)HAL_ADC_Stop(&s_hadc4);

    return (int32_t)value;
}

/* ------------------------------------------------------------------------- */
/* Public API: stream path                                                   */
/* ------------------------------------------------------------------------- */

bool adc_init(void)
{
    TIM_MasterConfigTypeDef master = {0};

    if (s_adc1_ready)
    {
        return true;
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_DMAMUX1_CLK_ENABLE();
    __HAL_RCC_TIM6_CLK_ENABLE();

    /* PA1 = ADC1_IN2 / ADC2_IN2, the V_cap node. High impedance, no pull: a
     * pull resistor would load the RC node being measured. */
    adc_gpio_analog(PIN_VCAP_PORT, PIN_VCAP);

    /* DMA1 channel 1, DMAMUX request 5 (ADC1), halfword, circular. The ADC
     * data register is 16-bit; using halfword transfers matches the data
     * width exactly and halves bus traffic compared with word transfers. */
    s_hdma_adc1.Instance = DMA1_Channel1;
    s_hdma_adc1.Init.Request             = DMA_REQUEST_ADC1;
    s_hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    s_hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
    s_hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    s_hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    s_hdma_adc1.Init.Mode                = DMA_CIRCULAR;
    s_hdma_adc1.Init.Priority            = DMA_PRIORITY_HIGH;

    if (HAL_DMA_Init(&s_hdma_adc1) != HAL_OK)
    {
        return false;
    }

    /* One transfer-complete interrupt per ADC_STREAM_CAPACITY samples keeps
     * the lap counter exact. At the fastest legal period this is about one
     * interrupt every 1.4 ms, far cheaper than a per-sample interrupt. */
    HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, ADC_DMA_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

    /* TIM6: basic timer whose only job is TRGO-on-update. PSC is fixed at 0
     * because the period maths depends on the 5.88 ns tick. The ARR here is
     * only a safe placeholder; adc_stream_start() writes the real one. */
    s_htim6.Instance               = ADC_TRIGGER_TIM;
    s_htim6.Init.Prescaler         = 0U;
    s_htim6.Init.CounterMode       = TIM_COUNTERMODE_UP;
    s_htim6.Init.Period            = ADC_TIMER_MAX_TICKS - 1U;
    s_htim6.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    s_htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_Base_Init(&s_htim6) != HAL_OK)
    {
        return false;
    }

    master.MasterOutputTrigger  = TIM_TRGO_UPDATE;
    master.MasterOutputTrigger2 = TIM_TRGO2_RESET; /* ignored on TIM6 */
    master.MasterSlaveMode      = TIM_MASTERSLAVEMODE_DISABLE;

    if (HAL_TIMEx_MasterConfigSynchronization(&s_htim6, &master) != HAL_OK)
    {
        return false;
    }

    /* Leave the peripheral initialised, calibrated and disabled. The first
     * adc_stream_start() only has to change ARR, oversampling and start. */
    if (!adc1_configure(1U))
    {
        return false;
    }

    s_streaming = false;
    s_dma_laps  = 0U;
    adc_consume_to(0U);

    s_adc1_ready = true;
    return true;
}

bool adc_stream_start(uint8_t os_ratio, uint32_t period_ns)
{
    uint32_t ticks;
    uint32_t ticks0;
    uint32_t min_ticks;
    uint32_t psc;
    uint32_t tickstart;

    if (!adc_ratio_is_valid(os_ratio) || (period_ns < ADC_MIN_REQUEST_NS))
    {
        return false;
    }

    /* Validate in TIM6 ticks. The conversion itself occupies ratio*240
     * ticks; 20 % headroom covers the trigger synchroniser and the fact that
     * a new trigger must not land while the previous oversampled frame is
     * still being accumulated. Doing the comparison in ticks avoids rounding
     * a nanosecond request up to a period that is still too short. */
    min_ticks = adc_min_period_ticks(os_ratio);
    ticks0    = adc_ns_to_ticks(period_ns);

    if ((ticks0 == 0U) || (min_ticks > ADC_TIMER_MAX_TICKS))
    {
        return false;
    }

    /* TIM6 is 16-bit, so at 170 MHz it can only express periods up to ~385 us.
     * The slow ranges need milliseconds, so divide the timer clock until the
     * period fits. This only coarsens the trigger grid: the conversion and the
     * oversampling are untouched, and the reported sample period is derived
     * back from the programmed registers so the fit always uses the true
     * interval rather than the rounded request. */
    psc = 0U;
    while (((ticks0 + psc) / (psc + 1U)) > ADC_TIMER_MAX_TICKS)
    {
        ++psc;
        if (psc > 65535U)
        {
            return false;
        }
    }

    ticks     = (ticks0 + psc) / (psc + 1U);        /* round up, never short */
    min_ticks = (min_ticks + psc) / (psc + 1U);     /* ceil on the new grid  */

    if ((ticks < min_ticks) ||
        (ticks > ADC_TIMER_MAX_TICKS) ||
        (ticks == 0U))
    {
        return false;
    }

    if (s_streaming)
    {
        adc_stream_stop();
    }
    if (!s_adc1_ready)
    {
        return false;
    }

    /* Program the real period before the dummy conversion, so the dummy
     * exercises the same trigger timing the stream will use. */
    s_htim6.Init.Prescaler = psc;
    s_htim6.Init.Period    = ticks - 1U;
    if (HAL_TIM_Base_Init(&s_htim6) != HAL_OK)
    {
        return false;
    }

    /* HAL_TIM_Base_Init() rewrites CR1/ARR/PSC but not CR2, so TRGO stays
     * selected; re-applying it documents the dependency and survives a HAL
     * that resets more than expected. */
    {
        TIM_MasterConfigTypeDef master = {0};

        master.MasterOutputTrigger  = TIM_TRGO_UPDATE;
        master.MasterOutputTrigger2 = TIM_TRGO2_RESET;
        master.MasterSlaveMode      = TIM_MASTERSLAVEMODE_DISABLE;
        if (HAL_TIMEx_MasterConfigSynchronization(&s_htim6, &master) != HAL_OK)
        {
            return false;
        }
    }

    __HAL_TIM_SET_COUNTER(&s_htim6, 0U);
    __HAL_TIM_CLEAR_FLAG(&s_htim6, TIM_FLAG_UPDATE);

    if (!adc1_configure(os_ratio))
    {
        return false;
    }

    /* Errata 2.7.3/2.7.4: one full dummy conversion through the trigger path
     * before DMA is armed. */
    if (!adc1_dummy_conversion())
    {
        return false;
    }

    /* HAL takes a uint32_t* because its API predates the 16-bit buffer rule;
     * the DMA data width is halfword (set above), so the transfer length is
     * still counted in uint16_t samples. The cast is a type-level
     * formality only: no 32-bit access is generated. */
    if (HAL_ADC_Start_DMA(&s_hadc1, (uint32_t *)(void *)s_stream_ring,
                          ADC_STREAM_CAPACITY) != HAL_OK)
    {
        adc_stream_stop();
        return false;
    }

    /* Reset all consumer/producer bookkeeping after Start_DMA has cleared the
     * DMA flags and reloaded CNDTR, and before the first trigger exists. */
    {
        uint32_t primask = __get_PRIMASK();

        __disable_irq();
        s_dma_laps = 0U;
        s_consumed = 0U;
        s_read_index = 0U;
        s_stream_info.total = 0U;
        s_stream_info.overruns = 0U;
        __set_PRIMASK(primask);
    }

    s_streaming = true;

    if (HAL_TIM_Base_Start(&s_htim6) != HAL_OK)
    {
        adc_stream_stop();
        return false;
    }

    /* HAL_ADC_Start_DMA() re-enables the ADC, so its first conversion is
     * again a "first after enable". Wait for it to arrive and consume it.
     * The wait is bounded; a stream that produces no samples is a failure,
     * not something to hide. */
    tickstart = HAL_GetTick();
    while (adc_producer_total() == 0U)
    {
        if ((HAL_GetTick() - tickstart) > ADC_FIRST_SAMPLE_TIMEOUT_MS)
        {
            adc_stream_stop();
            return false;
        }
    }

    /* Report the period the timer will actually produce, derived back from
     * the ARR register (ARR + 1 ticks) rather than echoing the rounded
     * nanosecond request. */
    s_stream_info.sample_period_ns =
        adc_ticks_to_ns(((uint32_t)ADC_TRIGGER_TIM->ARR + 1U) * (psc + 1U));
    s_stream_info.os_ratio = os_ratio;
    adc_consume_to(adc_producer_total()); /* discard the first sample(s) */

    return true;
}

void adc_stream_stop(void)
{
    if (!s_adc1_ready)
    {
        return;
    }

    /* Timer first: no new trigger can start a conversion while the rest of
     * the path is being shut down. */
    (void)HAL_TIM_Base_Stop(&s_htim6);
    __HAL_TIM_SET_COUNTER(&s_htim6, 0U);
    __HAL_TIM_CLEAR_FLAG(&s_htim6, TIM_FLAG_UPDATE);

    s_streaming = false;

    if (s_hadc1.DMA_Handle != NULL)
    {
        (void)HAL_ADC_Stop_DMA(&s_hadc1);
    }
    else
    {
        (void)HAL_ADC_Stop(&s_hadc1);
    }
}

const adc_stream_info_t *adc_stream_info(void)
{
    /* Refresh the free-running total on every query so a caller that only
     * polls the info structure still sees samples written since the last
     * adc_stream_take(). */
    if (s_streaming)
    {
        s_stream_info.total = adc_producer_total();
    }

    return &s_stream_info;
}

void adc_stream_sync(void)
{
    uint32_t primask;
    uint32_t total;

    if (!s_streaming)
    {
        return;
    }

    /* Short critical section: the timer reset and the consumer-origin reset
     * must describe the same instant. The caller issues the excitation edge
     * immediately after this returns, so the section must not wait for
     * anything, and must not touch the ADC or DMA configuration.
     *
     * The DMA ring is free-running: CNDTR and the memory pointer cannot be
     * rewritten while the channel is enabled without stopping the stream
     * (which would itself create the bad first conversion ES0430 2.7.4 warns
     * about). "Clearing the write index" is therefore done in the consumer's
     * absolute index: whatever slot the DMA writes next becomes sample 0 for
     * adc_stream_take(). */
    primask = __get_PRIMASK();
    __disable_irq();

    ADC_TRIGGER_TIM->CNT = 0U;
    __HAL_TIM_CLEAR_FLAG(&s_htim6, TIM_FLAG_UPDATE);
    total = adc_producer_total_unlocked();
    adc_consume_to(total);

    __set_PRIMASK(primask);
}

int adc_stream_take(uint16_t *dst, uint32_t max)
{
    uint32_t total_before;
    uint32_t available;
    uint32_t skip = 0U;
    uint32_t count;
    uint32_t first_abs;
    uint32_t index;
    uint32_t copied;
    uint32_t total_after;
    uint32_t age;
    uint32_t trim;
    bool overrun = false;

    if ((dst == NULL) || (max == 0U) || !s_streaming)
    {
        return 0;
    }

    total_before = adc_producer_total();
    available    = total_before - s_consumed;

    if (available == 0U)
    {
        s_stream_info.total = total_before;
        return 0;
    }

    /* More than a full buffer arrived since the last call: the producer has
     * lapped the consumer. The oldest samples are already gone; keep the
     * newest, coherent ones and report the overrun. */
    if (available > ADC_STREAM_CAPACITY)
    {
        skip    = available - ADC_STREAM_CAPACITY;
        overrun = true;
    }

    count = available - skip;          /* <= capacity by construction */

    /* "Copy the newest max": drop older samples from the front. */
    if (count > max)
    {
        skip += count - max;
        count = max;
    }

    first_abs = s_consumed + skip;
    index     = (s_read_index + skip) % ADC_STREAM_CAPACITY;
    copied    = 0U;

    while (copied < count)
    {
        uint32_t chunk = ADC_STREAM_CAPACITY - index;

        if (chunk > (count - copied))
        {
            chunk = count - copied;
        }

        memcpy(&dst[copied], &s_stream_ring[index], chunk * sizeof(uint16_t));

        copied += chunk;
        index  += chunk;
        if (index >= ADC_STREAM_CAPACITY)
        {
            index = 0U;
        }
    }

    /* The copy ran with interrupts enabled and the DMA kept writing. If the
     * producer has since written capacity samples past the first copied
     * sample, that front portion was overwritten while (or before) it was
     * copied. Trim the suspect front and report an overrun; the suffix is
     * coherent. */
    total_after = adc_producer_total();
    age         = total_after - first_abs;

    if (age > ADC_STREAM_CAPACITY)
    {
        trim = age - ADC_STREAM_CAPACITY;
        if (trim > copied)
        {
            trim = copied;
        }
        if (trim > 0U)
        {
            memmove(dst, &dst[trim], (copied - trim) * sizeof(uint16_t));
            copied -= trim;
        }
        overrun = true;
    }

    /* Advance past every sample that was considered, including any trimmed
     * front. Those were produced and are no longer recoverable. */
    s_consumed   = first_abs + count;
    s_read_index = s_consumed % ADC_STREAM_CAPACITY;
    s_stream_info.total = total_after;

    if (overrun)
    {
        s_stream_info.overruns++;
        return -1;
    }

    return (int)copied;
}

void adc_stream_flush(void)
{
    uint32_t primask;

    if (!s_streaming)
    {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    adc_consume_to(adc_producer_total_unlocked());
    __set_PRIMASK(primask);
}

/* ------------------------------------------------------------------------- */
/* Public API: conversion and reference                                      */
/* ------------------------------------------------------------------------- */

int adc_code_to_mv(uint16_t code, uint8_t os_ratio, uint32_t vref_mv)
{
    uint32_t vref = vref_mv;

    /* The shift is log2(ratio), so the code is always the mean on the native
     * 0..4095 scale. The ratio therefore cancels and is not needed in the
     * arithmetic; the parameter is kept for API compatibility and to make the
     * caller state which ratio produced the code. */
    (void)os_ratio;

    if (vref == 0U)
    {
        vref = adc_vdda_mv();
        if (vref == 0U)
        {
            vref = 3300U;   /* last-resort nominal, better than dividing by 0 */
        }
    }

    return (int)(((uint32_t)code * vref) / 4095U);
}

uint32_t adc_vdda_mv(void)
{
    uint16_t raw;
    uint32_t mv;

    if (s_vdda_mv != 0U)
    {
        return s_vdda_mv;
    }

    if (!adc3_read_vrefint(&raw))
    {
        return 0U;
    }

    /* Factory calibration word was taken with VREF+ = 3000 mV. The macro is
     * provided by stm32g4xx_ll_adc.h; use it so the conversion matches the
     * device constants rather than duplicating the formula. */
    mv = __LL_ADC_CALC_VREFANALOG_VOLTAGE(raw, ADC_RESOLUTION_12B);
    if (mv == 0U)
    {
        return 0U;
    }

    s_vdda_mv = mv;
    return mv;
}

/* ------------------------------------------------------------------------- */
/* Public API: single-read path                                              */
/* ------------------------------------------------------------------------- */

bool adc_read_init(void)
{
    if (s_adc2_ready && s_adc4_ready)
    {
        return true;
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_ADC345_CLK_ENABLE();

    /* Every R-mode sense node is an analogue input. PA3 (VREF) and PA2/PA7
     * (OPAMP1) are set analogue by the op-amp init; PB0/PB1/PB2 by OPAMP3. */
    adc_gpio_analog(PIN_VCAP_PORT, PIN_VCAP);
    adc_gpio_analog(PIN_B_PORT, PIN_B);
    adc_gpio_analog(PIN_TIA_PORT, PIN_TIA);
    adc_gpio_analog(PIN_VBIAS_SENSE_PORT, PIN_VBIAS_SENSE);
    adc_gpio_analog(PIN_VB_PORT, PIN_VB);
    adc_gpio_analog(PIN_VN_OUT_PORT, PIN_VN_OUT);

    if (!adc2_configure(1U))
    {
        return false;
    }
    if (!adc4_configure(1U))
    {
        return false;
    }

    return true;
}

int32_t adc_read(uint32_t channel, uint8_t os_ratio)
{
    if (!adc_ratio_is_valid(os_ratio))
    {
        return -1;
    }

    if (!s_adc2_ready && !adc_read_init())
    {
        return -1;
    }

    /* The external channels in this design tolerate the 47.5-cycle sample
     * time used by the stream, so both paths share it. VREFINT is the one
     * exception and is handled on ADC3 with a long sample time. */
    return adc2_read_channel(channel, os_ratio, ADC_SAMPLETIME_47CYCLES_5);
}

int32_t adc_read_adc3(uint32_t channel, uint8_t os_ratio)
{
    if (!adc_ratio_is_valid(os_ratio))
    {
        return -1;
    }
    return adc3_read_channel(channel, os_ratio, ADC_SAMPLETIME_47CYCLES_5);
}

int32_t adc_read_adc4(uint32_t channel, uint8_t os_ratio)
{
    if (!adc_ratio_is_valid(os_ratio))
    {
        return -1;
    }
    return adc4_read_channel(channel, os_ratio, ADC_SAMPLETIME_47CYCLES_5);
}

/* ------------------------------------------------------------------------- */
/* Interrupt hooks                                                           */
/* ------------------------------------------------------------------------- */

/**
 * @brief DMA1 channel 1 interrupt: ADC1 circular transfers.
 *
 * HAL routes the transfer-complete event through HAL_ADC_ConvCpltCallback().
 * One complete lap equals ADC_STREAM_CAPACITY samples; bumping the lap count
 * here is what lets the consumer read an absolute producer position without
 * polling a potentially stale flag.
 */
void DMA1_Channel1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hdma_adc1);
}

/**
 * @brief ADC1 DMA transfer-complete callback (one full ring lap).
 *
 * This is not the per-conversion callback: with DMAContinuousRequests the DMA
 * transfer spans the whole ring. Updating the lap counter under the same
 * interrupt that the producer-position reader masks gives a coherent
 * (laps, CNDTR) pair.
 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc == &s_hadc1)
    {
        /* Only the lap count belongs in the ISR. The public total is
         * refreshed from (laps, CNDTR) inside the API, where a coherent
         * snapshot is available; writing it here would make it step
         * backwards if the reader had already counted a pending TC flag. */
        s_dma_laps++;
    }
}
