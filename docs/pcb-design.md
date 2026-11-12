# ESWCap combined C+R carrier — PCB design (Rev B)

Status: **routed, DRC-clean (0 electrical errors, 0 unconnected), fabrication
outputs generated.** This document records the hardware revision that came out
of the independent schematic review, the board mechanics, the layout decisions,
the verification results, and the bench checks that remain.

Source of truth:

- Schematic: `/home/jaguar/Documents/KiCad/Project/Project.kicad_sch`
- PCB: `/home/jaguar/Documents/KiCad/Project/Project.kicad_pcb`
- Project symbol/footprint library: `Project/eswcap.kicad_sym`,
  `Project/eswcap.pretty/`
- Fabrication outputs: `Project/fab/`

---

## 1. Board and module mechanics

| Item | Value |
|---|---|
| Carrier outline | **111 mm × 85 mm**, 2-layer, 1.6 mm FR4 |
| Module | WeAct STM32G474CEU6 **Long** board, 61.42 mm × 18.24 mm |
| Module position | x = 47.58 … 109.00 mm, y = 4.00 … 22.24 mm (top edge) |
| Module headers | 24 pins per long edge, 2.54 mm pitch, rows **15.24 mm** apart |
| Carrier sockets | J4/J3 = 1×4 at x 49.08 … 56.70; J2/J1 = 1×20 at x 59.24 … 107.50; rows y = 5.50 / 20.74 |
| USB access | module USB-C is on the module's right edge (x = 109), 2 mm inside the carrier edge |
| Mounting holes | 5 × M3 NPTH 3.2 mm at (5,5), (30,5), (105,40), (5,80), (105,80) |
| DUT / bias terminals | J5 = DUT (N_DUT, B); J6 = V_BIAS 0–20 V; 5.08 mm screw terminals, left edge |
| Console | J7 = 1×4 socket: TX (PA9), RX (PA10), GND, +3V3 |
| +5 V source | WeAct P3-4 (USB VBUS); the two relay coils draw ~56 mA, so USB must be present |

### Module header order (verified against the WeAct Long board-shape PDF)

The module exposes the QFN48 ring as two rows of 24. **Pin 1 of P1 and P2 is at
the right-hand end**; P3/P4 occupy the four leftmost positions.

Top row, left → right (P4 then P2):

```
P4-1 GND, P4-2 GND, P4-3 3V3, P4-4 3V3,
P2-20 PB11, P2-19 PB10, P2-18 VREF+, P2-17 PB2, P2-16 PB1, P2-15 PB0,
P2-14 PC4, P2-13 PA7, P2-12 PA6, P2-11 PA5, P2-10 PA4, P2-9 PA3,
P2-8 PA2, P2-7 PA1, P2-6 PA0, P2-5 NRST, P2-4 PC13, P2-3 VB,
P2-2 3V3, P2-1 GND
```

Bottom row, left → right (P3 then P1):

```
P3-1 GND, P3-2 GND, P3-3 5V, P3-4 5V,
P1-20 PB12, P1-19 PB13, P1-18 PB14, P1-17 PB15, P1-16 PC6, P1-15 PA8,
P1-14 PA9, P1-13 PA10, P1-12 PA11, P1-11 PA12, P1-10 PA15, P1-9 PC10,
P1-8 PC11, P1-7 PB3, P1-6 PB4, P1-5 PB5, P1-4 PB6, P1-3 PB7,
P1-2 PB9, P1-1 GND
```

The carrier sockets are placed so that J1-1 / J2-1 are at the right end and J3-1
/ J4-1 at the left end. The module plugs in from above; the carrier is a base
board, not a pass-through shield.

---

## 2. Rev B hardware changes (from the independent review)

The review found three real physics blockers and several robustness gaps. All
were fixed in the schematic and carried into the PCB.

| # | Change | Why |
|---|---|---|
| B1 | **D6 = 1N4148 in series between Q1 drain and N_DUT** (new net `Q1_D`) | AO3401's body diode (anode = drain) would clamp N_DUT at ~4 V and inject ~1.6 A into +3V3 during a 20 V precharge. The series diode blocks that path. The 1N4148 was chosen over a Schottky for its much lower reverse leakage (25 nA max at 20 V), which matters on the 4.8 MΩ bias node. |
| B2 | **C1 = 1000 µF / 35 V bipolar electrolytic**, non-polarised 16 mm radial footprint | The original polarised 25 V part was reverse-biased by up to 20 V during precharge (V_BIAS → N_DUT → C_block → V_cap held at 0). The CP symbol is retained for library continuity; its `+` mark is not used. |
| B3 | **R9 = 47 Ω, 2 W 2512** (was 10 Ω) | 10 Ω gave ~1.7 A peak through the AQY212s (0.55 A rated) and 170 mJ in R9. 47 Ω limits the peak to ~0.4 A; 5τ ≈ 250 ms, well inside the firmware's 2 s precharge hold. |
| M1 | Added **C11–C14 100 nF** (one per U1/U2/U3/U4 package) and **C15 10 µF** on +5 V | Only C8/C9 existed for four packages; no bulk on the relay rail. |
| M2 | Added **R29 10 k** DRIVE→GND, **R30/R31 100 k** MUXC_EN/MUXR_EN→+3V3 | PA0 (was PA8)/PB5/PB2 are high-Z at reset; the buffer input could float and either mux could be enabled. The pull-ups disable both muxes (ENABLE is active-low). |
| M3 | Added **J7 console header** (TX=PA9, RX=PA10, GND, +3V3) and removed the NC flags on J1-13/14 | The console pins were marked no-connect, leaving no host UART. |
| M5 | **C3 moved from OP1_OUT–B to OP1_OUT–VREF** | C3 was on the non-inverting input (positive feedback around the op-amp). The conventional compensation node is the inverting input (VREF). |
| m1 | J3-2/J3-3 and J4-2/J4-3 tied to GND/GND and 5V/3V3 | The module has two pins per power rail; leaving the duplicates open halves the current path. |
| m2 | **R32 1 k** in series between VCAP and J2-7/PA1 | Protects the ADC input and forms a small RC with the sample capacitor. New net `VCAP_ADC`. |
| m4 | **R33 1 M** across C1 | Bleeds the 1000 µF block when the instrument is idle; SSR3 alone could not discharge V_cap without also asserting SSR1. |
| M4 | R10 moved to 2512 (1 W pulse), R11 marked 0.5 W pulse | 187 mJ precharge/discharge energy; 0805 parts are marginal. |

### Pending schematic/PCB change — DRIVE moves from PA8 to PA0 (2026-11-12)

**Must be applied in KiCad before fabrication.** The C-mode drive / COMP1
output net (`DRIVE`, the SN74LVC1G34 U2 input with the R29 10 k pull-down) is
wired to **J1-15 = WeAct P1-15 = PA8**. That pin cannot carry COMP1_OUT on the
STM32G474CEU6: ST's GPIO data (`GPIO-STM32G47x_gpio_v1_0`) gives
**PA8 AF8 = COMP7_OUT**, and lists COMP1_OUT only on **PA0, PA6, PA11, PB8**
(PA0 = `GPIO_AF8_COMP1`). With the board as drawn the oscillator never starts.

| | Before | After |
|---|---|---|
| `DRIVE` net | {J1-15, U2 input, R29-1} | {**J2-6**, U2 input, R29-1} |
| J1-15 (PA8) | `DRIVE` | **no connect** (mark NC) |
| J2-6 (PA0) | unused | `DRIVE` |

- PA0 is free on the WeAct Long module (schematic V1.0: MCU pin 8 → P2-6 only,
  no pull resistor), and unused elsewhere in this design.
- Keep R29 (10 k to GND) at U2's input; PA0 is high-Z at reset like PA8 was.
- Route J2-6 → U2 input on the top side, away from `V_cap`/`N_DUT`: it is a
  fast 0–3.3 V square wave in oscillator mode.
- No part is added or removed; only one net moves.
- Firmware already uses PA0 (`PIN_DRIVE` in `include/config.h`).
- After the change, re-run ERC/DRC and regenerate the fab outputs.

### Review findings checked and rejected

- **AQY212 package** — the review claimed the 4-pin symbol was an SOP-4
  AQY212S and that the DIP-6 AQY212 has output on 4/6. The Panasonic AQY212 is a
  **4-pin DIP** with 1 = LED+, 2 = LED−, 3/4 = output; the `DIP-4_W7.62mm`
  footprint is correct. The SOP-4 variant is the AQY212S and would need a
  different footprint.
- **G6S-2 pin numbering** — the official KiCad `Relay_DPDT_Omron_G6S-2`
  footprint has exactly eight pads numbered **1, 3, 4, 5, 8, 9, 10, 12**
  (coil 1/12). The symbol matches; there is no 8-pad 1..8 relay landing.
- **N_DUT TVS (M7)** — deliberately not added. A 24 V TVS on the 0–20 V bias
  node leaks ~µA, and through R12 = 4.8 MΩ that is volts of bias error. The DUT
  is a screw terminal, R12 limits fault current, and D6 already blocks the
  Q1 body-diode path. If ESD testing demands a clamp, use a low-leakage
  bidirectional part and verify the bias error.
- **Net rename V_BIAS/VBIAS (M6)** — not renamed. The two nets are visibly
  separated on the sheet and in the layout; renaming would churn the firmware
  docs for no electrical gain. Treat `V_BIAS` as the 0–20 V external bias and
  `VBIAS` as the 1.65 V TIA reference.
- **K2-without-K1 interlock (m7)** — not added in Rev B. The firmware orders
  the relays; a rework wire from Q3 emitter to Q2 collector is possible if the
  bench shows a sequencing hazard.

---

## 3. PCB layout

- **Placement**: module sockets along the top edge; C-mode chain (U2/U3, R1–R4,
  C1/C2, D1) immediately below the module; DUT/bias terminals on the left;
  relays K1/K2 and their drivers in the centre; TIA U1 and the VREF/VBIAS
  dividers on the right; SSRs U5–U7 and their pulse resistors along the bottom.
- **Grounding**: **B.Cu GND pour only.** The F.Cu pour was deliberately removed
  because a same-layer ground plane around N_DUT/B/VCAP would add stray
  capacitance and leakage to the pF-range measurement. The B.Cu plane is 1.6 mm
  away and is the reference for the digital/relay return currents.
- **High-impedance nodes** (`N_DUT`, `B`, `VCAP`, `C_BLOCK`, `TIA_OUT`,
  `RF_TOP`) use the `Analog` netclass (0.3 mm track, 0.3 mm clearance). Keep
  the DUT socket and C1 area clean; do not add flux or coatings across the
  N_DUT/B gap.
- **Relay keepout**: K1/K2 are through-hole G6S-2; the coil and contact pads are
  grouped so the contact wiring stays on the left of each relay.
- **Trace widths**: 0.25 mm default, 0.30 mm analog, 0.50 mm +3V3/+5V; the
  auto-router was allowed 0.20 mm minimum and the final board was widened to
  the 0.20 mm board minimum. Vias 0.8/0.4 mm.
- **Routing**: Freerouting 2.5.0 (jar, 20 passes, 0 unrouted, 0 violations),
  then imported and post-processed (thin tracks widened, right edge extended
  1 mm for edge clearance, B.Cu pour added, U5 local clearance 0.19 mm).
- **Silkscreen**: board title "ESWCap C+R REV B", "WeAct G474 Long", "DUT",
  "V_BIAS 0-20V", "USB >". Reference designators remain on the footprint silk;
  the 1×4/1×20 sockets butt end-to-end, so their outline lines overlap at the
  joints (cosmetic DRC warnings only).

---

## 4. Verification

### Schematic

- `kicad-cli sch erc --severity-all` → **0 errors, 0 warnings**.
- 77 components (73 physical + 4 PWR_FLAG), 56 named nets.
- Netlist member-by-member checked after the Rev B changes:
  - `Q1_D` = {D6-2, Q1-3}
  - `N_DUT` = {D6-1, J5-1, K1-9, R12-2, R20-1, R9-2, U7-4}
  - `VREF` = {C3-2, C5-1, J2-9, R17-2, R18-1}
  - `VCAP` = {C1-1, C2-1, D1-3, R1-4, R32-1, R33-1, U6-4}
  - `VCAP_ADC` = {J2-7, R32-2}
  - `UART_TX` = {J1-14, J7-1}; `UART_RX` = {J1-13, J7-2}
- No shorts, orphans, single-pin nets, or floating wires.

### PCB

- `kicad-cli pcb drc --severity-all` → **0 errors, 0 unconnected pads, 0
  footprint errors; 25 silkscreen-overlap warnings** (butting socket outlines
  and dense reference text; no copper issue).
- 78 footprints (73 electrical + 5 mounting holes), 820 track segments, 75
  vias, 1 B.Cu GND zone.
- Board bbox 111.1 × 85.1 mm (0.1 mm edge line width included).
- Every pad has a net; the netlist and PCB agree after the Rev B rebuild.

### Firmware

- All four PlatformIO environments build: `weact_g474ceu6`,
  `_debug`, `_stlink`, `_combined` (combined 93,188 B flash / 25,228 B RAM).
- Host tests: **21/21 pass**.
- `C_BLOCK_ELEC_F` updated to **1000 µF** for the Rev B bipolar part; measure the
  fitted capacitor and trim it (or set it via the calibration record).

---

## 5. Fabrication outputs

All under `/home/jaguar/Documents/KiCad/Project/fab/`:

| File | Contents |
|---|---|
| `Project-gerbers.zip` | F.Cu/B.Cu, paste, silkscreen, mask, Edge.Cuts, PTH/NPTH drill, gbrjob |
| `gerbers/` | individual Gerbers + `Project-PTH.drl`, `Project-NPTH.drl`, drill maps |
| `Project-pos.csv` | component position file (KiCad Y-up convention — **Y is negative** for top-side parts; flip sign for JLCPCB or use the JLCPCB CPL tool) |
| `Project-bom.csv` | grouped schematic BOM (includes PWR_FLAG) |
| `Project-bom-procurement.csv` | **orderable BOM**: PWR_FLAG removed, E96/nearest values, package and part notes |
| `Project-pcb-front.pdf`, `Project-pcb-back.pdf` | copper/silk plots |

### BOM notes

The schematic keeps the **measured** range values (98.9 Ω, 993.6 Ω, 98.6 kΩ,
1.01 MΩ) because the firmware calibrates against them. The procurement BOM maps
them to orderable E96 parts (100 Ω, 1 kΩ, 100 kΩ, 1 MΩ) and flags the 0.1 %
parts; after building, run `rcal` with the actual measured values. The 4.7 MΩ
and 10 MΩ 0.1 % parts are selected thin-film; check voltage coefficient and
availability. `C1` is the 35 V bipolar part — do not substitute a polarised
25 V cap.

---

## 6. Bring-up / bench checks

1. **Do not fit the module yet.** Check +3V3/+5V shorts, then apply 3.3 V with a
   current limit. Confirm +5V only when USB/VBUS is present.
2. **C1 polarity is irrelevant** (bipolar), but confirm the + pad is the V_cap
   side of the footprint; the schematic CP `+` is not used.
3. **Relay polarity/sequence**: K1 energised = R mode; K2 energised = Kelvin.
   Verify with the module unplugged and a bench supply on the coil rail.
4. **R9 precharge**: 47 Ω, ~0.4 A peak at 20 V, ~250 ms to 5τ. Confirm the
   firmware's 2 s hold and that SSR1/SSR3 do not overheat.
5. **D6 drop**: in R mode the force path loses ~0.7 V; the OPAMP1 loop should
   compensate. Check V_N compliance at the top range (4.7 MΩ).
6. **C3 loop stability**: step the DUT from 100 Ω to 1 MΩ and check for
   ringing/oscillation on B and TIA_OUT. C3 is now on VREF; if the loop is
   marginal, fit a series resistor with C3 or reduce it.
7. **TIA**: Rf 10 MΩ, Cf 10 pF → 1.59 kHz. Check the 1.65 V bias and the
   TIA_OUT rails in both C and R modes.
8. **Stray capacitance**: the B.Cu plane adds ~pF to the DUT node; run the open
   calibration with the DUT socket empty and with a known 10 pF/100 pF cap.
9. **Console**: J7 TX/RX/GND/3V3 at 115200. Confirm PA9/PA10 are not swapped.
10. **V_BIAS above 3.3 V**: only with D6 fitted and C1 bipolar. Start at 5 V,
    then 10 V, then 20 V, with the DUT socket covered.

---

## 7. Known limitations / deferred

- 25 silkscreen-overlap warnings; no electrical DRC errors. The 1×4/1×20 socket
  outlines butt at the module joints; hide/trim them if the assembler objects.
- No fiducials and no panelisation. Add three 1 mm fiducials if a PnP house
  requires them.
- The B.Cu GND plane is continuous under the analog section. If the pF-range
  repeatability is insufficient, add a plane cutout under N_DUT/B/VCAP and
  re-run the open calibration.
- No N_DUT TVS (leakage trade-off, §2).
- No K2-without-K1 hardware interlock.
- The schematic now carries a **REV B** note at the top (C1 bipolar, D6
  body-diode block, R9 47 Ω, C3 to VREF, console J7); the obsolete Rev A
  polarised-cap warning was removed. The C1 symbol remains `CP` for library
  continuity and still draws a `+`, but the note states that the `+` is not
  used; the PCB footprint is the non-polarised 16 mm radial.
- The position file uses KiCad's Y-up sign; convert before uploading a CPL.
