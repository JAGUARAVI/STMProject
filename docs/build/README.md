# Build pack (Rev C.1)

| File | Use it for |
|---|---|
| `ESWCap-build-guide.pdf` | **Start here.** Safety + firmware warning, parts list, PCB soldering order and orientation, hand-wiring rules and through-hole substitutes, bring-up tests, numbered wire list with tick boxes. Appendix A = PCB placement map, Appendix B = wiring map pages |
| `ESWCap-wiring-map.drawio` | Editable wiring map (open in draw.io / diagrams.net). Parts placed as on the PCB; every wire drawn and numbered; one **layer per circuit block** (Extras ▸ Layers) |
| `ESWCap-wiring-map.pdf` | Printable wiring map: overview + one page per block |
| `ESWCap-wire-list.csv` | The same wires as a spreadsheet checklist |

**Before powering:** the current firmware drives PB2 and PB15, which Rev C.1 rewired
(VBUF and MUXR_EN). Flash the firmware with the pin-map update (`docs/accuracy-plan.md`
F1) before plugging the WeAct module into an assembled board.

## How it is generated and checked

`make_build_docs.py` reads the routed PCB (part positions, pad coordinates, pad nets)
and the schematic netlist. It refuses to write anything unless every pad net on the PCB
matches the schematic, then builds each net's wires as a minimum spanning tree over the
real pad positions (GND and +3V3 become rails) and checks that the wires + rails
reproduce every net exactly.

```bash
# board.json: exported from the PCB by hardware/scripts/export_board_json.py
python3 docs/build/make_build_docs.py board.json Project.net placement.png docs/build
```

Requires ImageMagick, img2pdf, poppler-utils (pdfunite) and LibreOffice (for the guide PDF).
