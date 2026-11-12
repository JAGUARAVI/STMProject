# Handoff review — combined C + R circuit

**Reviewed:** `docs/HANDOFF-combined-circuit.md`
**Date:** 2026-10-28
**Reviewer:** firmware side, against the physical board documentation and the
STM32G474 datasheet/reference manual.

## Verdict in one line

The handoff is **right about the two things that matter most** — the DUT
topology and the OPAMP1/COMP1 feedback polarity — and **wrong about several
board facts that would have broken bring-up** (USART on a pin that does not
exist on this package, the LED on the wrong pin, and the wrong OPAMP1 input
constant). Its central epistemic error is treating a **design proposal** as
as-built hardware.

**Do not apply it literally. Apply the corrected version below.**

---

## 1. Evidence used

| Source | What it settles |
| --- | --- |
| `WeActStudio.STM32G474CoreBoard/Hardware/QFN48/.../...Long_V10 SchDoc.pdf` | LED = **PC6, active-high**; PC13 = user button; 8 MHz HSE on PF0/PF1; native USB on PA11/PA12; **no USB-UART bridge**; PC5 not bonded |
| `WeActStudio.../Examples/README.md` + `01-Blink/Bsp/board.h` | Confirms the Long board LED is `GPIOC` pin 6 |
| `stm32-data-generated` `STM32G474CE.json`, package `STM32G474CEUx` | Exact bonded pins and every ADC/OPAMP/COMP/USART alternate function |
| RM0440 Table 272 / HAL `TIM_TIM2_TI1_COMP1` | `COMP1_OUT → TIM2_CH1` via `TIM2_TISEL.TI1SEL = 0b0001`; no jumper |
| ESP32 repo `docs/combined-circuit.md` §1 | The existing C-mode board is `V_cap → C_block → N_DUT → DUT → GND`, with N_DUT the bias node |
| ESP32 `src/main.c` front-end comments | SSR1/SSR2/SSR3 all act on **N_DUT**, not on V_cap — independent confirmation of the topology |
| `~/.platformio/.../stm32g4xx_hal_opamp.h` | OPAMP1: IO0 = PA1, IO1 = PA3, **IO2 = PA7** |
| `~/.platformio/.../stm32g4xx_hal_comp.h` | `COMP_INPUT_PLUS_IO1` = PA1 for COMP1 (so the current code is correct) |
| `/home/jaguar/Documents/KiCad/Project/` | **Empty**: 13-line schematic, zero symbols; no PCB. It cannot corroborate anything |

---

## 2. What the handoff gets right

1. **The DUT is `N_DUT → B`, not `V_cap → N_DUT`.**
   `C_block` sits between `V_cap` and `N_DUT`; `B` is grounded in C mode and is
   the sense node in R mode. This is confirmed independently by the ESP32
   combined-circuit netlist and by the ESP32 firmware's front-end semantics.
   **My R-mode topology was invented and wrong.**

2. **`V_cap` cannot be the R-mode force node.** Forcing `V_cap` only charges
   `C_block`, which is an open circuit at DC. The handoff is correct.

3. **OPAMP1 polarity was wrong in my code.** With a high-side P-FET whose gate
   is pulled up (off) and driven low (on):
   - old `VINP = DAC`, `VINM = sense` → sense rises → output falls → FET turns
     on harder → sense rises more = **positive feedback**;
   - correct `VINP = B`, `VINM = VREF` → B rises → output rises → FET turns off
     → B falls = **negative feedback**.

4. **COMP1 output polarity was wrong in my code.** With `INP = V_cap` and
   `INM = DAC`, `NONINVERTED` gives "drive high while V_cap is above the
   threshold", which latches the relaxation loop. The loop needs the inverted
   output so the drive is high while V_cap is **below** the threshold.

5. **OPAMP2 cannot be the TIA.** Its VINP is PA7 (B) and its VINM is PA5
   (VBIAS) — the opposite polarity from a TIA. The external LMC6482 A is the
   correct device.

6. **The relay state table is electrically coherent** for the specified
   G6S-2 Form C contacts, and K1 is genuinely fail-safe in C mode.

7. **The safety invariants are the right ones** (with one wording fix:
   `K1B` is normally closed, so the rule is "never *de-energise* K1 — which
   closes K1B — while N_DUT is biased").

8. **The old 900 MΩ/100 MΩ V_N divider had to go**; it destroys the low-ohm
   Kelvin resolution. The 1 MΩ + clamp + OPAMP3 buffer is the right replacement.

---

## 3. Where the handoff is wrong or unsafe to apply literally

### 3.1 USART1 on PC4/PC5 — **impossible on UFQFPN48**

The package has **PC4 but no PC5**. PC5 is not bonded out. `USART1_TX` can be
PC4, but there is no `USART1_RX` there. The only fully bonded USART1 pair on
this board is **PA9/PA10** (AF7); PB6/PB7 is the other option but those pins are
needed by the R mux.

**Correction: keep the console on PA9/PA10.** The WeAct board has no on-board
USB-UART bridge (the Type-C is native USB on PA11/PA12), so the console is on
the P1 header either way; PA9/PA10 is the pair WeAct exposes and documents.

### 3.2 LED on PC13 — **wrong pin**

The Long board's user LED is **PC6, active-high** (`PC6 → 5.1 kΩ → LED → GND`).
**PC13 is the user button**, not an LED. WeAct's own `Examples/README.md` and
`01-Blink/Bsp/board.h` say `LED_Pin = GPIO_PIN_6`, `LED_GPIO_Port = GPIOC`.

**Correction: `PIN_LED = PC6`, `LED_ACTIVE_LOW = 0`.** My code had this wrong
too.

### 3.3 OPAMP1 input constant — **wrong HAL constant**

The handoff says "Change `NonInvertingInput` to
`OPAMP_NONINVERTINGINPUT_IO0` (PA7)". That is wrong:

```
OPAMP1 VINP0 = PA1   -> OPAMP_NONINVERTINGINPUT_IO0
OPAMP1 VINP1 = PA3   -> OPAMP_NONINVERTINGINPUT_IO1
OPAMP1 VINP2 = PA7   -> OPAMP_NONINVERTINGINPUT_IO2
```

Implemented literally, the op-amp would use **PA1 = V_cap** as its positive
input and the Kelvin loop would be nonsense. **Correction: `IO2` for PA7.**

### 3.4 The `bsp_comp.c` description is stale

The handoff says the current code "uses an EXTI mirror and a PA15/TIM2_CH1
jumper" and that the internal route may not be configurable. Both are false for
the current tree: `bsp_comp.c` already calls
`HAL_TIMEx_TISelection(&s_htim2, TIM_TIM2_TI1_COMP1, TIM_CHANNEL_1)`, which is
`TIM2_TISEL.TI1SEL = 0b0001` — the documented internal path (RM0440 Table 272).
No jumper is needed. The handoff reviewed an earlier snapshot.

### 3.5 "The plan is authoritative / this is the real circuit"

The handoff's only cited authority is `docs/resistance-autoranging-plan.md`
(rev 2), which is explicitly a **design plan, not as-built hardware**. The
KiCad project it points at is **empty**. The topology conclusion happens to be
correct because the ESP32 netlist independently confirms it, but the reasoning
is backwards: a plan cannot make a circuit real.

**Correction: treat rev 2 as the target design, verify it against the built
board, and keep the firmware's board-specific assumptions in one place.**

### 3.6 K1/K2 vs SSR_CB/SSR_LO

The ESP32 `docs/combined-circuit.md` specifies `SSR_CB` and `SSR_LO`
(AQY212-class). The local rev 2 plan and the handoff switch to G6S-2 relays.
That is a **BOM decision**, not a fact, and the handoff states it as if the
board already has them. Confirm the parts before wiring.

### 3.7 PB8 as the R-mux enable — **conflicts with BOOT0**

The handoff assigns the R mux `A0/A1/EN = PB6/PB7/PB8`. On the WeAct Long
board **PB8 is BOOT0** (button/jumper). Driving it as a mux enable fights the
boot strap and the button.

**Correction: R mux `A0/A1/EN = PB6/PB7/PB2`.** PB2 is free because COMP4's
inverting input uses the internal `DAC1_CH1`, not the pin.

### 3.8 PB3/PB4 as the C-mux pins

PB3/PB4 are JTDO/NJTRST. They work as GPIO and SWD is unaffected, but JTAG is
lost and the pins have reset-state pull-ups. That is acceptable — but it must
be documented and the debug environment must stay on SWD. (The alternative is
PA0/PA4/PB2, at the cost of analog-header pins.)

### 3.9 PA8 is both COMP1_OUT and the C drive

> **Correction (2026-11-12): PA8 cannot be COMP1_OUT on this package.** ST's
> GPIO data (`GPIO-STM32G47x_gpio_v1_0`) gives PA8 AF8 = `COMP7_OUT`; COMP1_OUT
> exists only on PA0, PA6, PA11 and PB8. Configuring PA8 as AF8 would output the
> disabled COMP7 and the oscillator would never start. The drive / COMP1_OUT
> pin is now **PA0** (WeAct P2-6, nothing else on the module). The AF/GPIO
> switching described below is unchanged, just on PA0.

The handoff says PA8 drives the buffer and is COMP1_OUT, but does not mention
that the pin must be **reconfigured between AF (comparator) and GPIO output
(RC step)**. In AF mode a GPIO write has no effect; in GPIO mode the comparator
does not drive it. The firmware must switch modes explicitly, and the buffer
input needs the 10 kΩ pull-down the handoff mentions.

### 3.10 The 5 V relay coils

The G6S-2 coils are 5 V. The WeAct board's Type-C VBUS is 5 V, but the board
itself runs 3.3 V and the handoff never says where the 5 V rail comes from.
The existing front-end may have one; confirm before committing to relays.

### 3.11 The R-mode exit sequence omits the N_DUT discharge

The handoff's §6 sequence is:

```
→ R_DISCHARGE: Q1 off; OPAMP1 off; R mux off; K2 off; K1 off; then IDLE
```

That de-energises K1 **without first bleeding N_DUT**. K1B is normally closed,
so this reconnects the 936 uF C_block to a node still at the Kelvin/TIA force
voltage — exactly what the document's own invariant #1 forbids. The firmware
implements the safe order: keep K1 energised, bleed N_DUT through SSR2's 100 ohm
path, then de-energise K1. This is a real safety gap in the handoff, not a
style issue.

### 3.12 Minor / imprecise

- `PB2 = COMP4_INM (DAC1_CH1 internal)` conflates the pin (INM1 = PB2) with the
  internal DAC selection. If the DAC is used, PB2 is not needed.
- `V_N = ADC3_IN13 (OPAMP3 internal)` is unverified. The safe route is
  **PB1/ADC3_IN1**, which is bonded and documented; use that.
- "Never close K1B with N_DUT biased" should be "never de-energise K1 (K1B is
  NC) while N_DUT is biased."
- The handoff underestimates the ADC work: the map needs ADC4 and multi-channel
  reads on ADC1/2/3, not just a channel renumber.

---

## 4. What my implementation got wrong

| # | Error | Consequence | Fix |
| --- | --- | --- | --- |
| 1 | R-mode topology `V_cap → R_dut → node_bot → R_sense → GND` | R-mode could never work: V_cap is behind C_block | Rewrite around N_DUT (force) / B (sense) |
| 2 | OPAMP1 `VINP = DAC3_CH1`, `VINM = sense` | Positive feedback with a P-FET; the servo latches | `VINP = B`, `VINM = VREF` |
| 3 | COMP1 `NONINVERTED` | Relaxation loop latches high | `INVERTED` |
| 4 | LED on PC13 active-low | No visible status on the real board | PC6 active-high |
| 5 | R-mode pins PA0/PA1/PA3/PA2 | No relationship to the real nodes | Corrected map below |
| 6 | Invented `SSR4/SSR5/SSRR` | Controls hardware that does not exist | Replace with K1/K2 |
| 7 | R-mode reuses the C-mode range mux | The R sense bank is a separate MAX4619 | Separate R mux |
| 8 | R-mode autorange probes from the top element down | A 100 Ω DUT is invisible on 4.7 MΩ | Up/down compliance ladder |
| 9 | Docs said "verify against the schematic" while the only KiCad project is empty | False sense of verification | State the evidence honestly |
| 10 | `kelvin-ohmmeter-g474.md` says "force into V_cap" | Wrong topology carried into the docs | Rewrite |
| 11 | `port-plan.md` says the fix is "swap ADC_CH_VCAP/ADC_CH_VBOT" | A channel swap cannot move the force node | Rewrite the risk item |

**What was not wrong:** the C-mode series-capacitance maths is unchanged by the
topology order — the DUT and C_block are in series either way. The RC fit,
oscillator model, fusion and calibration solvers carry over.

---

## 5. Corrected pin map

Use this as the single source of truth. Everything below is confirmed against
the UFQFPN48 pin database and the WeAct Long schematic.

| Signal | Pin | Peripheral / note |
| --- | --- | --- |
| `V_cap` | **PA1** | ADC1_IN2, COMP1_INP (HAL `COMP_INPUT_PLUS_IO1`) |
| `N_DUT` | — | Analog node only; no MCU pin. Forced by Q1, biased by R_bias/SSR1, discharged by SSR2 |
| `B` (DUT bottom) | **PA7** | OPAMP1_VINP2 (`OPAMP_NONINVERTINGINPUT_IO2`), ADC2_IN4; buffered by LMC6482 B |
| `VREF` (0.405 V) | **PA3** | OPAMP1_VINM0 (`OPAMP_INVERTINGINPUT_IO0`), ADC1_IN4 |
| Q1 gate | **PA2** | OPAMP1_VOUT |
| `V_N_SENSE` | **PB0** | OPAMP3_VINP0 (`OPAMP_NONINVERTINGINPUT_IO0`), COMP4_INP |
| `V_N` buffer out | **PB1** | OPAMP3_VOUT, ADC3_IN1 (use the pin, not the unverified internal channel) |
| `V_B` buffer out | **PB12** | LMC6482 B output, ADC4_IN3 |
| `TIA_OUT` | **PA6** | LMC6482 A output, ADC2_IN3 |
| `VBIAS` (1.65 V) | **PA5** | ADC2_IN13 |
| C drive / `COMP1_OUT` | **PA0** | AF8 COMP1 in oscillator mode; GPIO output in RC-step mode. **Not PA8** — PA8's AF8 is COMP7_OUT |
| C mux A0/A1/EN | **PB3/PB4/PB5** | MAX4619 #1; EN active-low; PB3/PB4 are JTAG pins (SWD unaffected) |
| R mux A0/A1/EN | **PB6/PB7/PB2** | MAX4619 #2; EN active-low; **not PB8 (BOOT0)** |
| `R_MODE` (K1 coil) | **PB9** | 5 V relay driver; energised in R mode |
| `KELVIN` (K2 coil) | **PB10** | 5 V relay driver; energised in Kelvin only |
| `SSR13` | **PB11** | SSR1 precharge + SSR3 clamp, shared interlock |
| `SSR2` | **PB13** | N_DUT discharge |
| `VBIAS_RETURN` | **PB15** | Bias-domain return, held low in the safe state |
| `GPIO_R_FORCE` | **PB14** | High-Z in Kelvin/C; low (Q1 fully on) in TIA |
| Console | **PA9/PA10** | USART1 AF7, 115200 8N1. **Not PC4/PC5** |
| LED | **PC6** | Active-high user LED. **Not PC13** |
| SWD | PA13/PA14 | Reserved |
| USB | PA11/PA12 | Reserved (native USB, DFU) |
| HSE | PF0/PF1 | 8 MHz crystal, confirmed |
| User button | PC13 | Optional input; active-**high** (PC13 → 330 Ω → key → 3.3 V, WeAct V1.0 schematic) |

Free after this map: PA4, PA8, PA15, PC4, PC10, PC11. **PB8 is BOOT0** and must stay unused.

---

## 6. Chosen plan

The target is the rev 2 combined board, but the firmware is staged so C mode
keeps working and nothing pretends the R hardware exists.

### Stage 1 — correctness (this change)

- Correct the pin map, LED, console and comparator polarity.
- Correct OPAMP1 to `VINP = B (IO2)`, `VINM = VREF (IO0)`.
- Rewrite the R-mode model around `N_DUT`/`B`, with the Kelvin and TIA
  equations from the plan.
- Add the B-node relay layer (K1/K2) and the separate R mux.
- Add OPAMP3 as the V_N follower and the multi-ADC read paths.
- Gate the R hardware behind `ESWCAP_HAS_RMODE` (default **0**): `mode r`
  refuses with a clear message until the board is fitted.
- Keep the C-mode maths, calibration and `@@EVT` schema unchanged.
- Update the host tests for the new equations.

### Stage 2 — when the board is built

1. Verify the C-mode front end on the new pins (PA1/PA0/PB11/PB13/PB15/PB3-5).
2. Verify COMP1 output polarity and the hardware loop on PA0 with a scope.
3. Verify K1/K2 contact states with a multimeter before powering the DUT.
4. Calibrate `RSENSE_eff` per element and `Rf`/`VBIAS` for the TIA.
5. Set `ESWCAP_HAS_RMODE=1` and run the Kelvin/TIA bring-up checklist.

### What is deliberately **not** done

- No KiCad edits (the project is empty; it is the user's to fill in).
- No change to the ESP32 repo.
- No invented relay polarity: the relay drive polarity is a single compile-time
  constant, documented at its definition.
- No claim that any of this has run on silicon.

---

## 7. Bring-up checks that would have caught these errors

| Check | Expected | If it fails |
| --- | --- | --- |
| `status` at boot | banner on PA9/PA10, `SYSCLK 170000000` | console wiring/pins |
| LED | PC6 lights, active-high | check `PIN_LED`/polarity |
| C-mode `probe 2` | V_cap responds to drive | PA0 drive/COMP1_OUT mode |
| Oscillator `zero` | stable T0, no latch | COMP1 `INVERTED` |
| `mode r` | refuses until `ESWCAP_HAS_RMODE=1` | feature gate |
| Kelvin, 100 Ω | V_B ≈ 0.405 V, V_N ≈ 0.41 V | OPAMP1 `VINP=PA7/IO2`, Q1 polarity |
| Kelvin, 1 kΩ then 4.7 MΩ | the ladder moves up/down and keeps the better sample | `rmode_measure_auto` classification |
| TIA, 100 MΩ | V_TIA between 0 and 1.65 V | LMC6482 A orientation, K2B |
| `rcal 1 <measured ohm>` | `rcal?` shows the value and `calibrated` | `cmd_rcal_set`, flash store |
| `rcal tia <Rf> 1650` | `rcal?` shows the TIA line as `calibrated` | TIA calibration record v2 |

---

## 8. Bottom line

The handoff's **analysis of the analog topology is correct and valuable**; its
**pin-level instructions are not safe to apply literally**. My own R-mode was
based on an invented circuit and had two feedback-polarity bugs. Both are now
corrected in the tree, and the R hardware is explicitly gated until it exists.

---

## 9. Implementation status (2026-10-28)

The corrected plan is implemented in the tree:

- `include/config.h` is the single source of truth and carries the corrected
  combined-board map, plus `ESWCAP_HAS_RMODE` (default 0).
- LED is PC6/active-high; the console stays on PA9/PA10.
- COMP1 is `INVERTED`, INP = PA1 (`COMP_INPUT_PLUS_IO1`), and PA0 is both
  COMP1_OUT (AF8) and the drive-buffer input. The oscillator loop has no CPU in
  it; PA0 is switched back to GPIO for the RC step. (Originally PA8 — corrected,
  see §3.9.)
- OPAMP1 is `VINP = PA7` (`OPAMP_NONINVERTINGINPUT_IO2`), `VINM = PA3`
  (`OPAMP_INVERTINGINPUT_IO0`), `VOUT = PA2`.
- OPAMP3 buffers `V_N` on PB0/PB1; ADC3_IN1 reads the pin.
- The front end has K1/K2, the separate R mux, and the new `FE_R_*` states.
- R-mode is rewritten around `N_DUT`/`B`, with an up/down compliance ladder and
  the LMC6482 A TIA above ~22 Mohm.
- ADC2/ADC3/ADC4 single-read paths and the COMP4 overvoltage guard are in.
- The calibration record is version 2 and includes the TIA fields.
- The R-mode sample and telemetry now use the circuit's own vocabulary:
  `v_n_mv` / `v_b_mv` / `v_tia_mv` in `res_sample_t`, and
  `vn` / `vb` / `vtia` / `i` / `flags` in the `@@EVT res` line. The old
  `v_top` / `v_bot` names were the wrong topology's names.
- `rcal <element> <rref_ohm> [gain]`, `rcal tia <rf_ohm> <vbias_mv>
  [gain]`, `rcal?` and `rcalclear` cover the whole R-mode calibration
  loop; before this the values could be read and reset but never stored.
- `weact_g474ceu6_combined` builds the R path; the default build refuses
  `mode r` rather than energising hardware that is not fitted.

**Verified locally:** all four PlatformIO environments build with zero warnings;
`make -C test/host` passes 21/21, including the Kelvin/TIA equations and the
calibration-setter validation. **Not verified:** anything on silicon.

### Plan items deliberately not implemented

| Plan item | Status | Reason |
| --- | --- | --- |
| `mode c|r|both|auto` | `c|r` only | `both`/`auto` are undefined for a single DUT; adding them would guess at scheduling semantics |
| `rset` command | covered by `rcal` | the plan never defines `rset`'s operand; `rcal` stores every value the plan lists |
| COMP5 TIA-saturation flag | not fitted | COMP5's only bonded inputs are PB13 and PB10, which are SSR2 and K2 in this map. It needs a board change, not firmware |
| `DAC3_CH2 → OPAMP3 → ADC3_IN13` self-cal | not implemented | calibrates V_N only; there is no matching reference for the V_B path, so it cannot separate buffer gain from DUT ratio error |
| `vbuf` / per-ADC gain+offset records | not implemented | both buffers are unity followers into the ADC; the per-element and TIA gain terms already absorb the residual scale error that is measurable without a lab reference |

**Next:** the bring-up table in §7 is the list to run on the real board.

---

## 10. Rev B hardware revision (2026-10-28)

An independent schematic review (`kicad-design-review-agent`) found three
physics-level blockers and several robustness gaps. The schematic, PCB and BOM
were revised; the full record is in **`docs/pcb-design.md`**.

| Change | Ref | Reason |
| --- | --- | --- |
| Series body-diode block | **D6** 1N4148 between Q1 drain and `N_DUT` (new net `Q1_D`) | The AO3401 body diode would clamp `N_DUT` at ~4 V and inject ~1.6 A into +3V3 during a 20 V precharge |
| Bipolar C_block | **C1** 1000 µF / 35 V bipolar, 16 mm radial | The polarised 25 V part was reverse-biased by up to 20 V during precharge |
| Precharge resistor | **R9** 47 Ω 2 W (was 10 Ω) | Limits the AQY212 peak from ~1.7 A to ~0.4 A; 5τ ≈ 250 ms |
| Per-package decoupling | **C11–C14** 100 nF, **C15** 10 µF on +5 V | Only C8/C9 covered four ICs; no bulk on the relay rail |
| Fail-safe pulls | **R29** 10 k DRIVE→GND; **R30/R31** 100 k EN→+3V3 | PA0/PB5/PB2 are high-Z at reset |
| Console | **J7** 1×4 (TX=PA9, RX=PA10, GND, +3V3) | PA9/PA10 were NC-flagged, so no host UART |
| Compensation | **C3** moved from `B` to `VREF` | It was on the non-inverting input (positive feedback) |
| ADC protection | **R32** 1 k VCAP→PA1 (net `VCAP_ADC`) | Series protection / RC for the ADC |
| C1 bleed | **R33** 1 M across C1 | SSR3 could not discharge V_cap alone |
| Pulse resistors | R10 → 2512 1 W; R11 0.5 W | 187 mJ pulse energy |

**Checked and rejected:** the review's AQY212 "DIP-6" claim (the AQY212 is a
4-pin DIP; 3/4 are the output) and the G6S-2 "8-pad 1..8" claim (the official
footprint's pads are 1,3,4,5,8,9,10,12). **Deliberately not changed:** no TVS on
`N_DUT` (µA leakage through the 4.8 MΩ bias resistor), net names `V_BIAS` vs
`VBIAS`, and no K2-without-K1 interlock.

**PCB:** 111 × 85 mm, 2-layer, B.Cu ground plane only, 78 footprints, 0 DRC
errors / 0 unconnected, 25 silkscreen-only warnings. Fabrication files are in
`/home/jaguar/Documents/KiCad/Project/fab/`.

**Firmware:** `C_BLOCK_ELEC_F` is now 1000 µF; all four environments build and
the host tests pass 21/21. Nothing has been run on silicon yet.
