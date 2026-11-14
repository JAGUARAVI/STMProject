"""Post-route ground repair: F.Cu GND pour, GND stitching-via grid, guard cleanup."""
import math
import os

import pcbnew

HOME = os.path.expanduser('~')
PCB = f'{HOME}/Desktop/CourseWare/STMProject/hardware/kicad/Project.kicad_pcb'
MM = pcbnew.FromMM
F, B = pcbnew.F_Cu, pcbnew.B_Cu
CUTS = [(2.5, 29.0, 21.5, 56.0), (22.5, 9.5, 34.5, 29.5), (38.0, 33.5, 47.5, 44.5)]
GUARD = CUTS[2]
VIA_D, VIA_DRILL, GAP = 0.8, 0.4, 0.35
STEP = 3.0

board = pcbnew.LoadBoard(PCB)
gnd = board.FindNet('/GND')


def poly(x0, y0, x1, y1):
    o = pcbnew.SHAPE_POLY_SET()
    o.NewOutline()
    for x, y in ((x0, y0), (x1, y0), (x1, y1), (x0, y1)):
        o.Append(MM(x), MM(y))
    return o


# --- cleanup: guard stitching vias, B.Cu guard, autorouter duplicates -------
for t in list(board.GetTracks()):
    n = t.GetNetname()
    if t.Type() == pcbnew.PCB_VIA_T and n == '/VBIAS' and t.IsLocked():
        board.Delete(t)          # only my two guard-stitching vias (locked); keep routed ones
    elif n == '/SH_DIV' and not t.IsLocked():
        board.Delete(t)
for z in list(board.Zones()):
    if z.GetNetname() == '/VBIAS' and z.GetLayer() == B:
        board.Delete(z)
    elif z.GetNetname() == '/VBIAS':
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)

# --- F.Cu GND pour + F.Cu cutouts --------------------------------------------
z = pcbnew.ZONE(board)
z.SetLayer(F)
z.SetNet(gnd)
o = poly(0.4, 0.4, 110.6, 84.6)
z.SetOutline(o)
o.thisown = 0
z.SetAssignedPriority(0)
z.SetLocalClearance(MM(0.3))
z.SetMinThickness(MM(0.25))
z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
z.SetThermalReliefGap(MM(0.4))
z.SetThermalReliefSpokeWidth(MM(0.4))
z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
z.SetZoneName('GND top pour')
board.Add(z)
for i, c in enumerate(CUTS[:2]):
    k = pcbnew.ZONE(board)
    k.SetIsRuleArea(True)
    k.SetLayer(F)
    o = poly(*c)
    k.SetOutline(o)
    o.thisown = 0
    k.SetDoNotAllowZoneFills(True)
    k.SetDoNotAllowTracks(False)
    k.SetDoNotAllowVias(False)
    k.SetDoNotAllowPads(False)
    k.SetDoNotAllowFootprints(False)
    k.SetZoneName(f'P2 top cutout {i}')
    board.Add(k)
for zz in board.Zones():
    if zz.GetNetname() == '/GND' and zz.GetLayer() == B:
        zz.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)

# --- stitching vias -------------------------------------------------------------
obst = []   # (kind, net, layers, geometry)
for fp in board.GetFootprints():
    for p in fp.Pads():
        bb = p.GetBoundingBox()
        obst.append(('rect', p.GetNetname(), (p.IsOnLayer(F), p.IsOnLayer(B)),
                     (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                      pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())), 0))
    # stay off parts' courtyards on the top side (no vias under SMD bodies)
    cy = fp.GetCourtyard(F)
    if cy.OutlineCount():
        bb = cy.BBox()
        obst.append(('rect', None, (True, False),
                     (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                      pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())), -GAP))
for t in board.GetTracks():
    if t.Type() == pcbnew.PCB_VIA_T:
        p = t.GetPosition()
        obst.append(('circ', t.GetNetname(), (True, True),
                     (pcbnew.ToMM(p.x), pcbnew.ToMM(p.y)), pcbnew.ToMM(t.GetWidth(F)) / 2))
    else:
        a, b = t.GetStart(), t.GetEnd()
        lay = t.GetLayer()
        obst.append(('seg', t.GetNetname(), (lay == F, lay == B),
                     (pcbnew.ToMM(a.x), pcbnew.ToMM(a.y), pcbnew.ToMM(b.x), pcbnew.ToMM(b.y)),
                     pcbnew.ToMM(t.GetWidth()) / 2))


def d_point_seg(px, py, x0, y0, x1, y1):
    dx, dy = x1 - x0, y1 - y0
    L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - x0) * dx + (py - y0) * dy) / L))
    return math.hypot(px - (x0 + t * dx), py - (y0 + t * dy))


def free(x, y):
    r = VIA_D / 2
    for kind, net, (onF, onB), g, w in obst:
        if net == '/GND' and kind == 'seg':
            continue             # same-net copper is fine, but keep hole spacing to GND vias/pads
        if kind == 'rect':
            x0, y0, x1, y1 = g
            d = math.hypot(max(x0 - x, 0, x - x1), max(y0 - y, 0, y - y1)) - r
            need = GAP if w >= 0 else 0.0
        elif kind == 'circ':
            d = math.hypot(x - g[0], y - g[1]) - r - w
            need = GAP
        else:
            d = d_point_seg(x, y, *g) - r - w
            need = GAP
        if d < need:
            return False
    return True


placed = 0
y = 2.5
while y < 83:
    x = 2.5
    while x < 109:
        inside_cut = any(c[0] - 1 <= x <= c[2] + 1 and c[1] - 1 <= y <= c[3] + 1 for c in CUTS)
        near_hole = any(math.hypot(x - hx, y - hy) < 4.2
                        for hx, hy in ((5, 5), (30, 5), (105, 40), (5, 80), (105, 80)))
        if not inside_cut and not near_hole and free(x, y):
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
            v.SetWidth(MM(VIA_D))
            v.SetDrill(MM(VIA_DRILL))
            v.SetNet(gnd)
            board.Add(v)
            obst.append(('circ', '/GND', (True, True), (x, y), VIA_D / 2))
            placed += 1
        x += STEP
    y += STEP

board.Save(PCB)
print('stitching vias placed:', placed)
