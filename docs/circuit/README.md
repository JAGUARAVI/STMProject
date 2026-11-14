# Circuit diagrams (Rev C.1)

`ESWCap-circuit-diagrams.pdf`: 7 A3 pages for evaluators.

| Sheet | Content |
|---|---|
| 1 | System overview, measurement modes (K1/K2 states), every amplifier/comparator and where it lives |
| 2 | C-mode path: COMP1/DAC3/TIM2/ADC1 inside the STM32, U2, U3 mux, R1–R4, C1 ‖ C2 ‖ R33, K1A/K1B |
| 3 | DUT bias (J6, R12), safety SSRs U5/U6/U7, DC-bias monitor (R38/R39, U10) |
| 4 | R-mode 4-wire Kelvin: Q1 + OPAMP1 current loop, U4 mux + R5–R8, U9/R20 → OPAMP3 (PGA), R40 → U1B |
| 5 | R-mode TIA: U1A + R16 ‖ C4, K2B, VBIAS, guard ring |
| 6 | Power, relay drivers, console J7, bias header J8, complete MCU pin map |
| 7 | The KiCad schematic itself (every wire), exported from `hardware/kicad/Project.kicad_sch` |

Sheets 1–6 are vector SVGs in `svg/`.

## How they are checked

`draw_circuit.py` reads the KiCad netlist and:

- labels every part from one table (reference + value from the netlist + part number),
- asserts every drawn connection (`net('FL', 'J5-4', 'K1-4', …)`) against the netlist,
  so a wire that does not exist on the board fails the build,
- fails if any netlist part is missing from every sheet.

A deliberately corrupted netlist (one pin moved) makes it fail with
`U1-2: drawn on FL, netlist says None`.

What the netlist check cannot see is the inside of a symbol (which op-amp
input is + or −, which way a diode points). Those are drawn through helpers
that take polarity by name (`opamp(...)` returns `plus` / `minus`), and were
reviewed by eye on every sheet.

## Regenerate

```bash
flatpak run --command=kicad-cli org.kicad.KiCad sch export netlist \
    --format kicadsexpr -o /tmp/eswcap.net hardware/kicad/Project.kicad_sch
python3 -m venv /tmp/sd && /tmp/sd/bin/pip install schemdraw==0.23
/tmp/sd/bin/python docs/circuit/draw_circuit.py /tmp/eswcap.net docs/circuit/svg
# PDF: rasterise each SVG at 220 dpi, img2pdf to A3 landscape, then
# pdfunite sheets.pdf hardware/fab/Project-schematic.pdf docs/circuit/ESWCap-circuit-diagrams.pdf
```

## Notes

- The STM32-internal configuration shown (OPAMP3 PGA mode, PC4 driving U10,
  PB15 as MUXR_EN, PB2 as the PGA reference) follows `docs/accuracy-plan.md`
  F1–F3. **The firmware has not been updated to match yet.**
- Part numbers for passives are given as value + tolerance; ICs, semiconductors,
  relays and SSRs carry manufacturer part numbers.
