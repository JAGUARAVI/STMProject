"""SVG drawing of the breadboard layout (one board per page, plus an overview)."""
import math
from bb_engine import ROWS, BW, COLX, RAILX, RAILNET, A_OFF, is_rail, rail_row, node, hs

S = 20                                              # px per 0.1 inch
MOD_B, MOD_ROWS = 4, 24                             # module board and length
MCU = {'J2': 'GND 3V3 VB PC13 NRST PA0 PA1 PA2 PA3 PA4 PA5 PA6 PA7 PC4 PB0 PB1 PB2 VREF+ PB10 PB11'.split(),
       'J4': 'GND GND 3V3 3V3'.split(),
       'J1': 'GND PB9 PB7 PB6 PB5 PB4 PB3 PC11 PC10 PA15 PA12 PA11 PA10 PA9 PA8 PC6 PB15 PB14 PB13 PB12'.split(),
       'J3': 'GND GND 5V 5V'.split()}
PALETTE = ['#1d4ed8', '#059669', '#d97706', '#7c3aed', '#db2777', '#0891b2', '#65a30d', '#b45309', '#4f46e5',
           '#0d9488', '#c026d3', '#ea580c']


def wire_color(net):
    if net == '+3V3':
        return '#dc2626'
    if net == '+5V':
        return '#f97316'
    if net == 'GND':
        return '#111827'
    return PALETTE[sum(ord(c) * (i + 3) for i, c in enumerate(net)) % len(PALETTE)]


def lx(h):
    """x of a hole inside its own board"""
    return A_OFF + (RAILX[h[1]] if h[1] in RAILX else COLX[h[1]])


def ly(h):
    return float(rail_row(h[2]) if is_rail(h) else h[2])


def esc(s):
    return str(s).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


class Svg:
    def __init__(self, w, h):
        self.head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w * S:.0f}" height="{h * S:.0f}" '
                     f'viewBox="0 0 {w * S:.0f} {h * S:.0f}">')
        self.layers = {i: [] for i in range(5)}     # 0 board+parts, 1 wires, 2 part text, 3 wire numbers
        self.cur = 0
        self.textlayer = None

    def add(self, s, layer=None):
        self.layers[self.cur if layer is None else layer].append(s)

    def out(self):
        return '\n'.join([self.head] + [x for i in sorted(self.layers) for x in self.layers[i]] + ['</svg>'])

    def rect(self, x, y, w, h, fill, stroke='none', sw=0.05, rx=0, op=1):
        self.add(f'<rect x="{x * S:.1f}" y="{y * S:.1f}" width="{w * S:.1f}" height="{h * S:.1f}" rx="{rx * S:.1f}" '
                 f'fill="{fill}" stroke="{stroke}" stroke-width="{sw * S:.1f}" opacity="{op}"/>')

    def circ(self, x, y, r, fill, stroke='none', sw=0.05, op=1):
        self.add(f'<circle cx="{x * S:.1f}" cy="{y * S:.1f}" r="{r * S:.1f}" fill="{fill}" stroke="{stroke}" '
                 f'stroke-width="{sw * S:.1f}" opacity="{op}"/>')

    def line(self, pts, color, w, op=1, dash=None):
        d = ' '.join(f'{x * S:.1f},{y * S:.1f}' for x, y in pts)
        da = f' stroke-dasharray="{dash}"' if dash else ''
        self.add(f'<polyline points="{d}" fill="none" stroke="{color}" stroke-width="{w * S:.1f}" '
                 f'stroke-linecap="round" stroke-linejoin="round" opacity="{op}"{da}/>')

    def text(self, x, y, t, size=0.55, fill='#111', anchor='middle', rot=0, bold=False, halo=False):
        tr = f' transform="rotate({rot} {x * S:.1f} {y * S:.1f})"' if rot else ''
        st = ' stroke="#fff" stroke-width="3" paint-order="stroke"' if halo else ''
        self.add(layer=self.textlayer, s=f'<text x="{x * S:.1f}" y="{y * S:.1f}" font-size="{size * S:.1f}" fill="{fill}" text-anchor="{anchor}" '
                 f'dominant-baseline="central" font-family="DejaVu Sans, sans-serif"'
                 f'{" font-weight=\'bold\'" if bold else ""}{st}{tr}>{esc(t)}</text>')


def short_val(v):
    return v.replace(' 1%', '').replace(' 0.1%', '').replace('uF', 'µ').replace('nF', 'n').replace('pF', 'p')


def draw_board(g, L, b, ox, oy, detail=True):
    """draw board b with its top-left hole origin at (ox, oy) (units)"""
    X = lambda h: ox + lx(h)
    Y = lambda h: oy + ly(h)
    tl, g.textlayer = g.textlayer, 0
    g.rect(ox, oy - 0.2, BW, ROWS + 1.4, '#f8f6ef', '#b8b4a6', 0.06, 0.4)
    g.rect(ox + A_OFF + 4.9, oy + 0.3, 1.2, ROWS + 0.4, '#e2dfd3')
    for rl, x in RAILX.items():
        col = '#dc2626' if rl[1] == '+' else '#2563eb'
        g.line([(ox + A_OFF + x - 0.5, oy + 2.4), (ox + A_OFF + x - 0.5, oy + 61.6)], col, 0.07, 0.6)
        for r in range(3, 62):
            if (r - 3) % 6 != 5:
                g.circ(ox + A_OFF + x, oy + r, 0.13, '#9aa0a6')
        if detail:
            g.text(ox + A_OFF + x, oy + 1.5, rl[1], 0.9, col, bold=True)
            g.text(ox + A_OFF + x, oy + 62.6, rl[1], 0.9, col, bold=True)
    for c, x in COLX.items():
        for r in range(1, ROWS + 1):
            g.circ(ox + A_OFF + x, oy + r, 0.13, '#9aa0a6')
        if detail:
            g.text(ox + A_OFF + x, oy + 0.15, c, 0.6, '#374151', bold=True)
            g.text(ox + A_OFF + x, oy + ROWS + 0.85, c, 0.6, '#374151', bold=True)
    if detail:
        for r in range(1, ROWS + 1):
            big = r % 5 == 0 or r == 1
            for x in (A_OFF - 1.0, A_OFF + 5.5, A_OFF + 12.0):
                g.text(ox + x, oy + r, r, 0.5 if big else 0.4, '#111' if big else '#9ca3af', bold=big)

    g.textlayer = tl
    # ------------------------------------------------------------- parts
    for ref, P in L.parts.items():
        pins = P['pins']
        hl = list(pins.values())
        if hl[0][0] != b:
            continue
        k = P['kind']
        val = short_val(L.value.get(ref, ''))
        if k in ('res', 'cap', 'diode'):
            (x1, y1), (x2, y2) = (X(hl[0]), Y(hl[0])), (X(hl[1]), Y(hl[1]))
            d = math.hypot(x2 - x1, y2 - y1)
            ang = math.degrees(math.atan2(y2 - y1, x2 - x1))
            cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
            g.line([(x1, y1), (x2, y2)], '#6b7280', 0.13)
            g.circ(x1, y1, 0.17, '#6b7280')
            g.circ(x2, y2, 0.17, '#6b7280')
            bl = max(0.9, min(d - 0.7, 3.4)) if k != 'cap' else max(0.8, min(d - 0.5, 1.5))
            fill, stroke, tc = {'res': ('#ead7a4', '#8a6d2f', '#111'), 'cap': ('#fbbf77', '#b45309', '#111'),
                                'diode': ('#1f2937', '#000', '#fff')}[k]
            g.add(f'<g transform="rotate({ang:.1f} {cx * S:.1f} {cy * S:.1f})">')
            g.rect(cx - bl / 2, cy - 0.42, bl, 0.84, fill, stroke, 0.05, 0.3 if k == 'cap' else 0.12)
            if k == 'diode':
                g.rect(cx - bl / 2 + 0.12, cy - 0.42, 0.22, 0.84, '#e5e7eb')
            g.add('</g>')
            if detail:
                label = f'{ref} {val}' if bl >= 3.0 else ref
                rot = ang if -90 <= ang <= 90 else ang + 180
                if abs(abs(ang) - 90) < 1:
                    rot = -90
                g.text(cx, cy, label, 0.5 if bl >= 1.4 else 0.4, tc, rot=rot, bold=True)
                if k == 'cap' and val.startswith('10µ'):
                    g.text(x1 + (0.45 if x2 <= x1 else -0.45) * (0 if abs(x2 - x1) < 0.1 else 1),
                           y1 + (-0.5 if abs(x2 - x1) > 0.1 else (0.0)), '+', 0.6, '#b91c1c', bold=True, halo=True)
        elif k == 'c1':
            cx, cy = (X(hl[0]) + X(hl[1])) / 2, Y(hl[0])
            g.circ(cx, cy, 3.15, '#93c5fd', '#1d4ed8', 0.07, 0.55)
            for h in hl:
                g.circ(X(h), Y(h), 0.2, '#1d4ed8')
            if detail:
                g.text(cx, cy - 1.3, f'{ref}  1000 µF 35 V', 0.6, '#1e3a8a', bold=True)
                g.text(cx, cy + 1.3, 'BIPOLAR, either way', 0.5, '#1e3a8a')
        elif k in ('dip', 'relay'):
            rows = [h[2] for h in hl]
            r0, r1 = P['row'], P['row'] + P['npk'] // 2 - 1
            xe, xf = ox + A_OFF + COLX[P['cols'][0]], ox + A_OFF + COLX[P['cols'][1]]
            ov = 0.42 if k == 'relay' else 0
            g.rect(xe - 0.45, oy + r0 - 0.48 - ov, xf - xe + 0.9, r1 - r0 + 0.96 + 2 * ov, '#374151' if k == 'dip' else '#92400e', '#111', 0.05, 0.15)
            n1 = [p for p, pk in P['pkgpin'].items() if pk == 1]
            for p, h in pins.items():
                first = P['pkgpin'][p] == 1
                g.rect(X(h) - 0.36, Y(h) - 0.36, 0.72, 0.72, '#fde047' if first else '#e5e7eb', '#111', 0.04, 0.1)
                if detail:
                    g.text(X(h), Y(h), p, 0.44, '#111', bold=True)
            if k == 'dip':
                for h in P.get('unused', []):
                    g.rect(X(h) - 0.36, Y(h) - 0.36, 0.72, 0.72, '#6b7280', '#111', 0.04, 0.1)
                    if detail:
                        g.text(X(h), Y(h), 'x', 0.44, '#fff', bold=True)
            # pin-1 end notch
            ny = oy + (r1 + 0.48 + ov if P.get('rot') else r0 - 0.48 - ov)
            g.circ((xe + xf) / 2, ny, 0.32, '#f8f6ef', '#111', 0.04)
            if detail:
                tall = r1 - r0 >= 2
                if tall:
                    g.text((xe + xf) / 2, oy + (r0 + r1) / 2, f'{ref} {P["label"]}' if r1 - r0 >= 4 else ref,
                           0.6, '#fff', rot=-90, bold=True)
                else:
                    g.text((xe + xf) / 2, oy + (r0 + r1) / 2, ref, 0.55, '#fff', bold=True)
        elif k == 'module':
            if ref != 'J2':
                pass
            for p, h in pins.items():
                name = MCU[ref][int(p) - 1]
                left = h[1] == 'd'
                g.rect(X(h) - 0.36, Y(h) - 0.36, 0.72, 0.72, '#e5e7eb', '#111', 0.04, 0.1)
        elif k in ('sip', 'to92', 'hdr'):
            xs, ys = X(hl[0]), [Y(h) for h in hl]
            fill = {'sip': '#15803d', 'to92': '#111827', 'hdr': '#111827'}[k]
            if k == 'to92':
                fx, qx = (xs - 0.45, xs + 1.9) if P.get('face', 'a') == 'a' else (xs + 0.45, xs - 1.9)   # flat face side
                g.add(f'<path d="M {fx * S:.1f} {(min(ys) - 0.5) * S:.1f} L {fx * S:.1f} {(max(ys) + 0.5) * S:.1f} '
                      f'Q {qx * S:.1f} {((min(ys) + max(ys)) / 2) * S:.1f} {fx * S:.1f} {(min(ys) - 0.5) * S:.1f} Z" '
                      f'fill="{fill}" stroke="#000" opacity="0.9"/>')
            else:
                g.rect(xs - 0.47, min(ys) - 0.47, 0.94, max(ys) - min(ys) + 0.94, fill, '#000', 0.04, 0.12)
            names = P.get('names') or ({'3': 'C', '1': 'B', '2': 'E'} if k == 'to92' else None)
            for p, h in pins.items():
                if detail:
                    g.text(X(h), Y(h), names[p] if names else p, 0.46, '#fff', bold=True)
            if detail:
                side = P.get('lab', 'r')
                dx = {'r': 0.65, 'l': -0.65}[side] + (0.5 if k == 'to92' and side == 'r' and P.get('face', 'a') == 'a' else 0)
                if k == 'to92' and P.get('face') == 'j':
                    dx = -1.15
                g.text(xs + dx, (min(ys) + max(ys)) / 2, ref, 0.52, '#111', rot=-90, bold=True, halo=True)
        elif k == 'term':
            xs, ys = X(hl[0]), [Y(h) for h in hl]
            g.rect(xs - 2.0, min(ys) - 0.95, 3.4, max(ys) - min(ys) + 1.9, '#34a06a', '#14532d', 0.06, 0.15, 0.93)
            for p, h in pins.items():
                g.circ(xs - 0.9, Y(h), 0.55, '#d1d5db', '#374151', 0.05)
                g.line([(xs - 1.25, Y(h) - 0.25), (xs - 0.55, Y(h) + 0.25)], '#374151', 0.09)
                if detail:
                    g.text(xs + 0.55, Y(h), f'{p} {L.pinnet[(ref, p)]}', 0.42, '#fff', bold=True)
            if detail:
                g.text(xs - 1.65, (min(ys) + max(ys)) / 2, ref, 0.55, '#fff', rot=-90, bold=True)
    # module body on top of its pins
    if b == MOD_B:
        xd, xh = ox + A_OFF + 3, ox + A_OFF + 9
        g.rect(xd + 0.42, oy + 0.45, 5.16, MOD_ROWS + 0.1, '#1e3a5f', '#0b1f33', 0.06, 0.2, 0.95)
        g.rect(xd + 2.0, oy - 0.9, 2.0, 1.6, '#9ca3af', '#374151', 0.05, 0.15)
        if detail:
            g.text(xd + 3, oy - 0.2, 'USB-C', 0.45, '#111', bold=True)
            g.text(xd + 3, oy + 12.5, 'WeAct STM32G474 "Long" module', 0.62, '#fff', rot=-90, bold=True)
            for ref in ('J1', 'J2', 'J3', 'J4'):
                for p, h in L.parts[ref]['pins'].items():
                    left = h[1] == 'd'
                    g.text(X(h), Y(h), p, 0.4, '#111', bold=True)
                    g.text(X(h) + (0.55 if left else -0.55), Y(h), f'{MCU[ref][int(p) - 1]}', 0.44, '#fde68a',
                           anchor='start' if left else 'end', bold=True)
            g.text(xd + 1.3, oy + 25.1, 'P2 / P4', 0.42, '#1e3a5f', bold=True)
            g.text(xd + 4.7, oy + 25.1, 'P1 / P3', 0.42, '#1e3a5f', bold=True)


def path_len(pts):
    return sum(math.hypot(pts[i + 1][0] - pts[i][0], pts[i + 1][1] - pts[i][1]) for i in range(len(pts) - 1))
