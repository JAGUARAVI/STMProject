/**
 * @file  bsp_opamp3.c
 * @brief OPAMP3: unity-gain buffer for the R-mode V_N sense node.
 *
 * V_N reaches the MCU through 1 kohm + 1 Mohm and a BAT54S clamp, so the
 * source impedance is high enough that a direct ADC read would be corrupted by
 * the sample-and-hold current. OPAMP3 is bonded on UFQFPN48 and costs no
 * external parts:
 *
 *   VINP = PB0 (V_N_SENSE)
 *   VOUT = PB1 (ADC3_IN1)
 *   VINM = internally tied to VOUT in follower mode; the PB2 pin is not used
 *          (it is the R-mux ENABLE output)
 *
 * The internal output path to ADC3 is deliberately not used: PB1 is bonded and
 * ADC3_IN1 is documented, so the pin route is the one that can be verified on
 * the bench.
 */

#include "bsp_opamp3.h"

#include "config.h"

static OPAMP_HandleTypeDef s_hopamp3;
static bool s_ready;
static bool s_enabled;

static void opamp3_pins_analog(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* Only PB0 (VINP) and PB1 (VOUT) are analogue. PB2 is the OPAMP3 VINM0
     * pin, but follower mode connects VINM internally, so the pin is NOT used
     * by the op-amp. It is the R-mux ENABLE output (PIN_MUXR_EN) and must stay
     * a GPIO. This was a real conflict in the first combined-map draft. */
    gpio.Pin = PIN_VN | PIN_VN_OUT;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &gpio);
}

bool bsp_opamp3_init(void)
{
    if (s_ready) {
        return true;
    }

    __HAL_RCC_SYSCFG_CLK_ENABLE();

    opamp3_pins_analog();

    s_hopamp3.Instance = OPAMP3;
    s_hopamp3.Init.PowerMode = OPAMP_POWERMODE_NORMALSPEED;
    s_hopamp3.Init.Mode = OPAMP_FOLLOWER_MODE;
    s_hopamp3.Init.InvertingInput = OPAMP_INVERTINGINPUT_IO0;      /* internal */
    s_hopamp3.Init.NonInvertingInput = OPAMP_NONINVERTINGINPUT_IO0; /* PB0 */
    s_hopamp3.Init.InternalOutput = DISABLE;                        /* drive PB1 */
    s_hopamp3.Init.TimerControlledMuxmode = OPAMP_TIMERCONTROLLEDMUXMODE_DISABLE;
    s_hopamp3.Init.InvertingInputSecondary = OPAMP_SEC_INVERTINGINPUT_IO0;
    s_hopamp3.Init.NonInvertingInputSecondary = OPAMP_SEC_NONINVERTINGINPUT_IO0;
    s_hopamp3.Init.PgaConnect = OPAMP_PGA_CONNECT_INVERTINGINPUT_NO;
    s_hopamp3.Init.PgaGain = OPAMP_PGA_GAIN_2_OR_MINUS_1;
    s_hopamp3.Init.UserTrimming = OPAMP_TRIMMING_FACTORY;
    s_hopamp3.Init.TrimmingValueP = 0;
    s_hopamp3.Init.TrimmingValueN = 0;

    if (HAL_OPAMP_Init(&s_hopamp3) != HAL_OK) {
        return false;
    }

    s_ready = true;
    s_enabled = false;
    return true;
}

void bsp_opamp3_enable(bool enable)
{
    if (!s_ready) {
        return;
    }

    if (enable) {
        if (!s_enabled) {
            if (HAL_OPAMP_Start(&s_hopamp3) == HAL_OK) {
                s_enabled = true;
            }
        }
    } else {
        if (s_enabled) {
            (void)HAL_OPAMP_Stop(&s_hopamp3);
            s_enabled = false;
        }
    }
}

bool bsp_opamp3_enabled(void)
{
    return s_enabled;
}
