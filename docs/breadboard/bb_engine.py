"""Breadboard layout engine: holes, strips, parts, automatic jumpers, netlist check.

Model: N standard 830-point boards side by side (B1 on the left). Each board has
rows 1-63, columns a-e (left strip of a row) and f-j (right strip of a row), and
four full-length rails: L+ L- on the left, R+ R- on the right.
Units: 1 = 0.1 inch (2.54 mm).
"""
import math
import re

ROWS = 63
BW = 21.5                                   # board pitch
COLX = {c: i for i, c in enumerate('abcde')}
COLX.update({c: 7 + i for i, c in enumerate('fghij')})
RAILX = {'L+': -3.6, 'L-': -2.6, 'R+': 13.6, 'R-': 14.6}
RAILNET = {'L+': '+3V3', 'L-': 'GND', 'R+': '+3V3', 'R-': 'GND'}
A_OFF = 5.25                                # board left edge -> column a


def H(s):
    """'B2:e12' -> (2,'e',12);  'B2:L-:12' -> (2,'L-',12)"""
    p = s.split(':')
    b = int(p[0][1:])
    if len(p) == 3:
        return (b, p[1], int(p[2]))
    return (b, p[1][0], int(p[1][1:]))


def hs(h):
    b, c, r = h
    return f'B{b}:{c}{r}' if c in COLX else f'B{b}:{c} rail (near row {r})'


def is_rail(h):
    return h[1] in RAILX


def node(h):
    b, c, r = h
    if c in RAILX:
        return (b, c)
    return (b, 'L' if c in 'abcde' else 'R', r)


def xy(h):
    b, c, r = h
    x = (b - 1) * BW + A_OFF + (RAILX[c] if c in RAILX else COLX[c])
    return (x, float(r))


def rail_row(r):
    """snap to a row that has a rail hole (groups of 5, rows 3-61)"""
    r = min(max(r, 3), 61)
    if (r - 3) % 6 == 5:
        r -= 1
    return r


def parse_netlist(path):
    t = open(path).read()
    comps = t[t.index('(components'):t.index('(libparts')]
    value = dict(re.findall(r'\(comp\s*\(ref "([^"]+)"\)\s*\(value "([^"]*)"\)', comps))
    t2 = t[t.index('(nets'):]
    pinnet, pinfn = {}, {}
    for b in re.split(r'\n\t\t\(net\b', t2)[1:]:
        name = re.search(r'\(name "([^"]*)"', b).group(1).lstrip('/')
        for r, p, f in re.findall(r'\(ref "([^"]+)"\)\s*\(pin "([^"]+)"\)\s*(?:\(pinfunction "([^"]*)"\))?', b):
            pinnet[(r, p)] = name
            pinfn[(r, p)] = re.sub(r'_\d+$', '', f) if f else ''
    return value, pinnet, pinfn


class Layout:
    def __init__(self, nboards, value, pinnet, pinfn):
        self.n = nboards
        self.value, self.pinnet, self.pinfn = value, pinnet, pinfn
        self.parts = {}          # ref -> dict(pins={pin: hole}, kind, **draw)
        self.used = {}           # hole -> description
        self.blocked = {}        # hole -> ref whose body covers it
        self.jumpers = []        # dict(a, b, net, auto)
        self.keepout = {}        # strip -> reason (must stay empty)
        self.manual = []
        self.errors = []

    # ---------------------------------------------------------------- parts
    def part(self, ref, pins, kind, **kw):
        assert ref not in self.parts, ref
        pins = {str(k): (H(v) if isinstance(v, str) else v) for k, v in pins.items()}
        self.parts[ref] = dict(pins=pins, kind=kind, **kw)
        for p, h in pins.items():
            if h in self.used and not is_rail(h):
                self.errors.append(f'hole {hs(h)} used twice: {self.used[h]} and {ref}-{p}')
            self.used[h] = f'{ref}-{p}'
        for h in self._body(ref):
            if h not in pins.values():
                self.blocked[h] = ref

    def _body(self, ref):
        P = self.parts[ref]
        k, pins = P['kind'], list(P['pins'].values())
        out = set()
        if k in ('res', 'cap', 'diode', 'link'):
            a, b = pins
            (x1, y1), (x2, y2) = xy(a), xy(b)
            L = math.hypot(x2 - x1, y2 - y1)
            for bd in {a[0], b[0]}:
                for c in COLX:
                    for r in range(1, ROWS + 1):
                        x, y = xy((bd, c, r))
                        t = ((x - x1) * (x2 - x1) + (y - y1) * (y2 - y1)) / (L * L)
                        if 0 < t < 1 and math.hypot(x - (x1 + t * (x2 - x1)), y - (y1 + t * (y2 - y1))) < 0.4:
                            out.add((bd, c, r))
        elif k == 'module':
            for h in pins:
                for c in 'efg':
                    out.add((h[0], c, h[2]))
        elif k == 'c1':
            (x1, y1), (x2, y2) = xy(pins[0]), xy(pins[1])
            cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
            bd = pins[0][0]
            for c in COLX:
                for r in range(1, ROWS + 1):
                    x, y = xy((bd, c, r))
                    if math.hypot(x - cx, y - cy) < 3.3:
                        out.add((bd, c, r))
        elif k == 'relay':
            for i in range(P['npk'] // 2):
                out.add((P['board'], 'e', P['row'] + i))
        elif k == 'term':
            bd, c = pins[0][0], pins[0][1]
            rows = [h[2] for h in pins]
            nb = 'b' if c == 'a' else 'i'
            for r in range(min(rows) - 1, max(rows) + 2):
                if 1 <= r <= ROWS:
                    out.add((bd, c, r))
                    out.add((bd, nb, r))
        return out

    def dip(self, ref, b, row, n, label, pinmap=None, kind='dip', rot=False, cols='ef'):
        """0.3 inch DIP across the centre gap. Normal: pin 1 at e<row> (top left).
        rot=True: turned 180 degrees, pin 1 at f<row+n/2-1> (bottom right).
        pinmap: schematic pin -> package pin."""
        half = n // 2
        pos = {}
        for i in range(half):
            if rot:
                pos[i + 1] = (b, cols[1], row + half - 1 - i)
                pos[n - i] = (b, cols[0], row + half - 1 - i)
            else:
                pos[i + 1] = (b, cols[0], row + i)
                pos[n - i] = (b, cols[1], row + i)
        if pinmap is None:
            pinmap = {p: int(p) for (r, p) in self.pinnet if r == ref}
        self.part(ref, {sp: pos[pp] for sp, pp in pinmap.items()}, kind, row=row, npk=n, label=label, rot=rot, board=b, cols=cols,
                  unused=[pos[q] for q in pos if q not in pinmap.values()],
                  pkgpin={str(sp): pp for sp, pp in pinmap.items()})

    def two(self, ref, h1, h2, kind='res', **kw):
        self.part(ref, {'1': h1, '2': h2}, kind, **kw)

    def sip(self, ref, b, col, row, order, kind='sip', **kw):
        """pins in one column on consecutive rows, listed top to bottom"""
        self.part(ref, {p: (b, col, row + i) for i, p in enumerate(order)}, kind, **kw)

    def check_bodies(self):
        for h, ref in self.blocked.items():
            u = self.used.get(h)
            if u and not u.startswith(ref + '-'):
                self.errors.append(f'{u} at {hs(h)} is under the body of {ref}')

    def jumper(self, a, b):
        self.manual.append((H(a), H(b)))

    # ------------------------------------------------------------- routing
    def free(self, nd, toward=None, rail_side=None):
        """free holes of a strip, best first"""
        b, s, r = nd
        cols = 'abcde' if s == 'L' else 'fghij'
        cand = [(b, c, r) for c in cols if (b, c, r) not in self.used and (b, c, r) not in self.blocked]
        if nd in self.keepout:
            cand = []
        if toward is not None:
            cand.sort(key=lambda h: (abs(xy(h)[0] - toward), ))
        else:
            cand.sort(key=lambda h: COLX[h[1]] if s == 'L' else -COLX[h[1]])
        return cand

    def _take(self, nd, toward=None):
        if len(nd) == 2:                         # rail: unlimited
            return None
        c = self.free(nd, toward)
        if not c:
            self.errors.append(f'no free hole left in strip {nd} (net {self.node_net.get(nd)})')
            return (nd[0], 'a' if nd[1] == 'L' else 'j', nd[2])
        return c[0]

    def _add(self, a, b, net, auto=True):
        for h in (a, b):
            if not is_rail(h):
                if h in self.used:
                    self.errors.append(f'jumper hole {hs(h)} already used by {self.used[h]}')
                if h in self.blocked:
                    self.errors.append(f'jumper hole {hs(h)} is under {self.blocked[h]}')
                self.used[h] = f'wire({net})'
        self.jumpers.append(dict(a=a, b=b, net=net, auto=auto))

    def route(self):
        self.check_bodies()
        nn = self.node_net = {}
        for b in range(1, self.n + 1):
            for rl, net in RAILNET.items():
                nn[(b, rl)] = net
        for ref, P in self.parts.items():
            for p, h in P['pins'].items():
                net = self.pinnet.get((ref, p))
                if net is None:
                    self.errors.append(f'{ref}-{p} is not in the netlist')
                    continue
                nd = node(h)
                if nd in nn and nn[nd] != net:
                    self.errors.append(f'SHORT: strip {nd} joins {nn[nd]} and {net} ({ref}-{p} at {hs(h)})')
                nn[nd] = net
        for nd in self.keepout:
            if nd in nn:
                self.errors.append(f'strip {nd} must stay empty ({self.keepout[nd]}) but carries {nn[nd]}')
        placed = {(r, p) for r, P in self.parts.items() for p in P['pins']}
        for rp in self.pinnet:
            if rp not in placed:
                self.errors.append(f'{rp[0]}-{rp[1]} is not placed')
        # union-find
        par = {nd: nd for nd in nn}

        def find(x):
            while par[x] != x:
                par[x] = par[par[x]]
                x = par[x]
            return x
        for a, b in self.manual:
            na, nb = node(a), node(b)
            net = nn.get(na) or nn.get(nb)
            for nd in (na, nb):
                if nd not in nn:
                    nn[nd] = net
                    par[nd] = nd
            if nn[na] != nn[nb]:
                self.errors.append(f'manual jumper {hs(a)}-{hs(b)} joins {nn[na]} and {nn[nb]}')
            self._add(a, b, net, auto=False)
            par[find(na)] = find(nb)
        nets = {}
        for nd, net in nn.items():
            nets.setdefault(net, []).append(nd)

        def centre(nd):
            if len(nd) == 2:
                return None
            return xy((nd[0], 'c' if nd[1] == 'L' else 'h', nd[2]))
        for net in sorted(nets):
            nds = nets[net]
            if net.startswith('unconnected'):
                continue
            if net in ('GND', '+3V3'):
                for nd in sorted(n for n in nds if len(n) == 3):
                    if any(len(m) == 2 and find(m) == find(nd) for m in nds):
                        continue
                    rl = (nd[0], nd[1] + ('+' if net == '+3V3' else '-'))
                    a = self._take(nd)
                    self._add(a, (rl[0], rl[1], rail_row(nd[2])), net)
                    par[find(nd)] = find(rl)
                continue
            while True:
                groups = {}
                for nd in nds:
                    groups.setdefault(find(nd), []).append(nd)
                if len(groups) < 2:
                    break
                best = None
                gl = list(groups.values())
                for i in range(len(gl)):
                    for j in range(i + 1, len(gl)):
                        for p in gl[i]:
                            for q in gl[j]:
                                (x1, y1), (x2, y2) = centre(p), centre(q)
                                d = abs(x1 - x2) + abs(y1 - y2)
                                if best is None or d < best[0]:
                                    best = (d, p, q)
                _, p, q = best
                a = self._take(p, centre(q)[0])
                b = self._take(q, centre(p)[0])
                self._add(a, b, net)
                par[find(p)] = find(q)
        # final check: each net is one group, no group holds two nets
        gnet = {}
        for nd, net in nn.items():
            g = find(nd)
            if g in gnet and gnet[g] != net:
                self.errors.append(f'SHORT between {gnet[g]} and {net}')
            gnet[g] = net
        for net, nds in nets.items():
            if net in ('GND', '+3V3') or net.startswith('unconnected'):
                continue
            if len({find(n) for n in nds}) != 1:
                self.errors.append(f'net {net} is not fully connected')
        for net in ('GND', '+3V3'):
            gs = {find(n) for n in nets[net]}
            if len(gs) != 1:
                self.errors.append(f'{net}: {len(gs)} separate islands (add rail links)')
        self.nets = nets
        return not self.errors

    def length_mm(self, j):
        (x1, y1), (x2, y2) = xy(j['a']), xy(j['b'])
        return math.hypot(x2 - x1, y2 - y1) * 2.54
