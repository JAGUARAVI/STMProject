# ESWCap Rev C hardware

This directory contains the KiCad source for the Rev C carrier:
USB-only power, 3.3 V relays, true 4-wire C/R, and the corrected J8 bias
header.

## Status

- **Schematic:** implemented and ERC-clean (0 errors, 0 warnings).
- **PCB:** intentionally not part of this push. The routed
  `wip/Project-revC-routing-baseline.kicad_pcb` is an **intermediate
  pre-review baseline** and does not match the current schematic. Do not send
  it to fabrication. It is kept only to preserve routing work.
- **BOM:** generated from the schematic; `fab/Project-bom.csv`.
- **Schematic PDF/SVG:** `fab/Project-schematic.pdf` and `.svg`.

Open `kicad/Project.kicad_pro` with KiCad 10. The project-local libraries
(`eswcap.kicad_sym`, `eswcap.pretty/`) are included; the schematic will not
resolve symbols/footprints without them.

## Rev C circuit summary

- **Power:** USB-C VBUS → module D4 → VDD5V → module ME6231A33M3G LDO → +3.3 V.
  J3-3/4 (`VDD5V`) are NC on the carrier. No 5 V carrier rail. Only Vbias
  (0–20 V) is external, on J6.
- **Relays:** K1/K2 = Omron **G6S-2 DC3** (3 V coils) on +3.3 V. D4/D5
  flyback cathodes, C10/C15, and coil high side are on +3V3.
- **DUT:** J5 is 4-pin, ordered **FH, SH, SL, FL**. R-mode is true 4-wire
  Kelvin; `SL` has a 100 kΩ series resistor (R40) into U1B.
- **C-mode 4-terminal sense:** 10 M/1 M divider on `SH` (R38/R39, both 0.1%),
  compensated by C21 10 pF across the top leg and C20 100 pF across the bottom
  leg. The divider is buffered by the **internal STM32 OPAMP3** (no second
  LMC6482). U9 isolates the `SH → R20 → VN_SENSE` branch when K1 is off; U10
  connects `SH_DIV` to `VN_SENSE` when K1 is off. U10 is controlled by PC4
  (`C_MODE_SW`, active low). C7 is **10 pF C0G** (was 1 nF), so the 4TP pole is
  ≈17.7 kHz instead of ≈177 Hz.
- **SH divider correction:** the divider is permanently connected to `SH`.
  In R-mode its current bypasses the DUT, so the firmware subtracts
  `I_div = V_SH / 11 MΩ` from the Kelvin force current before computing R.
- **Switches:** U5–U7, U9, U10 are **AQY221N2VY** (SSOP-4, 1 pF, 10 nA max
  leakage, 120 mA). R9 is **100 Ω** to keep the 20 V precharge under the
  300 mA peak rating.
- **R20 / D2:** R20 = 100 kΩ; D2 = BAV199 (low-leakage clamp).
- **J8 bias-control header:**

| Pin | Signal | Module pin | Note |
| --- | --- | --- | --- |
| 1 | `BIAS_SET` | PA4 / J2-10 | TIM3_CH2 PWM, R34 1 k + C16 10 µF |
| 2 | `BIAS_EN` | PA15 / J1-10 | GPIO, R35 10 k pulldown |
| 3 | `BIAS_MON` | **PA8 / J1-15** | ADC5_IN1, R36 1 k + C17 100 nF |
| 4 | GND | — | — |
| 5 | `+3V3` | J4 | future module <50 mA |
| 6 | `BIAS_UART_TX` | PC10 / J1-9 | USART3_TX |
| 7 | `BIAS_UART_RX` | PC11 / J1-8 | USART3_RX |
| 8 | GND | — | — |

- **PA0 is `DRIVE` / COMP1_OUT** (firmware fix of 2026-11-12). PA8 is now free
  and carries `BIAS_MON`. PC4 is the U10 control output.
- **J5 pin order:** 1 FH, 2 SH, 3 SL, 4 FL.

## DUT connection

Use a true 4-wire connection at the DUT, not at the connector:

| J5 pin | Net | DUT side |
| --- | --- | --- |
| 1 | `FH` | Force high: outer contact on DUT terminal A |
| 2 | `SH` | Sense high: inner contact on DUT terminal A, as close to the body as possible |
| 3 | `SL` | Sense low: inner contact on DUT terminal B |
| 4 | `FL` | Force low: outer contact on DUT terminal B |

For a 2-wire DUT, link 1–2 and 3–4 at J5, then connect pins 2 and 3 to the DUT.
That is a convenience mode only; it loses the Kelvin/4TP benefit.

Recommended contacts:

- **Leaded parts:** a commercial 4-wire Kelvin clip with two isolated contacts
  per jaw, or a 4-terminal fixture with one force and one sense spring pin per
  DUT lead. The sense pin must be the inner contact, closer to the component
  body.
- **SMD parts:** 4-wire Kelvin tweezers or a custom fixture with two force and
  two sense pogo pins per pad. Keep the sense pins at the component terminals
  and the force pins outside them.
- **Low resistance (≤10 Ω):** gold-plated, low-thermal-EMF contacts; keep both
  DUT terminals at the same temperature; use current reversal in firmware to
  cancel thermal EMF and offset.
- **High resistance / low capacitance:** guarded fixture, PTFE insulation,
  short shielded sense leads, clean and dry surfaces. For pF measurements use a
  guarded enclosure and open/short calibration at the DUT plane.
- **Cables:** twisted pair for `FH`/`FL` (force), shielded twisted pair for
  `SH`/`SL` (sense). Connect the sense shield to guard/GND at the board end
  only. Do not use a solderless breadboard.

## BOM

`fab/Project-bom.csv` is generated from the schematic. It contains 88 physical
parts, 45 grouped lines and 3 PWR_FLAGs. The five switches are AQY221N2VY
(SSOP-4); R9 is 100 Ω; C7 is 10 pF. The only new orderable part types are J8
(1×8 2.54 mm header), J5 (4-pin 5.08 mm terminal block), and the AQY221N2VY
SSOP-4 part. K1/K2 are G6S-2 DC3. See the CSV for the complete list.

## What is deliberately not here

- The second LMC6482 was removed: it is not available. The C-mode SH divider
  now uses the internal OPAMP3.
- The PCB is not final. It must be regenerated and rerouted after the
  schematic is frozen. The WIP baseline has the pre-U9/U10 netlist and must
  not be fabricated.
- The OLED/button additions proposed in the integration-plan review are not
  part of this revision.
