# Kelvin Ohmmeter — STM32G474-native build (combined with the capacitance meter)

> ## ⛔ SUPERSEDED — read this first
>
> This note was written for the **earlier, wrong R-mode topology**: it forces
> current into `V_cap` and senses `V_cap`/`N_DUT`. That cannot work, because
> `C_block` sits between `V_cap` and `N_DUT` and is an open circuit at DC.
>
> The correct topology is:
>
> ```
> +3.3 V -> Q1 -> N_DUT -> DUT -> B -> K2A -> R mux -> RSENSE -> GND
> ```
>
> `N_DUT` is the force node, `B` is the sense node, and `C_block` is isolated by
> `K1B` in R mode. The authoritative, evidence-backed map is in
> **`docs/handoff-verdict.md` §5**; the implementation is
> `src/measure/rmode.c` and `src/bsp/bsp_frontend.c`.
>
> The **error-budget, protection and calibration reasoning** below is still
> useful, but every occurrence of "force into V_cap", "V_top = V_cap" and
> "V_bot = N_DUT" must be read as **force into N_DUT, V_N = N_DUT, V_B = B**.
>
> The original package note is retained for context.
>
> ## ⚠️ Package difference — re-validate the pin map before porting
>
> This note was written for a **100-pin STM32G474VE**. The actual target is a
> **48-pin STM32G474CEU6** (WeAct "Long" board). The good news is that the
> internal analog is largely intact: **OPAMP1/2/3 are bonded out** and
> **all 7 comparators** have pins, and DAC1 (`OUT1=PA4`, `OUT2=PA5`) is present.
> USB is on PA11/PA12, so it does **not** conflict with OPAMP1's PA2/PA3.
>
> What does change: `VREF+` is a dedicated pin (good for a clean reference), and
> `PA4`/`PA5` are shared between `DAC1_OUT` and COMP1/COMP2 `INM`, so they cannot
> double as general DAC setpoints. See the project `README.md` for the verified
> per-pin availability tables.
>
> The **electrical design** below (sensing topology, ratiometric maths, error
> budget, mode interlock, protection) is still valid. The **pin map (§9)** should
> be re-checked against the G474CEU6 tables in the README.

DC 4-wire Kelvin resistance front-end for **1 Ω – 600 Ω**, folded into the ESWCap
instrument on an **STM32G474**, using the G474's **own ADC** (no ADS1115, no
ESP32 ADC path). Shares the DUT interface, the safety bank, and the
firmware/dashboard stack with the capacitance meter.

> Supersedes the earlier ESP32-ADC/ADS1115 version. The G474 replaces *all* of the
> MCU-side analog: internal ADC for sensing, and (optionally) an internal op-amp +
> DAC for the current source.

---

## 1. Instrument overview (two modes, one DUT)

```
   C mode (existing, unchanged maths)
   range R -> V_cap -> C_block -> N_DUT -> C_DUT -> B -> K1A -> GND
                    (ADC1_IN2)                 (bias: R_bias 4.8M, SSR1/SSR2)

   R mode (combined board)
   +3.3 V -> Q1 -> N_DUT -> R_DUT -> B -> K2A -> R mux -> RSENSE -> GND
              |            |          |
              |            |          +--> LMC6482 B -> ADC4_IN3 (V_B)
              |            +--> 1k/1M -> OPAMP3 -> ADC3_IN1 (V_N)
              +--> K1B isolates C_block; OPAMP1 servos B to VREF
                   (VINP = B/PA7, VINM = VREF/PA3, VOUT = Q1 gate/PA2)
```

- **C mode**: the existing AC/transient front-end. `C_block` is in series with
  the DUT, so the measured series equivalent is
  `C_block*C_dut/(C_block+C_dut)`; the order of the two elements does not
  matter.
- **R mode**: DC 4-wire Kelvin into `N_DUT`, sensed at `N_DUT` (`V_N`) and `B`
  (`V_B`). `C_block` is disconnected by `K1B` because it is an open circuit at
  DC and would otherwise sit in the force path. Above ~22 Mohm the external
  LMC6482 A TIA takes over.
- The two modes are **mutually exclusive** (mode interlock, §8).

A separate DC front-end is required because the capacitance chain is AC and the
DUT sits behind the 1000 uF (Rev B bipolar) `C_block` — an **open at DC**.

---

## 2. Why the G474 changes the design

| G474 resource | Count / spec | Use here |
| --- | --- | --- |
| ADC | 5 × 12-bit, 4 Msps, HW oversampling to 16-bit | V_top + V_bot (or one shared channel) |
| Op-amp | 6 × (PGA-capable, standalone mode) | current-source error amplifier (replaces LMC6482) |
| DAC | 3 × external buffered (+4 internal) | NSET setpoint (replaces the R1/R2 divider) |
| Comparator | 7 | optional: replace the LM393 oscillator |
| VREFBUF | internal 2.048 / 2.5 / 2.9 V | optional stable reference |
| VREF+ range | 0 – 3.3 V (to VDDA) | sensing full-scale |

Two important consequences:

1. **Single-rail current source.** The internal op-amp and DAC run on 3.3 V, so
   the current source must too (the 5 V/4.36 V NSET of the original plan is
   impossible — the op-amp cannot servo the emitter above 3.3 V). This lowers the
   compliance and re-sizes RSET/RREF (§4).
2. **Ratiometric design still applies.** `VREF` and the excitation are not
   critical to accuracy (they cancel in the ratio) — but **ADC channel matching**
   and **RREF** are (§5, §7).

---

## 3. Shared resources (reused from the capacitance meter)

| Existing element | R-mode use | Notes |
| --- | --- | --- |
| DUT socket (`N_DUT` ↔ `B`) | 2-terminal resistance DUT | a 4-terminal fixture gives true Kelvin |
| `N_DUT` | Force-Hi **and** V_N sense | Q1 drain; 1k/1M -> OPAMP3 -> ADC3_IN1 |
| `B` | Force-Lo **and** V_B sense | K1A grounds it in C mode; K2A connects it to the R mux |
| `C_block` | isolated by `K1B` in R mode | open at DC; would otherwise sit in the force path |
| `R_bias` | no DC effect in R mode | 4.8 Mohm to V_BIAS; V_BIAS = 0 in R mode |
| BAT54S on `V_cap` | C-mode clamp only | not in the R force path |
| NVS calibration + console + `@EVT` + dashboard | resistance value, cal constants, panel | extend, do not fork |

**Net new parts:** Q1 (AO3401 P-FET), R13/R14/R15/C3 gate network, the LMC6482
A/B, the K1/K2 G6S-2 relays, the second MAX4619 (R mux), OPAMP3 as the V_N
buffer, and the VREF/VBIAS dividers. The internal OPAMP1 is the Kelvin error
amplifier; OPAMP2 is **not** usable (wrong polarity for the TIA and no DAC
path).

---

## 4. R-mode front-end (G474-native)

### 4.1 Implemented: OPAMP1 + Q1, single 3.3 V rail

```
                       +3.3 V
                         │
                    ┌────┴────┐
                    │  Q1     │  AO3401 P-FET
                    │  P-FET  │  R13 100k gate pull-up (off)
                    └────┬────┘  R14 1k from OPAMP1_OUT
                         │       R15 10k from GPIO_R_FORCE (TIA only)
                      N_DUT
                         │
                     [ R_DUT ]
                         │
                         B ──┬── LMC6482 B -> ADC4_IN3 (V_B)
                             │
                     K2A ── R mux ── RSENSE ── GND
```

- `OPAMP1` is a standalone error amplifier:
  `VINP = PA7 = B`, `VINM = PA3 = VREF = 0.405 V`, `VOUT = PA2 = Q1 gate`.
  This is **negative feedback** with a high-side P-FET: B rises -> output rises
  -> gate rises -> Q1 conducts less -> B falls.
- The loop forces `V_B = VREF`, so `I = VREF / RSENSE_eff`.
- `V_N = I*(R_DUT + RSENSE_eff)` and
  `R_DUT = RSENSE_eff*(V_N - V_B)/V_B`.
- `VREF` is a 7.15k/1k divider. It cancels in the ratio, so its accuracy is not
  critical; it only has to keep `V_N <= 2.3 V`.
- `RSENSE_eff` includes the MAX4619 Ron and the K2A contact resistance; calibrate
  per element. The 98.9 ohm element is the one where this matters most.
- `GPIO_R_FORCE` is high-Z in Kelvin (R13 turns Q1 off if OPAMP1 is off) and low
  in TIA (Q1 fully on, ~0.3 V gate).

**Why not the old DAC setpoint:** OPAMP1's non-inverting input must be B for
negative feedback. A DAC on VINP with the sense on VINM is positive feedback and
latches. DAC3_CH1 is therefore used only by COMP1, and DAC1_CH1 only by COMP4.

### 4.2 Historical alternative: external 5 V source (not used)

> Superseded by §4.1. The combined board uses OPAMP1 + Q1 + the LMC6482 A
> TIA; there is no 5 V current source.

If you prefer the original discrete current source (`+5 V`, `NSET ≈ 4.36 V`,
`I ≈ 4.3 mA`, `V_top@600 ≈ 3.0 V`), keep LMC6482 + divider and supply only the
**sensing** from G474. Set `VREF+ = VDDA (3.3 V)` so V_top fits. This is the
lowest-risk path if you want to change only the ADC, but it keeps a 5 V rail and
the extra parts.

### 4.3 Return and RSENSE (historical sketches)

> Superseded. The implemented return is `B -> K2A -> MAX4619 -> RSENSE -> GND`.
> The two options below are the earlier sketches and do not describe the
> firmware.

- **Option A (fewest parts):** reuse the existing `SSR2` + 100 Ω as the DC return
  and RREF; sense V_bot at `N_DUT`; calibrate the 100 Ω into NVS. SSR on-resistance
  (≈1–2 Ω, tempco) adds to RREF → fine for ~1 %, not 0.1 %.
- **Option B (recommended for a permanent build):** dedicated **RREF = 98 Ω,
  0.1 %, low tempco** in a new branch `N_DUT → RREF → SSR_R → GND`, with its own
  switch; SSR2 stays discharge-only.

Either way, **Kelvin-sense RREF's own ground return** (ADC ground = bottom of
RREF, star point) or a systematic offset appears.

---

## 5. ADC strategy (the part that matters most)

> Terminology: in this section `V_top` means **V_N (N_DUT)** and `V_bot` means
> **V_B**, the two Kelvin sense nodes.

The ratiometric formula cancels supply, current-source, transistor-β and VREF
errors. What **does not** cancel is **channel-to-channel gain/offset mismatch**
between the V_top and V_bot readings — and that dominates the error at the low
end (at 1 Ω, `V_top − V_bot` is only ~3 mV).

**Recommended — one ADC input, signals muxed in time:**
Route `V_top` and `V_bot` through a **2:1 analog mux** (a spare half of the
existing MAX4618/4619 if present, or a small TS5A/ADG switch) into **one** G474
ADC channel. Both nodes are then converted by the *identical* ADC path, so gain
and offset cancel exactly. Mux `R_on` feeds a high-impedance ADC input (no
current) → negligible. Add per-channel-of-the-mux offset trim only if needed.

**Fallback — two ADC channels of one ADC:** simpler wiring, but each channel
needs its own gain/offset calibration (G474 is far better matched than the ESP32,
but still calibrate). Acceptable if the low end tolerates a few %.

**Do not assume differential mode is easy here.** STM32 differential ADC expects
the inputs biased around `VREF/2`; our common mode (`(V_top+V_bot)/2`) swings
from ~0.33 V (1 Ω) to ~1.3 V (600 Ω), so it does not satisfy that across the
range. Use differential only if you add a proper difference amplifier.

**Oversampling:** enable HW oversampling (e.g. 64–256×) on the R-mode channels;
this is where the G474 wins over a bare ESP32 ADC. Oversample both nodes
independently, then form the ratio.

**Reference:** `VREF+ = VDDA (3.3 V)` is simplest and range-compatible; internal
VREFBUF (2.5 V) gives lower noise if you size the current so `V_top ≤ 2.5 V`
(§4.1 optional variant). Because the result is a ratio, reference absolute value
is irrelevant — only its stability during the two conversions matters.

---

## 6. Forcing / sensing topology (4-wire)

```
  +3.3 V ── Q1 ──► N_DUT ──┬── Force-Hi
                            │
                            ├── V_N sense ── 1k/1M ── OPAMP3 ── ADC3_IN1
                            │
                        [ R_DUT ]
                            │
                            B ──┬── Force-Lo / V_B sense ── LMC6482 B ── ADC4_IN3
                                │
                        K2A ── R mux ── RSENSE ── GND

  K1B isolates C_block from N_DUT in R mode.
  K1A grounds B in C mode; open in R mode.
```

A 2-terminal socket gives 2-wire only (lead drop adds ~0.1–1 ohm). For the full
Kelvin benefit, provide a **4-terminal fixture** so Force and Sense join at the
DUT lead and the sense path carries no current.

The TIA path is separate:

```
  B ── LMC6482 A IN- ──┬── Rf 10M / Cf 10p ── TIA_OUT ── ADC2_IN3
                       │
  VBIAS 1.65 V ── IN+  └── K2B opens the feedback in Kelvin
```

## 7. Measurement math and error budget

```
V_N = I * (R_DUT + RSENSE_eff)
V_B = I * RSENSE_eff
R_DUT = RSENSE_eff * (V_N - V_B) / V_B

TIA: R_DUT = Rf * (V_N - VBIAS) / (VBIAS - V_TIA)
```

| Error source | Cancels? | Mitigation |
| --- | --- | --- |
| Supply / excitation (DAC, rail) | **Yes** (ratio) | none |
| Pass-transistor β / gate current | **Yes** | none (MOSFET removes it anyway) |
| Op-amp offset | **Yes** | none |
| ADC reference VREF | **Yes** | no precision VREF needed |
| **ADC channel gain/offset mismatch** | **No** | time-muxed single-channel (§5) — primary |
| **RREF tolerance + tempco** | **No** | 0.1 % low-tempco; Option B |
| RREF ground-return drop | **No** | Kelvin-sense RREF bottom / star ground |
| ADC noise | Partial | HW oversampling 64–256× |
| Lead/trace drop | only in 4-wire | 4-terminal fixture |

Expected: sub-1 % across 1–600 Ω with the time-muxed front-end and a 0.1 % RREF;
a few % with the two-channel fallback and Option A.

---

## 8. Mode interlock (safety-critical)

Only one front-end may drive the DUT.

| Signal | C-mode | R-mode |
| --- | --- | --- |
| Drive buffer / COMP1_OUT (PA8) | COMP1 AF in oscillator, GPIO in RC step | **held LOW** (GPIO) |
| C mux (MAX4619 #1) | ON (selected range) | **OFF** |
| R mux (MAX4619 #2) | **OFF** | ON in Kelvin, OFF in TIA |
| K1 (`R_MODE`) | **OFF** (B grounded, C_block connected) | **ON** (B lifted, C_block isolated) |
| K2 (`KELVIN`) | OFF | ON in Kelvin, OFF in TIA |
| OPAMP1 (Kelvin loop) | OFF | ON in Kelvin, OFF in TIA |
| OPAMP3 (V_N buffer) | OFF | ON for the whole R session |
| LMC6482 A (TIA) | powered, feedback closed but B grounded | Kelvin: feedback open (K2); TIA: closed |
| SSR1/SSR3 | per C state machine | **OFF** |
| SSR2 | never during a C measurement | **OFF** (used only in the discharge phase) |
| V_BIAS | as set | **0 V / off** |
| COMP1 | active in oscillator | **OFF** |
| COMP4 | off | **armed** (V_N overvoltage) |

Rules:
1. Entering R-mode: `discharge` first, then K1 on, K2 off, R mux off, OPAMP3 on.
2. Entering Kelvin: R mux select+enable, then K2 on, then OPAMP1 on.
3. Entering TIA: OPAMP1 off, K2 off, R mux off, then `GPIO_R_FORCE` low.
4. Leaving R-mode: force off, K2 off, K1 off, discharge, OPAMP3 off.
5. K2 must never be energised without K1.
6. **"Never close SSR2 during a measurement" is a C-mode-only rule.**
7. COMP4 trips at 2.6 V; the ISR removes OPAMP1 drive, `GPIO_R_FORCE` and the
   R mux immediately, and the main loop finishes with `fe_r_exit()`.

---

## 9. G474 pin map — corrected for the UFQFPN48

| Function | Pin | Notes |
| --- | --- | --- |
| `V_cap` | PA1 | ADC1_IN2, COMP1_INP (`COMP_INPUT_PLUS_IO1`) |
| `B` | PA7 | OPAMP1_VINP2 (`OPAMP_NONINVERTINGINPUT_IO2`), ADC2_IN4 |
| `VREF` (0.405 V) | PA3 | OPAMP1_VINM0 (`OPAMP_INVERTINGINPUT_IO0`), ADC1_IN4 |
| Q1 gate | PA2 | OPAMP1_VOUT |
| `V_N_SENSE` | PB0 | OPAMP3_VINP0, COMP4_INP |
| `V_N` buffer out | PB1 | OPAMP3_VOUT, ADC3_IN1 |
| `V_B` buffer out | PB12 | LMC6482 B output, ADC4_IN3 |
| `TIA_OUT` | PA6 | LMC6482 A output, ADC2_IN3 |
| `VBIAS` (1.65 V) | PA5 | ADC2_IN13 |
| C drive / `COMP1_OUT` | PA8 | AF8 in oscillator mode, GPIO in RC step |
| C mux A0/A1/EN | PB3/PB4/PB5 | MAX4619 #1; EN active-low |
| R mux A0/A1/EN | PB6/PB7/PB2 | MAX4619 #2; EN active-low; **not PB8 (BOOT0)** |
| `R_MODE` (K1) | PB9 | 5 V relay driver |
| `KELVIN` (K2) | PB10 | 5 V relay driver; never without K1 |
| `SSR13` | PB11 | SSR1 precharge + SSR3 clamp |
| `SSR2` | PB13 | N_DUT discharge |
| `VBIAS_RETURN` | PB15 | held low in the safe state |
| `GPIO_R_FORCE` | PB14 | high-Z Kelvin/C, low TIA |
| Console | PA9/PA10 | USART1 AF7. **PC5 is not bonded** |
| LED | PC6 | active-high. **PC13 is a button** |
| SWD / USB / HSE | PA13/PA14, PA11/PA12, PF0/PF1 | reserved |

Free: PA0, PA4, PA15, PB8 (BOOT0), PC4, PC10, PC11.

## 10. Firmware hosting — SUPERSEDED

> **This section is superseded by [`port-plan.md`](port-plan.md).** It sketched
> a 1:1 block-for-block translation; the actual plan goes further, because the
> G474 replaces whole *mechanisms* rather than individual APIs. Summary of the
> differences:
>
> | This document said | The plan actually does |
> | --- | --- |
> | "EXTI/PWM capture **or** its internal comparator" | **COMP1 → TIM2_CH1 input capture**, internally, no pin, no ISR |
> | "emulated EEPROM in flash" | reserved 2 KB page in **bank 2**, append-only CRC records |
> | "calibrate per-channel gain/offset in firmware" | per-**range** oversampling ratio; offset never via `ADC_OFRy` (ignored under oversampling) |
> | (implied) port the ADC poll loop | **TIM6-triggered ADC + circular DMA**; the poll loop and its decimation are deleted |
>
> The parts that do survive: the **`@@EVT` protocol byte-for-byte**, the
> `mode c|r` / `rcal?` / `rset rref` command additions, the `res` telemetry
> event, and the current-source enable/disable interlock.

---

## 11. Protection and failure modes

- **Open/missing DUT:** collector floats toward the rail → clamp **V_top**
  (existing BAT54S) and **V_bot** (add BAT54S + ~1 k series). Use ~1 k, not 100 Ω
  (100 Ω limits fault current to ~17 mA, too high).
- **SSR3 on in R-mode** shorts the source — enforce interlock (§8).
- **C-mode with source enabled** loads the RC node — disable DAC/OPAMP in C-mode.
- **Bias node:** set V_BIAS = 0 in R-mode; optionally ground the far side of
  C_block to bound its leakage.
- **Loop stability:** CC (≈100 pF) on OPAMP1 may be needed; verify on a scope.

---

## 12. Bring-up / validation checklist

1. Current source standalone: measure `I = V(RREF)/RREF` for R_DUT = short, 1, 10,
   100, 600 Ω; confirm linearity and compliance (≤ ~700 Ω).
2. Interlock: no combination drives both front-ends; SSR/DAC states match §8.
3. Ratiometric check: swap RREF ±10 % → `R_DUT` unchanged.
4. Time-mux channel matching: read the same node on both mux positions → zero
   residual; then re-run the range.
5. Over-voltage: remove the DUT → V_top/V_bot clamped, no pin stress.
6. C-mode regression: full capacitance sweep before/after R-mode (bias rules
   intact).
7. Host: existing dashboard still parses telemetry; new `res` event renders.

---

## 13. Open items to verify

- **Mux part:** MAX4619 dual 4:1 is assumed; the C mux and the R mux are two
  halves. Confirm the second half is actually wired to the RSENSE bank.
- **OPAMP/DAC pin routing** is fixed in `include/config.h`; verify the built
  board matches, especially PA7 (B) and PA3 (VREF).
- **Relay BOM:** the plan specifies G6S-2; the ESP32 combined-circuit note
  proposed SSR_CB/SSR_LO. Confirm which is fitted. The firmware drives K1/K2.
- **5 V rail** for the relay coils: the WeAct board only has Type-C VBUS.
- **RSENSE tolerance/tempco**, especially the 98.9 ohm element where the mux and
  relay Ron dominate.
- **4-terminal fixture** decision (2-wire vs true Kelvin).
- **TIA bias current at 1 Gohm** — bench-verify the LMC6482 A before trusting
  the top decade.