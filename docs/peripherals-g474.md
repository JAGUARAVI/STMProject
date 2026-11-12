# STM32G474CEU6 peripheral reference — verified facts

Condensed and **independently verified** reference for the ESWCap port. Facts
were checked against the local CubeG4 package
(`~/.platformio/packages/framework-stm32cubeg4/`), RM0440, the STM32G474CE pin
database, and the ES0430 errata sheet.

Where an external source and the local headers disagreed, the headers won.
Corrections to earlier claims made in conversation are noted.

---

## 0. WeAct Long board facts (schematic-verified)

These are board-level, not silicon-level; they were checked against the WeAct
`WeAct-STM32G474CoreBoard_Long_V10 SchDoc.pdf` and the official `Examples/`.

| Fact | Value | Consequence |
| --- | --- | --- |
| User LED | **PC6, active high** (`PC6 -> 5.1k -> LED -> GND`) | `PIN_LED = PC6`, `LED_ACTIVE_LOW = 0` |
| User button | **PC13**, active low to GND | not an LED |
| Console | **PA9/PA10**, USART1 AF7, on header P1 | no on-board USB-UART; the Type-C is native USB on PA11/PA12 |
| PC5 | **not bonded** on UFQFPN48 | never assign a signal to it |
| PB8 | **BOOT0** on this board | do not use it for a mux/relay |
| HSE | 8 MHz crystal on PF0/PF1 | `HSE_VALUE=8000000` |
| SWD | PA13/PA14 | JTAG not exposed; PB3/PB4 usable as GPIO |
| 5 V rail | Type-C VBUS only | relay coils need a 5 V feed, not 3.3 V |

### Mechanical / header order (from the board-shape PDF)

The Long board is **61.42 mm × 18.24 mm**, with **24 pins per long edge** at
2.54 mm pitch and **15.24 mm** between rows.  P1/P2 are the 20-pin sections;
P3/P4 are the four leftmost pins on each row.  **Pin 1 of P1 and P2 is at the
right-hand end.**

Top row, left → right:

```
P4-1 GND, P4-2 GND, P4-3 3V3, P4-4 3V3,
P2-20 PB11, P2-19 PB10, P2-18 VREF+, P2-17 PB2, P2-16 PB1, P2-15 PB0,
P2-14 PC4, P2-13 PA7, P2-12 PA6, P2-11 PA5, P2-10 PA4, P2-9 PA3,
P2-8 PA2, P2-7 PA1, P2-6 PA0, P2-5 NRST, P2-4 PC13, P2-3 VB,
P2-2 3V3, P2-1 GND
```

Bottom row, left → right:

```
P3-1 GND, P3-2 GND, P3-3 5V, P3-4 5V,
P1-20 PB12, P1-19 PB13, P1-18 PB14, P1-17 PB15, P1-16 PC6, P1-15 PA8,
P1-14 PA9, P1-13 PA10, P1-12 PA11, P1-11 PA12, P1-10 PA15, P1-9 PC10,
P1-8 PC11, P1-7 PB3, P1-6 PB4, P1-5 PB5, P1-4 PB6, P1-3 PB7,
P1-2 PB9, P1-1 GND
```

This is the map the carrier PCB in `docs/pcb-design.md` uses.  Note that the
physical pin names on the module silkscreen omit the `P` prefix (e.g. `B9` =
PB9) and that PB8/BOOT0 is not broken out on the 20-pin sections.

## 1. ADC

**Instances:** ADC1–ADC5, all 12-bit. Dual-mode pairs are only
**ADC1/ADC2** and **ADC3/ADC4**; ADC5 is standalone.

**Resolution:** 12 / 10 / 8 / 6-bit. Conversion = `SMP + 12.5` ADC clocks for
12-bit.

**Clock:** synchronous (`ADC_CLOCK_SYNC_PCLK_DIV1/2/4`) or asynchronous with
`ADC_CLOCK_ASYNC_DIV1…256`. **Hard limit 60 MHz** single-ADC, **52 MHz** when
ADCs run concurrently. From 170 MHz SYSCLK, `ASYNC_DIV4` → **42.5 MHz** is the
fastest legal single-ADC rate (≈2.83 Msps at `SAMPLETIME_2CYCLES_5`). Full
4 Msps needs an exact 60 MHz kernel clock.

### 1.1 Hardware oversampling — the feature that matters most

Enable via `hadc.Init.OversamplingMode`. Ratio `ADC_OVERSAMPLING_RATIO_2 … _256`
(2–256×). Shift `ADC_OVERSAMPLING_SHIFT_*`, **right shift 0–8 only** — there is
no hardware left shift.

Semantics: sum N raw samples, right-shift by M, **truncate to 16 bits**.

| Goal | Rule |
| --- | --- |
| Mean-preserving (same scale as raw codes) | `M = log2(N)` — e.g. 256× → M=8 |
| Maximum resolution | `M = log2(N) − 4` — e.g. 256× → M=4 → 16 bits |

**Ratiometric work should use the mean-preserving setting** so signal and
reference scale identically and the ratio is exact.

Triggered mode (`ADC_CFGR2.TROVS`):
* `ADC_TRIGGEREDMODE_SINGLE_TRIGGER` — one trigger releases all N conversions.
* `ADC_TRIGGEREDMODE_MULTI_TRIGGER` — a trigger per conversion.

**Oversampling output rate is decimated by N.** This is the reason the
oversampling ratio must be **per-range**: at 256× the output is ~2.7 kHz, far
too slow for a 100 Ω range with τ ≈ 28 µs.

### 1.2 Calibration

* `HAL_ADCEx_Calibration_Start(hadc, ADC_SINGLE_ENDED)`. **ADC must be
  disabled** first. Single-ended and differential calibrate separately.
* G4 self-calibration is **offset-only**; there is no factory gain calibration.
* System-memory constants (`stm32g4xx_ll_adc.h`):
  * `VREFINT_CAL_ADDR` = **0x1FFF75AA**, `VREFINT_CAL_VREF` = 3000 mV
  * `TEMPSENSOR_CAL1_ADDR` = **0x1FFF75A8** (30 °C)
  * `TEMPSENSOR_CAL2_ADDR` = **0x1FFF75CA** (130 °C)
  * VREFINT is channel **7**; use `__LL_ADC_CALC_VREFANALOG_VOLTAGE()`.
* `GCOMP` gain compensation exists and **is** applied after oversampling.
* **Offset registers (`ADC_OFR1..4`) are IGNORED when oversampling is enabled.**
  Do offset correction in software.

### 1.3 Differential mode

* Formed from **adjacent even/odd channel pairs** of the same ADC.
* **Common mode must be within ±180 mV of VREF+/2** — a tight constraint.
  The R-mode plan's common mode swings 0.33–1.3 V, so **single-ended is the
  right choice** unless a difference amplifier is added.
* Output is **offset binary** (`0x800` = zero), **not** two's complement.
* Differential LSB = 2× single-ended LSB, so it buys common-mode rejection,
  not resolution.

### 1.4 Triggering and DMA

* Regular group: up to 16 ranks, supports DMA. Injected: 4 ranks, **no DMA**.
* `ADC_EXTERNALTRIG_T6_TRGO` (+ `..._RISING` edge) is the clean timer-trigger
  route. Sources include TIM1/2/3/4/6/7/8/15/20 TRGO and HRTIM TRG1–10.
* `HAL_ADC_Start_DMA(&hadc, buf, N)`; buffer must be **`uint16_t`**.
* Request IDs: ADC1=5, ADC2=36, ADC3=37, ADC4=38, ADC5=39.
* `DMAContinuousRequests = ENABLE` → circular: the recommended pattern here.

### 1.5 Errata (ES0430) — code-level consequences

| Erratum | Consequence | Mitigation |
| --- | --- | --- |
| §2.7.1 10/8/6-bit EOC disturbs other ADCs | cross-ADC corruption | **stay 12-bit** |
| §2.7.2 channel switch disturbs conversions | mux-switch noise / DNL | single channel, continuous/bulb mode |
| §2.7.3 conversion >1 ms after the previous is wrong | slow readings silently bad | keep a continuous stream; or dummy-then-discard |
| §2.7.4 spurious conversion returns zero | zero after stop, or injected/regular collision | no injected+regular mixing; circular not one-shot; discard first sample after a stop |

**Practical upshot:** a single ADC, one channel, continuous circular DMA at a
timer-locked rate satisfies §2.7.2, §2.7.3 and §2.7.4 simultaneously.

### 1.6 Precision notes

* Long sample times (≥247.5 cycles) for high-impedance sources, or buffer with
  an OPAMP. Respect the datasheet max-RAIN table.
* Avoid negative injection on analog pins.
* Keep VREF+ decoupling tight (1 µF + 100 nF) — SAR switching current spikes
  VREF+, and internal RLC can oscillate.

---

## 2. VREFBUF and VREF+

* `VREFBUF` voltages: **2.048 / 2.5 / 2.9 V** (`VREFBUF_CSR.VRS`).
* Modes (`ENVR`, `HIZ`):
  * `ENVR=1,HIZ=0` → internal buffer drives VREF+
  * `ENVR=0,HIZ=1` → **external reference mode** (VREF+ is an input)
  * `ENVR=0,HIZ=0` → off, VREF+ pulled to VSSA
* Drive capability ≈6.5 mA, shared with the ADC/DAC load.
* **VREF+ is a dedicated pin (20)** on UFQFPN48 — an external reference is
  possible and is the lowest-noise option. Ratiometric maths makes the absolute
  value unimportant; only short-term stability matters.

---

## 3. COMP (comparators)

All **COMP1–COMP7 are bonded out** on UFQFPN48.

* **Inputs.** `INP` has only two options per comparator (a pin). `INM` selects
  between VREFINT scalers, a DAC channel, or two pins.
* **No internal `INP` source** on G4 — a DAC cannot drive `INP`.
* COMP1 `INP` on PA1 is `COMP_INPUT_PLUS_IO1` (the HAL's IO1 is PA1; IO2 is
  PB1). COMP4 `INP` on PB0 is also `IO1`.
* COMP4 `INM` can select the internal `DAC1_CH1`; that is how the R-mode V_N
  overvoltage guard is built without a pin.
* `COMP3`'s only `INM` is **`PF1` = the HSE crystal pin**, so COMP3 is
  unusable while the crystal is fitted.
* **Programmable hysteresis, 8 levels**, `HYST[2:0]`, applies to the **falling
  edge** only. Nominal spec labels are 10–70 mV; datasheet *typical* values run
  roughly double the label (HYST=2 → ~18 mV typ).
* **No speed/power mode on G4** (unlike F3). Fixed ultra-fast.
* **No window mode on G4.** Confirmed by the register map (no window bits) and
  by AN4232; many blog posts claim otherwise and are wrong for G4.
* **No clocked output filter.** Blanking is a *timer-gated* mask, not a filter.
* `VALUE` bit reads the output **before** polarity and blanking.
* Propagation delay **16.7 ns typ / 31 ns max**; startup ≤5 µs; IDDA ≈450 µA.
* Clock is shared with **SYSCFG** (`__HAL_RCC_SYSCFG_CLK_ENABLE()`).
* `HAL_COMP_Init / Start`, `HAL_COMP_GetOutputLevel`, `LL_COMP_*`.

### 3.1 COMP → timer routing

The path is **internal and needs no pin**. The selector is on the *timer* side:

```
TIMx->TISEL.TI1SEL = 0b0001   ->  tim_ti1_in1 = comp1_out
TIMx->CCMR1.CC1S   = 0b01     ->  tim_ic1 mapped onto tim_ti1
```

RM0440 Table 272 defines `tim_ti1_in1 = comp1_out` for TIM2, and the mux
populations differ per timer:

| Timer | Width | Comparators reachable on `TI1` |
| --- | --- | --- |
| **TIM2** | **32-bit** | **COMP1–COMP5** |
| TIM3 / TIM4 | 16-bit | COMP1–COMP7 |
| TIM1 / TIM8 / TIM20 | 16-bit | COMP1–COMP4 only |

Note TIM2 does *not* get COMP6/7 on TI1 (COMP6 is on TI2). G4 has **no
output-routing field in the comparator register** — unlike STM32F3, whose
`COMP_CSR.OUTSEL` did this job. Reading only the COMP chapter makes the
internal path look impossible; the selector is in the timer chapter.

Also available: COMP → `ETR` (`TIMx_AF1.ETRSEL`), `OCREF_CLR`
(`TIMx_AF2.OCRSEL`), `BKIN`/`BKIN2` (`BKCMPxE`), EXTI (COMP1→EXTI21 …).

**Implemented: `COMP1_OUT` → `TIM2_CH1` via `TISEL`, internal, no pin.**

---

## 4. DAC

| | Pins | Internal destinations | Rate |
| --- | --- | --- | --- |
| DAC1 | `OUT1=PA4`, `OUT2=PA5` | COMP1/3/4, COMP2/5 | 1 MSPS |
| DAC2 | `OUT1=PA6` | COMP6/7 | 1 MSPS |
| **DAC3** | **none** | **COMP1/3, COMP4, OPAMP1, OPAMP3, OPAMP6** | 15 MSPS |
| **DAC4** | **none** | COMP5/7, COMP6, OPAMP4, OPAMP5 | 15 MSPS |

* **DAC3/DAC4 are internal-only by design** — free threshold sources with no pin
  cost. This is the key finding for this project.
* Output mode `DAC_MCR.MODEx`: `011` = buffer off, **on-chip peripherals only**
  (pin freed). `001` = buffer on, pin **and** internal.
* Unbuffered internal loads switch fast (**16 ns** 10–90 % vs ~1.4 µs buffered
  into 5 kΩ).
* 12-bit, `DAC_OUT = VREF+ × DHR/4095`.
* Wave generation: noise (LFSR), triangle, and **sawtooth** (G4 addition) with
  independent step/reset triggers — useful for a swept threshold.
* **No DAC→ADC internal channel.** Read a DAC back via
  `DAC3 → OPAMP(follower) → ADC`.

---

## 5. OPAMP

`OPAMP1–OPAMP6` on die; **only OPAMP1/2/3 reachable on UFQFPN48** (OPAMP4/5/6
need PD/PE pins).

| | `VINP` | Internal `VINP` | `VINM` | `VOUT` | `VOUT`→ADC |
| --- | --- | --- | --- | --- | --- |
| OPAMP1 | PA1 (IO0), PA3 (IO1), PA7 (**IO2**) | **DAC3_CH1** | PA3 (IO0) | PA2 | `ADC1_IN13` |
| OPAMP2 | PA7, PB14, PB0 | — | PA5 | PA6 | `ADC2_IN16` |
| OPAMP3 | PB0, PB13, PA1 | **DAC3_CH2** | PB2, PB10 | PB1 | `ADC2_IN18`, `ADC3_IN13` |

* Modes: standalone, follower, **PGA**. PGA gains **×2…×64** non-inverting,
  **−1…−63** inverting.
* `OPAINTOEN` (`OPAMPINTEN`) routes `VOUT` to the ADC **internally and frees the
  pin**.
* Power-down (`OPAEN=0`) → **output high-Z**, config retained.
* Offset trim via `TRIMOFFSETN`/`TRIMOFFSETP` and `HAL_OPAMP_SelfCalibrate()`;
  result <±3 mV.
  **Gotcha:** with `OPAINTOEN=1` the `CALOUT` flag cannot be observed — calibrate
  with the pin output enabled first, then switch to internal.
* **HAL numbering matters.** `OPAMP_NONINVERTINGINPUT_IO0` is VINP0 = PA1,
  `IO1` is VINP1 = PA3, `IO2` is VINP2 = PA7 for OPAMP1. The handoff's
  "`IO0` (PA7)" is wrong and would select PA1/V_cap.
* **`VPSEL=11` means either `VINP3` or DAC**, depending on the instance
  (`OPAMP_NONINVERTINGINPUT_IO3` and `..._DAC` are the **same bit**). Set it
  deliberately.
* **No OPAMP→OPAMP and no OPAMP→COMP internal chaining.** Internal chains are
  only `DAC→COMP`, `DAC→OPAMP`, `OPAMP→ADC`.
* GBW 13 MHz, rail-to-rail, slew 6.5 V/µs normal / 45 V/µs high-speed.
  Input bias current is specified via the pin leakage table, not a standalone
  number — measure it on the bench for high-impedance work.

---

## 6. Timers

| Timer | Width | Notes |
| --- | --- | --- |
| **TIM2, TIM5** | **32-bit** | best for long period capture |
| TIM1, TIM8, TIM20 | 16-bit | advanced; complementary outputs, break |
| TIM3, TIM4 | 16-bit | general purpose |
| **TIM6, TIM7** | 16-bit | basic, TRGO only — **ideal ADC trigger** |
| TIM15/16/17 | 16-bit | TIM15 has 2 channels |
| LPTIM1 | 16-bit | low-power, LSE/LSI capable |
| HRTIM1 | 6×16-bit | **184 ps** resolution; DLL must be calibrated first |

**On UFQFPN48 the usable timers are TIM1–4, TIM6, TIM7, TIM15, TIM16, TIM17,
TIM20** (TIM5/TIM8 pins are not bonded).

### 6.1 Input capture for period measurement

* **PWM input mode** gives period *and* duty with zero CPU: `CC1S=01` (TI1→IC1
  direct), `CC2S=10` (TI1→IC2 indirect), `CC1P=0`, `CC2P=1`, slave mode
  `TS=TI1FP1`, `SMS=Reset`. Then `CCR1 = period`, `CCR2 = high time`.
  **Caveat:** reset mode loses overflow detection, so a stopped input is
  indistinguishable from a very long period.
* **Free-running capture** on a 32-bit timer avoids that ambiguity: capture
  rising edges and difference successive `CCR1` values.
* `HAL_TIM_IC_Start_DMA` streams captures to RAM; averaging N=256 at 5.88 ns
  resolution gives ~0.37 ns standard error.
* **TIM2 at 170 MHz, PSC=0:** 5.88 ns/tick, 25.3 s wrap → covers **10 Hz–1 MHz**
  with no prescaler and no overflow handling. Max `f_timer/2` = 85 MHz in theory;
  comparator/filter limits dominate in practice.

### 6.2 HRTIM1

184 ps resolution, 6 timers A–F. On UFQFPN48 only **timers A and B** are
reachable: `CHA1=PA8`, `CHA2=PA9`, `CHB1=PA10`, `CHB2=PA11` (AF13).
Requires DLL calibration (`HAL_HRTIM_DLLCalibrationStart` + poll) before use.
**Conflict:** PA8 is also a candidate `COMP1_OUT` pin — cannot be both.

### 6.3 Timer → ADC triggering

```
TIM6, PSC=0, ARR = 170/f − 1, MasterOutputTrigger = TIM_TRGO_UPDATE
ADC external trigger = ADC_EXTERNALTRIG_T6_TRGO, edge RISING
```
Gives a crystal-locked, CPU-free conversion rate.

**Important constraint:** TIM6 is **16-bit**, so at 170 MHz (PSC=0) it cannot
express a period longer than **385.5 µs**. The slow ranges need milliseconds, so
the timer prescaler must be chosen from the requested period:

```
psc   = 0; while (ceil(ticks0 / (psc+1)) > 65536) ++psc;
ticks = ceil(ticks0 / (psc+1));
```
This only coarsens the trigger grid — the conversion and oversampling are
unaffected. At PSC=5 (28.3 MHz) the maximum period is 2.31 ms with 35 ns
resolution, which covers the 1 MΩ range comfortably. Verify the sample period
from the programmed `PSC`/`ARR`, not from the request.

For a *delayed* sample point on a charge curve, use `TIM_TRGO_OC1REF` with
`CCR1` as the delay.

---

## 7. Storage

* 512 KB flash, base `0x08000000`, **page = 2 KB**, 128 pages (single-bank) or
  64 per bank (dual-bank).
* **Program granularity is 64-bit** (`FLASH_TYPEPROGRAM_DOUBLEWORD`); flash is
  ECC-protected, which is why.
* Dual-bank via option bit `FLASH_OPTR_DBANK` (bit 22), giving **read-while-write**
  across banks.
* API: `HAL_FLASH_Unlock` → `HAL_FLASHEx_Erase` (`FLASH_TYPEERASE_PAGES`) →
  `HAL_FLASH_Program` → `HAL_FLASH_Lock`.
* Endurance ~10 k erase cycles per page.
* **Recommended store:** reserve one 2 KB page in **bank 2**; append
  `{magic, version, crc32, payload}` records (~256 per erase cycle, atomic via
  CRC); newest valid record wins at boot. Direct analogue of ESP32 NVS.
* **TAMP backup registers:** 32 × 32-bit (`TAMP->BKP0R..BKP31R`, 128 bytes),
  accessed via `HAL_RTCEx_BKUPWrite/Read`. Survive reset and power-down **if
  VBAT is present**; cleared on VBAT-domain reset. Good for flags, not for the
  full calibration set (no ECC/CRC, small).

---

## 8. Watchdogs and RTC

* **IWDG** — LSI-clocked (32 kHz), independent of the system clock, so it still
  fires if the PLL or SYSCLK fails. Prescaler `IWDG_PRESCALER_4…256`, 12-bit
  reload. **Use this as the primary safety net.**
* **WWDG** — PCLK1-clocked, 7-bit, resets on underflow *or* on a refresh before
  the window opens. Only worth adding to catch a task running *too fast*.
* **RTC** — LSE/LSI/HSE-32, calendar, alarms, wakeup timer, owns the TAMP
  registers. Useful for timestamping telemetry and periodic recalibration.

---

## 9. Corrections to earlier statements

Recorded so the reasoning is auditable:

1. **"OPAMP pins collide with USB" — wrong.** USB is on PA11/PA12. OPAMP1
   (PA1/PA2/PA3), OPAMP2 (PA5/PA6/PA7) and OPAMP3 (PB0/PB1/PB2/PB10/PB13) are
   all fully available.
2. **"DAC1 only, DAC3/DAC4 have no pins here" — incomplete.** DAC2 (PA6) also
   exists, and DAC3/DAC4 having no pins is a *feature*: they are internal-only
   threshold sources.
3. **"COMP5/6/7 unavailable" (from the research agent) — wrong.** They resolve
   to PB13/PB11/PB14 `INP` and PB10/PB15/PB12 `INM`, all bonded. All 7
   comparators are reachable; only COMP3 is practically blocked, and that is
   because its `INM` is the HSE pin, not because of PD/PE.
4. **HSE crystal is on PF0/PF1**, not PA — this matters for the PLL work and
   for freeing COM3's `INM`.