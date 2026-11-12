/**
 * @file    config.h
 * @brief   Board pin map, hardware constants and build configuration.
 *
 * Target: WeAct Studio STM32G474CEU6 "Long" core board (UFQFPN48).
 *
 * This is the single place that maps firmware signals to pins. Every BSP
 * module includes it. Change the map here only.
 *
 * The map below is the **combined C + R** front end described in
 * docs/resistance-autoranging-plan.md, corrected against the physical WeAct
 * board and the STM32G474CEU6 pin database. The evidence and every correction
 * are recorded in docs/handoff-verdict.md.
 *
 * Board facts this map relies on (verified):
 *   - The WeAct Long board's user LED is PC6, ACTIVE HIGH. PC13 is a button.
 *   - The board has native USB on PA11/PA12, not a USB-UART bridge. The
 *     console is on the P1 header: PA9 = TX, PA10 = RX (USART1 AF7).
 *   - PC5 is NOT bonded on UFQFPN48. Do not put a signal there.
 *   - PB8 is BOOT0 on this board. Do not use it for a mux.
 *   - PA0 = COMP1_OUT (AF8); it is also the C-mode drive buffer input, so the
 *     oscillator is closed in hardware. The pin is switched between AF and
 *     GPIO by bsp_comp.c.
 *     **Not PA8:** on this package PA8's AF8 is COMP7_OUT, not COMP1_OUT.
 *     ST's pin data (STM32_open_pin_data, GPIO-STM32G47x_gpio_v1_0) lists
 *     COMP1_OUT only on PA0, PA6, PA11 and PB8 — PA6 is TIA_OUT, PA11 is
 *     USB D-, PB8 is BOOT0, so PA0 is the only usable one. PA0 has nothing
 *     else on the WeAct Long board (schematic V1.0: MCU pin 8 -> P2-6 only).
 *   - PC13 (user key) is ACTIVE HIGH: PC13 -> 330 R -> key -> 3.3 V.
 *   - VREF+ is tied to VDDA through R7 = 0 R on the module. Never enable
 *     VREFBUF unless R7 is removed.
 *
 * Analogue topology (the one thing the handoff got right):
 *
 *   C mode:   range R -> V_cap -> C_block -> N_DUT -> DUT -> B -> GND
 *   R mode:   +3.3 V -> Q1 -> N_DUT -> DUT -> B -> K2A -> R mux -> RSENSE -> GND
 *
 * N_DUT is the C-mode bias node and the R-mode force node. B is grounded in C
 * mode and is the Kelvin/TIA sense node in R mode. V_cap is NOT the force node:
 * forcing it would only charge C_block, which is an open circuit at DC.
 *
 * Reserved and never used by application code:
 *   PA13/PA14 SWD, PF0/PF1 HSE crystal, PA11/PA12 USB (DFU bootloader).
 */

#ifndef ESWCAP_CONFIG_H
#define ESWCAP_CONFIG_H

/*
 * The pin map below is written in terms of STM32 HAL macros, but the
 * measurement maths includes this header and must also compile on a host for
 * unit tests. ESWCAP_HOST_TEST substitutes a tiny shim so that the constants
 * stay in one place without dragging the vendor headers into host builds.
 */
#ifdef ESWCAP_HOST_TEST
#include "hal_shim.h"
#else
#include "stm32g4xx_hal.h"
#endif

/* ------------------------------------------------------------------------- */
/* Firmware identity                                                         */
/* ------------------------------------------------------------------------- */

#define ESWCAP_FW_NAME      "ESWCap"
#define ESWCAP_FW_TARGET    "STM32G474CEU6 / WeAct Long"
#define ESWCAP_FW_VERSION   "2.1.0-g474"

/* ------------------------------------------------------------------------- */
/* Feature gates                                                             */
/* ------------------------------------------------------------------------- */

/* 1 when the combined-board R hardware is fitted: Q1, the LMC6482 A/B, the
 * K1/K2 relays, the second MAX4619 and the OPAMP3 V_N buffer. The firmware is
 * always compiled with the R-mode code, but `mode r` refuses to energise
 * anything until this is 1. That keeps a C-mode-only board safe and honest. */
#ifndef ESWCAP_HAS_RMODE
#define ESWCAP_HAS_RMODE    0
#endif

/* Define to 0 to suppress the human-readable logs alongside @@EVT telemetry. */
#ifndef ESWCAP_VERBOSE_LOG
#define ESWCAP_VERBOSE_LOG  1
#endif

/* ------------------------------------------------------------------------- */
/* Analogue front end                                                        */
/* ------------------------------------------------------------------------- */

/* V_cap: the RC node. One pin serves both acquisition methods. */
#define PIN_VCAP_PORT       GPIOA
#define PIN_VCAP            GPIO_PIN_1
#define ADC_CH_VCAP         ADC_CHANNEL_2   /* ADC1_IN2 / ADC2_IN2 */

/* C-mode drive buffer input, and COMP1_OUT. The same physical pin: in
 * oscillator mode it is COMP1_OUT (AF8, open-loop hardware oscillator); in
 * RC-step mode it is a plain GPIO output. Note that C-mode drive current is
 * limited by the range resistor, so the buffer sees a logic-level input. */
#define PIN_DRIVE_PORT      GPIOA
#define PIN_DRIVE           GPIO_PIN_0      /* COMP1_OUT (AF8) / SN74LVC1G34 input, P2-6 */
#define PIN_DRIVE_AF        GPIO_AF8_COMP1  /* valid on PA0; on PA8 AF8 is COMP7 */

/* R-mode nodes. */
#define PIN_B_PORT          GPIOA
#define PIN_B               GPIO_PIN_7      /* OPAMP1_VINP2, ADC2_IN4 */
#define ADC_CH_B            ADC_CHANNEL_4   /* ADC2_IN4 */

#define PIN_VREF_PORT       GPIOA
#define PIN_VREF            GPIO_PIN_3      /* OPAMP1_VINM0, ADC1_IN4 */
#define ADC_CH_VREF         ADC_CHANNEL_4   /* ADC1_IN4 */

#define PIN_RFORCE_PORT     GPIOA
#define PIN_RFORCE          GPIO_PIN_2      /* OPAMP1_VOUT -> Q1 gate */

#define PIN_VN_PORT         GPIOB
#define PIN_VN              GPIO_PIN_0      /* OPAMP3_VINP0, COMP4_INP */
#define ADC_CH_VN           ADC_CHANNEL_1   /* ADC3_IN1 via OPAMP3_VOUT/PB1 */

#define PIN_VN_OUT_PORT     GPIOB
#define PIN_VN_OUT          GPIO_PIN_1      /* OPAMP3_VOUT, ADC3_IN1 */

#define PIN_VB_PORT         GPIOB
#define PIN_VB              GPIO_PIN_12     /* LMC6482 B output, ADC4_IN3 */
#define ADC_CH_VB           ADC_CHANNEL_3   /* ADC4_IN3 */

#define PIN_TIA_PORT        GPIOA
#define PIN_TIA             GPIO_PIN_6      /* LMC6482 A output, ADC2_IN3 */
#define ADC_CH_TIA          ADC_CHANNEL_3   /* ADC2_IN3 */

#define PIN_VBIAS_SENSE_PORT GPIOA
#define PIN_VBIAS_SENSE     GPIO_PIN_5      /* 1.65 V divider, ADC2_IN13 */
#define ADC_CH_VBIAS        ADC_CHANNEL_13  /* ADC2_IN13 */

/* ------------------------------------------------------------------------- */
/* Digital control                                                          */
/* ------------------------------------------------------------------------- */

/* MAX4619 #1: C-mode range mux. EN is active low. PB3/PB4 are the JTAG pins;
 * using them as GPIO disables JTAG but leaves SWD intact.
 *
 * WIRING NOTE: the original ESWCap board may tie the C-mux ENABLE low (always
 * enabled). In that case leave PB5 unconnected; do NOT wire PB5 to a net that
 * is hard-tied to GND, because fe_init() drives it high to disable the mux. */
#define PIN_MUXC_A0_PORT    GPIOB
#define PIN_MUXC_A0         GPIO_PIN_3
#define PIN_MUXC_A1_PORT    GPIOB
#define PIN_MUXC_A1         GPIO_PIN_4
#define PIN_MUXC_EN_PORT    GPIOB
#define PIN_MUXC_EN         GPIO_PIN_5

/* MAX4619 #2: R-mode sense-resistor mux. EN is active low. PB2 is used for EN
 * deliberately: PB8 is BOOT0 on this board and must stay untouched.
 *
 * Channel order assumed by rmode.c: A1:A0 = 00 -> 100 R, 01 -> 1 k,
 * 10 -> 100 k, 11 -> 4.7 M. Wire the RSENSE bank in that order. */
#define PIN_MUXR_A0_PORT    GPIOB
#define PIN_MUXR_A0         GPIO_PIN_6
#define PIN_MUXR_A1_PORT    GPIOB
#define PIN_MUXR_A1         GPIO_PIN_7
#define PIN_MUXR_EN_PORT    GPIOB
#define PIN_MUXR_EN         GPIO_PIN_2

/* B-node relays. K1 (R_MODE) is energised for the whole R session; K2
 * (KELVIN) only in Kelvin. K2 must never be energised without K1.
 *
 * The G6S-2 coils are 5 V, not 3.3 V: the driver stage needs a 5 V feed (the
 * WeAct board only provides Type-C VBUS). GPIO high = coil energised through
 * the driver transistor. */
#define PIN_K1_PORT         GPIOB
#define PIN_K1              GPIO_PIN_9      /* R_MODE, K1 coil */
#define PIN_K2_PORT         GPIOB
#define PIN_K2              GPIO_PIN_10     /* KELVIN, K2 coil */

/* C-mode safety bank. SSR1 (precharge) and SSR3 (V_cap clamp) share one pin:
 * that is the existing hardware interlock and must stay shared. */
#define PIN_SSR13_PORT      GPIOB
#define PIN_SSR13           GPIO_PIN_11     /* SSR1 precharge + SSR3 clamp */
#define PIN_SSR2_PORT       GPIOB
#define PIN_SSR2            GPIO_PIN_13     /* N_DUT discharge */
#define PIN_VBIAS_PORT      GPIOB
#define PIN_VBIAS           GPIO_PIN_15     /* bias-domain return, held LOW */

/* R-mode force override: high-Z in Kelvin/C, driven low in TIA so Q1 is fully
 * on and the LMC6482 A owns the loop. */
#define PIN_RFORCE_OVR_PORT GPIOB
#define PIN_RFORCE_OVR      GPIO_PIN_14
#define PIN_RFORCE_OVR_NUM  14U

/* ------------------------------------------------------------------------- */
/* Console and status                                                        */
/* ------------------------------------------------------------------------- */

#define PIN_UART_PORT       GPIOA
#define PIN_UART_TX         GPIO_PIN_9      /* USART1_TX, AF7 */
#define PIN_UART_RX         GPIO_PIN_10     /* USART1_RX, AF7 */
#define CONSOLE_BAUD        115200U

/* The WeAct Long board's user LED is PC6 and is ACTIVE HIGH:
 * PC6 -> 5.1 kohm -> LED anode -> cathode -> GND. PC13 is the user button. */
#define PIN_LED_PORT        GPIOC
#define PIN_LED             GPIO_PIN_6
#define LED_ACTIVE_LOW      0

/* Optional user button. On the WeAct Long board (schematic V1.0) the key is
 * PC13 -> 330 R -> key -> 3.3 V, so it reads HIGH when pressed and needs the
 * internal pull-down. Not required by the application; defined so a future
 * factory-reset gesture has a pin. */
#define PIN_BUTTON_PORT     GPIOC
#define PIN_BUTTON          GPIO_PIN_13
#define BUTTON_ACTIVE_LOW   0

/* ------------------------------------------------------------------------- */
/* Ranges                                                                    */
/* ------------------------------------------------------------------------- */

#define RANGE_COUNT         4
#define MAX_SAMPLES         12

/* C-mode range resistor bank, measured values of the actual board (see
 * HANDOVER.md). An NVS/flash override may replace these per unit at runtime. */
#define RES_100_OHM         98.9
#define RES_1K_OHM          993.6
#define RES_100K_OHM        98.6e3
#define RES_1M_OHM          1.01e6

/* R-mode sense resistors. The first three deliberately reuse the C-mode range
 * resistors (the plan reuses the existing bank); the fourth is the 4.7 Mohm
 * top element. RSENSE_eff includes the mux and relay Ron and is calibrated per
 * element. */
#define RMODE_ELEMENTS      4
#define RSENSE_100_OHM      98.9
#define RSENSE_1K_OHM       993.6
#define RSENSE_100K_OHM     98.6e3
#define RSENSE_4M7_OHM      4.7e6

/* DC bias injection resistor V_BIAS -> N_DUT (stiff at DC, open at AC). */
#define R_BIAS_OHM          4.8e6

/* DC blocking capacitor between the logic domain and the bias domain.
 * Rev B uses a 1000 uF / 35 V bipolar electrolytic (the old 936 uF was the
 * measured value of the Rev A polarised part).  Measure the fitted part and
 * trim this constant, or set it with the calibration record. */
#define C_BLOCK_ELEC_F      1000.0e-6
#define C_BLOCK_HF_F        67.8e-9

/* Nominal node stray capacitance: cold-start guess only. The measured
 * open-node tare T0 is the real mechanism. */
#define STRAY_CAPACITANCE_F 12.0e-12

/* ------------------------------------------------------------------------- */
/* R-mode operating points                                                   */
/* ------------------------------------------------------------------------- */

/* Kelvin setpoint at B. The ratio equation cancels this value, so accuracy is
 * not critical; it only has to keep V_N inside the ADC range. 0.405 V comes
 * from the 7.15k/1k divider in the plan. */
#define RMODE_VREF_MV       405

/* TIA bias at the LMC6482 A non-inverting input (10k/10k divider). */
#define RMODE_VBIAS_MV      1650

/* TIA feedback resistor and capacitor. */
#define RMODE_RF_OHM        10.0e6
#define RMODE_CF_F          10.0e-12

/* Compliance and resolution gates.
 *
 * RMODE_MIN_DV_MV is the smallest V_N - V_B worth trusting: 3 mV is about
 * 3.7 ADC LSB at 3.3 V, below which the ratio is dominated by quantisation
 * and channel offset. It is also the ladder's "go one element lower" trigger. */
#define RMODE_VN_MAX_MV     2300
#define RMODE_VB_MIN_MV     20
#define RMODE_MIN_DV_MV     3.0
#define RMODE_VN_ABS_MAX_MV 3200
#define RMODE_TIA_VOUT_MIN_MV 50

/* Relay and loop settling. G6S-2 is a mechanical relay: 10 ms covers bounce
 * and the contact wetting transient. The Kelvin loop needs a little longer for
 * the DUT capacitance. */
#define RMODE_RELAY_SETTLE_MS 10U
#define RMODE_LOOP_SETTLE_MS  10U

/* ------------------------------------------------------------------------- */
/* Acquisition                                                               */
/* ------------------------------------------------------------------------- */

/* ADC clock: 170 MHz SYSCLK / 4 = 42.5 MHz (legal: <=60 MHz single ADC). */
#define ADC_ASYNC_DIV       ADC_CLOCK_ASYNC_DIV4
#define ADC_CLOCK_HZ        42500000U

/* TIM6 is the ADC trigger. Its counter is reset at the excitation edge, so
 * sample n occurs at exactly n * period with no timestamp bookkeeping. */
#define ADC_TRIGGER_TIM     TIM6

/* Per-range oversampling ratios. Oversampling decimates the output rate, so a
 * fixed ratio makes fast ranges unmeasurable (100 ohm + 1 nF has zero usable
 * points at 64x). See docs/port-plan.md 3.2. */
#define ADC_OS_RATIO_100R   4U
#define ADC_OS_RATIO_1K     8U
#define ADC_OS_RATIO_100K   16U
#define ADC_OS_RATIO_1M     16U

/* Streaming ring buffer. 1024 x uint16 = 2 KB of the 128 KB SRAM. */
#define ADC_STREAM_CAPACITY 1024U

/* Capture limits */
#define RC_FIT_MAX_PTS      256
#define RC_FIT_MIN_PTS      8
#define RC_FIT_MAX_ITER     50
#define RC_FIT_GLITCH_MV    40

/* Physics gates, ported from the ESP32 firmware. */
#define ADC_STEP_THRESHOLD_US   250.0
#define ADC_SUBNF_GATE_F        1.0e-9
#define OSC_MIN_F_HZ            10.0
#define OSC_MAX_F_HZ            1000000.0
#define OSC_TARGET_PERIODS      50

/* COMP1 relaxation oscillator. The swing is set by the comparator hysteresis
 * around the 1.65 V DAC3 threshold, driven 0 .. 3.3 V:
 *
 *     K = ln((Vdd - Vl)/(Vdd - Vh)) + ln(Vh/Vl)
 *
 * With 70 mV (the largest G4 setting) K ~= 0.085. The ESP32 LM393 board
 * (10k/10k + 100k) had a 157 mV band and K ~= 0.19; the old 20 mV setting gave
 * K ~= 0.024, below the solver's acceptance gate, so calibration was refused.
 * OSC_K_NOMINAL is only the cold-start value; `cal1`/`cal2` measure the real
 * one (the hysteresis has a wide datasheet tolerance). */
#define OSC_COMP_HYSTERESIS     COMP_HYSTERESIS_70MV
#define OSC_K_NOMINAL           0.085
#define OSC_CAPTURE_MAX_EDGES   512

/* Fusion */
#define FUSION_GATE_REL         0.875
#define METHOD_MISMATCH_RATIO   1.5

/* Front-end timing */
#define PRECHARGE_HOLD_MS       2000
#define DISCHARGE_HOLD_MS       2000
#define ISOLATION_DELAY_US      2000

/* Watchdog: LSI 32 kHz /32 prescaler, 4095 reload -> ~4 s. */
#define IWDG_PRESCALER          IWDG_PRESCALER_32
#define IWDG_RELOAD             4095U

/* ------------------------------------------------------------------------- */
/* Storage                                                                   */
/* ------------------------------------------------------------------------- */

/* Reserved calibration page: the last 2 KB page of flash bank 2.
 * 512 KB single-bank addresses are 0x08000000..0x0807FFFF; the last page is
 * 0x0807F800. If the part is configured dual-bank, this is still the top of
 * the address space and remains outside the program image. */
#define STORE_FLASH_ADDR        0x0807F800UL
#define STORE_FLASH_PAGE        127U
#define STORE_RECORD_MAGIC      0x45535743UL   /* "ESWC" */
/* Version 2 adds the R-mode calibration block. A version-1 record is rejected
 * and the compiled defaults are used, which is safe: R-mode is gated off until
 * the hardware is fitted anyway. */
#define STORE_RECORD_VERSION    2U

#endif /* ESWCAP_CONFIG_H */
