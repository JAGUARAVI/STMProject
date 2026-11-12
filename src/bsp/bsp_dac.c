/**
 * @file  bsp_dac.c
 * @brief Internal DAC references for the comparators.
 *
 * Two channels, both **internal-only** so they cost no I/O and are not exposed
 * to board noise:
 *
 *   DAC3_CH1 -> COMP1_INM   C-mode oscillator threshold (1.65 V)
 *   DAC1_CH1 -> COMP4_INM   R-mode V_N overvoltage threshold (~2.6 V)
 *
 * Neither DAC output pin is bonded on UFQFPN48, and neither needs to be: both
 * loads are high-impedance comparator inputs. The channels are configured
 * **unbuffered** (`DAC_CHIPCONNECT_INTERNAL`, output buffer disabled). An
 * unbuffered update settles in tens of nanoseconds instead of the ~1.4 us a
 * buffered pin output needs into 5 kohm.
 *
 * DAC1_CH1 is internal-only, so PA4 remains free for the header. (PA4 is the
 * DAC1_OUT1 pin, but `DAC_CHIPCONNECT_INTERNAL` keeps it disconnected.)
 */

#include "bsp_dac.h"

#include "config.h"
#include "bsp_adc.h"

static DAC_HandleTypeDef s_hdac3;
static DAC_HandleTypeDef s_hdac1;
static uint16_t s_mv3;
static uint16_t s_mv1;
static bool s_ready3;
static bool s_ready1;

/** Millivolt scale for a DAC. The DAC and the ADC share VREF+, so tracking
 *  the measured VDDA keeps a threshold correct even if the rail is not exactly
 *  3.3 V. Falls back to the nominal rail before the ADC has measured it. */
static uint32_t dac_vref_mv(void)
{
    uint32_t v = adc_vdda_mv();
    return (v >= 1800U && v <= 3600U) ? v : 3300U;
}

uint32_t bsp_dac3_vref_mv(void)
{
    return dac_vref_mv();
}

/** Common channel setup. Trigger is `DAC_TRIGGER_NONE` because the value is
 *  written directly: there is nothing to synchronise a threshold update to. */
static bool dac_channel_config(DAC_HandleTypeDef *hdac, uint32_t channel)
{
    DAC_ChannelConfTypeDef ch;

    ch.DAC_HighFrequency = DAC_HIGH_FREQUENCY_INTERFACE_MODE_DISABLE;
    ch.DAC_DMADoubleDataMode = DISABLE;
    ch.DAC_SignedFormat = DISABLE;
    ch.DAC_SampleAndHold = DAC_SAMPLEANDHOLD_DISABLE;
    ch.DAC_Trigger = DAC_TRIGGER_NONE;
    ch.DAC_Trigger2 = DAC_TRIGGER_NONE;
    ch.DAC_OutputBuffer = DAC_OUTPUTBUFFER_DISABLE;
    ch.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_INTERNAL;
    ch.DAC_UserTrimming = DAC_TRIMMING_FACTORY;
    ch.DAC_TrimmingValue = 0;
    ch.DAC_SampleAndHoldConfig.DAC_SampleTime = 0;
    ch.DAC_SampleAndHoldConfig.DAC_HoldTime = 0;
    ch.DAC_SampleAndHoldConfig.DAC_RefreshTime = 0;

    return HAL_DAC_ConfigChannel(hdac, &ch, channel) == HAL_OK;
}

static uint32_t dac_code(uint16_t mv, uint32_t vref)
{
    uint32_t code = ((uint32_t)mv * 4095U) / vref;
    return (code > 4095U) ? 4095U : code;
}

bool bsp_dac3_init(void)
{
    if (s_ready3) {
        return true;
    }

    __HAL_RCC_DAC3_CLK_ENABLE();

    s_hdac3.Instance = DAC3;
    if (HAL_DAC_Init(&s_hdac3) != HAL_OK) {
        return false;
    }
    if (!dac_channel_config(&s_hdac3, DAC_CHANNEL_1)) {
        return false;
    }

    s_mv3 = 1650;
    if (HAL_DAC_SetValue(&s_hdac3, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
                         dac_code(s_mv3, dac_vref_mv())) != HAL_OK) {
        return false;
    }

    s_ready3 = true;
    return true;
}

void bsp_dac3_set_mv(uint16_t mv)
{
    if (!s_ready3) {
        return;
    }

    uint32_t vref = dac_vref_mv();
    if (mv > vref) {
        mv = (uint16_t)vref;
    }

    (void)HAL_DAC_SetValue(&s_hdac3, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
                           dac_code(mv, vref));
    s_mv3 = mv;
}

uint16_t bsp_dac3_get_mv(void)
{
    return s_mv3;
}

void bsp_dac3_enable(bool enable)
{
    if (!s_ready3) {
        return;
    }
    if (enable) {
        (void)HAL_DAC_Start(&s_hdac3, DAC_CHANNEL_1);
    } else {
        (void)HAL_DAC_Stop(&s_hdac3, DAC_CHANNEL_1);
    }
}

bool bsp_dac1_init(void)
{
    if (s_ready1) {
        return true;
    }

    __HAL_RCC_DAC1_CLK_ENABLE();

    s_hdac1.Instance = DAC1;
    if (HAL_DAC_Init(&s_hdac1) != HAL_OK) {
        return false;
    }
    if (!dac_channel_config(&s_hdac1, DAC_CHANNEL_1)) {
        return false;
    }

    /* 2.6 V: V_N must stay below ~2.3 V for a valid Kelvin reading, so this is
     * a fault threshold, not an operating point. */
    s_mv1 = 2600;
    if (HAL_DAC_SetValue(&s_hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
                         dac_code(s_mv1, dac_vref_mv())) != HAL_OK) {
        return false;
    }

    s_ready1 = true;
    return true;
}

void bsp_dac1_set_mv(uint16_t mv)
{
    if (!s_ready1) {
        return;
    }

    uint32_t vref = dac_vref_mv();
    if (mv > vref) {
        mv = (uint16_t)vref;
    }

    (void)HAL_DAC_SetValue(&s_hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R,
                           dac_code(mv, vref));
    s_mv1 = mv;
}

void bsp_dac1_enable(bool enable)
{
    if (!s_ready1) {
        return;
    }
    if (enable) {
        (void)HAL_DAC_Start(&s_hdac1, DAC_CHANNEL_1);
    } else {
        (void)HAL_DAC_Stop(&s_hdac1, DAC_CHANNEL_1);
    }
}
