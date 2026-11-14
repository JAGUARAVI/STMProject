"""Build hardware/kicad/Project.kicad_pcb from the WIP outline + current netlist.

Run with KiCad's Python:  flatpak run --command=python3 org.kicad.KiCad - < build_board.py
Paths are read from env-free constants below (flatpak sees $HOME).
"""
import os
import re
import sys

import pcbnew

HOME = os.path.expanduser('~')
REPO = f'{HOME}/Desktop/CourseWare/STMProject/hardware/kicad'
NETLIST = f'{HOME}/eswcap-pcbwork/after.net'
BASE = f'{REPO}/wip/Project-revC-routing-baseline.kicad_pcb'
OUT = f'{REPO}/Project.kicad_pcb'
LIB = f'{REPO}/eswcap.pretty'
MM = pcbnew.FromMM


# ---------------------------------------------------------------- netlist ---
def sexpr(text):
    tok = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text)
    stack, cur = [], []
    for t in tok:
        if t == '(':
            stack.append(cur)
            cur = []
        elif t == ')':
            done, cur = cur, stack.pop()
            cur.append(done)
        else:
            cur.append(t[1:-1] if t.startswith('"') else t)
    return cur[0]


def find(n, k):
    return [c for c in n if isinstance(c, list) and c and c[0] == k]


def val(n, k):
    f = find(n, k)
    return f[0][1] if f and len(f[0]) > 1 else None


root = sexpr(open(NETLIST).read())
COMPS, FIELDS, UUID = {}, {}, {}
for c in find(find(root, 'components')[0], 'comp'):
    ref = val(c, 'ref')
    COMPS[ref] = (val(c, 'value'), val(c, 'footprint'))
    UUID[ref] = [t for t in find(c, 'tstamps')][-1][1]
    FIELDS[ref] = {}
    for fl in find(c, 'fields'):
        for f in find(fl, 'field'):
            name = val(f, 'name')
            if name not in ('Footprint', 'Datasheet', 'Description') and len(f) > 2:
                FIELDS[ref][name] = f[2]
PADNET = {}
for n in find(find(root, 'nets')[0], 'net'):
    for x in find(n, 'node'):
        PADNET[(val(x, 'ref'), val(x, 'pin'))] = val(n, 'name')

# --------------------------------------------------------------- placement ---
# ref: (x_mm, y_mm, rotation_deg).  Origin = footprint origin (pad 1 for THT).
P = {
    # module sockets (fixed by the WeAct module) and holes keep WIP positions
    # --- C-mode (top-left) ---
    'C1': (24.5, 17.5, 180), 'C2': (14.0, 7.5, 0), 'R33': (20.0, 7.5, 0),
    'R1': (31.1, 11.5, 90), 'R2': (31.1, 15.1, 90), 'R3': (31.1, 18.7, 90), 'R4': (31.1, 22.3, 90),
    'U3': (37.0, 15.0, 0), 'C12': (37.0, 22.0, 0),
    'U2': (44.5, 9.0, 0), 'R29': (44.5, 13.0, 0), 'C11': (44.5, 16.0, 0), 'R30': (44.5, 19.0, 0),
    'D1': (31.5, 27.0, 0), 'R32': (35.5, 27.0, 0),
    'U6': (40.0, 26.0, 0), 'R23': (52.0, 27.0, 0), 'R11': (52.0, 30.0, 0),
    # --- DUT / bias terminals (left edge) ---
    'J5': (7.0, 33.0, -90), 'J6': (7.0, 58.0, -90),
    # --- force path ---
    'Q1': (16.0, 28.0, 0), 'D6': (20.0, 31.0, 90),
    'R13': (24.5, 28.0, 90), 'R14': (27.0, 28.0, 90), 'R15': (24.5, 31.5, 0),
    # --- SH divider / bias monitor and V_N selector (left column) ---
    'R38': (14.0, 34.0, 90), 'C21': (17.0, 34.0, 90),
    'R39': (14.0, 39.0, 90), 'C20': (17.0, 39.0, 90),
    'U10': (12.5, 44.0, 0), 'U9': (12.5, 51.0, 0),
    'R20': (24.0, 52.0, 0), 'C7': (28.0, 52.0, 0), 'D2': (28.0, 55.5, 0),
    'R41': (24.0, 55.5, 0), 'R43': (16.0, 57.5, 0),
    'R12': (12.5, 60.0, 90),
    # --- relays + drivers ---
    'K1': (28.0, 47.0, 180), 'K2': (39.0, 47.0, 180),
    'Q2': (33.0, 52.0, 0), 'R25': (33.0, 55.5, 0), 'R27': (36.0, 52.0, 90), 'D4': (36.0, 57.5, 90),
    'Q3': (40.0, 52.0, 0), 'R26': (40.0, 55.5, 0), 'R28': (43.0, 52.0, 90), 'D5': (43.0, 57.5, 90),
    'C10': (46.5, 52.0, 90), 'C15': (46.5, 56.5, 90),
    # --- TIA / SL buffer (U1) ---
    'U1': (47.0, 42.0, 0), 'R16': (43.0, 36.5, 90), 'C4': (45.5, 36.5, 90),
    'R40': (47.0, 47.5, 0), 'C13': (52.5, 42.0, 90), 'D3': (52.0, 47.5, 0),
    'R19': (52.5, 51.5, 90), 'R21': (55.0, 51.5, 90), 'C6': (57.5, 51.5, 90),
    # --- Kelvin sense mux + resistors ---
    'U4': (60.0, 38.0, 0), 'C14': (60.0, 30.5, 0), 'R31': (60.0, 46.0, 0),
    'R5': (66.0, 34.0, 0), 'R6': (66.0, 37.0, 0), 'R7': (66.0, 40.0, 0), 'R8': (66.0, 43.0, 0),
    # --- references near the module analog pins ---
    'R17': (84.0, 28.0, 90), 'R18': (87.0, 28.0, 90), 'C5': (90.0, 28.0, 90), 'C3': (81.0, 28.0, 90),
    'C8': (57.0, 26.0, 0), 'C9': (61.0, 26.0, 0),
    # --- safety SSR bank (bottom) ---
    'U5': (16.0, 66.0, 0), 'R9': (19.0, 74.0, 0), 'R22': (12.5, 70.0, 90),
    'U7': (28.0, 66.0, 0), 'R10': (31.0, 74.0, 0), 'R24': (39.0, 66.0, 90),
    # --- J8 bias header + filters (bottom right), console ---
    'J8': (76.0, 80.0, 0),
    'R34': (78.0, 74.0, 0), 'C16': (82.0, 74.0, 0), 'R36': (86.0, 74.0, 0),
    'C17': (90.0, 74.0, 0), 'R35': (94.0, 74.0, 0),
    'J7': (16.0, 82.0, 90),
}
KEEP = {'J1', 'J2', 'J3', 'J4', 'H1', 'H2', 'H3', 'H4', 'H5'}

# ------------------------------------------------------------------ build ---
board = pcbnew.BOARD()
board.SetCopperLayerCount(2)
outline = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_RECTANGLE)
outline.SetStart(pcbnew.VECTOR2I(MM(0), MM(0)))
outline.SetEnd(pcbnew.VECTOR2I(MM(111), MM(85)))
outline.SetLayer(pcbnew.Edge_Cuts)
outline.SetWidth(MM(0.1))
board.Add(outline)
# fixed by the WeAct Long module and the WIP mechanical layout (docs/pcb-design.md)
kept_pos = {
    'J1': ((107.5, 20.74), -90), 'J2': ((107.5, 5.5), -90),
    'J3': ((49.08, 20.74), 90), 'J4': ((49.08, 5.5), 90),
    'H1': ((5, 5), 0), 'H2': ((30, 5), 0), 'H3': ((105, 40), 0), 'H4': ((5, 80), 0), 'H5': ((105, 80), 0),
}
kept_pos = {k: (pcbnew.VECTOR2I(MM(x), MM(y)), r) for k, ((x, y), r) in kept_pos.items()}

nets = {}


def net(name):
    if name not in nets:
        ni = board.FindNet(name)
        if ni is None:
            ni = pcbnew.NETINFO_ITEM(board, name)
            board.Add(ni)
        nets[name] = ni
    return nets[name]


missing = []
for ref, (value, fpid) in sorted(COMPS.items()):
    if ref.startswith('FLG') or not fpid:
        continue
    lib, name = fpid.split(':')
    fp = pcbnew.FootprintLoad(LIB, name)
    assert fp is not None, (ref, fpid)
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    fp.SetReference(ref)
    fp.SetValue(value)
    fp.SetPath(pcbnew.KIID_PATH('/' + UUID[ref]))
    for k, v in FIELDS[ref].items():
        fp.SetField(k, v)
        fp.GetField(k).SetVisible(False)
    if ref in kept_pos:
        pos, rot = kept_pos[ref]
        fp.SetPosition(pos)
        fp.SetOrientationDegrees(rot)
    elif ref in P:
        x, y, r = P[ref]
        fp.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
        fp.SetOrientationDegrees(r)
    else:
        missing.append(ref)
    board.Add(fp)
    for pad in fp.Pads():
        n = PADNET.get((ref, pad.GetNumber()))
        if n:
            pad.SetNet(net(n))

# mounting holes come from the library too (unchanged positions)
for ref in ('H1', 'H2', 'H3', 'H4', 'H5'):
    fp = pcbnew.FootprintLoad(LIB, 'MountingHole_3.2mm_M3')
    fp.SetFPID(pcbnew.LIB_ID('eswcap', 'MountingHole_3.2mm_M3'))
    fp.SetReference(ref)
    fp.SetValue('M3')
    pos, rot = kept_pos[ref]
    fp.SetPosition(pos)
    fp.SetAttributes(fp.GetAttributes() | pcbnew.FP_BOARD_ONLY)
    board.Add(fp)

unused = sorted(set(P) - set(COMPS))
print('missing placement:', missing)
print('placement for refs not in netlist:', unused)

# silkscreen identity
for txt, x, y, size in [('ESWCap C+R  REV C.1', 60.0, 64.0, 1.5),
                        ('WeAct G474 Long', 78.0, 25.0, 1.0),
                        ('DUT  FH SH SL FL', 8.0, 26.5, 1.0),
                        ('V_BIAS 0-20V', 9.0, 54.0, 1.0),
                        ('USB POWER ONLY', 60.0, 67.0, 1.0)]:
    t = pcbnew.PCB_TEXT(board)
    t.SetText(txt)
    t.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    t.SetLayer(pcbnew.F_SilkS)
    t.SetTextSize(pcbnew.VECTOR2I(MM(size), MM(size)))
    t.SetTextThickness(MM(size * 0.15))
    board.Add(t)

board.Save(OUT)

# report pad extents of the edge parts so placement can be checked
for ref in ('J5', 'J6', 'K1', 'K2', 'C1', 'J7', 'J8'):
    fp = board.FindFootprintByReference(ref)
    pads = sorted((p.GetNumber(), round(pcbnew.ToMM(p.GetPosition().x), 2),
                   round(pcbnew.ToMM(p.GetPosition().y), 2)) for p in fp.Pads())
    cy = fp.GetCourtyard(pcbnew.F_CrtYd).BBox()
    print(ref, 'crtyd x %.1f..%.1f y %.1f..%.1f' % (pcbnew.ToMM(cy.GetLeft()), pcbnew.ToMM(cy.GetRight()),
                                                pcbnew.ToMM(cy.GetTop()), pcbnew.ToMM(cy.GetBottom())), pads)
print('saved', OUT, 'footprints', len(board.GetFootprints()), 'nets', board.GetNetCount())
