/**
 * @file  bsp_led.c
 * @brief Status LED driver for the WeAct STM32G474CEU6 "Long" board.
 *
 * The board has one monochrome LED on **PC6, wired active high** (PC6 -> 5.1k
 * -> LED anode -> cathode -> GND). There is no WS2812 and no RGB channel, so
 * the ESP32 firmware's colour/range language is reproduced here as brightness
 * and timing patterns:
 *
 *   - idle / safe to touch : slow, dim breathe
 *   - precharge            : medium 2 Hz pulse
 *   - biased               : steady full brightness ("do not touch")
 *   - measuring            : 8 Hz blink, brightness selects the active range
 *   - discharge            : short, slow pulse
 *   - fault                : rapid 10 Hz blink at full brightness
 *   - result               : 1/2/3 flash overlay for 600 ms
 *
 * @section pwm_scheme Software PWM
 *
 * PC6 is used as a plain GPIO (its timer channels stay free), so brightness
 * is produced by
 * duty-cycling the pin from led_tick(). The PWM frame is 8 ms (125 Hz), split
 * into eight 1 ms slots. An internal phase counter holds the current slot
 * (0..7) and is advanced by the number of milliseconds that elapsed since the
 * previous call. Driving the slot from elapsed time rather than from a call
 * count means the duty cycle stays correct when the main loop runs faster than
 * 1 kHz (many calls then share one slot).
 *
 * A 0..255 brightness is quantised to 0..8 "on" slots:
 *
 *     level = (brightness * 8 + 127) / 255
 *
 * and the LED is lit while phase < level. level == 0 is fully off and
 * level == 8 is fully on, so the eight visible steps are 12.5 %, 25 %, ...,
 * 100 %. Eight steps are deliberate: a main-loop-driven PWM cannot produce
 * 256 clean steps without a timer ISR, and this LED is an indicator, not a
 * lamp. The 1 kHz slot rate keeps flicker above the visible threshold.
 *
 * The caller must therefore invoke led_tick() at least once per millisecond,
 * ideally at ~1 kHz or faster; the scheduler does this. If calls are spaced
 * more than 8 ms apart the duty cycle becomes approximate (whole slots are
 * skipped), but the function still never waits and never blocks.
 *
 * @section wrapping Timestamp wrapping
 *
 * All timing uses uint32_t millisecond timestamps supplied by the caller.
 * Every interval is computed with unsigned subtraction, e.g.
 *
 *     elapsed = now_ms - start_ms;
 *
 * which is correct across the 2^32 ms wrap (~49.7 days) as long as an
 * individual interval is shorter than that, which every pattern here is.
 * No comparison against absolute time is ever made. The phase accumulator is
 * likewise advanced by the unsigned delta and masked to three bits.
 *
 * @section blocking Why this is non-blocking
 *
 * The measurement path is timing-critical. led_tick() performs a fixed,
 * small amount of work: a subtraction, a phase update, one pattern expression
 * (integer triangle or a modulo by a constant), a brightness quantisation and
 * at most one GPIO write. There is no loop, no delay, no busy-wait, and no
 * library transcendental. The GPIO write is a single register store inside
 * HAL_GPIO_WritePin(). The worst case is therefore bounded on the order of a
 * hundred CPU cycles (well under 1 us at 170 MHz), far below one 1 ms PWM
 * slot, and is independent of the animation phase. Animation can never delay
 * a sample.
 */

#include "bsp_led.h"
#include "config.h"

/* ------------------------------------------------------------------------- */
/* Private constants                                                         */
/* ------------------------------------------------------------------------- */

/** Number of software PWM slots per frame (three phase bits, 8 ms at 1 ms
 *  per slot, i.e. a 125 Hz refresh). */
#define LED_PWM_SLOTS       8U

/** Duration of the result overlay. Bounded so it cannot hide the next state. */
#define LED_OVERLAY_MS      600U

/* Pattern timing, in milliseconds. */
#define LED_IDLE_PERIOD_MS       3000U   /**< Slow breath, 0.33 Hz. */
#define LED_PRECHARGE_PERIOD_MS   500U   /**< 2 Hz pulse. */
#define LED_MEASURING_PERIOD_MS   125U   /**< 8 Hz blink. */
#define LED_DISCHARGE_PERIOD_MS  1000U   /**< One short pulse per second. */
#define LED_DISCHARGE_ON_MS       120U   /**< Pulse length. */
#define LED_FAULT_PERIOD_MS       100U   /**< 10 Hz blink. */

/* Pattern brightness values (0..255). */
#define LED_IDLE_MIN         4U
#define LED_IDLE_MAX        80U
#define LED_PRECHARGE_LEVEL 140U
#define LED_DISCHARGE_LEVEL 190U
#define LED_FULL            255U

/* ------------------------------------------------------------------------- */
/* Private state                                                             */
/* ------------------------------------------------------------------------- */

/** Last animated mode selected by led_set_mode(). */
static led_mode_t s_auto_mode;

/** Active range index (0..RANGE_COUNT-1), used by LED_MODE_MEASURING. */
static uint8_t s_range_idx;

/** Last animated pattern start, latched on the first tick after a change. */
static uint32_t s_pattern_start_ms;
static bool     s_pattern_restart;

/** Fixed-brightness selection made by led_manual(). */
static bool    s_manual_active;
static uint8_t s_manual_brightness;

/** Blank selection made by led_off(). */
static bool s_off_active;

/** Result overlay: a short, self-terminating pattern with priority. */
static bool     s_overlay_active;
static bool     s_overlay_restart;
static uint32_t s_overlay_start_ms;
static uint8_t  s_overlay_flashes;

/** Timestamp of the previous led_tick(), for delta computation. */
static uint32_t s_last_ms;
static bool     s_have_last;

/** Software PWM phase, 0..LED_PWM_SLOTS-1. */
static uint8_t s_pwm_phase;

/** Cached pin state to avoid redundant GPIO writes. */
static bool s_pin_on;

/* ------------------------------------------------------------------------- */
/* Private helpers                                                           */
/* ------------------------------------------------------------------------- */

/**
 * @brief Drive the LED pin, honouring the board's active-low wiring.
 *
 * Only writes the port when the requested state differs from the cached one,
 * which keeps the common case (same slot as the previous tick) to a compare.
 * The HAL call is a single BSRR/BRR store and cannot block.
 *
 * @param on true to emit light, false to extinguish it.
 */
static void led_drive(bool on)
{
    if (on == s_pin_on)
    {
        return;
    }
    s_pin_on = on;

#if LED_ACTIVE_LOW
    HAL_GPIO_WritePin(PIN_LED_PORT, PIN_LED,
                      on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#else
    HAL_GPIO_WritePin(PIN_LED_PORT, PIN_LED,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#endif
}

/**
 * @brief Integer triangle wave used for the idle breathe.
 *
 * The value rises linearly from @p low to @p high over the first half of the
 * period and falls back over the second half. A triangle is visually close to
 * a sine at these low brightness levels and costs one modulo, one comparison
 * and one multiply -- no sinf() in the hot path.
 *
 * @param elapsed_ms Milliseconds since the pattern started.
 * @param period_ms  Full triangle period.
 * @param low        Brightness at the trough.
 * @param high       Brightness at the peak.
 * @return Brightness in [low, high].
 */
static uint8_t led_triangle(uint32_t elapsed_ms, uint32_t period_ms,
                            uint8_t low, uint8_t high)
{
    uint32_t half = period_ms / 2U;
    uint32_t pos  = elapsed_ms % period_ms;

    if (pos >= half)
    {
        pos = period_ms - pos;
    }

    return (uint8_t)(low + (((uint32_t)(high - low) * pos) / half));
}

/**
 * @brief Brightness for one of the state-driven animations.
 *
 * @param elapsed_ms Milliseconds since the animation was (re)started.
 * @return Brightness in 0..255.
 */
static uint8_t led_pattern_brightness(uint32_t elapsed_ms)
{
    switch (s_auto_mode)
    {
        case LED_MODE_IDLE:
            /* Dim breath: never bright enough to be uncomfortable to touch. */
            return led_triangle(elapsed_ms, LED_IDLE_PERIOD_MS,
                                LED_IDLE_MIN, LED_IDLE_MAX);

        case LED_MODE_PRECHARGE:
            /* 2 Hz, half on / half off, medium brightness. */
            return ((elapsed_ms % LED_PRECHARGE_PERIOD_MS) <
                    (LED_PRECHARGE_PERIOD_MS / 2U))
                       ? (uint8_t)LED_PRECHARGE_LEVEL
                       : 0U;

        case LED_MODE_BIASED:
            /* Steady and unmistakable: the DUT is live. */
            return (uint8_t)LED_FULL;

        case LED_MODE_MEASURING:
        {
            /* 8 Hz blink; the duty within the "on" phase encodes the range:
             * range 0..3 -> 25 %, 50 %, 75 %, 100 %. */
            uint32_t range_level;

            if ((elapsed_ms % LED_MEASURING_PERIOD_MS) >=
                (LED_MEASURING_PERIOD_MS / 2U))
            {
                return 0U;
            }

            range_level = ((uint32_t)(s_range_idx + 1U) * 255U) / RANGE_COUNT;
            return (uint8_t)range_level;
        }

        case LED_MODE_DISCHARGE:
            /* A short pulse once per second: discharging continues. */
            return ((elapsed_ms % LED_DISCHARGE_PERIOD_MS) <
                    LED_DISCHARGE_ON_MS)
                       ? (uint8_t)LED_DISCHARGE_LEVEL
                       : 0U;

        case LED_MODE_FAULT:
            /* 10 Hz at full brightness. */
            return ((elapsed_ms % LED_FAULT_PERIOD_MS) <
                    (LED_FAULT_PERIOD_MS / 2U))
                       ? (uint8_t)LED_FULL
                       : 0U;

        case LED_MODE_OFF:
        default:
            return 0U;
    }
}

/**
 * @brief Brightness for the result overlay.
 *
 * clean = one long flash, low confidence = two flashes, rejected = three
 * quick flashes. The pulse/gap pairs are chosen so the whole sequence fits
 * inside LED_OVERLAY_MS; after the last pulse the LED is dark until the
 * overlay expires.
 *
 * @param elapsed_ms Milliseconds since the overlay started.
 * @return Brightness in 0..255.
 */
static uint8_t led_overlay_brightness(uint32_t elapsed_ms)
{
    /* Written as explicit edge comparisons rather than divide/modulo: the
     * pulse schedule is fixed and short, and this keeps the overlay path free
     * of a variable-divisor division on every tick. */
    switch (s_overlay_flashes)
    {
        case 1U:
            /* One 250 ms flash, then dark until the 600 ms overlay ends. */
            return (elapsed_ms < 250U) ? (uint8_t)LED_FULL : 0U;

        case 2U:
            /* Two 120 ms flashes separated by 100 ms. */
            if (elapsed_ms < 120U)
            {
                return (uint8_t)LED_FULL;
            }
            if (elapsed_ms < 220U)
            {
                return 0U;
            }
            if (elapsed_ms < 340U)
            {
                return (uint8_t)LED_FULL;
            }
            return 0U;

        case 3U:
        default:
            /* Three quick 70 ms flashes separated by 60 ms. */
            if (elapsed_ms < 70U)
            {
                return (uint8_t)LED_FULL;
            }
            if (elapsed_ms < 130U)
            {
                return 0U;
            }
            if (elapsed_ms < 200U)
            {
                return (uint8_t)LED_FULL;
            }
            if (elapsed_ms < 260U)
            {
                return 0U;
            }
            if (elapsed_ms < 330U)
            {
                return (uint8_t)LED_FULL;
            }
            return 0U;
    }
}

/**
 * @brief Resolve the brightness that should currently be displayed.
 *
 * Priority: result overlay, then explicit blank, then manual brightness, then
 * the state-driven animation. The selection underneath an overlay is left
 * untouched, so the overlay simply expires back onto it.
 *
 * @param now_ms Current caller timestamp.
 * @return Brightness in 0..255.
 */
static uint8_t led_current_brightness(uint32_t now_ms)
{
    if (s_overlay_active)
    {
        return led_overlay_brightness(now_ms - s_overlay_start_ms);
    }

    if (s_off_active)
    {
        return 0U;
    }

    if (s_manual_active)
    {
        return s_manual_brightness;
    }

    return led_pattern_brightness(now_ms - s_pattern_start_ms);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief Configure the user LED (PC6 on the WeAct Long board) and start off.
 *
 * Enables the GPIOC clock, configures PC6 as a low-speed push-pull output
 * with no pull resistor, and drives the inactive level for the board's
 * active-high wiring. Internal animation state is reset so a repeated call
 * returns the driver to a defined state.
 */
void led_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* Preload the inactive level before HAL_GPIO_Init switches PC6 to an
     * output so there is no boot flash. */
#if LED_ACTIVE_LOW
    HAL_GPIO_WritePin(PIN_LED_PORT, PIN_LED, GPIO_PIN_SET);
#else
    HAL_GPIO_WritePin(PIN_LED_PORT, PIN_LED, GPIO_PIN_RESET);
#endif

    gpio.Pin   = PIN_LED;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(PIN_LED_PORT, &gpio);

    s_auto_mode         = LED_MODE_OFF;
    s_range_idx         = 0U;
    s_pattern_start_ms  = 0U;
    s_pattern_restart   = true;
    s_manual_active     = false;
    s_manual_brightness = 0U;
    s_off_active        = false;
    s_overlay_active    = false;
    s_overlay_restart   = false;
    s_overlay_start_ms  = 0U;
    s_overlay_flashes   = 0U;

    s_last_ms   = 0U;
    s_have_last = false;
    s_pwm_phase = 0U;
    s_pin_on    = false; /* PC6 was preloaded dark above */
}

/**
 * @brief Advance the software PWM and the selected animation.
 *
 * O(1) and non-blocking: no loops, no delays, no floating point. Must be
 * called from the main loop at least once per millisecond (the scheduler calls
 * it at ~1 kHz). The timestamp is supplied by the caller so the driver never
 * reads HAL_GetTick() itself.
 *
 * See the file-level comment for the PWM scheme, the wrap-safe arithmetic and
 * why the worst-case execution time is a small constant.
 *
 * @param now_ms Current millisecond tick (may wrap at 2^32).
 */
void led_tick(uint32_t now_ms)
{
    uint32_t elapsed_ms;
    uint32_t level;
    uint8_t  brightness;
    bool     on;

    /* Elapsed time since the previous call. Unsigned subtraction makes this
     * correct across the 2^32 ms wrap. The first call has no predecessor, so
     * it contributes no phase advance. */
    if (s_have_last)
    {
        elapsed_ms = now_ms - s_last_ms;
    }
    else
    {
        elapsed_ms = 0U;
        s_have_last = true;
    }
    s_last_ms = now_ms;

    /* One PWM slot per elapsed millisecond, wrapping at 8 (125 Hz). */
    s_pwm_phase = (uint8_t)((s_pwm_phase + elapsed_ms) &
                            (LED_PWM_SLOTS - 1U));

    /* Let the overlay latch its start on the first tick after it was
     * requested, and retire it once its bounded lifetime has passed. */
    if (s_overlay_active)
    {
        if (s_overlay_restart)
        {
            s_overlay_start_ms = now_ms;
            s_overlay_restart  = false;
        }

        if ((uint32_t)(now_ms - s_overlay_start_ms) >= LED_OVERLAY_MS)
        {
            s_overlay_active  = false;
            s_pattern_restart = true; /* resume the animation from phase 0 */
        }
    }

    /* Latch pattern starts on the first tick after a selection change; this
     * avoids depending on a millisecond value captured at call time and keeps
     * the arithmetic wrap-safe. */
    if (s_pattern_restart)
    {
        s_pattern_start_ms = now_ms;
        s_pattern_restart  = false;
    }

    brightness = led_current_brightness(now_ms);

    /* Quantise 0..255 to 0..8 on-slots; 8 means fully on, 0 fully off. */
    level = ((uint32_t)brightness * LED_PWM_SLOTS + 127U) / 255U;
    on    = ((uint32_t)s_pwm_phase < level);

    led_drive(on);
}

/**
 * @brief Select the state-driven animation and remember it for led_set_auto().
 *
 * If a result overlay is running it is allowed to finish; the new mode is
 * already installed underneath and is shown when the overlay expires. Manual
 * and blank selections are cancelled.
 *
 * @param mode      Animation to run.
 * @param range_idx Active range index for LED_MODE_MEASURING (clamped).
 */
void led_set_mode(led_mode_t mode, uint8_t range_idx)
{
    /* Defensive clamp: an out-of-range enum falls back to "off". */
    if ((unsigned)mode > (unsigned)LED_MODE_FAULT)
    {
        mode = LED_MODE_OFF;
    }

    if (range_idx >= RANGE_COUNT)
    {
        range_idx = RANGE_COUNT - 1U;
    }

    s_auto_mode       = mode;
    s_range_idx       = range_idx;
    s_manual_active   = false;
    s_off_active      = false;
    s_pattern_restart = true;
}

/**
 * @brief Hold a fixed brightness, bypassing the animations.
 *
 * An explicit manual request cancels any result overlay. led_set_auto()
 * returns to the last mode selected with led_set_mode().
 *
 * @param brightness 0 (off) .. 255 (full).
 */
void led_manual(uint8_t brightness)
{
    s_manual_active     = true;
    s_manual_brightness = brightness;
    s_off_active        = false;
    s_overlay_active    = false;
}

/**
 * @brief Return to the last state-driven animation.
 *
 * Cancels manual, blank and overlay selections. The animation restarts from
 * its first phase so a stale timestamp warning is never shown.
 */
void led_set_auto(void)
{
    s_manual_active   = false;
    s_off_active      = false;
    s_overlay_active  = false;
    s_pattern_restart = true;
}

/**
 * @brief Blank the LED immediately.
 *
 * The last animated mode is retained, so led_set_auto() resumes it. Any
 * result overlay is cancelled: an explicit blank wins over transient feedback.
 */
void led_off(void)
{
    s_manual_active  = false;
    s_off_active     = true;
    s_overlay_active = false;
}

/**
 * @brief Flash the result of a measurement cycle.
 *
 * clean = 1 flash, low_confidence = 2 flashes, rejected = 3 quick flashes.
 * The overlay lasts at most LED_OVERLAY_MS (600 ms), is driven entirely from
 * led_tick(), and requires no delay. When it expires, whatever selection was
 * underneath (animated mode, manual brightness or blank) resumes.
 *
 * If several flags are set, rejected takes precedence over low_confidence,
 * which takes precedence over clean. With no flags set nothing is shown.
 *
 * @param clean          Cycle completed normally.
 * @param low_confidence Cycle completed but the result should be treated with
 *                       suspicion.
 * @param rejected       Cycle was rejected outright.
 */
void led_report_result(bool clean, bool low_confidence, bool rejected)
{
    if (rejected)
    {
        s_overlay_flashes = 3U;
    }
    else if (low_confidence)
    {
        s_overlay_flashes = 2U;
    }
    else if (clean)
    {
        s_overlay_flashes = 1U;
    }
    else
    {
        return;
    }

    s_overlay_active  = true;
    s_overlay_restart = true;
}
