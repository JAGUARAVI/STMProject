"""Generate the breadboard build pack from the schematic netlist.

    python3 docs/breadboard/make_breadboard.py <kicad-netlist.net> docs/breadboard

Reads the netlist, places every part (bb_layout.py), routes the jumper wires
(bb_engine.py) and refuses to write anything unless the breadboard strips plus
wires reproduce every net of the schematic exactly, with no shorts.
"""
import csv
import math
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bb_engine import *          # noqa
import bb_layout                 # noqa
import bb_variant                # noqa
from bb_draw import *            # noqa
import bb_draw                   # noqa

NETLIST, OUT = sys.argv[1], sys.argv[2]
NB = 5
value, pinnet, pinfn = parse_netlist(NETLIST)
bb_variant.apply(value, pinnet, pinfn)          # parts actually fitted (see bb_variant.py)
L = Layout(NB, value, pinnet, pinfn)
bb_layout.place(L)
if not L.route():
    for e in L.errors:
        print('ERROR', e)
    sys.exit(1)

# ---------------------------------------------------------------- wires ---
def wclass(j):
    a, b = j['a'], j['b']
    if is_rail(a) and is_rail(b):
        return 0                                   # rail link
    if a[0] != b[0]:
        return 3                                   # board to board
    if is_rail(a) or is_rail(b):
        return 1                                   # strip to rail
    return 2                                       # local signal


def wkey(j):
    c = wclass(j)
    a, b = j['a'], j['b']
    if c == 0:
        return (0, 0, a[0], ly(a))
    if c == 3:
        return (9, 0, min(a[0], b[0]), max(a[0], b[0]), j['net'])
    s = a if not is_rail(a) else b
    return (1, s[0], c, ly(s), lx(s))


wires = sorted(L.jumpers, key=wkey)
MODX0, MODX1 = A_OFF + 3, A_OFF + 9


def beside(h):
    """-1 / +1 when the hole sits left / right of the module, 0 otherwise"""
    if h[0] != MOD_B or ly(h) > MOD_ROWS + 0.5:
        return 0
    return -1 if lx(h) < A_OFF + 5.5 else 1


def detour(h, o):
    """the wire end at h must go round the bottom end of the module to reach o"""
    s = beside(h)
    if s == 0:
        return False
    if o[0] != MOD_B:
        return s != (-1 if o[0] < MOD_B else 1)
    if beside(o):
        return beside(o) != s
    t = (MOD_ROWS + 0.6 - ly(h)) / (ly(o) - ly(h))
    x = lx(h) + t * (lx(o) - lx(h))
    return x < MODX1 + 0.4 if s > 0 else x > MODX0 - 0.4


ends = {-1: [], 1: []}
lanes = []
for j in wires:
    j['det'] = (detour(j['a'], j['b']), detour(j['b'], j['a']))
    for h, d in zip((j['a'], j['b']), j['det']):
        if d:
            ends[beside(h)].append((ly(h), id(j), h))
    if any(j['det']):
        lanes.append(j)
chan = {}
for s in (-1, 1):
    for k, (_, jid, h) in enumerate(sorted(ends[s], key=lambda e: -e[0])):
        chan[(jid, h)] = (A_OFF + 11.75 + 0.2 * k) if s > 0 else (A_OFF - 0.75 - 0.2 * k)


def lane_key(j):
    rs = [ly(h) for h, d in zip((j['a'], j['b']), j['det']) if d and beside(h) > 0]
    ls = [ly(h) for h, d in zip((j['a'], j['b']), j['det']) if d and beside(h) < 0]
    return (0, -max(rs)) if rs else (1, -max(ls))


for i, j in enumerate(sorted(lanes, key=lane_key)):
    j['lane'] = MOD_ROWS + 1.0 + 0.38 * i


def path(j, gx):
    a, b = j['a'], j['b']
    pa, pb = (gx(a[0]) + lx(a), ly(a)), (gx(b[0]) + lx(b), ly(b))
    if 'lane' not in j:
        return [pa, pb]
    yy, g4 = j['lane'], gx(MOD_B)
    da, db = j['det']
    pts = [pa]
    if da:
        ca = g4 + chan[(id(j), a)]
        pts += [(ca, pa[1]), (ca, yy)]
    elif a[0] != MOD_B:
        pts.append((g4 + (0 if a[0] < MOD_B else BW), yy))
    if db:
        cb = g4 + chan[(id(j), b)]
        pts += [(cb, yy), (cb, pb[1])]
    elif b[0] != MOD_B:
        pts.append((g4 + (0 if b[0] < MOD_B else BW), yy))
    pts.append(pb)
    return pts


GX = lambda b: (b - 1) * BW
for n, j in enumerate(wires, 1):
    j['no'] = n
    j['mm'] = path_len(path(j, GX)) * 2.54
    j['cls'] = wclass(j)


def cut(j):
    if j['mm'] < 4:
        return 'bare link, 1 row'
    if j['mm'] > 95:
        return '20 cm jumper lead'
    return f"{int(math.ceil((j['mm'] + 16) / 5.0) * 5)} mm"


# ------------------------------------------------------------- drawings ---
ML, MR, MT = 14.5, 14.5, 4.2


def wire_label(g, x, y, j, size=0.42):
    w = 0.42 + 0.3 * len(str(j['no']))
    c, g.cur = g.cur, 3
    g.rect(x - w / 2, y - 0.33, w, 0.66, '#fff', wire_color(j['net']), 0.06, 0.2)
    g.text(x, y, j['no'], size, '#111', bold=True)
    g.cur = c


def draw_wire(g, j, pts, label_at=None, op=0.9):
    col = wire_color(j['net'])
    c, g.cur = g.cur, 1
    g.line(pts, '#fff', 0.36, 0.9)
    g.line(pts, col, 0.24, op)
    for p in (pts[0], pts[-1]):
        g.circ(p[0], p[1], 0.24, col, '#fff', 0.04)
    g.cur = c


def mid(pts, f=0.5):
    tot = path_len(pts)
    acc = 0
    for i in range(len(pts) - 1):
        d = math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1])
        if acc + d >= tot * f and d > 0:
            t = (tot * f - acc) / d
            return (pts[i][0] + t * (pts[i + 1][0] - pts[i][0]), pts[i][1] + t * (pts[i + 1][1] - pts[i][1]))
        acc += d
    return pts[-1]


def board_page(b):
    W, Hh = ML + BW + MR, MT + ROWS + 2.6
    g = Svg(W, Hh)
    g.rect(0, 0, W, Hh, '#fff')
    g.text(W / 2, 1.0, f'Breadboard B{b} of {NB}: {bb_layout.BOARD_TITLE[b]}', 1.0, '#111', bold=True)
    g.text(W / 2, 2.2, 'Left / right columns of text = the net on that half-row.  [Wn > Bx:hole] = wire n leaves for another board.  '
           'Yellow pad = pin 1.', 0.5, '#374151')
    oy = MT
    g.textlayer = 2
    draw_board(g, L, b, ML, oy)
    g.textlayer = None
    gx = lambda bb: ML + (bb - b) * BW
    # margin labels
    notes = {}
    for j in wires:
        for h, o in ((j['a'], j['b']), (j['b'], j['a'])):
            if h[0] == b and o[0] != b and not is_rail(h):
                dest = f'B{o[0]}:{o[1]}{o[2]}' if not is_rail(o) else f'B{o[0]} {o[1]}'
                notes.setdefault(node(h), []).append(f'W{j["no"]} > {dest}')
    for nd, net in sorted(L.node_net.items(), key=lambda kv: str(kv[0])):
        if nd[0] != b or len(nd) == 2:
            continue
        nc = net.startswith('unconnected')
        col = '#9ca3af' if nc else '#b91c1c' if net == '+3V3' else '#c2410c' if net == '+5V' else '#374151' if net == 'GND' else '#1e3a8a'
        name = 'no connection: keep empty' if nc else net
        extra = '   '.join(f'[{t}]' for t in notes.get(nd, []))
        if nd[1] == 'L':
            g.text(ML - 0.5, oy + nd[2], (extra + '   ' if extra else '') + name, 0.5, col, anchor='end', bold=not nc)
        else:
            g.text(ML + BW + 0.5, oy + nd[2], name + ('   ' + extra if extra else ''), 0.5, col, anchor='start', bold=not nc)
    for nd, why in L.keepout.items():
        if nd[0] == b:
            g.text(ML + BW + 0.5 if nd[1] == 'R' else ML - 0.5, oy + nd[2], f'{why}: keep empty', 0.5, '#9ca3af',
                   anchor='start' if nd[1] == 'R' else 'end')
    # wires
    for j in wires:
        a, c = j['a'], j['b']
        if a[0] != b and c[0] != b:
            continue
        if a[0] == b and c[0] == b:
            pts = [(x, y + oy) for x, y in path(j, gx)]
            draw_wire(g, j, pts)
            if j['cls'] == 1:
                r = a if is_rail(a) else c
                x = ML + (0.6 if r[1][0] == 'L' else BW - 0.6)
                wire_label(g, x, oy + ly(r), j)
            else:
                m = mid(pts, 0.5)
                wire_label(g, m[0], m[1], j)
        else:
            full = [(x, y + oy) for x, y in path(j, gx)]
            if a[0] != b:
                full = full[::-1]
            lo, hi = ML, ML + BW
            pts = [full[0]]
            for p in full[1:]:
                q = pts[-1]
                if lo < p[0] < hi:
                    pts.append(p)
                    continue
                ex = lo if p[0] <= lo else hi
                pts.append((ex, q[1]))                 # leave horizontally at this height
                break
            draw_wire(g, j, pts, op=0.8)
            e = pts[-1]
            dx = 0.9 if e[0] == lo else -0.9
            g.add(f'<path d="M {e[0] * S:.1f} {e[1] * S:.1f} l {dx * 0.5 * S:.1f} {-0.35 * S:.1f} l 0 {0.7 * S:.1f} Z" fill="{wire_color(j["net"])}"/>')
            wire_label(g, e[0] + dx, e[1], j)
    return g.out()


def overview():
    W, Hh = NB * BW + 2, ROWS + 7
    g = Svg(W, Hh)
    g.rect(0, 0, W, Hh, '#fff')
    g.text(W / 2, 1.0, 'ESWCap Rev C.1 on five 830-point breadboards: overview (long edges touching, B1 on the left)', 1.0, '#111', bold=True)
    oy = 5.0
    for b in range(1, NB + 1):
        g.text(1 + (b - 0.5) * BW, 2.6, f'B{b}', 1.1, '#111', bold=True)
        g.text(1 + (b - 0.5) * BW, 3.7, bb_layout.BOARD_TITLE[b], 0.55, '#374151')
        g.textlayer = 2
        draw_board(g, L, b, 1 + (b - 1) * BW, oy)
        g.textlayer = None
    gx = lambda bb: 1 + (bb - 1) * BW
    for j in wires:
        pts = [(x, y + oy) for x, y in path(j, gx)]
        draw_wire(g, j, pts, op=0.85)
    for j in wires:
        if j['cls'] in (2, 3):
            pts = [(x, y + oy) for x, y in path(j, gx)]
            m = mid(pts, 0.5)
            wire_label(g, m[0], m[1], j, 0.4)
    return g.out()


os.makedirs(os.path.join(OUT, 'svg'), exist_ok=True)
tmp = tempfile.mkdtemp()
pngs = []
pages = [('overview', overview(), 150)] + [(f'board-B{b}', board_page(b), 260) for b in range(1, NB + 1)]
for name, svg, dens in pages:
    sp = os.path.join(OUT, 'svg', name + '.svg')
    open(sp, 'w').write(svg)
    pp = os.path.join(tmp, name + '.png')
    subprocess.run(['magick', '-density', str(dens), sp, '-background', 'white', '-flatten', '-alpha', 'remove', pp], check=True)
    pngs.append(pp)
if '--png' in sys.argv:
    import shutil
    for p in pngs:
        shutil.copy(p, sys.argv[sys.argv.index('--png') + 1])
subprocess.run(['img2pdf', '--pagesize', 'A4^T', '--fit', 'into', pngs[0], '-o', os.path.join(tmp, 'p0.pdf')], check=True)
subprocess.run(['img2pdf', '--pagesize', 'A4', '--fit', 'into', *pngs[1:], '-o', os.path.join(tmp, 'p1.pdf')], check=True)

# ---------------------------------------------------------------- lists ---
import bb_tables                 # noqa
bb_tables.write(L, wires, cut, OUT, tmp, NB)
print(f'verified: {len(L.parts)} parts, {len(wires)} wires, {NB} boards')
