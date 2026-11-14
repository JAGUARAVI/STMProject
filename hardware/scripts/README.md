# PCB generation scripts (Rev C.1)

These scripts are the record of how `kicad/Project.kicad_pcb` was produced from
the schematic. They run with KiCad 10's own Python (KiCad installed as the
Flathub app):

```bash
flatpak run --command=python3 org.kicad.KiCad -u - < hardware/scripts/<script>.py
```

They read/write `hardware/kicad/Project.kicad_pcb` via
`~/Desktop/CourseWare/STMProject/...` and use `~/eswcap-pcbwork/` for the
netlist, DSN and SES scratch files. Adjust the path constants at the top of
each script if your checkout lives elsewhere.

| Step | Script | What it does |
|---|---|---|
| 1 | `build_board.py` | Fresh 111 × 85 mm outline; WeAct sockets J1–J4 and holes H1–H5 at their fixed positions; every other footprint loaded from `eswcap.pretty`, nets assigned from the KiCad netlist, symbol fields and UUID paths copied (schematic parity clean), placement by functional block |
| 2 | `route_critical.py` | Hand-routes and **locks** the accuracy-critical analog nets (FL, SL, SH, SH_DIV, FH local, RF_TOP, VCAP cluster, Q1_D, VN_IN). Every segment is clearance-checked before writing. Exports `Project.dsn` |
| 3 | Freerouting 2.1.0 | `java -jar freerouting-2.1.0.jar -de Project.dsn -do Project.ses -mp 50 --gui.enabled=false`, then `pcbnew.ImportSpecctraSES` (locked routes are preserved) |
| 4 | `zones2.py` | `ZMODE=route`: full B.Cu GND plane + P2 rule-area cutouts (DUT/sense, VCAP, TIA). Re-export DSN, second Freerouting pass for the leftovers. `ZMODE=final`: add the P1 VBIAS guard pour |
| 5 | `stitch.py` | F.Cu GND pour (same cutouts) and a 3 mm grid of GND stitching vias wherever clearance allows (~318 vias) |
| 6 | `astar_fix.py`, `astar_fix2.py` | Small two-layer A* grid router for the last connections (VCAP to U6, VBIAS to U1/R19, GND islands), with no via-in-pad |
| 7 | `silk.py` | Values to F.Fab, 0.8 mm references, J8-4 solid zone connection |

Zones are filled by `kicad-cli pcb drc --refill-zones --save-board` (the
`ZONE_FILLER` Python binding crashes in KiCad 10.0.8).

## Verification (2026-11-13)

```
kicad-cli pcb drc --severity-all --schematic-parity --refill-zones Project.kicad_pcb
  0 unconnected pads, 0 footprint (parity) errors,
  0 clearance / short / hole errors
  45 silkscreen warnings (reference text overlapping outlines/pads; cosmetic)
kicad-cli sch erc --severity-all Project.kicad_sch
  0 errors, 0 warnings
```
