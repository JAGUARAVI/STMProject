# ESWCap combined C + R on STM32G474 — enhanced design plan (rev 2)

> ## ✅ Review status — topology confirmed, pin table corrected
>
> This document's **central topology is correct** and has been adopted by the
> firmware:
>
> ```
> V_cap --[ C_block ]--[K1B]-- N_DUT --[ R_DUT ]-- B --[K1A]-- GND
> ```
>
> `N_DUT` is the force node, `B` is the sense node, and `V_cap` cannot be the
> R-mode force node because `C_block` is an open circuit at DC. This is
> independently confirmed by the ESP32 repo's `docs/combined-circuit.md` and by
> the ESP32 firmware's SSR1/SSR2/SSR3 semantics.
>
> The earlier STM32 firmware assumed `V_cap -> DUT -> N_DUT`; that was wrong and
> has been rewritten. See `docs/handoff-verdict.md` for the evidence and the
> error list.
>
> **The pin table in §9 of this document is not safe to apply literally.** It
> contains four board-level errors found in review:
>
> | This document | Correct |
> | --- | --- |
> | USART1 PC4/PC5 | **PA9/PA10** — PC5 is not bonded on UFQFPN48 |
> | LED PC13 active-low | **PC6 active-high** — PC13 is the user button |
> | R mux EN on PB8 | **PB2** — PB8 is BOOT0 |
> | `OPAMP_NONINVERTINGINPUT_IO0` for PA7 | **IO2** — IO0 is PA1 |
>
> The corrected map is `docs/handoff-verdict.md` §5 and `include/config.h`.
>
> **BOM note:** this plan specifies K1/K2 G6S-2 relays. The ESP32
> `docs/combined-circuit.md` proposes SSR_CB/SSR_LO (AQY212). The firmware
> implements the K1/K2 state machine; confirm which parts are actually fitted
> before wiring. The 5 V relay-coil rail is not specified here.
>
> The rest of this document — the accuracy budget, the Kelvin/TIA reasoning,
> the protection scheme and the calibration plan — remains the design
> rationale and is worth reading.

**Status:** design plan; the topology is implemented, the pin table is
superseded by `docs/handoff-verdict.md` §5.
**Target:** STM32G474CEU6, UFQFPN48 (WeAct “Long” core board), 170 MHz.
**Scope:** one instrument, one DUT socket, capacitance **and** resistance,
autoranging, 0–20 V external bias, reusing the existing ESWCap analog hardware
and the spare parts already on hand.

> **Revision 2 — what changed and why**
>
> 1. **The DUT topology is corrected.** The DUT is **N_DUT → B**, with
>    `C_block` between `V_cap` and `N_DUT`. Rev 1 assumed the DUT sat between
>    `V_cap` and `N_DUT`; that is not the ESWCap board. `V_cap` cannot be the
>    R-mode force node.
> 2. **The LM393 is deleted.** COMP1 + DAC3_CH1 + TIM2 closes the C-mode
>    oscillator in hardware. No comparator ISR, no 100 kΩ hysteresis resistor,
>    no 10 kΩ pull-up. The 1.65 V divider is retained only as the TIA VBIAS
>    reference.
> 3. **The TIA moves to the LMC6482 A**, not the internal OPAMP2. The G474
>    internal op-amp input bias current is the 1 GΩ limit; the LMC6482 is
>    20 fA-class. OPAMP2 becomes the fallback TIA (100 MΩ guaranteed).
> 4. **The 900 MΩ/100 MΩ V_N divider is deleted.** It destroyed the low-ohm
>    Kelvin resolution: a 1 Ω DUT produces only 4.1 mV across the DUT, and a
>    ÷10 divider leaves 0.41 mV — below one 12-bit LSB. V_N is now sensed
>    directly through a 1 MΩ protection resistor and buffered.
> 5. **The V_N buffer is OPAMP3**, time-multiplexed with the internal
>    DAC3_CH2 → OPAMP3 → ADC self-calibration path.
> 6. **The B-node switches are two Omron G6S-2 DC5 DPDT signal relays**
>    (2 Form C, 5 V coil, on hand). One relay (`K1`, `R_MODE`) grounds `B`
>    and connects `C_block` in C mode; the other (`K2`, `KELVIN`) connects
>    `B` to the `RSENSE` bank and opens the TIA feedback in Kelvin. In TIA,
>    both relays are in the R state / de-energized, so `B` is open and the
>    `RSENSE` bank is disconnected. The G6S-2 contacts are low-leakage and
>    make 1 GΩ feasible. With only the two spare AQY212, the guaranteed range
>    would be 100 MΩ; the G6S-2 removes that limitation.
> 7. **One circuit, two modes.** The page/schematic must stop presenting the C
>    and R paths as separate front-ends. They share the MCU, DUT, bias,
>    `C_block`, clamps, references and mode switches; only the range banks and
>    the force/sense physics differ.

---

## 1. The real ESWCap topology (read this first)

```
                         +3.3 V
                           │
                     ┌─────┴─────┐
                     │  Q1 P-FET │   R-mode force (Kelvin / TIA)
                     └─────┬─────┘
                           │
   V_cap ──[ C_block ]──[K1B]── N_DUT ──[ R_DUT ]── B ──[K1A]── GND
      │                                  │             │
   C front-end                     R_bias to V_BIAS   R sense / TIA
   (buffer, mux,                   (0–20 V)           (MAX4619, Rf,
    range resistors,                                   K2A/K2B)
    ADC, COMP1)
```

| Node | C-mode | R-mode |
| --- | --- | --- |
| `V_cap` | drive/sense node, 0–3.3 V | floating, C front-end off |
| `N_DUT` | DUT top, sits at external bias 0–20 V | **force** node, 0–3.3 V |
| `B` | DUT bottom, tied to GND by `K1A` | **sense** node, lifted from GND |
| `C_block` | in series with the DUT | isolated by `K1B` |

**Why rev 1 was wrong.** Rev 1 put `P = V_cap` as the R-mode force node and
`N = N_DUT` as the sense node, with the DUT between them. On the real board the
DUT is between `N_DUT` and ground; `C_block` is between `V_cap` and `N_DUT`.
Forcing `V_cap` would only charge `C_block` through the DUT; it would not force
the DUT top. The current ESWCap firmware comments confirm the series path:
`V_cap → C_block → N_DUT → DUT → GND`, with SSR1 precharging `N_DUT` while SSR3
clamps `V_cap`.

### 1.1 Mode state table

| Element | C-mode | R Kelvin | R TIA |
| --- | --- | --- | --- |
| `K1` coil (`R_MODE`) | off | **on** | **on** |
| `K1A` (B↔GND, NC) | **closed** | open | open |
| `K1B` (N_DUT↔C_block, NC) | **closed** | open | open |
| `K2` coil (`KELVIN`) | off | **on** | off |
| `K2A` (B↔U4 common, NO) | open | **closed** | open |
| `K2B` (TIA feedback, NC) | closed (TIA off) | open | **closed** |
| `SSR1` (V_BIAS→10 Ω→N_DUT) | precharge | off | off |
| `SSR2` (N_DUT→100 Ω→GND) | discharge | off | off |
| `SSR3` (V_cap→1 Ω→GND) | precharge clamp | off | off |
| C mux U3 ENABLE | **on** | off | off |
| R mux U4 ENABLE | off | **on** | off |
| Q1 P-FET | off (gate high) | regulating | fully on |
| OPAMP1 (Kelvin loop) | off | **on** | off |
| OPAMP2 (spare, off) | off | off | off |
| OPAMP3 (V_N buffer) | off | **on** | **on** |
| V_BIAS | 0–20 V | 0 V | 0 V |

The two G6S-2 relays implement the three B-node states with their Form C
contacts:

1. **C mode:** `K1` de-energized. `K1A` grounds `B`; `K1B` connects `C_block`.
   `K2` de-energized: `K2A` open (R mux disconnected); `K2B` closed (TIA
   feedback connected, but the TIA is unused).
2. **Kelvin:** `K1` energized (B lifted, C_block isolated); `K2` energized
   (`K2A` connects the R mux, `K2B` opens the TIA feedback).
3. **TIA:** `K1` energized (B lifted, C_block isolated); `K2` de-energized
   (`K2A` open, `K2B` closed). `B` is open except for the TIA input, the V_B
   buffer and the clamps.

`K2` must not be energized unless `K1` is energized; the firmware enforces it,
and the relay wiring can also power the `K2` driver from the `K1` coil low
side as a hardware interlock (see §8).

Power loss de-energizes both relays: `B` is grounded and `C_block` is
connected, which is the C-mode safe state. The DUT is not left floating.

---

## 2. One circuit, two modes — not two front-ends

The page and the KiCAD schematic should show **one** board. “C front-end” and
“R front-end” are descriptions of the active path, not separate assemblies.

### 2.1 Shared backbone

| Shared block | Why it is shared |
| --- | --- |
| STM32G474 | both modes use the same ADC, op-amps, comparator, timers and console |
| DUT socket J1 | one set of two terminals, `N_DUT` and `B` |
| 0–20 V bias input J2, `R_bias`, SSR1, SSR2, SSR3 | C-mode bias/precharge/discharge; R-mode uses 0 V bias |
| `C_block` + `K1B` | C-mode series element; R-mode isolates it |
| `V_cap` clamp, ADC, COMP1 | C-mode measurement; R-mode unused |
| `V_N` / `V_B` buffers | both modes monitor the DUT nodes |
| VREF / VBIAS references | Kelvin setpoint and TIA virtual ground |
| BAT54S/BAV199 clamps | one protection network for all modes |
| +3.3 V, GND, UART, SWD, LED, watchdog | one controller |

### 2.2 Mode-specific blocks

| Block | C-mode | R-mode |
| --- | --- | --- |
| Range bank | C mux U3 + 98.9 Ω / 993.6 Ω / 98.6 kΩ / 1.01 MΩ, **series** from buffer to `V_cap` | R mux U4 + 98.9 Ω / 993.6 Ω / 98.6 kΩ / 4.7 MΩ, **shunt** from `B` to GND |
| Force | SN74LVC1G34 buffer | Q1 P-FET + OPAMP1 |
| Sense | `V_cap` ADC + COMP1 | Kelvin `V_N`/`V_B`, or TIA `V_N`/`V_TIA` |

**Why the R sense bank is separate.** This is the one deliberate duplication,
and it is not cosmetic:

- The C bank is a **series-drive** network: its common is the buffer output and
  its far ends all sit on `V_cap`. The R bank must be a **shunt-sense** network:
  its common is `B` and its far ends are GND. They are not the same two-terminal
  network.
- Reusing the C resistors would require grounding `V_cap` through a switch that
  is in series with the Kelvin sense resistor. The 98.9 Ω element would then see
  the C-mux on-resistance **plus** the ground-switch on-resistance in series:
  two silicon Ron terms, roughly 20 Ω, or 20 % of 98.9 Ω, with the tempco of both.
  The dedicated R bank has one mux Ron, and it is calibrated per element.
- Grounding `V_cap` also exposes the 1000 µF (Rev B bipolar) `C_block` to an
  inrush path through a small analog switch. The C-mode sequence can discharge
  it first, but a fault can destroy the switch. The dedicated R bank never
  grounds `V_cap`.
- The C range resistors are 1 % (measured, but not low-tempco); the R bank uses
  0.1 %, 25 ppm/°C parts. At 1 Ω the shunt is 98.9 Ω; a 1 % resistor plus mux
  tempco cannot meet a 0.5 % target.

The cost is one spare MAX4619 and four 0.1 % resistors. That is the minimum-risk
use of the available spare, and it leaves the C-mode calibration untouched.

**If absolute minimum parts matter more than low-range accuracy**, the shared
variant is possible: use the existing C mux as the 4:1 selector, add a
`B↔V_cap` mode switch and a `V_cap→GND` switch, and accept two mux Ron terms
plus the `C_block` inrush risk. This plan does **not** recommend it for a
1 Ω–1 GΩ instrument. It is recorded here so the trade-off is explicit.

---

## 3. G474 peripheral allocation — the STM advantage

The 48-pin package is not the limitation it looks like. Verified against the
G474CE pin database, RM0440 and the local CubeG4 package:

- **6 op-amps on die; OPAMP1/2/3 bonded on UFQFPN48.**
- **7 comparators, all bonded** (COMP3 is effectively blocked by the HSE pin).
- **5 ADCs**, 4 Msps, hardware oversampling 2–256×.
- **4 DACs**; DAC3/DAC4 are internal-only and cost no pins.
- **TIM2 32-bit**, accepts any comparator on TI1.
- Dedicated `VREF+` pin, VREFBUF, flash record store, TAMP, IWDG.

### 3.1 Allocation table

| Peripheral | Use | Why this instance |
| --- | --- | --- |
| **OPAMP1** | Kelvin error amplifier: `VINP=PA7(B)`, `VINM=PA3(VREF)`, `VOUT=PA2(Q1 gate)` | only inverting-input topology that gives negative feedback with a P-FET pass device; PA7 is the only free OPAMP1 VINP pin that can carry B |
| **OPAMP2** | spare, powered down | **cannot be the TIA** while OPAMP1 uses B: OPAMP2 VINP is fixed at PA7 (B) and VINM at PA5 (VBIAS), which is the wrong polarity for a TIA. The external LMC6482 A provides the correct TIA. |
| **OPAMP3** | `V_N` buffer: `VINP=PB0`, `VOUT=PB1` (ADC3_IN1), VINM internal | follower; PB2 is the R-mux ENABLE, not used by OPAMP3 |
| **LMC6482 A** | TIA: `IN+=VBIAS`, `IN-=B`, `OUT=TIA_OUT` | 20 fA input bias; the internal op-amp Ib is the 1 GΩ limit |
| **LMC6482 B** | `V_B` buffer: `IN+=B`, `IN-=OUT`, `OUT=ADC_VB` | 20 fA; B can be 4.7 MΩ source in the top Kelvin range |
| **K1, K2** | Omron G6S-2 DC5 DPDT: `K1A` B↔GND, `K1B` N_DUT↔C_block, `K2A` B↔R mux, `K2B` TIA feedback | two Form C contacts each; low leakage; 5 V coils; fail-safe C state |
| **COMP1** | C-mode relaxation oscillator: `INP=PA1(V_cap)`, `INM=DAC3_CH1`, `OUT=PA8` | hardware loop; replaces LM393 and the ISR; PA1 is both ADC1_IN2 and COMP1_INP |
| **COMP4** | `V_N` overvoltage flag: `INP=PB0(V_N_SENSE)`, `INM=DAC1_CH1` | one comparator, no pin; EXTI shutdown |
| **COMP5** | TIA saturation flag: `INP=PB13(TIA_OUT)`, `INM=DAC4_CH1` | optional; detects TIA output below ~0.1 V |
| **DAC3_CH1** | COMP1 threshold, 1.65 V | internal, no pin |
| **DAC3_CH2** | OPAMP3 self-cal source, 0–3.3 V | internal; time-multiplexed with `V_N` |
| **DAC1_CH1** | COMP4 threshold, 2.5 V | internal to COMP4; PA4 can stay free |
| **DAC4_CH1** | COMP5 threshold, 0.1 V | internal to COMP5 |
| **ADC1** | `V_cap` (PA1/IN2); `VREF` (PA3/IN4) during setup | C-mode RC step |
| **ADC2** | `TIA_OUT` (PA6/IN3); `V_BIAS` (PA5/IN13) | R-mode TIA |
| **ADC3** | `V_N` (PB1/IN1, OPAMP3 output); VREFINT | R-mode Kelvin/TIA |
| **ADC4** | `V_B` (PB12/IN3) | R-mode Kelvin |
| **ADC5** | spare (PA9/IN2) | future |
| **TIM2** | 32-bit COMP1 input capture | 5.88 ns resolution, 25.3 s wrap |
| **TIM6** | ADC trigger for the RC step | crystal-locked sample instants |
| **USART1** | PA9/PA10 console | PC5 is not bonded; PA2/PA3 are OPAMP1 |
| **USB** | PA11/PA12 DFU/data | reserved |
| **IWDG** | capture watchdog | independent of PLL |
| **Flash record store** | `rcal`, `cal`, board data | 2 KB page, CRC records |
| **VREF+** | VDDA = 3.3 V (default) | optional external 3.0 V reference; see §9 |

### 3.2 What the STM gains

1. **The oscillator is closed in silicon.** COMP1 output drives the SN74LVC1G34
   buffer directly; the same output is captured by TIM2. There is no software
   edge mirroring and no ISR latency in the period. The comparator's output
   polarity is inverted so the buffer produces the correct drive phase.
2. **The comparator threshold is programmable.** DAC3_CH1 sets 1.65 V; COMP1
   hysteresis is set in registers instead of a 100 kΩ feedback resistor. The
   hysteresis can be tuned per C range.
3. **The 5 ADCs remove the ESP32's channel-switching pain.** `V_cap`, `V_B`,
   `V_N` and `V_TIA` can be assigned to separate ADCs; the critical ratiometric
   pairs are sampled without interleaving channels in one sequence.
4. **The 12-bit ADC + 64–256× oversampling** gives 14–16 effective bits; the
   4.1 mV low-ohm Kelvin signal is no longer buried in LSBs.
5. **DAC3 + OPAMP3 + ADC3 form a zero-pin self-cal path.** `DAC3_CH2 →
   OPAMP3(follower) → ADC3_IN13` measures the ADC/op-amp offset and gain at
   power-up and between measurements.
6. **DAC/COMP protection is free.** COMP4 watches `V_N` and can trip an EXTI
   shutdown; COMP5 flags a saturated TIA. No external comparator or divider.
7. **TIM2 captures the C period in hardware**, so `T0` no longer absorbs ISR
   jitter. The remaining `T0` is the comparator/buffer propagation delay and the
   RC geometry, which is stable and calibratable.

---

## 4. C-mode design (existing physics, new silicon)

### 4.1 Hardware oscillator

```
 PA8 (COMP1_OUT, inverted) ──► SN74LVC1G34 ──► C mux U3 ──► R_range ──► V_cap
        ▲                                                                  │
        │                                                                  │
     COMP1 INP = PB1 ◄──────────────────────────────────────────────────────┘
     COMP1 INM = DAC3_CH1 = 1.65 V
     COMP1 OUT ──► TIM2_CH1 (internal capture)
```

- COMP1 output polarity is set so `PA8` is **high when `V_cap` is below the
  lower threshold**. That is the drive phase; the buffer then charges the RC
  network until the upper threshold, when the comparator output goes low.
- The comparator has programmable hysteresis. Start at 20 mV; the effective
  thresholds are 1.65 V ± 10 mV. The exact values are calibrated.
- `PA8` is a dual-purpose pin:
  - **Oscillator:** COMP1 alternate function, push-pull output.
  - **RC step:** GPIO output high; the comparator is disabled.
  A 10 kΩ pull-down on the buffer input defines drive low when COMP1 is off.
- `TIM2_CH1` captures the COMP1 edges internally. The period is the difference
  of successive captures; the 32-bit counter covers 10 Hz–1 MHz without
  overflow handling.
- The C mux U3 selects the range resistor. The existing range values and the
  measured `R_eff`/`C0` calibration carry over.
- The external LM393, its 100 kΩ hysteresis and 10 kΩ pull-up are **removed**.
  The 10 kΩ/10 kΩ divider is retained only as the TIA `VBIAS` reference.

### 4.2 RC-step mode

- Drive `PA8` high as a GPIO; the buffer steps the selected range resistor.
- `TIM6` triggers `ADC1` (PA0, `V_cap`) into a circular DMA buffer.
- Per-range oversampling: small ratios for fast ranges, 64–256× for slow ones.
  The ESP32's software-poll jitter and adaptive decimation disappear.
- Fit `V(t) = V_inf − (V_inf − V0)·e^(−(t−t0)/τ)`; `C_eq = τ / R_eff`.
- Apply the existing `C_block` series inversion first, then subtract `C_stray`.

### 4.3 C-mode range table

| Range | Resistor | C mux address | Typical use |
| --- | --- | --- | --- |
| 0 | 98.9 Ω | 2 | large electrolytics |
| 1 | 993.6 Ω | 1 | µF range |
| 2 | 98.6 kΩ | 0 | nF range |
| 3 | 1.01 MΩ | 3 | pF–nF / oscillator |

The range resistor is the **measured** value. The comparator's drive resistance
includes the buffer Zo and the mux Ron; the firmware's `R_eff` absorbs them.

### 4.4 C-mode calibration

- `T = K·R·C + T0`, per range. `K` is the geometry/threshold constant; `T0` is
  the measured open-node tare including the comparator and buffer delays.
- The comparator threshold DAC code and hysteresis are stored with `K`/`T0`.
- Recalibrate after changing the COMP1 threshold or hysteresis.
- The RC-step path keeps `R_eff` and `C0` per range.

---

## 5. R-mode design

### 5.1 Kelvin (1 Ω – 22 MΩ)

```
 +3.3 V ──► Q1 ──► N_DUT ──[ R_DUT ]── B ──[ R mux U4 ]── RSENSE ──► GND
                    ▲                    │
                    │                    ├── OPAMP1 VINP (PA7)
              OPAMP1 VOUT (PA2) ──► Q1 gate
              OPAMP1 VINM (PA3) = VREF = 0.405 V
```

- OPAMP1 regulates `V_B = VREF`. The current through the selected `RSENSE` is
  `I = VREF / RSENSE_eff`.
- `V_N` is the DUT top; `R_DUT = RSENSE_eff · (V_N − V_B) / V_B`.
- `RSENSE_eff` includes the mux Ron; calibrate each element.
- `V_N` must stay ≤ 2.3 V to keep Q1 in regulation and the ADC safe:
  `R_DUT,max ≈ 4.68 · RSENSE`.
- `V_N − V_B = V_B · R_DUT / RSENSE`; at the low end of each range it is
  ~4 mV, which is why V_N is sensed directly (no ÷10 divider).

| RSENSE | I at VREF=0.405 V | R_DUT span | Notes |
| --- | --- | --- | --- |
| 98.9 Ω | 4.10 mA | 1 Ω – 470 Ω | 0.1 %, 25 ppm |
| 993.6 Ω | 408 µA | 10 Ω – 4.7 kΩ | 0.1 % |
| 98.6 kΩ | 4.11 µA | 1 kΩ – 470 kΩ | 0.1 % |
| 4.7 MΩ | 86.2 nA | 47 kΩ – 22 MΩ | 0.1 %; mux leakage matters |

- The R mux is the spare MAX4619, three SPDT binary-gated as a 4:1 bank. Common
  = `B` through `K2A` (K2 pole A); throws = the four `RSENSE` tops; bottoms =
  GND.
- The mux ENABLE is active-low. In TIA/C, the mux is disabled **and** `K2A` is
  open; disabling alone leaves the off-leakage from `B` to the unselected
  resistors.
- Q1 is an AO3401 P-MOSFET (or BD140 PNP alternative). Gate: R13 100 kΩ to
  +3.3 V (off), R14 1 kΩ from OPAMP1 VOUT, R15 10 kΩ from `GPIO_R_FORCE`.
  C3 100 pF from VOUT to VINP compensates the loop.
- In TIA, `GPIO_R_FORCE` is low: the gate divider 100 kΩ/10 kΩ holds the gate
  at ~0.3 V, Q1 fully on. In Kelvin, `GPIO_R_FORCE` is high-Z; OPAMP1 drives
  the gate. In C, OPAMP1 is off and R13 pulls the gate high.

### 5.2 TIA (10 MΩ – 1 GΩ)

```
                    Rf = 10 MΩ ∥ Cf = 10 pF
        B ──────────/\/\/\/\───────────────┐
        │                 │                │
        │           K2B (NC)               │
        │                                  │
        └── LMC6482 A IN−          OUT ────┴──► TIA_OUT
            LMC6482 A IN+ = VBIAS = 1.65 V
```

- The LMC6482 A holds `B = VBIAS` and converts the DUT current:
  `V_TIA = VBIAS − (V_N − VBIAS) · Rf / R_DUT`.
- `R_DUT = Rf · (V_N − VBIAS) / (VBIAS − V_TIA)`.
- At 10 MΩ, `V_TIA = 0 V`; at 1 GΩ, `V_TIA = 1.6335 V`. The usable TIA range
  starts around 15 MΩ where the output is ~0.55 V.
- `K2B` (K2 pole B, NC) is in series with Rf/Cf. `K2` is energized in Kelvin,
  so `K2B` opens and the saturated TIA cannot drive current through Rf into
  `B`. In TIA, `K2` is de-energized and `K2B` closes. In C, `K2B` is also
  closed, but the TIA is unused; the LMC6482 A output saturates and Rf draws
  ~330 nA, which is harmless. The open contact sees a floating node, so it does
  not load `B`; its capacitance adds a small pole.
- The LMC6482 A is powered from +3.3 V; its 20 fA input bias is the reason it
  is used instead of OPAMP2. OPAMP2 cannot be the TIA in this pin map: its
  `VINP` is PA7 = B and its `VINM` is PA5 = VBIAS, the opposite of the TIA
  polarity. OPAMP2 stays powered down; its pins are reused as B, VBIAS and
  TIA_OUT nodes, but its op-amp is not in the signal path.
- Rf = 10 MΩ, 0.1 %, 25 ppm; Cf = 10 pF C0G. The TIA pole is 1.6 kHz; the
  LMC6482's 3 MHz GBW gives ample loop gain. Cf may be tuned on the bench for
  the actual B-node capacitance.

### 5.3 V_N and V_B sensing

**V_N.** `N_DUT → 1 MΩ (R25) → V_N_SENSE → BAT54S clamp → OPAMP3 VINP (PB0)`.
- In R mode, `V_N ≤ 2.3 V`; the clamp does not conduct. The 1 MΩ only limits
  fault current; the OPAMP3 input bias is pA, so there is no divider error.
- In C mode, `N_DUT` can be 20 V. The 1 MΩ limits the clamp current to ~20 µA;
  the BAT54S clamps at ~3.5 V. OPAMP3 is powered down in C mode. The V_N reading
  is saturated and ignored by the C firmware.
- `C7 = 1 nF` at V_N_SENSE; with 1 MΩ the pole is 159 Hz, which filters
  line-frequency noise without slowing the R-mode settling.
- This replaces the 900 MΩ/100 MΩ divider. The divider was the reason a 1 Ω
  DUT produced only 0.41 mV at the ADC; direct sensing preserves the 4.1 mV.

**V_B.** `B → LMC6482 B IN+ (pin 5)`, `IN− (pin 6) = OUT B (pin 7)`,
`OUT B → PB12 / ADC4_IN3`. The LMC6482 B is a unity follower; its 20 fA input
bias does not load the 4.7 MΩ `RSENSE` node. Add a 1 kΩ series resistor and a
100 pF cap at the input for filtering; the bias current error is negligible.

**V_TIA.** `TIA_OUT → PA6 / ADC2_IN3`, with a BAV199 clamp to GND/+3.3 V. The
LMC6482 output is low-impedance; the clamp leakage does not affect the TIA
summing node.

**V_BIAS.** 10 kΩ/10 kΩ divider from +3.3 V, 100 nF. It is the TIA `VBIAS` and
is measured on PA5 / ADC2_IN13. The LM393 comparator is gone, but this divider
is retained and is the only 1.65 V reference.

**VREF.** 7.15 kΩ/1 kΩ divider = 0.405 V, 100 nF. It is OPAMP1's `VINM`
(PA3) and is measured on ADC1_IN4 during setup. VREF cancels in the Kelvin
ratio, but its stability matters.

### 5.4 R-mode sequencing

```
IDLE
  → SAFE:      V_BIAS=0, Q1 off, OPAMP1 off; SSR2 on and wait until
               N_DUT ≈ 0 V (discharge first); then K1 off (K1A B=GND,
               K1B C_block connected), K2 off (K2A open, K2B closed),
               C mux off, R mux off, OPAMP2/3 off
  → R_ISOLATE: SSR2 off; K1 on (K1A open, K1B open); K2 off; R mux off;
               OPAMP3 on; wait 10 ms for the relay contacts
  → R_KELVIN:  K2 on (K2A closed, K2B open); R mux on; OPAMP1 on;
               Q1 loop; wait for B=0.405 V; sample V_N/V_B
  → R_TIA:     OPAMP1 off; K2 off (K2A open, K2B closed); R mux off;
               Q1 force on; sample V_N/V_TIA
  → R_DISCHARGE: Q1 off, OPAMP1 off, R mux off, K2 off, K1 off
               (B=GND, C_block reconnected), then IDLE
```

Interlocks:

1. Never close `K1B` while `N_DUT` is biased. Discharge and set `V_BIAS=0`
   first.
2. Never open `K1A` (energize K1) while `B` is driven. Turn Q1 off first.
3. Never energize `K2` unless `K1` is energized. Kelvin requires B lifted and
   C_block isolated. The firmware enforces this; the optional hardware
   interlock in §8 enforces it too.
4. Never enable the R mux in TIA. `K2A` is open, so the off-leakage is
   disconnected from `B`.
5. Never enable the C mux in R. Its off-leakage and the C range network are
   unnecessary loads on `V_cap`.

---

## 6. Pin map (UFQFPN48, G474CEU6)

> This table is the corrected map (matches `include/config.h` and
> `docs/handoff-verdict.md` §5). The earlier rev-2 table had PA1/PA7, PA5/PA7,
> PB1/PB2 and PC4/PC5 wrong; those rows are replaced.

| Pin | Function | Notes |
| --- | --- | --- |
| PA0 | spare | |
| PA1 | `V_cap` — `ADC1_IN2`, `COMP1_INP` | C-mode RC node |
| PA2 | `OPAMP1_VOUT` — Q1 gate | through R14 1 kΩ |
| PA3 | `OPAMP1_VINM` — `VREF`; `ADC1_IN4` | 0.405 V |
| PA4 | spare | DAC1_OUT1 used internally only |
| PA5 | `VBIAS` — `ADC2_IN13` | 1.65 V TIA reference |
| PA6 | `TIA_OUT` — `ADC2_IN3` | LMC6482 A output; BAV199 clamp |
| PA7 | `B` — `OPAMP1_VINP2`, `ADC2_IN4` | Kelvin sense |
| PA8 | `COMP1_OUT` / drive buffer; GPIO for RC step | 10 kΩ pull-down on buffer input |
| PA9/PA10 | `USART1_TX/RX` | console |
| PA11/PA12 | USB D−/D+ | reserved |
| PA13/PA14 | SWD | reserved |
| PA15 | spare | |
| PB0 | `V_N_SENSE` — `OPAMP3_VINP0`, `COMP4_INP` | 1 MΩ + clamp |
| PB1 | `OPAMP3_VOUT` — `ADC3_IN1` | V_N buffer output |
| PB2 | R mux ENABLE | active-low; PB8 is BOOT0 |
| PB3 | C mux A0 | JTAG pin; SWD unaffected |
| PB4 | C mux A1 | JTAG pin; SWD unaffected |
| PB5 | C mux ENABLE | active-low |
| PB6 | R mux A0 | |
| PB7 | R mux A1 | |
| PB9 | `R_MODE` — K1 coil driver | 5 V relay; energised in R |
| PB10 | `KELVIN` — K2 coil driver | 5 V relay; energised in Kelvin only |
| PB11 | `SSR13` control | SSR1 + SSR3 shared interlock |
| PB12 | `ADC4_IN3` — `V_B` | LMC6482 B output |
| PB13 | `SSR2` control | N_DUT discharge |
| PB14 | `GPIO_R_FORCE` | high-Z Kelvin/C, low TIA |
| PB15 | `VBIAS_RETURN` | held low in the safe state |
| PC4 | spare | PC5 is not bonded |
| PC6 | user LED | active-high |
| PC13 | user button | active-low |
| PC14/PC15 | OSC32 | optional |
| PF0/PF1 | HSE 8 MHz | reserved |

**Pin-count check:** C mux 3, R mux 3, SSRs 4 (`SSR13`, `SSR2`, `R_MODE`,
`KELVIN`), `GPIO_R_FORCE` 1, UART 2, LED 1, SWD 2, HSE 2, USB 2 = 20; plus the
analog pins above. The 48-pin package has enough; the exact CubeMX allocation
can move the digital GPIOs to any free pin.

**Do not use PA2/PA3 for UART.** They are OPAMP1. USART1 is on PA9/PA10;
PC5 is not bonded on UFQFPN48 and PB8 is BOOT0.

---

## 7. Coverage, equations and accuracy

### 7.1 Coverage

| Method | Range | Notes |
| --- | --- | --- |
| Kelvin 98.9 Ω | 1 Ω – 470 Ω | 4.1 mA; low-ohm resolution limited by V_N−V_B |
| Kelvin 993.6 Ω | 10 Ω – 4.7 kΩ | 408 µA |
| Kelvin 98.6 kΩ | 1 kΩ – 470 kΩ | 4.1 µA |
| Kelvin 4.7 MΩ | 47 kΩ – 22 MΩ | 86 nA; mux leakage |
| TIA 10 MΩ | 15 MΩ – 1 GΩ | 1.65 nA at 1 GΩ; LMC6482 |

Overlap: 15–22 MΩ. The Kelvin/TIA handoff uses 2× hysteresis and a
cross-check; a disagreement beyond tolerance is flagged, not hidden.

### 7.2 Equations

```
Kelvin:
  V_B = VREF
  I   = VREF / RSENSE_eff
  R   = RSENSE_eff · (V_N − V_B) / V_B
  V_N,max = VREF · (1 + R_max/RSENSE) ≤ 2.3 V

TIA:
  V_TIA = VBIAS − (V_N − VBIAS) · Rf / R
  R     = Rf · (V_N − VBIAS) / (VBIAS − V_TIA)

C:
  T = K·R·C + T0
  V(t) = V_inf · (1 − e^(−t/τ)),  C_eq = τ / R_eff
  C_eq = C_block·C_dut/(C_block + C_dut) + C_stray
```

### 7.3 Accuracy budget

| Term | Kelvin | TIA | Mitigation |
| --- | --- | --- | --- |
| RSENSE / Rf | 0.1 % | 0.1 % | 0.1 %, 25 ppm, NVS calibration |
| Mux Ron | cal’d in | n/a | per-element `RSENSE_eff`; tempco residual |
| V_B / V_N buffer offset | 0.75 mV | 0.75 mV | LMC6482; calibrate differential offset |
| V_N buffer Ib | 0.1 mV (OPAMP3, 1 MΩ) | same | 100 pA × 1 MΩ; calibrate |
| ADC LSB | 0.8 mV | 0.8 mV | 12-bit + 16× oversampling → 0.05 mV |
| ADC channel mismatch | <0.05 % | <0.05 % | separate ADCs; per-channel cal |
| Relay/mux leakage | high range | 1 GΩ | K1/K2 G6S-2 disconnect `B`; MAX4619 off in TIA |
| PCB leakage | n/a | 1–2 % at 1 GΩ | guard ring, clean board |

**The 1 Ω case.** `V_N − V_B = 0.405 × 1/98.9 = 4.1 mV`. With a 12-bit ADC
and 16× oversampling, the effective LSB is ~0.05 mV; 4.1 mV is ~80 counts. The
LMC6482 offset (0.75 mV) is 18 % of the signal if uncalibrated, so the V_B/V_N
differential offset **must** be calibrated at 0 Ω and a known low resistor. A
direct ÷10 divider would have left only 8 counts; that is why it is gone.

**The 1 GΩ case.** `I = 1.65 nA`; `V_TIA` is 16.5 mV below VBIAS. The ADC
resolution is adequate, but every leakage path matters:
- In TIA, `K1A` is open, `K2A` is open and `K2B` is closed. The MAX4619 bank is
  disconnected from `B` by `K2A`; the AQY212s are no longer in the B path.
- The G6S-2 Form C contacts are low-leakage signal contacts; they are the
  reason 1 GΩ is feasible.
- The BAV199 clamps and PCB surface leakage set the practical floor.

**Therefore:** with the G6S-2 relays, **1 GΩ is achievable** with a guarded
B/TIA layout and BAV199 clamps. With only the two spare AQY212 (no G6S-2), the
guaranteed range is **100 MΩ** and 1 GΩ is best-effort. The relay is not an
optional luxury for the stated 1 GΩ requirement; it is the enabling part.

---

## 8. B-node switching with the Omron G6S-2

The G6S-2 DC5 is a **2 Form C (DPDT) signal relay**, 5 V coil, 2 A / 30 VDC
contacts, low contact leakage. Two relays implement all four B-node functions
with no semiconductor switch in the high-impedance path.

### 8.1 Contact assignment

**K1 — `R_MODE` (energized in R mode: Kelvin and TIA)**

| Contact | Common | NC (de-energized) | NO (energized) |
| --- | --- | --- | --- |
| `K1A` | `B` | **GND** | open |
| `K1B` | `N_DUT` | **`C_block`** | open |

- C mode: K1 off → `B` grounded, `C_block` connected.
- R mode: K1 on → `B` lifted, `C_block` isolated.
- Power loss: K1 off → C-safe state. The DUT is grounded, not floating.

**K2 — `KELVIN` (energized in Kelvin only)**

| Contact | Common | NC (de-energized) | NO (energized) |
| --- | --- | --- | --- |
| `K2A` | `B` | open | **R mux common** |
| `K2B` | `TIA_OUT` | **Rf/Cf top** | open |

- Kelvin: K2 on → `K2A` connects the `RSENSE` bank; `K2B` opens the TIA
  feedback.
- TIA: K2 off → `K2A` disconnects the `RSENSE` bank; `K2B` closes the TIA
  feedback.
- C: K2 off → `K2A` open; `K2B` closed, but the TIA is unused. The LMC6482 A
  output saturates and Rf draws ~330 nA; harmless.

### 8.2 Coil drivers and interlock

- 5 V coil rail from the WeAct board's 5 V pin; 100 nF at each coil.
- Driver per relay: BC847B (or 2N7002) + 1 kΩ base/gate resistor + 1N4148
  flyback across the coil. Coil current is ~28 mA; the driver sinks it.
- **Hardware interlock (recommended):** return the K2 driver's emitter/source
  to the K1 coil low side, not to GND. K2 can only energize when K1 is
  energized; the K1 driver carries both coil currents (~56 mA), which the
  BC847B/2N7002 handles. The firmware also enforces `K2 ⇒ K1`.
- Relay operate/release is ~5 ms and the contacts bounce. Wait **10 ms** after
  any coil change before enabling OPAMP1, the TIA, or sampling.
- Keep the coil traces and flyback nodes away from `B`, the TIA summing node
  and the `RSENSE` bank. The relay is a mechanical contact; treat the coil as a
  noise source.

### 8.3 Contact ratings and C-mode inrush

- C-mode DUT current through `K1A` is ≤33 mA (98.9 Ω range at 3.3 V); the 2 A
  contact is comfortable.
- `K1B` carries the `C_block` precharge inrush. Rev B uses **R9 = 47 Ω, 2 W**
  (2512): at 20 V bias the peak is ~0.4 A and 5τ ≈ 250 ms with the 1000 µF
  `C_block`, inside the firmware's 2 s precharge hold.  The earlier 10 Ω/22 Ω
  analysis was for the polarised 936 µF Rev A part; 10 Ω exceeded the AQY212
  rating, and 22 Ω was still marginal.
- The relay contacts are break-before-make; the firmware never switches K1/K2
  during a measurement. K1 changes only in SAFE/R_ISOLATE; K2 changes only
  between R_ISOLATE/KELVIN/TIA.
- The 2 spare AQY212 are no longer used for the B node. Keep them as spares,
  or use one for a separate `V_cap` clamp if desired. They are not part of the
  1 GΩ path.

### 8.4 If the G6S-2 is not fitted

Use the two spare AQY212 for `K1A` and `K1B`, omit `K2A` (connect the R mux
common directly to `B`), and keep `K2B` as a 74LVC1G3157 or equivalent. The
TIA feedback **must** be disconnected in Kelvin; omitting `K2B` would let the
saturated TIA drive current through Rf into `B`. The guaranteed range is then
**100 MΩ**; 1 GΩ is best-effort. This is the fallback, not the recommended
build.

---

## 9. Firmware plan

### 9.1 Modes

```
mode c | r | both | auto
```

- `c`: existing C pipeline; no R switches.
- `r`: R-mode only; C front-end off.
- `both`: measure R, discharge, then C; report both.
- `auto`: DC probe decides; finite R → R, open → C.

### 9.2 R autorange

```
PROBE:   Kelvin, 4.7 MΩ element, one fast sample
         if V_B ≈ 0 and V_N ≈ 3.3 V  → open
         if V_N ≈ V_B                → short
         else finite
KELVIN:  if R < 900 kΩ, select the element with V_N in the best window
TIA:     if R ≥ 900 kΩ, verify; use TIA above 15 MΩ with 2× hysteresis
VERIFY:  re-measure the neighbouring element; flag > tolerance
```

- Kelvin element choice: largest `RSENSE` with `V_N ≤ 2.3 V` and
  `R ≥ 0.01·RSENSE`; prefer `V_N` near mid-scale for SNR.
- TIA: `V_TIA` must be between 0.1 V and 1.6 V; below 0.1 V the TIA is
  saturated and Kelvin should take over.
- Overlap 15–22 MΩ: both methods; if they disagree, flag and report the
  weighted result.

### 9.3 Sampling

- Kelvin: K1 on, K2 on (`K2A` closed, `K2B` open), R mux on, OPAMP1 on; wait
  for B settling (watch `V_B`); sample `V_N` (ADC3) and `V_B` (ADC4);
  average/oversample.
- TIA: K1 on, K2 off (`K2A` open, `K2B` closed), Q1 force on; sample `V_N`
  (ADC3) and `V_TIA` (ADC2); average.
- C: COMP1 + TIM2 capture (oscillator) or TIM6 + ADC1 + DMA (RC step).
- Use the G474 hardware oversampling; keep the ADC channel fixed during a
  sequence (errata §2.7.2/2.7.3). Use separate ADCs for simultaneous nodes.

### 9.4 Calibration records

| Record | Contents |
| --- | --- |
| `rcal` | per Kelvin element: `RSENSE_eff`, gain, offset, date |
| `rcal_tia` | `Rf`, `VBIAS`, `V_TIA` offset/gain |
| `vbuf` | `V_B`/`V_N` buffer gain/offset |
| `adc` | per-channel gain/offset |
| `osc_cal` | per C range: `K`, `T0`, threshold/hysteresis |
| `adc_cal` | per C range: `R_eff`, `C0` |
| `board` | `C_block`, `R_bias`, range resistor values |

Store in a reserved flash page as CRC records; keep the newest valid record.
Use TAMP registers for a `cal_valid` flag if VBAT is present.

### 9.5 Self-calibration

`DAC3_CH2 → OPAMP3(follower) → ADC3_IN13`:
1. Set VPSEL to DAC3_CH2; power OPAMP3.
2. Sweep the DAC: 0.25, 0.5, 1.0, 1.65, 2.0, 2.5, 3.0 V.
3. Read ADC3; solve gain/offset for the V_N buffer/ADC path.
4. Restore VPSEL to PB0; power OPAMP3 for V_N.

This costs no pins and catches ADC/op-amp drift without an external source.
The LMC6482 paths are calibrated with external resistors.

### 9.6 Host protocol

Keep the existing `@@EVT` events byte-for-byte. Add:

```
@@EVT {"ev":"res","rx":...,"method":"kelvin|tia","element":...,"vn":...,
       "vb":...,"vtia":...,"i":...,"q":...,"flags":[...]}
```

C-mode telemetry and commands are unchanged. Add `mode c|r|both|auto`,
`rcal`, `rcal?`, `rcalclear`, `rset`.

### 9.7 Safety

- IWDG on every capture; timeout → all off.
- COMP4 `V_N` overvoltage → EXTI → Q1 off, R mux off, flag.
- COMP5 TIA saturation → flag/reject.
- Sequence the SSRs/relays with settle delays; verify `V_B`/`V_N` before
  enabling the next stage.
- Never close `K1B` with `N_DUT` biased.
- Never energize `K1` (open `K1A`) with `B` driven.
- Keep the C-mode “never close SSR2 during a measurement” rule C-mode-only.

---

## 10. Bill of materials (delta from the ESP32 build)

### 10.1 Reused

- DUT socket, `C_block` (1000 µF/35 V bipolar ∥ 67.8 nF), `R_bias` 4.8 MΩ.
- SSR1 (precharge), SSR2 (discharge), SSR3 (`V_cap` clamp), 330 Ω LED resistors.
- C mux MAX4619 #1 + 98.9 Ω / 993.6 Ω / 98.6 kΩ / 1.01 MΩ.
- SN74LVC1G34 buffer, BAT54S on `V_cap`.
- 3.3 V rail, UART, enclosure, display/console.

### 10.2 Removed

- LM393, its 100 kΩ hysteresis resistor and 10 kΩ pull-up. The 10 kΩ/10 kΩ
  1.65 V divider is **retained** as the TIA `VBIAS` reference.
- 900 MΩ/100 MΩ `V_N` divider (rev 1).
- The ESP32-S3 and its ADC/PCNT path.

### 10.3 Added

| Qty | Part | Value / PN | Purpose |
| --- | --- | --- | --- |
| 1 | MCU | STM32G474CEU6 | controller, op-amps, comparator, ADC, DAC |
| 1 | P-MOSFET | AO3401 | R-mode force pass |
| 1 | Dual op-amp | LMC6482 | TIA A + `V_B` buffer B |
| 1 | R mux | MAX4619 (spare) | 4:1 `RSENSE` bank |
| 2 | Signal relay | Omron G6S-2 DC5, DPDT 2 Form C | K1: `B`/`C_block`; K2: R mux/TIA feedback |
| 4 | Resistor 0.1 % | 98.9 Ω, 993.6 Ω, 98.6 kΩ, 4.7 MΩ | `RSENSE` bank |
| 1 | Resistor 0.1 % | 10 MΩ | TIA Rf |
| 1 | Capacitor | 10 pF C0G | TIA Cf |
| 1 | Resistor | 100 kΩ | Q1 gate pull-up |
| 1 | Resistor | 1 kΩ | Q1 gate series |
| 1 | Resistor | 10 kΩ | Q1 force divider |
| 1 | Capacitor | 100 pF | OPAMP1 compensation |
| 1 | Resistor | 7.15 kΩ | VREF top |
| 1 | Resistor | 1 kΩ | VREF bottom |
| 2 | Resistor | 10 kΩ | VBIAS divider |
| 1 | Resistor | 1 MΩ | V_N series protection |
| 1 | Capacitor | 1 nF | V_N filter |
| 1 | Resistor | 1 kΩ | V_B buffer input |
| 1 | Capacitor | 100 pF | V_B filter |
| 1 | Schottky | BAT54S | V_N clamp |
| 2 | Low-leakage diode | BAV199 | B and TIA_OUT clamps |
| 2 | Relay driver | BC847B / 2N7002 + 1 kΩ + 1N4148 | K1/K2 coils, 5 V |
| 1 | Capacitor | 100 nF | 5 V coil decoupling |
| — | Decoupling | 100 nF per IC, 10 µF bulk | |

**G6S-2 is the chosen build.** The two spare AQY212 are no longer in the B
path; keep them as spares. If the G6S-2 is not fitted, the AQY212 fallback
(§8.4) guarantees 100 MΩ; 1 GΩ is then best-effort.

---

## 11. Bring-up and acceptance

| Stage | Test | Pass |
| --- | --- | --- |
| 1 | 3.3 V, HSE, UART, SWD, LED | boots, console banner |
| 2 | COMP1 + DAC3 + TIM2 oscillator, 1 MΩ / 100 pF | stable period; TIM2 capture; no ISR |
| 3 | C-mode RC step, 1 µF / 100 kΩ | fit residual < 1 %; C within 1 % |
| 4 | Kelvin loop, 100 Ω / 1 kΩ / 100 kΩ | `V_B` within 1 mV of VREF; stable |
| 5 | Kelvin 1 Ω, 10 Ω, 1 kΩ, 100 kΩ, 1 MΩ | ≤0.5 % low, ≤1 % high |
| 6 | Kelvin 4.7 MΩ, 10 MΩ, 22 MΩ | ≤2 %; mux leakage measured |
| 7 | TIA 10 MΩ, 100 MΩ, 1 GΩ | ≤2 % at 100 M; 1 G with G6S-2 |
| 8 | Crossover 15–22 MΩ | Kelvin/TIA agree within combined tolerance |
| 9 | Open/short, wrong mode, SSR sequencing | clamped, no damage |
| 10 | C-mode regression, pF–1000 µF | unchanged |
| 11 | Host `res` event, CSV | renders correctly |

---

## 12. Open decisions

1. **G6S-2 relay wiring:** confirm the contact assignment and measure the
   open-contact leakage at 1.65 V on the bench. Verify the K2⇒K1 coil interlock
   and the 10 ms settle time. The G6S-2 is the chosen part; the AQY212-only
   fallback is 100 MΩ.
2. **TIA op-amp:** LMC6482 A (recommended) vs. OPAMP2 (100 MΩ, fewer parts).
   Measure the G474 OPAMP2 input bias current before deciding.
3. **V_N clamp:** BAT54S (low Vf, higher leakage) vs. BAV199 (low leakage,
   higher Vf) vs. a mode-gated V_N input. The 1 GΩ/0.5 % target favors BAV199
   plus a 1 MΩ series resistor; the C-mode clamp voltage must stay within the
   OPAMP3 absolute maximum.
4. **Kelvin 4.7 MΩ element:** MAX4619 off-leakage may limit 22 MΩ accuracy;
   consider a reed relay for the top element if the measured leakage is high.
5. **VREF source:** resistor divider (simple, stable) vs. DAC (programmable,
   but OPAMP1 VINM cannot be driven by the DAC internally). The divider is
   retained.
6. **VBIAS source:** 10 kΩ/10 kΩ divider (simple) vs. DAC1/OPAMP buffer. The
   divider is retained for noise and simplicity.
7. **External VREF+:** 3.3 V VDDA vs. a 3.0 V reference. VDDA is retained so
   `V_cap` can use the full 3.3 V range; a 3.0 V reference would need a divider
   on `V_cap`.
8. **C-mode drive:** COMP1 output directly to the buffer (recommended) vs. a
   timer/FF. The direct comparator loop has the lowest latency and no extra
   logic.

---

## 13. Relationship to the ESWCap repository

The authoritative ESWCap circuit document and the interactive page must be
reconciled with this plan:

- `ESWCap/docs/combined-circuit.md` currently shows the LMC6482 half B as the
  `V_N` buffer, the 900 MΩ/100 MΩ divider, and the LM393. Those are superseded
  by §3–§5 here.
- `ESWCap/references/index-combined.html` and `app-combined.js` must show the
  single shared circuit, the COMP1 oscillator, the OPAMP3 `V_N` buffer, the
  LMC6482 TIA, the direct `V_N` sense, and the G6S-2 relay network.
- The KiCAD schematic should be one root sheet (or a root with shared DUT/bias
  and two range-bank sub-sheets), not two independent front-end boards.

The physics and the host protocol remain the same. The calibration values
(`K`, `T0`, `R_eff`, `C0`, `C_block`, `R_bias`) carry over; the R-mode values
are new.
