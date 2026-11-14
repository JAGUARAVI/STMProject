"""Grid A* router for the last few connections (2 layers, vias), plus cleanup.

Obstacles = every other-net pad / track / via, inflated by clearance + half
track width (or via radius). Zone fills are ignored (they are refilled after).
"""
import heapq
import math
import os
import sys

import pcbnew

HOME = os.path.expanduser('~')
PCB = f'{HOME}/Desktop/CourseWare/STMProject/hardware/kicad/Project.kicad_pcb'
MM = pcbnew.FromMM
F, B = pcbnew.F_Cu, pcbnew.B_Cu
RES = 0.1
CLR = 0.32
W = 0.3
VIA_R = 0.4
VIA_COST = 25.0

# (net, start (x,y,layer|None), goal (x,y,layer|None)); layer None = pad on both
JOBS = [
    ('/GND', (44.52, 43.91, 'F'), 'MAIN'),                       # U1-4 -> GND via in the main ground
]
# stale autorouter copper to drop first: dangling VCAP chain and the VBIAS stubs
DROP = []

board = pcbnew.LoadBoard(PCB)


def mm(v):
    return round(pcbnew.ToMM(v), 2)


for t in list(board.GetTracks()):
    if t.Type() == pcbnew.PCB_VIA_T:
        p = t.GetPosition()
        if t.GetNetname() == '/VBIAS' and (mm(p.x), mm(p.y)) == (51.42, 49.53):
            print('drop duplicate VBIAS via', (mm(p.x), mm(p.y)))
            board.Delete(t)
        continue
    s = (mm(t.GetStart().x), mm(t.GetStart().y))
    if (t.GetNetname(), s) in DROP and not t.IsLocked():
        board.Delete(t)

# ---------------------------------------------------------------- obstacles ---
items = []   # (net, onF, onB, kind, geom)
for fp in board.GetFootprints():
    for p in fp.Pads():
        pos = p.GetPosition()
        bb = p.GetBoundingBox()
        if p.GetShape() == pcbnew.PAD_SHAPE_CIRCLE:
            items.append((p.GetNetname(), p.IsOnLayer(F), p.IsOnLayer(B), 'c',
                          (mm(pos.x), mm(pos.y), pcbnew.ToMM(p.GetSize().x) / 2)))
        else:
            items.append((p.GetNetname(), p.IsOnLayer(F), p.IsOnLayer(B), 'r',
                          (pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                           pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom()))))
gnd_vias = []
for t in board.GetTracks():
    if t.Type() == pcbnew.PCB_VIA_T:
        p = t.GetPosition()
        items.append((t.GetNetname(), True, True, 'c', (mm(p.x), mm(p.y), pcbnew.ToMM(t.GetWidth(F)) / 2)))
        if t.GetNetname() == '/GND':
            gnd_vias.append((mm(p.x), mm(p.y)))
    else:
        a, b = t.GetStart(), t.GetEnd()
        items.append((t.GetNetname(), t.GetLayer() == F, t.GetLayer() == B, 's',
                      (mm(a.x), mm(a.y), mm(b.x), mm(b.y), pcbnew.ToMM(t.GetWidth()) / 2)))


def dps(px, py, x0, y0, x1, y1):
    dx, dy = x1 - x0, y1 - y0
    L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - x0) * dx + (py - y0) * dy) / L))
    return math.hypot(px - (x0 + t * dx), py - (y0 + t * dy))


def dist(kind, g, x, y):
    if kind == 'c':
        return math.hypot(x - g[0], y - g[1]) - g[2]
    if kind == 'r':
        return math.hypot(max(g[0] - x, 0, x - g[2]), max(g[1] - y, 0, y - g[3]))
    return dps(x, y, *g[:4]) - g[4]


def build_grid(net, x0, y0, x1, y1):
    nx, ny = int((x1 - x0) / RES) + 1, int((y1 - y0) / RES) + 1
    blocked = [[bytearray(nx * ny), bytearray(nx * ny)], [bytearray(nx * ny), bytearray(nx * ny)]]
    # blocked[layer][0] = blocked for a track, [layer][1] = blocked for a via
    near = [it for it in items if it[0] != net]
    for li, onidx in ((0, 1), (1, 2)):
        rel = [it for it in near if it[onidx]]
        for it in rel:
            kind, g = it[3], it[4]
            if kind == 'c':
                bx0, by0, bx1, by1 = g[0] - g[2], g[1] - g[2], g[0] + g[2], g[1] + g[2]
            elif kind == 'r':
                bx0, by0, bx1, by1 = g
            else:
                bx0, by0 = min(g[0], g[2]) - g[4], min(g[1], g[3]) - g[4]
                bx1, by1 = max(g[0], g[2]) + g[4], max(g[1], g[3]) + g[4]
            m = CLR + VIA_R
            i0, i1 = max(0, int((bx0 - m - x0) / RES)), min(nx - 1, int((bx1 + m - x0) / RES) + 1)
            j0, j1 = max(0, int((by0 - m - y0) / RES)), min(ny - 1, int((by1 + m - y0) / RES) + 1)
            for j in range(j0, j1 + 1):
                y = y0 + j * RES
                for i in range(i0, i1 + 1):
                    d = dist(kind, g, x0 + i * RES, y)
                    k = j * nx + i
                    if d < CLR + W / 2:
                        blocked[li][0][k] = 1
                    if d < CLR + VIA_R:
                        blocked[li][1][k] = 1
    return blocked, nx, ny


def astar(net, start, goal_pts):
    sx, sy, sl = start
    xs = [sx] + [g[0] for g in goal_pts]
    ys = [sy] + [g[1] for g in goal_pts]
    x0, y0 = max(0.5, min(xs) - 6), max(0.5, min(ys) - 6)
    x1, y1 = min(110.5, max(xs) + 6), min(84.5, max(ys) + 6)
    blocked, nx, ny = build_grid(net, x0, y0, x1, y1)

    def cell(x, y):
        return int(round((x - x0) / RES)), int(round((y - y0) / RES))

    goals = {}
    for gx, gy, gl in goal_pts:
        gi, gj = cell(gx, gy)
        for li in ((0, 1) if gl is None else ((0,) if gl == 'F' else (1,))):
            goals[(gi, gj, li)] = True
    si, sj = cell(sx, sy)
    starts = (0, 1) if sl is None else ((0,) if sl == 'F' else (1,))
    # free the immediate pad/endpoint neighbourhood (own copper)
    rad = int(1.2 / RES)
    for (ci, cj) in [(si, sj)] + [(k[0], k[1]) for k in goals]:
        for li in (0, 1):
            for j in range(cj - rad, cj + rad + 1):
                for i in range(ci - rad, ci + rad + 1):
                    if 0 <= i < nx and 0 <= j < ny and math.hypot(i - ci, j - cj) * RES <= 0.75:
                        blocked[li][0][j * nx + i] = 0

    # no via-in-pad: block vias within 1.0 mm of SMD start/goal points
    vr = int(1.0 / RES)
    smd_pts = ([(si, sj)] if sl is not None else []) + [(k[0], k[1]) for k in goals if len(goal_pts) == 1 and goal_pts[0][2] is not None]
    for (ci, cj) in smd_pts:
        for li in (0, 1):
            for j in range(cj - vr, cj + vr + 1):
                for i in range(ci - vr, ci + vr + 1):
                    if 0 <= i < nx and 0 <= j < ny:
                        blocked[li][1][j * nx + i] = 1

    def h(i, j):
        return min(math.hypot(i - g[0], j - g[1]) for g in goals) * RES

    openq, came, gs = [], {}, {}
    for li in starts:
        s = (si, sj, li)
        gs[s] = 0.0
        heapq.heappush(openq, (h(si, sj), s))
    moves = [(1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)]
    while openq:
        _, cur = heapq.heappop(openq)
        if cur in goals:
            path = [cur]
            while cur in came:
                cur = came[cur]
                path.append(cur)
            return path[::-1], (x0, y0)
        i, j, li = cur
        g0 = gs[cur]
        for di, dj in moves:
            ni, nj = i + di, j + dj
            if not (0 <= ni < nx and 0 <= nj < ny) or blocked[li][0][nj * nx + ni]:
                continue
            nxt = (ni, nj, li)
            ng = g0 + math.hypot(di, dj) * RES
            if ng < gs.get(nxt, 1e18):
                gs[nxt], came[nxt] = ng, cur
                heapq.heappush(openq, (ng + h(ni, nj), nxt))
        k = j * nx + i
        if not blocked[0][1][k] and not blocked[1][1][k]:
            nxt = (i, j, 1 - li)
            ng = g0 + VIA_COST * RES
            if ng < gs.get(nxt, 1e18):
                gs[nxt], came[nxt] = ng, cur
                heapq.heappush(openq, (ng + h(i, j), nxt))
    return None, None


def emit(net, path, origin):
    ni = board.FindNet(net)
    x0, y0 = origin
    pts = [(x0 + i * RES, y0 + j * RES, li) for i, j, li in path]
    segs, vias = 0, 0
    run = [pts[0]]
    for p in pts[1:]:
        if p[2] != run[-1][2]:
            flush(ni, run)
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pcbnew.VECTOR2I(MM(p[0]), MM(p[1])))
            v.SetWidth(MM(2 * VIA_R))
            v.SetDrill(MM(0.4))
            v.SetNet(ni)
            board.Add(v)
            vias += 1
            run = [p]
        else:
            run.append(p)
    flush(ni, run)
    return vias


def flush(ni, run):
    if len(run) < 2:
        return
    # collapse collinear points
    keep = [run[0]]
    for a, b in zip(run[1:], run[2:] + [None]):
        if b is None:
            keep.append(a)
            break
        d1 = (round(a[0] - keep[-1][0], 3), round(a[1] - keep[-1][1], 3))
        d2 = (round(b[0] - a[0], 3), round(b[1] - a[1], 3))
        if (d1[0] * d2[1] - d1[1] * d2[0]) != 0:
            keep.append(a)
    for a, b in zip(keep, keep[1:]):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I(MM(a[0]), MM(a[1])))
        t.SetEnd(pcbnew.VECTOR2I(MM(b[0]), MM(b[1])))
        t.SetWidth(MM(W))
        t.SetLayer(F if a[2] == 0 else B)
        t.SetNet(ni)
        board.Add(t)
        items.append((ni.GetNetname(), a[2] == 0, a[2] == 1, 's', (a[0], a[1], b[0], b[1], W / 2)))


def main_ground_vias():
    pieces = []
    for z in board.Zones():
        if z.GetNetname() != '/GND' or z.GetIsRuleArea():
            continue
        fp = z.GetFilledPolysList(z.GetLayer())
        for i in range(fp.OutlineCount()):
            pieces.append((fp, i, abs(fp.Outline(i).Area())))
    par = list(range(len(pieces)))

    def f(a):
        while par[a] != a:
            par[a] = par[par[a]]
            a = par[a]
        return a
    vias = [t for t in board.GetTracks() if t.Type() == pcbnew.PCB_VIA_T and t.GetNetname() == '/GND']
    pts = [t.GetPosition() for t in vias]
    pts += [p.GetPosition() for fpr in board.GetFootprints() for p in fpr.Pads()
            if p.GetNetname() == '/GND' and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH]
    hit_of = []
    for pt in pts:
        hits = [k for k, (fp, i, a) in enumerate(pieces) if fp.Outline(i).PointInside(pt, MM(0.45))]
        for h in hits[1:]:
            par[f(h)] = f(hits[0])
        hit_of.append(hits)
    area = {}
    for k, (fp, i, a) in enumerate(pieces):
        area[f(k)] = area.get(f(k), 0) + a
    main = max(area, key=area.get)
    return [(mm(t.GetPosition().x), mm(t.GetPosition().y)) for t, hits in zip(vias, hit_of)
            if hits and f(hits[0]) == main]


ok = True
for net, start, goal in JOBS:
    sl = None if start[2] is None else start[2]
    if goal == 'MAIN':
        main = main_ground_vias()
        near = sorted(main, key=lambda v: math.hypot(v[0] - start[0], v[1] - start[1]))[:8]
        goal_pts = [(v[0], v[1], None) for v in near]
    elif goal is None:
        near = sorted(gnd_vias, key=lambda v: math.hypot(v[0] - start[0], v[1] - start[1]))[1:9]
        goal_pts = [(v[0], v[1], None) for v in near]
    else:
        goal_pts = [goal]
    path, origin = astar(net, (start[0], start[1], sl), goal_pts)
    if path is None:
        print('NO PATH', net, start, goal)
        ok = False
        continue
    v = emit(net, path, origin)
    print(f'routed {net} {start[:2]} -> {path[-1][:2] if goal is None else goal[:2]}: {len(path)} cells, {v} vias')
board.Save(PCB)
print('saved' if ok else 'saved (some jobs failed)')
