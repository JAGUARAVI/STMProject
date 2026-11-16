# Breadboard layout (Rev C.1, five 830-point boards)

| File | Use it for |
|---|---|
| `ESWCap-breadboard-layout.pdf` | **Start here.** Rules, parts to prepare, build order, parts and wires board by board with tick boxes, then the drawings: one overview page and one page per board |
| `ESWCap-breadboard-parts.csv` | Every pin of every part with the hole it goes in |
| `ESWCap-breadboard-wires.csv` | Every wire (W1, W2, ...) with both holes and a suggested length |
| `svg/` | The drawings as SVG (zoomable) |

Boards sit side by side, long edges touching, B1 on the left. `B3:d10` means board 3, column d, row 10.
All + rails are +3V3 and all − rails are GND.

| Board | Holds |
|---|---|
| B1 | DUT terminal J5, V_BIAS terminal J6, bias and discharge SSRs, relay K1 and its driver |
| B2 | C-mode: drive buffer U2, range mux U3, range resistors, C_block (C1) |
| B3 | Sense divider and its SSRs, Kelvin force (Q1), op-amp U1, relay K2 and its driver |
| B4 | WeAct STM32G474 module, R-mux U4, reference divider |
| B5 | Bias-control header J8 and console header J7 |

A solderless breadboard leaks and adds stray capacitance, so the picofarad ranges and resistances above
about 10 MΩ will read wrong. Use this build to check that the circuit works; use perfboard or the PCB for
accuracy (see `docs/build/`).

## How it is generated and checked

```bash
# netlist exported from the schematic:
#   kicad-cli sch export netlist -o Project.net hardware/kicad/Project.kicad_sch
python3 docs/breadboard/make_breadboard.py Project.net docs/breadboard
```

`bb_layout.py` holds the position of every part. `bb_engine.py` models the boards (half-rows and rails),
routes the wires and refuses to write anything unless the half-rows plus wires reproduce every net of the
schematic exactly: no shorts, no missing connection, no hole used twice, nothing under a part body, and
unconnected pins alone on their half-row. `bb_draw.py` and `bb_tables.py` produce the drawings and lists.

Requires ImageMagick (with librsvg), img2pdf, poppler-utils (pdfunite) and LibreOffice.
