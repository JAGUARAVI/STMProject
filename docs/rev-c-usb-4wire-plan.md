# Rev C plan — USB-only power, true 4-wire C/R, corrected J8 bias header

> **Review:** see [`rev-c-scope-review.md`](rev-c-scope-review.md) before
> building. It keeps the G6S-2 **DC5** relays on VBUS, drops the C-mode 4TP
> block (U8/R38/R39/C20/C19), protects `SL`, isolates the V_N branch in C mode
> with a spare AQY212, uses `V_FL` (PA7) instead of nominal VREF, and adds the
> OLED/button the integration plan requires.

**Status:** design plan, not yet implemented. Supersedes the Rev B power/relay
arrangement and the earlier J8 pin proposal.
**Scope:** ESWCap combined C+R carrier for the WeAct STM32G474 “Long” module.
**Decisions already confirmed:** second LMC6482 allowed; J8 is 1×8; J5 is a
4-pin terminal block (5.08 mm baseline, mechanical change to 3.81 mm possible
later without circuit changes).

---

## 1. What this revision changes

1. **USB-only board power.** The carrier has no 5 V rail. The module’s USB-C
   VBUS feeds the module’s D4 and ME6231A33M3G LDO; the carrier takes only
   +3.3 V from the module (J4-3/4 and J2-2). J3-3/4 (`VDD5V`) are left NC.
2. **Relays move to 3.3 V.** K1/K2 become Omron **G6S-2 DC3** (3 V coils).
   Coil high side, D4/D5 cathodes, C10/C15 move from `+5V` to `+3V3`.
   The `+5V` net is deleted.
3. **True 4-wire DUT interface.** J5 becomes 4-pin: `FH`, `SH`, `FL`, `SL`.
   `N_DUT` splits into force-high/sense-high; `B` splits into
   force-low/sense-low. R-mode becomes true 4-wire Kelvin.
4. **True 4-terminal C measurement.** A fixed 10 M/1 M divider on `SH`, buffered
   by U8A, gives the C-mode DUT voltage; the selected C range resistor is the
   current-sense element. The firmware computes `Z_DUT = V_DUT / I_range`,
   separates lead resistance from capacitance, and subtracts the divider
   current. No analog switch, no third relay, no current-sense shunt.
5. **Corrected J8 bias-control header.** 1×8 header with hardware PWM on
   PA4/TIM3_CH2, enable on PA15 with a 10 k pulldown, monitor on PA0/ADC1_IN1,
   and a separate USART3 on PC10/PC11.
6. **BOM stays small.** Only **J8** is a new BOM line. U8 joins the existing
   LMC6482 line; all new passives merge into existing value/footprint lines.
   Physical parts: 73 → 84. PWR_FLAGs: 4 → 3.

**Rejected parts:**

- **TLC2262 / TLC2272** — minimum supply is 4.4 V; not usable on the 3.3 V
  rail. Powering them from 5 V would resurrect the carrier 5 V rail and require
  level shifting.
- **74LVC1G3157** — earlier switchable-divider idea. Dropped; the fixed divider
  needs no switch.
- No DAC, no third relay, no current-sense shunt.

---

## 2. Power architecture

```
USB-C VBUS ── module D4 (B5819WS) ── VDD5V ── module ME6231A33M3G ── +3V3
                                                                    │
                          J4-3/4 + J2-2 ────────────────────────────┘
                                                                    │
        MCU / ADC / op-amps / muxes / SSRs / LEDs / K1,K2 coils ────┘

External Vbias 0–20 V ── J6 (V_BIAS, GND)     ← only external supply
Future bias control  ── J8 (3.3 V signals)   ← passive header, no driver yet
```

- J3-3/4 (`VDD5V`) are **NC** on the carrier. J3 stays fitted for mechanical
  support; change its value/silkscreen to `WeAct P3 (NC)`.
- C10 100 nF, C15 10 µF, D4/D5 cathodes and K1/K2 coil+ move to `+3V3`.
- FLG2 (`+5V` PWR_FLAG) is removed; `+3V3` already has FLG1.
- No ORing diode, no boost, no external 5 V input.

### Relay coil check

| Parameter | G6S-2 DC3 |
| --- | --- |
| Coil resistance | 64.3 Ω ±10% |
| Rated current at 3 V | 46.7 mA |
| Coil power | ≈140 mW |
| Must operate | 2.25 V max |
| Must release | 0.3 V min |
| Max coil voltage | 6 V (200%) |

At 3.3 V minus Q2/Q3 Vce(sat), the coil sees ≈3.2 V, ≈49.8 mA per coil,
≈100 mA for two. Pull-in margin is ≈0.95 V; 3.3 V is 110% of rated and well
inside the 200% limit. **No series resistor is needed.** The DC5 coil would see
only 3.3 V at 18.5 mA, below its 3.75 V must-operate point — it will not work.

### LDO thermal

ME6231A33M3G: SOT-23-3, 500 mA, θJA = 230 °C/W, TJ max 150 °C.

| 3.3 V load | LDO dissipation | ΔT | TJ at 25 °C | TJ at 40 °C |
| --- | ---: | ---: | ---: | ---: |
| 150–180 mA | 0.20–0.24 W | 46–56 °C | 71–81 °C | 86–96 °C |
| 200–230 mA (both relays + 50 mA J8 module) | 0.27–0.31 W | 62–71 °C | 87–96 °C | 102–111 °C |

The module LDO will run warm (~60–70 °C case). At 25–40 °C ambient it is
within the 150 °C junction limit. At 85 °C ambient it is at or near the limit;
keep the J8 module load below ~50 mA or ventilate the enclosure. USB current
stays below ~250 mA at 5 V, inside USB 2.0’s 500 mA.

The 3.3 V rail is also the analog reference. `VREF` and `VBIAS` are ratiometric
to it, so steady-state relay current is tolerable; wait for relay switching to
settle before sampling. C15 10 µF is local to the coils.

---

## 3. 4-wire DUT interface

### 3.1 Connector

J5: 4-pin terminal block, **5.08 mm baseline** (3.81 mm is a mechanical
alternative later).

| Pin | Net | Meaning |
| --- | --- | --- |
| 1 | `FH` | Force high |
| 2 | `SH` | Sense high |
| 3 | `FL` | Force low |
| 4 | `SL` | Sense low |

For a 2-wire DUT, link 1–2 and 3–4 at the connector. For 4-wire, use four
separate leads and connect the sense leads at the DUT terminals.

### 3.2 Net split

| Rev B | Rev C | Notes |
| --- | --- | --- |
| `N_DUT` | `FH` + `SH` | Q1/D6/C_block/K1B/R9/U7/R12 stay on **FH**; R20/V_N sense moves to **SH** |
| `B` | `FL` + `SL` | K1A/K2A/TIA/OPAMP1 stay on **FL**; LMC6482 B `IN+` moves to **SL** |
| J5 | 2-pin → 4-pin | `FH/SH/FL/SL` |
| R20 | `N_DUT` → `VN_SENSE` | move to `SH` → `VN_SENSE` |
| U1 B `IN+` | `B` → `SL` | V_B buffer senses true DUT bottom |

Q1 drain → D6 → `FH`; D6 stays in the force path. The sense tap `SH` is after
D6, so D6’s drop is not included in the DUT voltage.

### 3.3 R-mode math

Current is set by OPAMP1 regulating `FL` to `VREF`; the selected RSENSE is
between `FL` and GND. The DUT voltage is `V_SH − V_SL`. Use:

```text
R_DUT = RSENSE_eff × (V_SH − V_SL) / VREF
I_DUT = VREF / RSENSE_eff
```

The old formula `RREF × (V_N − V_B) / V_B` is the 2-wire form; keep it as a
compatibility function, but the Rev C path uses VREF. Calibrate each
RSENSE_eff and the OPAMP1 offset. The LMC6482 B buffer now senses `SL` with
20 fA input bias; at 22 MΩ that is ≈0.44 µV, negligible.

### 3.4 C-mode 4-terminal measurement

The C-mode drive chain is unchanged:

```text
U2 74LVC1G34 → C mux U3 → R_range → V_cap → C_block → FH → DUT → FL → GND
```

Add:

```text
SH → R38 10 M → SH_DIV → R39 1 M → GND
                     └→ U8A LMC6482 unity buffer → PC4 / ADC2_IN5
```

- Divider ratio 1/11: 0–20 V → 0–1.82 V. C20 10 pF compensates the divider.
- U8A is a second LMC6482 (same part as U1); U8B is spare, tied as a unity
  buffer to GND or left as an optional gain stage.
- C19 100 nF decouples U8.
- The divider loads `SH` with 11 M. In R-mode this current bypasses the DUT and
  RSENSE, so it does not affect the 4-wire resistance result. In C-mode the
  firmware subtracts `V_SH / 11 M` from the range-resistor current.

Firmware measurement:

```text
I_total = (V_drive − V_cap) / R_eff
I_div   = V_SH / 11 M
I_DUT   = I_total − I_div
V_DUT   = V_SH − V_SL
Z_DUT   = V_DUT / I_DUT
C_DUT   = −1 / (ω · Im(Z_DUT))
R_lead  = Re(Z_DUT)
```

`V_drive` is the 74LVC1G34 output (0 or 3.3 V, calibrated); `V_cap` is the
existing PA1/COMP1 node; `V_SH` is the divider output ×11; `V_SL` is the
existing V_B buffer. The force-lead resistance appears in `Re(Z_DUT)` and is
therefore separated from the capacitance. This is the true 4-terminal-pair
path.

If the phase-sensitive firmware is not implemented in the first pass, the
fallback is the ratiometric form:

```text
C_DUT = C_block × (V_cap − V_SH) / (V_SH − V_SL)
```

That still uses the 4-wire sense, but it does not separate the force-lead
resistance. The plan targets the complex form.

**Bandwidth limit:** the LMC6482 unity buffer is ≈1.5 MHz; use the 4-terminal
C path up to ~100 kHz for full accuracy. Above that, the existing V_cap
oscillator still works, but the sense buffer rolls off and must be calibrated
or bypassed.

---

## 4. J8 bias-control header

J8: 1×8, 2.54 mm header.

| Pin | Signal | Module pin | Function / RC |
| --- | --- | --- | --- |
| 1 | `BIAS_SET` | **PA4 / J2-10** | **TIM3_CH2 hardware PWM**, R34 1 k + C16 10 µF |
| 2 | `BIAS_EN` | **PA15 / J1-10** | GPIO, R35 **10 k** pulldown |
| 3 | `BIAS_MON` | **PA0 / J2-6** | ADC1_IN1, R36 1 k + C17 100 nF |
| 4 | GND | — | — |
| 5 | `+3V3` | J4 | future module <50 mA |
| 6 | `BIAS_UART_TX` | **PC10 / J1-9** | USART3_TX, separate net |
| 7 | `BIAS_UART_RX` | **PC11 / J1-8** | USART3_RX, separate net |
| 8 | GND | — | — |

Internal only: **PC4 / J2-14 = ADC2_IN5**, driven by U8A for the C-mode `SH`
divider. It is not on J8.

### Pin verification

| Pin | Verified use | Conflict check |
| --- | --- | --- |
| **PA4** | AF2 = **TIM3_CH2** | TIM3 unused by firmware. DAC1_CH1 is internal-only for COMP4; do **not** enable the DAC1 output buffer. COMP1_INM (PA4) is not used; COMP1 uses DAC3_CH1 as INM. |
| **PA15** | GPIO output for `BIAS_EN` | JTDI has an internal pull-up at reset. 10 k pulldown gives ≈0.55–0.83 V, below the usual 0.3·VDD ≈0.99 V EN threshold. Using PA15 as GPIO disables JTAG but leaves SWD. TIM2_CH1 capture uses the internal COMP1_OUT, not the PA15 pin. |
| **PA0** | **ADC1_IN1** for `BIAS_MON` | Free; COMP3 is unused. |
| **PC4** | **ADC2_IN5** for the U8A C-mode `SH` buffer | Free; console UART is PA9/PA10 (USART1). PC4 is only an alternate USART1_TX pin. |
| **PC10/PC11** | **USART3_TX/RX** for the bias UART | Free; console remains USART1 on PA9/PA10. Use `BIAS_UART_TX/RX` net names, never the console `UART_TX/RX`. |

If the future bias module’s EN threshold is below 0.8 V, change R35 to 4.7 k.
If the module drives 5 V UART, PC10/PC11 are 5 V-tolerant, but the MCU TX is
3.3 V; the module must accept 3.3 V logic.

---

## 5. BOM delta

### 5.1 Changed

| Ref | Rev B | Rev C | Qty | Note |
| --- | --- | --- | ---: | --- |
| K1, K2 | G6S-2 DC5 | **G6S-2 DC3** | 2 | same footprint/driver; 3 V coils |
| C10 | 100 nF on `+5V` | 100 nF on `+3V3` | 1 | value unchanged |
| C15 | 10 µF on `+5V` | 10 µF on `+3V3` | 1 | value unchanged |
| D4, D5 | cathode `+5V` | cathode `+3V3` | 2 | flyback |
| J3 | pins 3/4 `+5V` | pins 3/4 **NC** | 1 | value `WeAct P3 (NC)` |
| J5 | 2-pin 5.08 mm | **4-pin 5.08 mm** | 1 | `FH/SH/FL/SL` |
| R35 | 100 k pulldown | **10 k pulldown** | 1 | PA15/JTDI reset pull-up |
| FLG2 | `+5V` PWR_FLAG | removed | −1 virtual | +3V3 has FLG1 |

### 5.2 Added

| Ref | Value / part | Package | Qty | Purpose |
| --- | --- | --- | ---: | --- |
| U8 | **LMC6482AIM/NOPB** (same as U1) | SOIC-8 | 1 | C-mode `SH` divider buffer; U8B spare |
| J8 | 1×8, 2.54 mm header | THT | 1 | bias control |
| R38 | 10 MΩ 0.1% | 0805 | 1 | `SH` divider top |
| R39 | 1 MΩ 1% | 0805 | 1 | `SH` divider bottom |
| C20 | 10 pF 50 V C0G | 0805 | 1 | divider compensation |
| C19 | 100 nF 50 V X7R | 0805 | 1 | U8 decoupling |
| R34 | 1 kΩ 1% | 0805 | 1 | `BIAS_SET` series |
| C16 | 10 µF 16 V X5R | 0805 | 1 | `BIAS_SET` filter |
| R36 | 1 kΩ 1% | 0805 | 1 | `BIAS_MON` series |
| C17 | 100 nF 50 V X7R | 0805 | 1 | `BIAS_MON` filter |

All new passives use values already in the BOM. U8 joins the existing `U1`
LMC6482 line. **J8 is the only new grouped BOM line.**

### 5.3 Counts and cost

| Item | Rev B | Rev C | Delta |
| --- | ---: | ---: | ---: |
| Physical components | 73 | **84** | +11 |
| Grouped BOM lines | 44 | **45** | +1 (J8) |
| PWR_FLAG | 4 | 3 | −1 |
| PCB footprints | 78 (73 + 5 holes) | **89** (84 + 5 holes) | +11 |
| Nets | 56 | ≈66 | +10 (split + J8 + C sense; exact after export) |
| Board size | 111 × 85 mm | 111 × 85 mm (verify J5/J8) | 0 or small |

Added material is dominated by the second LMC6482 (~$2–3 at qty 1); the header,
terminal block and passives add well under $1. The G6S-2 DC3 is the only part
with a possible stock/price difference from the DC5.

---

## 6. Firmware plan

1. **Pin reservations** in `include/config.h` (no driver required yet):
   - `BIAS_SET` PA4 (TIM3_CH2), `BIAS_EN` PA15, `BIAS_MON` PA0/ADC1_IN1,
     `BIAS_UART_TX/RX` PC10/PC11, `CMODE_SH` PC4/ADC2_IN5.
   - Update the obsolete “G6S-2 coils are 5 V” comment to “G6S-2 DC3 on 3.3 V”.
2. **4-wire R math**: add `rmode_resistance_4w(rref, vsh, vsl, vref)` and
   `rmode_current_ua_4w(rref, vref)`. Keep the existing 2-wire functions for
   compatibility tests. Update `rmode.c` to pass the measured VREF and read
   V_B from `SL`.
3. **C-mode 4TP**:
   - Sample `V_cap` (PA1), `V_SH` (PC4/ADC2_IN5 ×11), `V_SL` (PB12/ADC4_IN3) and
     the drive state, synchronously with the TIM2/COMP1 oscillator.
   - Compute `I_total`, subtract `I_div`, form `Z_DUT`, and derive C and lead R.
   - Calibrate `R_eff` per C range, divider ratio, C_block, drive levels, and
     the U8A path gain/phase.
   - Fallback: ratiometric `C = C_block × (V_cap − V_SH)/(V_SH − V_SL)`.
4. **J8 driver**: not in the first pass. PWM/filter on PA4, EN on PA15,
   monitor on PA0, USART3 on PC10/PC11 are documented and reserved.
5. **Tests**: update host math tests for the 4-wire formula; add 4TP math tests
   with synthetic V/I vectors. Keep all four PlatformIO environments building
   and `make -C test/host` passing.

---

## 7. PCB plan

- Delete the `+5V` copper; reroute `+3V3` to K1/K2 coil high side, D4/D5
  cathodes, C10/C15. J3-3/4 pads stay NC.
- J5 4-pin 5.08 mm on the left edge. The body is ≈20.3 mm; centered near the
  old J5 position it spans y≈34–54, leaving ≈6 mm to J6 at y=60. Verify; switch
  to 3.81 mm if clearance is tight.
- Split `N_DUT`/`B` around K1/K2, U1, R20, R16/C4 and route `FH/SH/FL/SL` to
  J5. Keep the `SH`/`SL` sense traces away from the relay coil and switch nodes.
- Place U8 near U1; R38/R39/C20 at the U8 input; C19 at U8’s supply pins.
  Route U8A output to PC4 / J2-14.
- J8 on the free bottom-right edge (x≈65–95, y≈80) so the SET/EN/MON/UART
  signals are short to J1/J2; +3V3/GND are available there. Place R34/C16 and
  R36/C17 near J8; R35 at PA15.
- Add a clear pin-1 mark and silkscreen labels for J5 (`FH SH FL SL`) and J8.
  Mark J3 `NC`; mark the board `USB POWER ONLY — VBIAS 0–20 V EXT`.
- B.Cu remains a GND plane; keep it continuous under the new traces and use
  thermal reliefs on J5/J8 GND pads.
- DRC target: 0 errors, 0 unconnected. Regenerate Gerbers/drill/BOM/PnP/PDFs.
  **Do not fabricate the Rev B files.**

---

## 8. Documentation updates

- `README.md` — USB-powered, only Vbias external, J8 pinout.
- `docs/pcb-design.md` — Rev C power, 4-wire J5, U8 divider, BOM delta, layout.
- `docs/handoff-verdict.md` — remove the 5 V relay statement; note 4-wire.
- `docs/resistance-autoranging-plan.md` — G6S-2 DC3, 4-wire R math, J5.
- `docs/peripherals-g474.md` — J3-3/4 are VDD5V inputs, NC on carrier.
- `docs/HANDOFF-combined-circuit.md` — Rev C banner/correction.
- `include/config.h` — pin reservations and relay comment.

---

## 9. Bench validation

1. **Power:** USB-C only, no Vbias. Measure +3V3 at J4-3/4 (3.3 V ±1%),
   USB current (<250 mA), module LDO case temperature with both relays on for
   10 minutes.
2. **Relays:** K1/K2 pickup/dropout, coil voltage ≈3.2 V, coil current ≈50 mA
   each, contact continuity. Verify K2 never energizes without K1.
3. **4-wire R:** 1 Ω, 100 Ω, 10 kΩ, 1 MΩ with a 4-wire fixture. Compare 2-wire
   vs 4-wire; confirm lead resistance is cancelled. Check 1 Ω at 4.1 mV.
4. **4-wire C:** 100 pF, 1 nF, 100 nF, 1 µF; compare against a known LCR meter.
   Verify the divider-current correction and the force-lead separation. Check
   the fallback ratiometric path.
5. **J8:** PA4 PWM ripple at C16, PA15 pulldown at reset, PA0 ADC reading,
   PC10/PC11 UART loopback.
6. **Thermal soak:** both relays on, Vbias 20 V, 30 minutes; check LDO, relay
   and board temperatures; confirm no 3.3 V droop or ADC drift.

---

## 10. Risks and open items

- **C-mode 4TP firmware is the largest task.** The hardware supports it, but
  synchronous sampling/phase math and calibration need bench time. The
  ratiometric fallback is not full 4TP.
- **LMC6482 bandwidth** limits the 4-terminal C path to ≈100 kHz; document it.
- **Divider loading:** 11 M on `SH` is subtracted in C-mode; verify at the 1 M
  range and low frequency.
- **G6S-2 DC3 stock** — check Digi-Key/Mouser/LCSC before layout.
- **J5 5.08 mm fit** — verify against J6; 3.81 mm is the mechanical fallback.
- **J8 +3V3 load** — keep the future module <50 mA, or add a fuse/separate
  supply; the module LDO thermal budget is the limit.
- **PA15 reset pull-up** — 10 k pulldown gives ≈0.55–0.83 V; use 4.7 k if the
  future EN threshold is lower.
- **PC4 analog input** — keep the U8A output and trace away from the C-mode
  switching nodes; add an RC filter at PC4 if needed.

---

## 11. Decision log

| Decision | Choice | Reason |
| --- | --- | --- |
| C-mode buffer | **U8 = LMC6482** (second one) | Already in BOM, 3.3 V capable, 20 fA; TLC2262 needs ≥4.4 V |
| J5 | 4-pin **5.08 mm** baseline | Matches J6 series; 3.81 mm is a later mechanical change |
| J8 | **1×8** | User confirmed; includes SET/EN/MON, GND, +3V3, UART |
| `BIAS_SET` | PA4 / TIM3_CH2 | Hardware PWM; TIM3 free; DAC1_CH1 stays internal for COMP4 |
| `BIAS_EN` | PA15 / GPIO, 10 k pulldown | Only free GPIO near J1; pulldown handles JTDI reset pull-up |
| `BIAS_MON` | PA0 / ADC1_IN1 | Free ADC; no conflict |
| C-mode `SH` sense | R38/R39 + U8A → PC4/ADC2_IN5 | Fixed divider, no switch; R-mode unaffected |
| 4-wire R current reference | VREF, not V_B | V_B now senses SL; avoids force-lead error |
| 74LVC1G3157 | rejected | No switch needed with fixed divider |
| TLC2262 | rejected | 4.4 V minimum supply |
