"""Hand-route the accuracy-critical analog nets, lock them, and re-export the DSN.

Every segment is checked against all other-net pads (pad bounding boxes, so
conservative) and against the other planned segments before the board is
written; DRC then confirms with exact shapes.
"""
import math
import os
import sys

import pcbnew

HOME = os.path.expanduser('~')
PCB = f'{HOME}/Desktop/CourseWare/STMProject/hardware/kicad/Project.kicad_pcb'
DSN = f'{HOME}/eswcap-pcbwork/Project.dsn'
MM = pcbnew.FromMM
F, B = pcbnew.F_Cu, pcbnew.B_Cu
W = 0.3          # Analog track width
CLR = 0.3        # Analog clearance
VIA_R = 0.4      # 0.8 mm via

# net: list of (layer, [points]) polylines, plus ('via', (x, y))
ROUTES = {
    '/SH_DIV': [
        (F, [(14.0, 33.09), (17.0, 33.05)]),
        (F, [(14.0, 39.91), (17.0, 39.95)]),
        (F, [(17.0, 33.05), (18.4, 33.05), (18.4, 46.54), (20.12, 46.54)]),
        (F, [(17.0, 39.95), (18.4, 39.95)]),
    ],
    '/SH': [
        (F, [(7.0, 38.08), (10.5, 38.08), (10.5, 34.91), (14.0, 34.91)]),
        (F, [(14.0, 34.91), (17.0, 34.95)]),
    ],
    '/FH': [
        (F, [(7.0, 33.0), (9.0, 31.4), (20.0, 31.4), (20.0, 32.65)]),
        (F, [(20.0, 32.65), (21.2, 33.8), (21.2, 39.38), (22.92, 39.38)]),
    ],
    '/Q1_D': [
        (F, [(16.94, 28.0), (20.0, 28.0), (20.0, 29.35)]),
    ],
    '/FL': [
        (B, [(7.0, 48.24), (7.7, 48.9), (29.9, 48.9), (29.9, 46.0)]),
        ('via', (29.9, 46.0)),
        (F, [(29.9, 46.0), (29.9, 39.38), (28.0, 39.38)]),
        (F, [(29.9, 44.5), (36.5, 44.5), (36.5, 39.38), (39.0, 39.38)]),
        (F, [(39.0, 39.38), (41.0, 39.38), (41.0, 37.43), (45.5, 37.45)]),
        (F, [(41.0, 39.38), (41.0, 41.37), (44.52, 41.37)]),
    ],
    '/RF_TOP': [
        (F, [(33.92, 41.92), (31.6, 41.92), (31.6, 34.6), (43.0, 34.6), (43.0, 35.59)]),
        (F, [(43.0, 34.6), (45.5, 34.6), (45.5, 35.55)]),
    ],
    '/SL': [
        (B, [(7.0, 43.16), (9.2, 41.0), (9.2, 35.3), (42.0, 35.3), (42.0, 49.5)]),
        ('via', (42.0, 49.5)),
        (F, [(42.0, 49.5), (46.09, 49.5), (46.09, 47.5)]),
    ],
    '/VCAP': [
        (F, [(31.1, 10.59), (29.6, 10.59), (29.6, 24.6), (32.44, 24.6), (32.44, 27.0), (34.59, 27.0)]),
        (F, [(31.1, 14.19), (29.6, 14.19)]),
        (F, [(31.1, 17.79), (29.6, 17.79)]),
        (F, [(31.1, 21.39), (29.6, 21.39)]),
        (F, [(29.6, 17.5), (24.5, 17.5)]),
    ],
    '/VN_IN': [
        (F, [(20.12, 51.0), (21.5, 51.0), (23.09, 52.0)]),
    ],
    '/VN_SENSE': [
        (F, [(24.91, 52.0), (27.05, 52.0)]),
    ],
    # P1 guard: stitch the B.Cu VBIAS guard islands (split by SL) to the F.Cu guard
    '/VBIAS': [
        ('via', (47.2, 38.6)),
        ('via', (40.6, 43.8)),
    ],
}

board = pcbnew.LoadBoard(PCB)
for t in list(board.GetTracks()):          # idempotent: drop earlier pre-routes
    board.Delete(t)

# ------------------------------------------------------------- validation ---
pads = []   # (net, layerset_has_F, has_B, x0, y0, x1, y1)
for fp in board.GetFootprints():
    for p in fp.Pads():
        bb = p.GetBoundingBox()
        pads.append((p.GetNetname(), p.IsOnLayer(F), p.IsOnLayer(B),
                     pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                     pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom()),
                     f'{fp.GetReference()}-{p.GetNumber()}'))


def seg_rect_dist(a, b, r):
    """Distance from segment a-b to axis-aligned rect r=(x0,y0,x1,y1)."""
    x0, y0, x1, y1 = r
    best = 1e9
    for i in range(41):
        t = i / 40
        px, py = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
        dx = max(x0 - px, 0, px - x1)
        dy = max(y0 - py, 0, py - y1)
        best = min(best, math.hypot(dx, dy))
    return best


def seg_seg_dist(a, b, c, d):
    best = 1e9
    for i in range(41):
        t = i / 40
        p = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
        for j in range(41):
            u = j / 40
            q = (c[0] + (d[0] - c[0]) * u, c[1] + (d[1] - c[1]) * u)
            best = min(best, math.hypot(p[0] - q[0], p[1] - q[1]))
    return best


segs = []   # (net, layer, a, b, halfwidth)
for net, items in ROUTES.items():
    for it in items:
        if it[0] == 'via':
            x, y = it[1]
            for lay in (F, B):
                segs.append((net, lay, (x, y), (x, y), VIA_R))
            continue
        lay, pts = it
        for a, b in zip(pts, pts[1:]):
            segs.append((net, lay, a, b, W / 2))

bad = 0
for net, lay, a, b, hw in segs:
    for pnet, onF, onB, x0, y0, x1, y1, name in pads:
        if pnet == net or not (onF if lay == F else onB):
            continue
        d = seg_rect_dist(a, b, (x0, y0, x1, y1)) - hw
        if d < CLR:
            bad += 1
            print(f'CLEARANCE {net} {"F" if lay == F else "B"} {a}->{b} vs {name} ({pnet}): {d:.2f} mm')
for i, (n1, l1, a1, b1, h1) in enumerate(segs):
    for n2, l2, a2, b2, h2 in segs[i + 1:]:
        if n1 != n2 and l1 == l2:
            d = seg_seg_dist(a1, b1, a2, b2) - h1 - h2
            if d < CLR:
                bad += 1
                print(f'CROSS {n1} {a1}->{b1} vs {n2} {a2}->{b2}: {d:.2f} mm')
if bad:
    print(f'{bad} problems: board NOT modified')
    sys.exit(1)

# ------------------------------------------------------------------ apply ---
n_tracks = n_vias = 0
for net, items in ROUTES.items():
    ni = board.FindNet(net)
    for it in items:
        if it[0] == 'via':
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pcbnew.VECTOR2I(MM(it[1][0]), MM(it[1][1])))
            v.SetWidth(MM(0.8))
            v.SetDrill(MM(0.4))
            v.SetNet(ni)
            v.SetLocked(True)
            board.Add(v)
            n_vias += 1
            continue
        lay, pts = it
        for a, b in zip(pts, pts[1:]):
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(pcbnew.VECTOR2I(MM(a[0]), MM(a[1])))
            t.SetEnd(pcbnew.VECTOR2I(MM(b[0]), MM(b[1])))
            t.SetWidth(MM(W))
            t.SetLayer(lay)
            t.SetNet(ni)
            t.SetLocked(True)
            board.Add(t)
            n_tracks += 1

board.Save(PCB)
ok = pcbnew.ExportSpecctraDSN(board, DSN)
print(f'pre-routed {len(ROUTES)} nets: {n_tracks} locked segments, {n_vias} vias; dsn {ok}')
