"""Second zone pass, after the first autoroute.

mode 'route':  full-board GND plane + P2 rule-area cutouts, NO guard pour
               (so the autorouter routes VBIAS to U1-3 explicitly), drop dangling
               autorouter leftovers, export DSN.
mode 'final':  add the P1 VBIAS guard pours back.
"""
import os
import sys

import pcbnew

MODE = os.environ.get('ZMODE', 'route')
HOME = os.path.expanduser('~')
PCB = f'{HOME}/Desktop/CourseWare/STMProject/hardware/kicad/Project.kicad_pcb'
DSN = f'{HOME}/eswcap-pcbwork/Project.dsn'
MM = pcbnew.FromMM

CUT_DUT = (2.5, 29.0, 21.5, 56.0)
CUT_VCAP = (22.5, 9.5, 34.5, 29.5)
GUARD_TIA = (38.0, 33.5, 47.5, 44.5)

board = pcbnew.LoadBoard(PCB)


def poly(x0, y0, x1, y1):
    o = pcbnew.SHAPE_POLY_SET()
    o.NewOutline()
    for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
        o.Append(MM(x), MM(y))
    return o


def add_zone(net, layer, r, priority, clearance, name):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(board.FindNet(net))
    o = poly(*r)
    z.SetOutline(o)
    o.thisown = 0
    z.SetAssignedPriority(priority)
    z.SetLocalClearance(MM(clearance))
    z.SetMinThickness(MM(0.25))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetThermalReliefGap(MM(0.4))
    z.SetThermalReliefSpokeWidth(MM(0.4))
    z.SetZoneName(name)
    board.Add(z)


def add_keepout(r, name):
    z = pcbnew.ZONE(board)
    z.SetIsRuleArea(True)
    z.SetLayer(pcbnew.B_Cu)
    o = poly(*r)
    z.SetOutline(o)
    o.thisown = 0
    (z.SetDoNotAllowZoneFills if hasattr(z, 'SetDoNotAllowZoneFills') else z.SetDoNotAllowCopperPour)(True)
    z.SetDoNotAllowTracks(False)
    z.SetDoNotAllowVias(False)
    z.SetDoNotAllowPads(False)
    z.SetDoNotAllowFootprints(False)
    z.SetZoneName(name)
    board.Add(z)


if MODE == 'route':
    for z in list(board.Zones()):
        board.Delete(z)
    add_zone('/GND', pcbnew.B_Cu, (0.4, 0.4, 110.6, 84.6), 0, 0.3, 'GND plane')
    add_keepout(CUT_DUT, 'P2 cutout: DUT / SH divider')
    add_keepout(CUT_VCAP, 'P2 cutout: VCAP')
    add_keepout(GUARD_TIA, 'P2 cutout: TIA summing node')
    # autorouter leftovers flagged dangling by DRC
    for t in list(board.GetTracks()):
        if t.IsLocked():
            continue
        p = t.GetPosition() if t.Type() == pcbnew.PCB_VIA_T else t.GetStart()
        xy = (round(pcbnew.ToMM(p.x), 2), round(pcbnew.ToMM(p.y), 2))
        if t.GetNetname() == '/SH_DIV' or (t.Type() == pcbnew.PCB_VIA_T and xy == (33.38, 22.42)):
            print('delete', t.GetNetname(), xy)
            board.Delete(t)
    board.Save(PCB)
    print('dsn', pcbnew.ExportSpecctraDSN(board, DSN))
else:
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        add_zone('/VBIAS', layer, GUARD_TIA, 10, 0.35, 'VBIAS TIA guard (P1)')
    board.Save(PCB)
    print('guard pours added')
