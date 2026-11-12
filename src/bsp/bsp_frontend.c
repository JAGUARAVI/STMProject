/**
 * @file  bsp_frontend.c
 * @brief Front-end safety state machine, relay/mux control and mode interlock.
 *
 * ============================================================================
 * SAFETY INVARIANTS — DO NOT "SIMPLIFY" THESE
 * ============================================================================
 *
 * 1. SSR1 (pre-charge, V_BIAS -> 10 R -> N_DUT) and SSR3 (V_cap clamp,
 *    V_cap -> 1 R -> GND) SHARE ONE PIN. Enabling one always enables the
 *    other. That is the hardware interlock that stops V_cap rising while
 *    N_DUT is charged. Never split it in firmware.
 *
 * 2. SSR2 (discharge, N_DUT -> 100 R -> GND) MUST NEVER BE CLOSED DURING A
 *    C-MODE MEASUREMENT. Keeping it open is what lets the DUT stay biased
 *    through R_bias while only the MCU-side node is reset.
 *
 * 3. All switches off does NOT mean discharged. With SSR2 off the DUT and
 *    C_block remain at V_BIAS through R_bias. The persistent charge state is
 *    tracked separately from the momentary phase.
 *
 * 4. K2 (KELVIN) MUST NEVER BE ENERGISED UNLESS K1 (R_MODE) IS ENERGISED.
 *    K1 lifts B and isolates C_block; K2 connects B to the R mux.
 *
 * 5. K1B IS NORMALLY CLOSED. Never DE-ENERGISE K1 while N_DUT is biased:
 *    that would reconnect C_block to a charged node. Discharge first.
 *
 * 6. The R mux must be disabled in TIA (K2A is open) and the C mux must be
 *    disabled in R mode.
 *
 * Polarity: the SSR and relay drive inputs are assumed ACTIVE HIGH (GPIO high
 * energises the coil/switch), matching the ESP32 firmware. The MAX4619 ENABLE
 * pins are ACTIVE LOW. Both assumptions are isolated behind the helpers below
 * so a board inversion is a one-line change.
 */

#include "bsp_frontend.h"

#include "bsp.h"
#include "bsp_opamp.h"
#include "bsp_opamp3.h"
#include "config.h"

#define FE_SSR_ACTIVE_HIGH      1
#define FE_RELAY_ACTIVE_HIGH    1
#define FE_MUX_EN_ACTIVE_LOW    1

static fe_state_t s_state = FE_IDLE;
static op_mode_t  s_mode  = OP_MODE_CAP;

static bool s_charged;
static bool s_discharged;
static bool s_r_active;
static bool s_fault;

static uint8_t s_range = 2;

/* ------------------------------------------------------------------------- */
/* Low-level helpers                                                         */
/* ------------------------------------------------------------------------- */

static void write_ssr(GPIO_TypeDef *port, uint16_t pin, bool on)
{
#if FE_SSR_ACTIVE_HIGH
    HAL_GPIO_WritePin(port, pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
    HAL_GPIO_WritePin(port, pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#endif
}

static void write_relay(GPIO_TypeDef *port, uint16_t pin, bool on)
{
#if FE_RELAY_ACTIVE_HIGH
    HAL_GPIO_WritePin(port, pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
    HAL_GPIO_WritePin(port, pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#endif
}

static void write_mux_en(GPIO_TypeDef *port, uint16_t pin, bool enable)
{
#if FE_MUX_EN_ACTIVE_LOW
    HAL_GPIO_WritePin(port, pin, enable ? GPIO_PIN_RESET : GPIO_PIN_SET);
#else
    HAL_GPIO_WritePin(port, pin, enable ? GPIO_PIN_SET : GPIO_PIN_RESET);
#endif
}

static void muxc_enable(bool enable)
{
    write_mux_en(PIN_MUXC_EN_PORT, PIN_MUXC_EN, enable);
}

static void muxr_enable(bool enable)
{
    write_mux_en(PIN_MUXR_EN_PORT, PIN_MUXR_EN, enable);
}

#if ESWCAP_HAS_RMODE
static void muxr_select(uint8_t element)
{
    HAL_GPIO_WritePin(PIN_MUXR_A0_PORT, PIN_MUXR_A0,
                      (element & 0x1U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PIN_MUXR_A1_PORT, PIN_MUXR_A1,
                      (element & 0x2U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
#endif

/** Q1 gate override high-Z. The external 100k pull-up R13 keeps Q1 off. */
static void rforce_hiz(void)
{
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin  = PIN_RFORCE_OVR;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(PIN_RFORCE_OVR_PORT, &gpio);
}

#if ESWCAP_HAS_RMODE
/** Drive the Q1 gate override low: Q1 fully on, used only in TIA. */
static void rforce_low(void)
{
    GPIO_InitTypeDef gpio = {0};

    HAL_GPIO_WritePin(PIN_RFORCE_OVR_PORT, PIN_RFORCE_OVR, GPIO_PIN_RESET);
    gpio.Pin  = PIN_RFORCE_OVR;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(PIN_RFORCE_OVR_PORT, &gpio);
}
#endif

/** Interlocked relay write: K2 is forced off unless K1 is on. */
static void relay_set(bool k1, bool k2)
{
    if (k2 && !k1) {
        k2 = false;
    }
    write_relay(PIN_K1_PORT, PIN_K1, k1);
    write_relay(PIN_K2_PORT, PIN_K2, k2);
}

/** Tear down every path that can deliver current to the DUT. */
static void force_path_off(void)
{
    bsp_opamp1_enable(false);
    relay_set(false, false);
    muxr_enable(false);
    rforce_hiz();
    muxc_enable(false);
}

/* ------------------------------------------------------------------------- */
/* Init                                                                      */
/* ------------------------------------------------------------------------- */

static void gpio_out_low(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef gpio = {0};

    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
    gpio.Pin   = pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(port, &gpio);
}

static void gpio_out_high(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef gpio = {0};

    HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);
    gpio.Pin   = pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(port, &gpio);
}

bool fe_init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* The drive pin (PA0) starts as a GPIO output low. bsp_comp.c switches it to AF8 when the
     * oscillator runs and back when it stops. */
    gpio_out_low(PIN_DRIVE_PORT, PIN_DRIVE);

    /* C-mode safety bank. */
    gpio_out_low(PIN_SSR13_PORT, PIN_SSR13);
    gpio_out_low(PIN_SSR2_PORT, PIN_SSR2);
    gpio_out_low(PIN_VBIAS_PORT, PIN_VBIAS);

    /* C mux: address low, ENABLE high (active-low -> disabled). PB3/PB4 are
     * the JTAG pins; using them as GPIO disables JTAG but leaves SWD. */
    gpio_out_low(PIN_MUXC_A0_PORT, PIN_MUXC_A0);
    gpio_out_low(PIN_MUXC_A1_PORT, PIN_MUXC_A1);
    gpio_out_high(PIN_MUXC_EN_PORT, PIN_MUXC_EN);

    /* R mux: address low, ENABLE high (disabled). */
    gpio_out_low(PIN_MUXR_A0_PORT, PIN_MUXR_A0);
    gpio_out_low(PIN_MUXR_A1_PORT, PIN_MUXR_A1);
    gpio_out_high(PIN_MUXR_EN_PORT, PIN_MUXR_EN);

    /* Relays: both off. K1 off = B grounded, C_block connected (C mode). */
    gpio_out_low(PIN_K1_PORT, PIN_K1);
    gpio_out_low(PIN_K2_PORT, PIN_K2);

    /* Q1 gate override high-Z; R13 pulls the gate up and Q1 off. */
    rforce_hiz();

    s_state      = FE_IDLE;
    s_mode       = OP_MODE_CAP;
    s_charged    = false;
    s_discharged = true;
    s_r_active   = false;
    s_fault      = false;
    s_range      = 2;

    return true;
}

/* ------------------------------------------------------------------------- */
/* C-mode control                                                            */
/* ------------------------------------------------------------------------- */

void fe_set_drive(bool high)
{
    /* Only has an effect while PA0 is a GPIO (the comparator loop is
     * stopped). bsp_comp.c owns the AF/GPIO switch. */
    HAL_GPIO_WritePin(PIN_DRIVE_PORT, PIN_DRIVE,
                      high ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void fe_select_range(uint8_t range_idx)
{
    if (range_idx >= RANGE_COUNT) {
        range_idx = (uint8_t)(RANGE_COUNT - 1U);
    }

    s_range = range_idx;

    uint8_t code = g_ranges[range_idx].mux_channel;
    HAL_GPIO_WritePin(PIN_MUXC_A0_PORT, PIN_MUXC_A0,
                      (code & 0x1U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(PIN_MUXC_A1_PORT, PIN_MUXC_A1,
                      (code & 0x2U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

uint8_t fe_selected_range(void)
{
    return s_range;
}

void fe_idle(void)
{
    fe_set_drive(false);
    force_path_off();
    write_ssr(PIN_SSR13_PORT, PIN_SSR13, false);
    write_ssr(PIN_SSR2_PORT, PIN_SSR2, false);
    HAL_GPIO_WritePin(PIN_VBIAS_PORT, PIN_VBIAS, GPIO_PIN_RESET);

    s_state = FE_IDLE;
}

void fe_precharge(void)
{
    fe_set_drive(false);
    force_path_off();
    write_ssr(PIN_SSR2_PORT, PIN_SSR2, false);
    HAL_GPIO_WritePin(PIN_VBIAS_PORT, PIN_VBIAS, GPIO_PIN_RESET);

    /* SSR1 + SSR3 together: charge N_DUT toward V_BIAS through 10 R while
     * clamping V_cap to GND through 1 R. */
    write_ssr(PIN_SSR13_PORT, PIN_SSR13, true);
    bsp_delay_ms(PRECHARGE_HOLD_MS);
    write_ssr(PIN_SSR13_PORT, PIN_SSR13, false);

    s_charged = true;
    s_discharged = false;
    s_state = FE_PRECHARGED;

    bsp_delay_us(ISOLATION_DELAY_US);
}

void fe_discharge(void)
{
    fe_set_drive(false);
    force_path_off();
    write_ssr(PIN_SSR13_PORT, PIN_SSR13, false);
    HAL_GPIO_WritePin(PIN_VBIAS_PORT, PIN_VBIAS, GPIO_PIN_RESET);

    s_state = FE_DISCHARGE;
    write_ssr(PIN_SSR2_PORT, PIN_SSR2, true);
    bsp_delay_ms(DISCHARGE_HOLD_MS);
    write_ssr(PIN_SSR2_PORT, PIN_SSR2, false);

    s_charged = false;
    s_discharged = true;
    s_state = FE_IDLE;
}

void fe_prepare_measurement(bool auto_precharge)
{
    fe_set_drive(false);

    /* C mode only: the R force amplifier must be off. */
    bsp_opamp1_enable(false);

    /* NEVER close SSR2 here: the DUT bias must be preserved. */
    write_ssr(PIN_SSR2_PORT, PIN_SSR2, false);
    HAL_GPIO_WritePin(PIN_VBIAS_PORT, PIN_VBIAS, GPIO_PIN_RESET);

    /* R-mode paths must be off in C mode. */
    relay_set(false, false);
    muxr_enable(false);
    rforce_hiz();

    if (auto_precharge) {
        write_ssr(PIN_SSR13_PORT, PIN_SSR13, true);
        bsp_delay_ms(PRECHARGE_HOLD_MS);
        write_ssr(PIN_SSR13_PORT, PIN_SSR13, false);
        s_charged = true;
        s_discharged = false;
    } else {
        write_ssr(PIN_SSR13_PORT, PIN_SSR13, false);
    }

    bsp_delay_us(ISOLATION_DELAY_US);

    /* Connect the selected range resistor to V_cap. */
    fe_select_range(s_range);
    muxc_enable(true);

    s_state = FE_MEASURING;
}

void fe_finish(bool auto_discharge, bool charged)
{
    if (auto_discharge) {
        fe_discharge();
        s_state = FE_IDLE;
    } else {
        fe_idle();
        s_state = charged ? FE_PRECHARGED : FE_IDLE;
    }
}

/* ------------------------------------------------------------------------- */
/* State accessors                                                           */
/* ------------------------------------------------------------------------- */

fe_state_t fe_state(void)
{
    return s_state;
}

void fe_set_state(fe_state_t s)
{
    s_state = s;
}

bool fe_charged(void)
{
    return s_charged;
}

bool fe_discharged(void)
{
    return s_discharged;
}

void fe_set_charged(bool charged)
{
    s_charged = charged;
    if (charged) {
        s_discharged = false;
    }
}

uint8_t fe_pin_bits(void)
{
    uint8_t bits = 0;

    if (HAL_GPIO_ReadPin(PIN_SSR13_PORT, PIN_SSR13) == GPIO_PIN_SET) bits |= 0x01U;
    if (HAL_GPIO_ReadPin(PIN_SSR2_PORT, PIN_SSR2) == GPIO_PIN_SET)   bits |= 0x02U;
    if (HAL_GPIO_ReadPin(PIN_DRIVE_PORT, PIN_DRIVE) == GPIO_PIN_SET) bits |= 0x04U;
    if (HAL_GPIO_ReadPin(PIN_K1_PORT, PIN_K1) == GPIO_PIN_SET)       bits |= 0x08U;
    if (HAL_GPIO_ReadPin(PIN_K2_PORT, PIN_K2) == GPIO_PIN_SET)       bits |= 0x10U;

#if FE_MUX_EN_ACTIVE_LOW
    if (HAL_GPIO_ReadPin(PIN_MUXR_EN_PORT, PIN_MUXR_EN) == GPIO_PIN_RESET) bits |= 0x20U;
    if (HAL_GPIO_ReadPin(PIN_MUXC_EN_PORT, PIN_MUXC_EN) == GPIO_PIN_RESET) bits |= 0x40U;
#else
    if (HAL_GPIO_ReadPin(PIN_MUXR_EN_PORT, PIN_MUXR_EN) == GPIO_PIN_SET) bits |= 0x20U;
    if (HAL_GPIO_ReadPin(PIN_MUXC_EN_PORT, PIN_MUXC_EN) == GPIO_PIN_SET) bits |= 0x40U;
#endif

    if (HAL_GPIO_ReadPin(PIN_RFORCE_OVR_PORT, PIN_RFORCE_OVR) == GPIO_PIN_RESET) bits |= 0x80U;

    return bits;
}

/* ------------------------------------------------------------------------- */
/* Mode interlock                                                            */
/* ------------------------------------------------------------------------- */

bool fe_set_mode(op_mode_t mode)
{
    if (mode == s_mode) {
        return true;
    }

    if (mode == OP_MODE_RES) {
#if !ESWCAP_HAS_RMODE
        return false;
#else
        /* The R force path must not be connected to a charged C-mode node. */
        if (s_charged) {
            return false;
        }
        if (s_r_active) {
            return true;
        }

        fe_set_drive(false);
        force_path_off();
        write_ssr(PIN_SSR13_PORT, PIN_SSR13, false);
        write_ssr(PIN_SSR2_PORT, PIN_SSR2, false);
        HAL_GPIO_WritePin(PIN_VBIAS_PORT, PIN_VBIAS, GPIO_PIN_RESET);

        /* A previous overvoltage latch is cleared only by a deliberate mode
         * change, never by the ISR itself, so `status` still reports what
         * happened until the operator acts. */
        s_fault = false;

        s_mode = OP_MODE_RES;
        return true;
#endif
    }

    /* Back to C mode. The R session must already be torn down. */
    if (s_r_active) {
        return false;
    }
    if (s_charged) {
        return false;
    }

    force_path_off();
    bsp_opamp3_enable(false);
    write_ssr(PIN_SSR13_PORT, PIN_SSR13, false);
    write_ssr(PIN_SSR2_PORT, PIN_SSR2, false);
    HAL_GPIO_WritePin(PIN_VBIAS_PORT, PIN_VBIAS, GPIO_PIN_RESET);

    s_mode = OP_MODE_CAP;
    return true;
}

op_mode_t fe_mode(void)
{
    return s_mode;
}

/* ------------------------------------------------------------------------- */
/* R-mode control                                                            */
/* ------------------------------------------------------------------------- */

bool fe_r_enter(void)
{
#if !ESWCAP_HAS_RMODE
    return false;
#else
    if (s_mode != OP_MODE_RES || s_r_active || s_fault) {
        return false;
    }
    if (s_charged) {
        return false;
    }

    /* Conservative pre-condition: bleed whatever the DUT/bias network may
     * still hold, then lift B and isolate C_block. */
    fe_discharge();

    fe_set_drive(false);
    force_path_off();

    /* K1 first: B is lifted from GND and C_block is disconnected from N_DUT. */
    relay_set(true, false);
    bsp_delay_ms(RMODE_RELAY_SETTLE_MS);

    /* V_N buffer on; the node is still undriven. */
    if (!bsp_opamp3_init()) {
        relay_set(false, false);
        return false;
    }
    bsp_opamp3_enable(true);
    if (!bsp_opamp3_enabled()) {
        relay_set(false, false);
        return false;
    }
    bsp_delay_ms(1U);

    s_r_active = true;
    s_state = FE_R_ISOLATE;
    return true;
#endif
}

bool fe_r_kelvin(uint8_t element)
{
#if !ESWCAP_HAS_RMODE
    (void)element;
    return false;
#else
    if (!s_r_active || s_fault || element >= RMODE_ELEMENTS) {
        return false;
    }
    if (!bsp_opamp3_enabled()) {
        return false;   /* V_N would be meaningless without the buffer */
    }

    /* Make sure the error amplifier is off before the mux/relay move. */
    bsp_opamp1_enable(false);
    rforce_hiz();

    muxr_select(element);
    muxr_enable(true);
    bsp_delay_us(1000U);

    /* K2 connects B to the R mux and opens the TIA feedback. */
    relay_set(true, true);
    bsp_delay_ms(RMODE_RELAY_SETTLE_MS);

    bsp_opamp1_enable(true);
    bsp_delay_ms(RMODE_LOOP_SETTLE_MS);

    s_state = FE_R_KELVIN;
    return bsp_opamp1_enabled();
#endif
}

void fe_r_kelvin_off(void)
{
    bsp_opamp1_enable(false);

    /* Remove the RSENSE load BEFORE K2 closes the TIA feedback, otherwise the
     * TIA would briefly try to hold B at VBIAS through the low-value sense
     * resistor. */
    muxr_enable(false);
    relay_set(true, false);
    rforce_hiz();

    if (s_r_active) {
        s_state = FE_R_ISOLATE;
    }
}

bool fe_r_tia(void)
{
#if !ESWCAP_HAS_RMODE
    return false;
#else
    if (!s_r_active || s_fault) {
        return false;
    }
    if (!bsp_opamp3_enabled()) {
        return false;
    }

    bsp_opamp1_enable(false);

    /* Disconnect the RSENSE bank first; then K2B closes the TIA feedback. */
    muxr_enable(false);
    relay_set(true, false);      /* K2 off: K2B closes the TIA feedback */
    bsp_delay_ms(RMODE_RELAY_SETTLE_MS);

    /* Q1 fully on. The LMC6482 A owns the loop through Rf. */
    rforce_low();
    bsp_delay_ms(RMODE_LOOP_SETTLE_MS);

    s_state = FE_R_TIA;
    return true;
#endif
}

void fe_r_exit(void)
{
    /* Order matters. K1B is normally closed, so de-energising K1 reconnects
     * C_block to N_DUT. If N_DUT is still at the Kelvin/TIA force voltage,
     * that is a low-impedance step into a 936 uF capacitor — exactly what the
     * "never close K1B with N_DUT biased" invariant forbids.
     *
     * Keep K1 energised (B lifted, C_block isolated) while SSR2 bleeds N_DUT
     * through its 100 ohm resistor. K2 is off, so the LMC6482 A holds B at
     * VBIAS (1.65 V); the 100 ohm SSR2 path bounds the loop current to about
     * 16.5 mA even into a shorted DUT. Only after N_DUT is at ~0 is it safe to
     * de-energise K1. */

    if (!s_r_active && !s_fault) {
        /* Not in an R session: do not energise K1 just to tear down. */
        force_path_off();
        bsp_opamp3_enable(false);
        s_state = FE_IDLE;
        return;
    }

    bsp_opamp1_enable(false);
    relay_set(true, false);       /* K2 off; K1 stays on */
    muxr_enable(false);
    rforce_hiz();
    bsp_opamp3_enable(false);
    muxc_enable(false);
    HAL_GPIO_WritePin(PIN_VBIAS_PORT, PIN_VBIAS, GPIO_PIN_RESET);

    s_state = FE_DISCHARGE;
    write_ssr(PIN_SSR13_PORT, PIN_SSR13, false);
    write_ssr(PIN_SSR2_PORT, PIN_SSR2, true);
    bsp_delay_ms(DISCHARGE_HOLD_MS);
    write_ssr(PIN_SSR2_PORT, PIN_SSR2, false);

    /* N_DUT is now at ~0 V: safe to reconnect C_block and ground B. */
    relay_set(false, false);
    bsp_delay_ms(RMODE_RELAY_SETTLE_MS);

    s_r_active = false;
    s_fault = false;
    s_charged = false;
    s_discharged = true;
    s_state = FE_IDLE;
}

void fe_r_fault(void)
{
    /* Called from COMP4's EXTI ISR. Keep it to register writes: remove the
     * gate drive and the R mux, and leave K1 energised so B stays lifted. The
     * main loop calls fe_r_exit() to finish the shutdown. */

    /* Lock-free immediate disable. HAL_OPAMP_Stop() refuses if the handle is
     * locked by the main loop, which is exactly the wrong failure mode in an
     * overvoltage ISR. */
    CLEAR_BIT(OPAMP1->CSR, OPAMP_CSR_OPAMPxEN);
    bsp_opamp1_enable(false);

    /* PB14 -> input (high-Z). Direct MODER access avoids a HAL call in the
     * ISR. */
    GPIOB->MODER &= ~(3UL << (PIN_RFORCE_OVR_NUM * 2U));

    /* R mux disable (active low) and K2 off. */
    HAL_GPIO_WritePin(PIN_MUXR_EN_PORT, PIN_MUXR_EN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(PIN_K2_PORT, PIN_K2, GPIO_PIN_RESET);

    s_fault = true;
    s_state = FE_FAULT;
}

bool fe_r_active(void)
{
    return s_r_active;
}

bool fe_r_faulted(void)
{
    return s_fault;
}
