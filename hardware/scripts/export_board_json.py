"""Export part positions and pad nets from the PCB for docs/build/make_build_docs.py.

    cd hardware/kicad
    flatpak run --command=python3 org.kicad.KiCad -u - < ../scripts/export_board_json.py
    # writes ~/eswcap-pcbwork/board.json
"""
import json
import os

import pcbnew

b = pcbnew.LoadBoard('Project.kicad_pcb')
mm = pcbnew.ToMM
out = []
for fp in b.GetFootprints():
    if fp.GetReference().startswith('H'):
        continue
    cy = fp.GetCourtyard(pcbnew.F_CrtYd)
    bb = cy.BBox() if cy.OutlineCount() else fp.GetBoundingBox(False)
    pads = [{'num': p.GetNumber(), 'net': p.GetNetname().lstrip('/'),
             'x': round(mm(p.GetPosition().x), 3), 'y': round(mm(p.GetPosition().y), 3),
             'tht': p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH} for p in fp.Pads()]
    out.append({'ref': fp.GetReference(), 'value': fp.GetValue(), 'fp': str(fp.GetFPID().GetLibItemName()),
                'x': round(mm(fp.GetPosition().x), 3), 'y': round(mm(fp.GetPosition().y), 3),
                'rot': fp.GetOrientationDegrees(), 'side': 'B' if fp.IsFlipped() else 'F',
                'bbox': [round(mm(bb.GetLeft()), 2), round(mm(bb.GetTop()), 2),
                         round(mm(bb.GetRight()), 2), round(mm(bb.GetBottom()), 2)],
                'pads': pads})
path = os.path.expanduser('~/eswcap-pcbwork/board.json')
os.makedirs(os.path.dirname(path), exist_ok=True)
json.dump(out, open(path, 'w'), indent=1)
print('parts', len(out), '->', path)
