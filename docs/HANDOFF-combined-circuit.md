# Handoff — implement the combined C + R plan in the STM32G474 firmware

> **Review status (2026-10-28): do not apply this document literally.**
> It has been reviewed against the physical WeAct board schematic and the
> STM32G474CEU6 pin database. The topology and feedback-polarity conclusions
> are correct; several pin-level instructions are not (PC5 is not bonded on
> UFQFPN48, the LED is PC6 not PC13, and OPAMP1 PA7 is `IO2`, not `IO0`).
> See **`docs/handoff-verdict.md`** for the claim-by-claim verdict, the
> evidence, and the corrected pin map. The firmware in `src/` implements the
> corrected version.

> **Rev B hardware (2026-10-28):** the independent review found and fixed three
> physics-level blockers: a series 1N4148 (D6) blocks Q1's body diode so the
> 0–20 V bias works, C1 is now a 1000 µF/35 V **bipolar** part, and R9 is 47 Ω
> to keep the precharge inside the AQY212 rating. C3 moved to VREF, the
> console is brought out on J7, and per-IC decoupling/fail-safe pulls were
> added. See **`docs/pcb-design.md`** for the full revision, board mechanics
> and fabrication files.

**Repo:** `/home/jaguar/Documents/STMProject`
**Authoritative design:** `docs/resistance-autoranging-plan.md` (rev 2)
**Supporting facts:** `docs/peripherals-g474.md`, `README.md`
**Scope:** this repo only. Do not edit the ESWCap repo, the KiCAD project, or the plan unless you find a factual error (report it instead).

The plan supersedes several assumptions in the current scaffold. The code builds
today, but its R-mode topology, pin map and OPAMP1 configuration are from the
earlier (wrong) design. Update the firmware to the plan; do not preserve the old
assumptions.

---

## 1. The topology correction (most important)

The current `src/measure/rmode.c` header and maths assume:

```
V_cap --[ R_dut ]-- node_bot --[ R_sense ]-- GND
```

That is **not** the ESWCap board. The real circuit is:

```
V_cap --[ C_block ]--[K1B]-- N_DUT --[ R_dut ]-- B --[K1A]-- GND
```

- `N_DUT` is the DUT top; it is the C-mode bias node and the R-mode **force**
  node.
- `B` is the DUT bottom; it is grounded in C mode and becomes the **sense**
  node in R mode.
- `C_block` is between `V_cap` and `N_DUT`; it is isolated by `K1B` in R mode.
- `V_cap` is **not** the R-mode force node. Forcing `V_cap` only charges
  `C_block`.

All R-mode comments, equations and node names must be rewritten around
`N_DUT`/`B`. The Kelvin equation stays the same in form:

```
R_dut = RSENSE_eff * (V_N - V_B) / V_B
```

but `V_N` is `N_DUT`, `V_B` is `B`, and the current is forced into `N_DUT` by
Q1, not into `V_cap`.

---

## 2. Pin map to implement

Replace `include/config.h` with this map. It is the plan's map; the current
scaffold's PA0/PA1/PB0 assignments are obsolete.

| Signal | Pin | Peripheral / note |
| --- | --- | --- |
| `V_cap` | **PA1** | `ADC1_IN2` **and** `COMP1_INP`; one pin for both |
| `B` (DUT bottom / Kelvin sense) | **PA7** | `OPAMP1_VINP`, `ADC2_IN4`; LMC6482 B buffers it |
| `VREF` | **PA3** | `OPAMP1_VINM`, `ADC1_IN4`; 0.405 V divider |
| Q1 gate | **PA2** | `OPAMP1_VOUT` |
| `VBIAS` | **PA5** | TIA reference divider, `ADC2_IN13`; OPAMP2 VINM (OPAMP2 off) |
| `TIA_OUT` | **PA6** | LMC6482 A output, `ADC2_IN3`; OPAMP2 VOUT (off) |
| `COMP1_OUT` / C drive | **PA8** | COMP1 AF8; GPIO output for the RC step; drives SN74LVC1G34 |
| `V_N_SENSE` | **PB0** | `OPAMP3_VINP`, `COMP4_INP`; 1 MΩ + BAT54S + 1 nF |
| `V_N` buffer out | **PB1** | `OPAMP3_VOUT` (internal `ADC3_IN13` also possible) |
| `OPAMP3_VINM` | **PB2** | NC in follower mode; `COMP4_INM` (DAC1_CH1 internal) |
| C mux A0/A1/EN | **PB3/PB4/PB5** | MAX4619 #1; EN active-low |
| R mux A0/A1/EN | **PB6/PB7/PB8** | MAX4619 #2 (spare); EN active-low |
| `R_MODE` (K1 coil) | **PB9** | 5 V relay driver; energized in R mode |
| `KELVIN` (K2 coil) | **PB10** | 5 V relay driver; energized in Kelvin only |
| `SSR13` | **PB11** | SSR1 precharge + SSR3 V_cap clamp, shared interlock |
| `V_B` buffer out | **PB12** | LMC6482 B output, `ADC4_IN3` |
| `SSR2` | **PB13** | N_DUT discharge |
| `GPIO_R_FORCE` | **PB14** | high-Z Kelvin/C, low TIA |
| `VBIAS_RETURN` | **PB15** | existing bias-domain return, held in the safe state |
| UART | **PC4/PC5** | USART1 TX/RX; do not use PA2/PA3 |
| SWD | PA13/PA14 | reserved |
| USB | PA11/PA12 | reserved |
| HSE | PF0/PF1 | reserved |
| LED | PC13 | active-low |

`VBIAS_RETURN` is the existing ESP32 `GPIO3` function; preserve its polarity
from the hardware. If the board hard-wires the return, leave the GPIO unused
and document it.

### 2.1 OPAMP1 must change polarity

Current `src/bsp/bsp_opamp.c` configures:

```
VINP = DAC3_CH1
VINM = PA3
VOUT = PA2
```

That is wrong for the P-FET servo. The plan uses:

```
VINP = PA7 = B
VINM = PA3 = VREF
VOUT = PA2 = Q1 gate
```

Negative feedback: if `B` rises, the op-amp output rises, the P-FET turns off,
and `B` falls. With the old `VINP=DAC` configuration the feedback is positive.
Change `NonInvertingInput` to `OPAMP_NONINVERTINGINPUT_IO0` (PA7), keep
`InvertingInput = OPAMP_INVERTINGINPUT_IO0` (PA3), and remove the DAC3 setpoint
from OPAMP1. DAC3_CH1 now belongs to COMP1.

`OPAMP2` is **not** a fallback TIA. Its `VINP` is PA7 = B and `VINM` is
PA5 = VBIAS, the opposite polarity from a TIA. Leave it powered down; its pins
are reused as B/VBIAS/TIA_OUT nodes. The TIA is the external LMC6482 A.

---

## 3. B-node switching: K1/K2 G6S-2 relays

The two spare AQY212 are no longer the B-node switches. Use the Omron
G6S-2 DC5 DPDT relays:

**K1 — `R_MODE` (energized in R mode: Kelvin and TIA)**

| Contact | Common | NC (off) | NO (on) |
| --- | --- | --- | --- |
| `K1A` | B | **GND** | open |
| `K1B` | N_DUT | **C_block** | open |

**K2 — `KELVIN` (energized in Kelvin only)**

| Contact | Common | NC (off) | NO (on) |
| --- | --- | --- | --- |
| `K2A` | B | open | **R mux common** |
| `K2B` | TIA_OUT | **Rf/Cf top** | open |

State table:

| Mode | K1 | K2 | B | C_block | R mux | TIA feedback |
| --- | --- | --- | --- | --- | --- | --- |
| C | off | off | GND | connected | open | closed (TIA unused) |
| Kelvin | on | on | lifted | isolated | connected | open |
| TIA | on | off | lifted | isolated | open | closed |

- K1 is fail-safe: power loss de-energizes it, grounding B and reconnecting
  C_block.
- K2 must not energize unless K1 is energized. Enforce in firmware; optionally
  return the K2 driver's emitter/source to the K1 coil low side as a hardware
  interlock.
- Relay coils are 5 V, ~28 mA; drive with BC847B/2N7002 + 1 kΩ + 1N4148
  flyback. Wait 10 ms after any coil change before enabling OPAMP1/TIA or
  sampling.
- C-mode DUT current through K1A is ≤33 mA. K1B carries the C_block precharge
  inrush; change R9 from 10 Ω to **47 Ω, 2 W** to keep the peak ~0.4 A
  (Rev B; the earlier 22 Ω recommendation is superseded — 22 Ω still lets
  ~0.9 A through the AQY212).
- The existing SSR1/SSR2/SSR3 remain AQY212; the two spare AQY212 are spares.

Replace the current `SSR5`/`SSR4`/`SSRR` routing in `bsp_frontend.c` with
K1/K2. The old `fe_res_isolation(range_on, force_on, rref_on)` API should become
a mode-aware relay control; keep the C-mode SSR1/SSR2/SSR3 semantics.

---

## 4. R front-end

### 4.1 Kelvin

- Dedicated R mux U4 = spare MAX4619, 4:1, common = B through `K2A`.
- `RSENSE` = 98.9 Ω, 993.6 Ω, 98.6 kΩ, 4.7 MΩ (0.1 %, 25 ppm).
- Throws → resistors → GND.
- OPAMP1 forces `V_B = VREF = 0.405 V`.
- `I = VREF / RSENSE_eff`; `R_dut = RSENSE_eff*(V_N - V_B)/V_B`.
- `V_N ≤ 2.3 V`; `R_dut,max ≈ 4.68*RSENSE`.
- Q1: AO3401, source +3.3 V, drain N_DUT; R13 100 kΩ gate pull-up, R14 1 kΩ
  from OPAMP1, R15 10 kΩ from `GPIO_R_FORCE`; C3 100 pF VOUT→VINP.
- In TIA, `GPIO_R_FORCE` low → gate divider ~0.3 V → Q1 fully on.

### 4.2 TIA

- LMC6482 A: `IN+ = VBIAS`, `IN- = B`, `OUT = TIA_OUT`.
- Rf = 10 MΩ, Cf = 10 pF; `K2B` disconnects the feedback in Kelvin.
- `V_TIA = VBIAS - (V_N - VBIAS)*Rf/R_dut`.
- `R_dut = Rf*(V_N - VBIAS)/(VBIAS - V_TIA)`.
- `VBIAS = 1.65 V`; TIA usable from ~15 MΩ to 1 GΩ.
- OPAMP2 off; do not use it as the TIA.

### 4.3 V_N / V_B sensing

- `N_DUT → 1 MΩ → V_N_SENSE → BAT54S clamp → OPAMP3 VINP (PB0)`.
- 1 nF at V_N_SENSE; OPAMP3 follower; VOUT PB1 / internal ADC3_IN13.
- No 900 MΩ/100 MΩ divider. It destroyed the 1 Ω resolution.
- `B → LMC6482 B IN+`, unity follower, OUT → PB12/ADC4_IN3.
- `TIA_OUT → PA6/ADC2_IN3`, BAV199 clamp.
- VBIAS divider 10 kΩ/10 kΩ → PA5/ADC2_IN13.
- VREF divider 7.15 kΩ/1 kΩ → PA3/ADC1_IN4.

### 4.4 ADC channel map

Use separate ADCs so the critical nodes are not interleaved in one sequence:

| Node | ADC | Pin |
| --- | --- | --- |
| `V_cap` | ADC1_IN2 | PA1 |
| `V_TIA` | ADC2_IN3 | PA6 |
| `V_N` | ADC3_IN13 (OPAMP3 internal) | PB1/internal |
| `V_B` | ADC4_IN3 | PB12 |
| `V_BIAS` | ADC2_IN13 | PA5 |
| `VREF` | ADC1_IN4 | PA3 |

Update `bsp_adc.c` and `config.h`. Keep the per-range oversampling and the
errata workarounds already implemented.

---

## 5. C-mode changes

- COMP1 `INP = PA1` (`V_cap`), `INM = DAC3_CH1` (1.65 V), `OUT = PA8`.
- Set COMP1 output polarity so PA8 is high when `V_cap` is below the lower
  threshold. PA8 drives the SN74LVC1G34 buffer directly; the comparator closes
  the oscillator in hardware.
- Add a 10 kΩ pull-down on the buffer input so drive is low when COMP1 is off.
- `COMP1_OUT → TIM2_CH1` capture. The current `bsp_comp.c` uses an EXTI mirror
  and a PA15/TIM2_CH1 jumper; replace that with the hardware loop where
  possible. If the internal COMP1→TIM2 route cannot be configured, keep the
  jumper but document it and make `ESWCAP_HW_OSC_LOOP` the default.
- RC-step mode: PA8 as GPIO high; ADC1_IN2 (PA1) + TIM6 + DMA; per-range
  oversampling. COMP1 disabled.
- Remove the LM393 and its 100 kΩ hysteresis / 10 kΩ pull-up. Keep the
  10 kΩ/10 kΩ 1.65 V divider as VBIAS.
- Preserve the C-mode `C_block`/stray maths and calibration.

---

## 6. Front-end state machine and safety

Implement the plan's sequence:

```
IDLE
  → SAFE: V_BIAS=0, Q1 off, OPAMP1 off; SSR2 on until N_DUT≈0;
          then K1 off (B=GND, C_block connected), K2 off,
          C mux off, R mux off, OPAMP2/3 off
  → R_ISOLATE: SSR2 off; K1 on; K2 off; R mux off; OPAMP3 on;
               wait 10 ms
  → R_KELVIN: K2 on; R mux on; OPAMP1 on; Q1 loop;
              wait B=0.405 V; sample V_N/V_B
  → R_TIA: OPAMP1 off; K2 off; R mux off; Q1 force on;
           sample V_N/V_TIA
  → R_DISCHARGE: Q1 off; OPAMP1 off; R mux off; K2 off; K1 off;
                 then IDLE
```

Invariants:

1. Never close K1B with N_DUT biased; discharge and V_BIAS=0 first.
2. Never energize K1 (open K1A) with B driven; Q1 off first.
3. Never energize K2 unless K1 is energized.
4. Never enable the R mux in TIA; K2A is open.
5. Never enable the C mux in R.
6. Preserve the C-mode “never close SSR2 during a measurement” rule; it is
   C-mode-only.
7. Wait 10 ms after relay changes; verify V_B/V_N before the next stage.
8. IWDG on every capture; COMP4 V_N overvoltage → EXIT → Q1/R mux off.

---

## 7. Calibration and telemetry

- Per Kelvin element: `RSENSE_eff` (includes mux Ron), gain/offset.
- TIA: `Rf`, `VBIAS`, `V_TIA` offset/gain.
- V_B/V_N buffer gain/offset; ADC channel gain/offset.
- C-mode `K`/`T0`, `R_eff`/`C0` unchanged; recalibrate because COMP1 replaces
  the LM393 and the drive is now hardware-closed.
- Store in the existing flash record store; keep the `rcal` command surface.
- Add the `@@EVT res` event exactly as the plan specifies. Do not change the
  existing C-mode event schema.

---

## 8. Build and test

- Use PlatformIO from `~/.platformio/penv/bin/pio` if `pio` is not on PATH.
- `pio run -e weact_g474ceu6` must build. Fix all errors and warnings you
  introduce.
- `make -C test/host` (or the existing host test target) must pass. Update the
  host tests for the new topology/equations.
- Audit with grep:
  - no `V_cap --[ R_dut ]`, `node_bot --[ R_sense ]` topology comments;
  - no `OPAMP1` `VINP=DAC` / `DAC3_CH1` setpoint;
  - no `SSR4`, `SSR5`, `SSRR` in the active front-end path;
  - no `PA0` as V_bot, no `PB0` as drive.
- Report changed files, build/test output, and any hardware question you
  cannot resolve.

---

## 9. Do not

- Do not edit `/home/jaguar/Documents/PlatformIO/Projects/ESWCap` or
  `/home/jaguar/Documents/KiCad/Project`.
- Do not rewrite the plan to match the old code. The plan is the design of
  record. If you believe the plan is wrong, stop and report the specific
  conflict with a datasheet reference.
- Do not invent pin numbers or relay polarities. Use the tables above.
- Do not use Orca.
