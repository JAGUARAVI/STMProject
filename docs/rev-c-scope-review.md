# Rev C 4-wire plan: scope check and corrections

**Reviewed:** `docs/rev-c-usb-4wire-plan.md` (commit 7f97914)
**Against:** *Automated Integrated Sensing: Integration Plan* (team PDF, Nov 2026)
and the as-designed Rev B board (`docs/pcb-design.md`,
`docs/resistance-autoranging-plan.md`, `include/config.h`).
**Constraint:** prefer parts already in the Rev B BOM or listed as on hand.

## Verdict

The 4-wire R path is sound. With the four changes in §3.1 it measures
**about 0.1 Ω to 22 MΩ in Kelvin mode**, and the TIA covers 22 MΩ to 1 GΩ. That
covers the PDF's R scope except for one thing: the **1 mΩ low end**. This
design cannot reach it, and no firmware change will fix that (§2.1).

The C-mode 4-terminal part (U8, R38/R39, C20, C19) is not in the PDF scope, and
it adds a load to the DUT node. **Do not fit it** (§3.2).

The relay change from G6S-2 DC5 to DC3 is not needed. The DC5 relays are
already on hand, and VBUS 5 V is always present on a USB-powered board.
**Keep the DC5 relays** (§3.3).

---

## 1. Scope alignment

| PDF requirement | Rev C as written | With the fixes below |
| --- | --- | --- |
| C 10 pF – 1000 µF | Same C front end as Rev B, plus U8/divider load on the DUT node | OK. U8 not fitted; V_N branch isolated in C mode |
| C under 0–20 V bias | V_N clamp branch drains 16 µA from N_DUT at 20 V (Rev B carry-over) | OK. Branch disconnected in C mode |
| **Kelvin 1 mΩ – 1 kΩ** | 4.1 mA max, 12-bit internal ADC: about 1 Ω and up is good, 0.1 Ω is coarse, **1 mΩ is impossible** | **0.1 Ω – 1 kΩ** (and up to 22 MΩ). 1 mΩ needs a scope decision (§2.1) |
| Kelvin current reversal | Not possible (unipolar P-FET source) | Replaced by a two-current delta in firmware. It cancels the same offsets |
| Kelvin ranges 100/10/1 mA (K1–K5) | 4.1 mA / 410 µA / 4.1 µA / 86 nA via the MAX4619 R bank | Same. This is a deliberate G474 redesign |
| TIA 1 MΩ – 1 GΩ, single supply | LMC6482 A, Rf 10 MΩ, VBIAS 1.65 V. 1 GΩ gives only ~9.5 mV of swing | OK. Kelvin covers ≤22 MΩ; optional Rf 20 MΩ doubles the 1 GΩ signal |
| Auto-detect R / C / open | Feasible: the Kelvin source is a constant current into a capacitor too | Firmware only (§4) |
| Safe input | **New `SL` net goes straight to an op-amp input with no protection** | 100 kΩ series resistor (§3.1) |
| OLED + button | **No pins or connector for either** | SSD1306 on J8 pins 4–7; button header on PC13 (§3.4) |
| USB JSON lines | Console is USART1 on J7. Native USB is reserved but has no CDC driver | Firmware: add USB-CDC on PA11/PA12 (hardware is ready) |
| Board: PDF suggests Nucleo-G431RB | Firmware uses ADC3/ADC4/OPAMP3; **G431 has no ADC3/ADC4** | Answer the PDF's open question: **WeAct G474CEU6** for everyone |

The PDF's architecture section also still describes the ESP32-era Kelvin block
(ADS1115, MCP6002 + BD140, ULN2803A, K1–K5 at 5 V) and the external LM393. The
repo replaced these with OPAMP1 + AO3401, the internal ADC, the MAX4619 R bank
and COMP1. Those are good changes, but the PDF should be updated so the team
builds the same thing.

---

## 2. Things that need a team decision

### 2.1 The 1 mΩ Kelvin floor

At 4.1 mA (98.9 Ω element, VREF 0.405 V) the DUT voltage is:

| R_DUT | Signal | Realistic result (oversampled 12-bit, delta method) |
| --- | --- | --- |
| 1 kΩ | 410 mV (0.41 mA element) | < 0.2 % |
| 10 Ω | 41 mV | ~0.2–0.4 % (matches the ESP32's ±0.40 %) |
| 1 Ω | 4.1 mV | ~0.5–1 % |
| 0.1 Ω | 0.41 mV | a few % with averaging |
| 10 mΩ | 41 µV | ~10 %+, noise-limited |
| 1 mΩ | 4.1 µV | below the ADC noise floor |

The only way to reach 1 mΩ is roughly 100 mA plus a µV-class ADC: the ESP32
block (MCP6002 + BD140 + ADS1115 + K1–K5). That hardware is on hand, but it
needs the 5 V rail (another reason to keep it, §3.3), I²C (bit-banged is fine),
and its own board area.

**Recommendation:** change the PDF's Kelvin spec to **0.1 Ω – 1 kΩ** for this
build. Keep the ADS1115 block as an add-on only if milliohms are a graded
requirement.

### 2.2 Does the 0–20 V bias stay?

This is the PDF's open question. If bias is dropped, §3.1(c) still matters
(it removes a 1 MΩ AC load from V_cap), but the clamp drain at 20 V no longer
does.

---

## 3. Circuit changes

### 3.1 Required for the 4-wire R path

**(a) Protect `SL`.** Rev C moves LMC6482 B `IN+` from `B` to the new `SL` pin
on J5. In Rev B, `B` was grounded by K1A in C mode and clamped by a BAV199. `SL`
has neither: a charged capacitor, or a sense jaw that touches before the force
jaw, drives the op-amp input directly.
**Fix:** put **100 kΩ** (same value as R30/R31) in series at U1B pin 5. Do not
add diodes: `SL` sits on the TIA summing node in TIA mode, so diode leakage
would add error. 20 V / 100 kΩ = 0.2 mA, which the LMC6482's internal ESD
diodes handle (abs. max ±5 mA). The error is 20 fA × 100 kΩ = 2 nV.

**(b) Measure the force current from `FL`, not nominal VREF.** Rev C uses
`R = RSENSE_eff × (V_SH − V_SL) / VREF`. OPAMP1's offset (up to ~3 mV) sits
between VREF and the real `FL` voltage, which is a 0.7 % current error.
`FL` is PA7, and PA7 is also **ADC2_IN4**, so it can be read directly:

```text
R_DUT = RSENSE_eff × (V_SH − V_SL) / V_FL        V_FL from PA7 / ADC2_IN4
```

This needs no hardware change. It is the Rev B ratio with the correct 4-wire
nodes.

**(c) Disconnect the V_N branch in C mode** (this fixes a Rev B issue that Rev C
carries over). `N_DUT/SH → 1 MΩ → BAT54S → OPAMP3` stays connected in C mode.
At 20 V bias, 16 µA flows into the clamp. Through the 4.8 MΩ R_bias, that
settles `N_DUT` at about 6.5 V. C_block slows the sag to about 16 mV/s, but it
never stops. At AC, the same branch puts a 1 MΩ load on V_cap (through C_block)
on the 1.01 MΩ oscillator range. The ESP32 build only had a 1 GΩ divider there.
**Fix with on-hand parts:** put one of the **two spare AQY212** in series
between `SH` and R20. Drive its LED from the K1 coil: an AQY212 LED plus a
**330 Ω** resistor (the existing LED-resistor value) in parallel with the K1
coil gives about 11 mA when K1 is on. The switch is then closed in R mode and
open in C mode by hardware, with no new GPIO and no firmware interlock.
Once this branch is isolated in C mode, also change:
- **R20: 1 MΩ → 100 kΩ.** It no longer has to stand off 20 V continuously, and
  the OPAMP3 bias-current error drops by 10×.
- **The V_N clamp: BAT54S → BAV199** (already in the BOM). Schottky leakage of
  tens of nA at room temperature, times the series resistor, shows up directly
  as a temperature-dependent V_SH offset. That is tens of mV with 1 MΩ, which
  is 10× the whole 1 Ω signal.

**(d) Reorder J5: `FH, SH, SL, FL`** (Rev C has FH, SH, FL, SL). In TIA mode,
`SL` is at the summing-node potential, so placing it between `SH` (~2.6 V) and
`FL` makes it a free guard on the terminal block. Kelvin and C modes are
unaffected.

### 3.2 Do not fit the C-mode 4TP block (U8, R38, R39, C20, C19)

- **Not in scope.** Lead *resistance* does not limit any PDF capacitance case:
  at the 98.9 Ω range, 0.1 Ω of lead resistance is 0.1 %. The 10 pF end is
  limited by stray *capacitance*, which tare handles and 4-wire does not.
- **It loads the DUT node.** It adds 11 MΩ plus ~5–10 pF across the DUT in
  C mode, and pulls the DC bias to 70 % of V_BIAS at steady state.
- **The compensation is wrong as specified.** 10 pF across 10 MΩ needs ~100 pF
  across 1 MΩ to match. With only stray on the bottom, the AC ratio is about 0.5
  instead of 1/11.
- **It is the largest firmware task in Rev C** (phase-sensitive sampling).

Keep the footprints as DNP if you want the option later. This also frees
**PC4 (ADC2_IN5)**.

### 3.3 Keep the G6S-2 DC5 relays on VBUS (revert Rev C §1.2 and §2)

- `resistance-autoranging-plan.md` says the **DC5 relays are on hand**. The DC3
  is a new purchase.
- "USB-only" does not remove 5 V: on USB power, the module's `VDD5V`
  (J3-3/4, ~4.7 V after D4) is always present. The DC5 must-operate voltage is
  3.75 V, so the margin is about 1 V.
- Coils on 5 V take ~100 mA off the 3.3 V LDO. That removes the
  102–111 °C junction case in Rev C's thermal table. It also keeps relay current
  steps off the rail that VREF and VBIAS are derived from.
- If a battery is added later (a PDF open question), it would feed that same
  5 V input.

Keep the rest of Rev C's power section: no external 5 V input, and V_BIAS on
J6 as the only external supply.

### 3.4 OLED and button (PDF scope, missing from Rev C)

- **OLED:** an SSD1306 4-pin module is wired GND, VCC, SCL, SDA, which matches
  **J8 pins 4, 5, 6, 7** (GND, +3V3, PC10, PC11). Use PC10/PC11 as bit-banged
  I²C (no hardware I²C pair is free on this pin map) with 10 kΩ pull-ups (R29
  value). The SSD1306 draws ~10–20 mA, inside J8's 50 mA budget. The
  bias-module UART reservation moves to "future" until such a module exists.
- **Button:** PC13 is the module's own button (J2-4), which is unreachable in a
  box. Add a 2-pin header from J2-4 to GND for a panel button. The internal
  pull-up is enough.

### 3.5 Optional improvements

- **Sense the 98.9 Ω element directly.** MAX4619 is a triple SPDT (4053-type),
  so a 4:1 bank built from it puts **two** switches in series. That is about
  20–40 Ω of Ron at 3.3 V, with ~0.4 %/°C tempco, inside `RSENSE_eff` for the
  range that covers 0.1 Ω–470 Ω. Wire the freed **PC4 / ADC2_IN5** through 1 kΩ
  to the top of the 98.9 Ω resistor, and use `I = V_PC4 / 98.9 Ω`. The current
  is then set by a 0.1 %, 25 ppm part instead of silicon. Also confirm the real
  mux wiring; `kelvin-ohmmeter-g474.md` §13 still says "dual 4:1 assumed".
- **TIA Rf 10 MΩ → 20 MΩ** (two 10 MΩ 0.1 % in series, the same part as Rf).
  D6 dropped the TIA excitation from 1.65 V to ~0.95 V, so 1 GΩ gives only
  9.5 mV of swing. 20 MΩ gives 19 mV, and the TIA's lower limit (~12 MΩ) still
  overlaps Kelvin's upper limit (22 MΩ). Re-check Cf on the bench.

---

## 4. Firmware changes the corrected 4-wire design needs

1. **4-wire R:** `R = RSENSE_eff × (V_SH − V_SL) / V_FL`, with `V_FL` read on
   ADC2_IN4 (PA7), or on PC4 if §3.5 is fitted.
2. **Two-current delta (replaces current reversal).** Measure on the 98.9 Ω
   element and then the 993.6 Ω element, and compute
   `R = (ΔV₁ − ΔV₂) / (I₁ − I₂)`. `FL` is held at the same VREF both times, so
   the OPAMP3-vs-LMC6482 offset, the ADC3-vs-ADC4 gain mismatch at that common
   mode, and thermal EMFs are all the same in both readings and cancel. Use it
   below ~10 Ω. Above that, a single reading plus a shorted-leads zero
   calibration is enough.
3. **Auto-detect:** probe in Kelvin mode. If the voltage settles, it is a
   resistor. If it ramps, it is a capacitor (big C). If it is pinned at
   compliance, it is either open or a small C; small caps hit compliance
   faster than the 1 ms V_N filter can show. Resolve the pinned case with a
   C-mode oscillator reading: near tare means open. Discharge through SSR2
   before switching modes, as the existing exit sequence already does.
4. **USB-CDC JSON lines** on PA11/PA12 for the FastAPI dashboard.
5. **OLED (software I²C on PC10/PC11) and the button** (single press:
   re-measure; long press: manual mode).
6. Drop Rev C firmware item 3 (C-mode 4TP) with §3.2.

---

## 5. BOM effect of these corrections, against Rev C

| Change | Parts |
| --- | --- |
| K1/K2 stay **G6S-2 DC5** | no DC3 purchase |
| U8, R38, R39, C20, C19 | DNP (−5 fitted) |
| AQY212 (spare) + 330 Ω | +2, both on hand / in BOM |
| SL series 100 kΩ | +1, value in BOM |
| R20 1 MΩ → 100 kΩ, V_N clamp BAT54S → BAV199 | value swaps, both in BOM |
| Button header (1×2) | +1 header |
| Optional: PC4 → 1 kΩ → 98.9 Ω top; second 10 MΩ for Rf | +1 / +1, values in BOM |

Net: no new part types except a 2-pin header. The only Rev C purchase it removes
is the DC3 relays.

## 6. Bench checks to add to Rev C §9

1. With bias at 20 V and the AQY212 open, `N_DUT` stays within 1 % of V_BIAS
   for 60 s.
2. Oscillator period on the 1.01 MΩ range with an open socket matches the
   reading taken with R20 lifted. This shows the V_N branch is no longer
   loading V_cap.
3. With shorted Kelvin leads, the 98.9 Ω vs 993.6 Ω delta reads 0 ± 5 mΩ, and
   1 Ω reads within 1 %.
4. With 20 V applied to `SL` alone, there is no damage and the U1B output is
   clamped at the rail.
5. Open-input TIA floor is above 1 GΩ with J5 in the FH, SH, SL, FL order.
