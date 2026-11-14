"""Silkscreen cleanup: values to F.Fab, compact references, J8-4 solid zone connection."""
import os

import pcbnew

HOME = os.path.expanduser('~')
PCB = f'{HOME}/Desktop/CourseWare/STMProject/hardware/kicad/Project.kicad_pcb'
MM = pcbnew.FromMM
board = pcbnew.LoadBoard(PCB)
for fp in board.GetFootprints():
    v = fp.Value()
    v.SetLayer(pcbnew.F_Fab)
    r = fp.Reference()
    if fp.GetReference().startswith('H'):
        r.SetVisible(False)
        continue
    r.SetTextSize(pcbnew.VECTOR2I(MM(0.8), MM(0.8)))
    r.SetTextThickness(MM(0.12))
    if fp.GetReference() == 'J8':
        for p in fp.Pads():
            if p.GetNumber() == '4':
                p.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
board.Save(PCB)
print('silk cleaned')
