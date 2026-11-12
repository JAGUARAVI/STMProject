/**
 * @file  bsp_opamp.c
 * @brief OPAMP1: the R-mode current-source error amplifier.
 *
 * Chosen deliberately over OPAMP2. On STM32G4 only OPAMP1, OPAMP3, OPAMP4,
 * OPAMP5 and OPAMP6 have an internal path from a DAC to VINP; **OPAMP2 has
 * none**. Of the instances with a DAC input, only 1, 2 and 3 are bonded out on
 * UFQFPN48, and 4/5/6 need PD/PE pins that do not exist here.
 *
 * Configuration is standalone (external loop) so Q1 and the DUT close the
 * servo:
 *
 *   VINP = PA7 = B    (OPAMP1 VINP2 -> OPAMP_NONINVERTINGINPUT_IO2)
 *   VINM = PA3 = VREF (OPAMP1 VINM0  -> OPAMP_INVERTINGINPUT_IO0)
 *   VOUT = PA2        (Q1 gate)
 *
 * Note the HAL numbering: for OPAMP1, VINP0 = PA1, VINP1 = PA3, **VINP2 =
 * PA7**. `OPAMP_NONINVERTINGINPUT_IO0` would select PA1 (= V_cap), which is
 * the classic mistake in the handoff. IO2 is the correct constant.
 *
 * `InternalOutput` is **disabled** because VOUT must reach PA2 to drive the
 * pass FET. (Enabling it would route VOUT to an ADC channel and stop driving
 * the pin — useful for a buffer, wrong for a regulator.)
 */

#include "bsp_opamp.h"

#include "config.h"

static OPAMP_HandleTypeDef s_hopamp1;
static bool s_ready;
static bool s_enabled;

/** PA2 is VOUT, PA3 is VINM, PA7 is VINP. PA1 is already analog for the ADC
 *  and comparator; it is not an OPAMP1 input in this configuration. */
static void opamp1_pins_analog(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin = GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);
}

bool bsp_opamp1_init(void)
{
    if (s_ready) {
        return true;
    }

    __HAL_RCC_SYSCFG_CLK_ENABLE();

    opamp1_pins_analog();

    s_hopamp1.Instance = OPAMP1;
    s_hopamp1.Init.PowerMode = OPAMP_POWERMODE_NORMALSPEED;
    s_hopamp1.Init.Mode = OPAMP_STANDALONE_MODE;
    s_hopamp1.Init.InvertingInput = OPAMP_INVERTINGINPUT_IO0;      /* PA3 = VREF */
    s_hopamp1.Init.NonInvertingInput = OPAMP_NONINVERTINGINPUT_IO2; /* PA7 = B */
    s_hopamp1.Init.InternalOutput = DISABLE;                        /* need PA2 */
    s_hopamp1.Init.TimerControlledMuxmode = OPAMP_TIMERCONTROLLEDMUXMODE_DISABLE;
    s_hopamp1.Init.InvertingInputSecondary = OPAMP_SEC_INVERTINGINPUT_IO0;
    s_hopamp1.Init.NonInvertingInputSecondary = OPAMP_SEC_NONINVERTINGINPUT_IO0;
    s_hopamp1.Init.PgaConnect = OPAMP_PGA_CONNECT_INVERTINGINPUT_NO;
    s_hopamp1.Init.PgaGain = OPAMP_PGA_GAIN_2_OR_MINUS_1;
    s_hopamp1.Init.UserTrimming = OPAMP_TRIMMING_FACTORY;
    s_hopamp1.Init.TrimmingValueP = 0;
    s_hopamp1.Init.TrimmingValueN = 0;

    if (HAL_OPAMP_Init(&s_hopamp1) != HAL_OK) {
        return false;
    }

    s_ready = true;
    s_enabled = false;
    return true;
}

void bsp_opamp1_enable(bool enable)
{
    if (!s_ready) {
        return;
    }

    if (enable) {
        if (!s_enabled) {
            if (HAL_OPAMP_Start(&s_hopamp1) == HAL_OK) {
                s_enabled = true;
            }
        }
    } else {
        if (s_enabled) {
            (void)HAL_OPAMP_Stop(&s_hopamp1);
            s_enabled = false;
        }
    }
}

bool bsp_opamp1_enabled(void)
{
    return s_enabled;
}

/**
 * @brief Offset self-calibration.
 *
 * The HAL routine needs to observe the CALOUT flag, which is read from the
 * output. That is available here because the output is routed to the pin.
 * Best-effort: a timeout is not fatal for a current-source error amplifier
 * whose offset largely cancels in the ratiometric resistance maths.
 */
bool bsp_opamp1_calibrate(void)
{
    if (!s_ready) {
        return false;
    }
    if (!s_enabled) {
        bsp_opamp1_enable(true);
        if (!s_enabled) {
            return false;
        }
    }
    return HAL_OPAMP_SelfCalibrate(&s_hopamp1) == HAL_OK;
}
