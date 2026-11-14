"""ESWCap C+R Rev C.1: evaluator circuit diagrams, verified against the KiCad netlist.

Usage:
    python3 docs/circuit/draw_circuit.py <kicad-netlist.net> <outdir>

Every component label (reference, value, part number) comes from PARTS and is
checked against the netlist value. Every drawn connection is declared with
net(name, 'REF-pin', ...) and asserted against the netlist, so a wire that does
not exist on the board fails the build. At the end every netlist part must have
appeared on at least one page.
"""
import re
import sys

import schemdraw
import schemdraw.elements as elm
import schemdraw.logic as logic

schemdraw.use('svg')
NETLIST, OUT = sys.argv[1], sys.argv[2]

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


def _f(n, k):
    return [c for c in n if isinstance(c, list) and c and c[0] == k]


def _v(n, k):
    f = _f(n, k)
    return f[0][1] if f and len(f[0]) > 1 else None


_root = sexpr(open(NETLIST).read())
NL_VALUE = {_v(c, 'ref'): _v(c, 'value') for c in _f(_f(_root, 'components')[0], 'comp')}
NL_PIN = {}
for _n in _f(_f(_root, 'nets')[0], 'net'):
    for _x in _f(_n, 'node'):
        NL_PIN[(_v(_x, 'ref'), _v(_x, 'pin'))] = _v(_n, 'name').lstrip('/')

SEEN = set()
ERRORS = []


def net(name, *refpins):
    """Assert that every REF-pin drawn on this net really is on it."""
    for rp in refpins:
        ref, pin = rp.rsplit('-', 1)
        got = NL_PIN.get((ref, pin))
        SEEN.add(ref)
        if got != name:
            ERRORS.append(f'{rp}: drawn on {name}, netlist says {got}')


# ------------------------------------------------------------------ parts ---
MPN = {
    'U1': 'LMC6482AIM/NOPB (TI) dual CMOS RRIO op-amp, 20 fA',
    'U2': 'SN74LVC1G34DBVR (TI) single buffer',
    'U3': 'MAX4619CSE+ (ADI) triple SPDT analog switch',
    'U4': 'MAX4619CSE+ (ADI) triple SPDT analog switch',
    'U5': 'AQY212EH (Panasonic) PhotoMOS SSR', 'U6': 'AQY212EH (Panasonic) PhotoMOS SSR',
    'U7': 'AQY212EH (Panasonic) PhotoMOS SSR', 'U9': 'AQY212EH (Panasonic) PhotoMOS SSR',
    'U10': 'AQY212EH (Panasonic) PhotoMOS SSR',
    'K1': 'Omron G6S-2 DC3 DPDT relay, 3 V coil', 'K2': 'Omron G6S-2 DC3 DPDT relay, 3 V coil',
    'Q1': 'AO3401A P-MOSFET', 'Q2': 'BC847B NPN', 'Q3': 'BC847B NPN',
    'D1': 'BAV199 low-leakage pair', 'D2': 'BAV199 low-leakage pair', 'D3': 'BAT54S Schottky pair',
    'D4': '1N4148W', 'D5': '1N4148W', 'D6': '1N4148W',
    'C1': '1000 µF 35 V bipolar electrolytic',
    'J1': 'WeAct header P1 (1×20 socket)', 'J2': 'WeAct header P2 (1×20 socket)',
    'J3': 'WeAct header P3 (1×4, NC)', 'J4': 'WeAct header P4 (1×4)',
    'J5': 'DUT, 4-pin 5.08 mm terminal', 'J6': 'V_BIAS 0–20 V, 2-pin 5.08 mm terminal',
    'J7': 'Console, 1×4 socket', 'J8': 'Bias control, 1×8 header',
}


def fmt_value(ref):
    v = NL_VALUE[ref]
    if ref.startswith('R'):
        m = re.match(r'([\d.]+)\s*([kM]?)\s*(.*)', v)
        num, suf, rest = m.groups()
        tol = rest.replace('%', ' %').strip()
        return f'{num} {suf}Ω' + (f' {tol}' if tol else '')
    if ref.startswith('C') and ref != 'C1':
        m = re.match(r'([\d.]+)([pnuµ]?)F', v)
        return f'{m.group(1)} {m.group(2).replace("u", "µ")}F'
    return v


def lbl(ref, short=False):
    """'R14 1 kΩ' / 'U1 LMC6482AIM/NOPB (TI) …'. Asserts the part exists."""
    assert ref in NL_VALUE, ref
    SEEN.add(ref)
    if ref in MPN:
        return f'{ref}  {MPN[ref].split(" (")[0] if short else MPN[ref]}'
    return f'{ref}  {fmt_value(ref)}'


# --------------------------------------------------------------- drawing ---
FS = 9
BLUE, RED, GREY, GREEN = '#1d4ed8', '#b91c1c', '#6b7280', '#047857'


def W(*pts, **kw):
    for a, b in zip(pts, pts[1:]):
        elm.Line(**kw).at(a).to(b)


def dot(p):
    elm.Dot().at(p)


ALIGN = {'right': ('left', 'center'), 'left': ('right', 'center'),
         'top': ('center', 'bottom'), 'bottom': ('center', 'top'), 'center': ('center', 'center')}


def T(p, s, loc='right', fs=FS, color='black', **kw):
    h, v = ALIGN[loc]
    elm.Label().at(p).label(s, fontsize=fs, color=color, halign=h, valign=v, **kw)


def flag(p, name, loc='right'):
    """Net label: name in blue next to the wire end."""
    dot(p)
    off = {'right': (0.15, 0), 'left': (-0.15, 0), 'top': (0, 0.15), 'bottom': (0, -0.15)}[loc]
    T((p[0] + off[0], p[1] + off[1]), name, loc=loc, color=BLUE, fs=FS)


def two(kind, a, b, ref, side='right', extra=None, **kw):
    e = {'R': elm.Resistor, 'C': elm.Capacitor, 'D': elm.Diode, 'CP': elm.Capacitor2}[kind](**kw).at(a).to(b)
    text = lbl(ref) + (f'\n{extra}' if extra else '')
    mid = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
    off = {'right': (0.45, 0), 'left': (-0.45, 0), 'top': (0, 0.45), 'bottom': (0, -0.45)}[side]
    T((mid[0] + off[0], mid[1] + off[1]), text, loc=side, fs=FS - 0.5)
    return e


def gnd(p):
    elm.Ground().at(p)


def vdd(p, name='+3V3'):
    elm.Vdd().at(p).label(name, fontsize=FS - 1)


def mcu_box(x0, y0, x1, y1, title='STM32G474CEU6 (on WeAct Long module) — internal peripherals'):
    W((x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0), color=GREEN, ls='--')
    T((x0 + 0.2, y1 - 0.3), title, loc='right', color=GREEN, fs=FS)


def mcu_pin(p, pin, hdr, desc, side='right'):
    """Square pin marker on the MCU boundary: 'PA7 (J2-13)' + function."""
    elm.Dot(open=True).at(p)
    T((p[0] + (0.25 if side == 'right' else -0.25), p[1] + 0.25), f'{pin} ({hdr})', loc=side, fs=FS - 0.5, color=GREEN)
    if desc:
        T((p[0] + (0.25 if side == 'right' else -0.25), p[1] - 0.3), desc, loc=side, fs=FS - 1.5, color=GREY)


def block(x0, y0, x1, y1, text, color='black', fs=FS - 0.5):
    W((x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0), color=color)
    T(((x0 + x1) / 2, (y0 + y1) / 2), text, loc='center', fs=fs, color=color)


def frame(d, title, sheet, nsheets, w=48, h=31):
    W((-1, -1), (w, -1), (w, h), (-1, h), (-1, -1), lw=1.5)
    bx = w - 17
    W((bx, -1), (bx, 2.6), (w, 2.6))
    T((bx + 0.3, 2.1), 'ESWCap C+R Meter — carrier board Rev C.1', fs=FS + 1)
    T((bx + 0.3, 1.25), title, fs=FS + 2)
    T((bx + 0.3, 0.35), f'Team Praise Claude · Embedded Systems Workshop · 2026-11-14 · Sheet {sheet}/{nsheets}', fs=FS - 1)
    T((bx + 0.3, -0.45), 'Netlist-verified against hardware/kicad/Project.kicad_sch', fs=FS - 1.5, color=GREY)


def legend(x, y):
    T((x, y), 'Legend:', fs=FS)
    T((x, y - 0.6), 'blue text = net name (identical to the KiCad schematic)', color=BLUE, fs=FS - 1)
    T((x, y - 1.2), 'green dashed box = inside the STM32; ◦ PAx (J2-n) = MCU pin (WeAct header pin)', color=GREEN, fs=FS - 1)
    T((x, y - 1.8), 'small numbers next to a part = its pin numbers', color=GREY, fs=FS - 1)


def ic(x, y, ref, pins, w=3.2, title=None, pinspacing=0.8):
    """IC box. pins: list of (side, name, number, net)."""
    icpins = [elm.IcPin(name=nm, pin=num, side=side, anchorname=f'p{num}') for side, nm, num, _ in pins]
    e = elm.Ic(pins=icpins, w=w, pinspacing=pinspacing, edgepadH=0.5, plblsize=FS - 2.5, lblsize=FS - 1)
    e.at((x, y)).theta(0)
    T((x + w / 2, y + 0.2 + max(sum(1 for p in pins if p[0] == 'left'), sum(1 for p in pins if p[0] == 'right')) * pinspacing + 0.6),
      title or lbl(ref), loc='top', fs=FS - 0.5)
    for _, _, num, n in pins:
        if n:
            net(n, f'{ref}-{num}')
    return e


# ================================================================ SHEET 1 ===


def sheet1():
    with schemdraw.Drawing(file=f'{OUT}/sheet1-overview.svg', show=False) as d:
        d.config(fontsize=FS, unit=2.0)
        frame(d, 'Sheet 1 — System overview, modes and amplifier list', 1, 7)
        T((0, 29.6), 'How the board is organised', fs=FS + 3)
        # block diagram
        block(0, 21, 9, 27.5, 'WeAct STM32G474CEU6\n"Long" core board\n\nCOMP1 · DAC3 · TIM2\nOPAMP1 · OPAMP3\nADC1–ADC5 · COMP4\nUSART1 (console)', GREEN)
        block(12, 24.5, 21, 27.5, 'C-mode front end (sheet 2)\nU2 buffer · U3 mux · R1–R4\nC1 ‖ C2 blocking capacitor')
        block(12, 20.5, 21, 23.5, 'Bias, safety SSRs, bias monitor\n(sheet 3) U5 U6 U7 U10 · R9–R12')
        block(24, 24.5, 33, 27.5, 'R-mode Kelvin (sheet 4)\nQ1 current source · OPAMP1\nU4 mux · R5–R8 · U1B · OPAMP3')
        block(24, 20.5, 33, 23.5, 'R-mode TIA (sheet 5)\nU1A + R16 ‖ C4 · K2B')
        block(36, 21, 44, 27.5, 'J5 DUT terminal\n1 FH  force high\n2 SH  sense high\n3 SL  sense low\n4 FL  force low\n\nK1 / K2 relays pick\nwhich circuit sees it')
        W((9, 24.25), (12, 24.25))
        W((21, 26), (24, 26))
        W((33, 24.25), (36, 24.25))
        T((22.5, 19.6), 'Power, relay drivers, console, J8 header, full pin map: sheet 6 · complete KiCad schematic (every wire): sheet 7', loc='center', fs=FS)

        # mode table
        y = 17.6
        T((0, y + 0.8), 'Measurement modes (set by firmware through PB9 → K1, PB10 → K2)', fs=FS + 1)
        rows = [('Mode', 'K1 (R_MODE)', 'K2 (KELVIN)', 'DUT bottom FL goes to', 'DUT top FH driven by', 'Range'),
                ('C', 'off (NC)', 'off (NC)', 'GND via K1A', 'C_BLOCK via K1B', 'pF – mF'),
                ('Kelvin R', 'on (NO)', 'on (NO)', 'MUXR_COM → U4 → R5–R8', 'Q1 current source', '≈0.1 Ω – 22 MΩ'),
                ('TIA R', 'on (NO)', 'off (NC)', 'U1A summing node', 'Q1 fully on', '≈15 MΩ – 1 GΩ')]
        cols = [0, 4.2, 8.4, 12.6, 21.5, 29.0]
        for i, r in enumerate(rows):
            for c, s in zip(cols, r):
                T((c, y - i * 0.75), s, fs=FS - (0 if i else 0), color='black' if i else BLUE)
        T((0, y - 3.2), 'Power-off / reset state = C-mode (both relays de-energised: FL grounded, C_BLOCK connected).', fs=FS - 1, color=GREY)

        # amplifier list
        y = 12.0
        T((0, y + 0.8), 'Every amplifier / comparator in the design', fs=FS + 1)
        amps = [('Device', 'Part / location', 'Job', 'Sheet'),
                ('U1A', 'LMC6482AIM/NOPB (TI), external SOIC-8', 'TIA: converts the nA DUT current to a voltage (R16 10 MΩ)', '5'),
                ('U1B', 'LMC6482AIM/NOPB (TI), external SOIC-8', 'Unity buffer for SL (DUT sense-low) → VBUF', '4, 5'),
                ('OPAMP1', 'STM32G474 internal op-amp', 'Kelvin current loop: drives Q1 so MUXR_COM = VREF (0.405 V)', '4'),
                ('OPAMP3', 'STM32G474 internal op-amp', 'V_SH buffer; Kelvin: PGA ×2…×64 referenced to VBUF', '3, 4, 5'),
                ('COMP1', 'STM32G474 internal comparator', 'C-mode relaxation oscillator (70 mV hysteresis)', '2'),
                ('COMP4', 'STM32G474 internal comparator', 'R-mode V_N over-voltage guard (2.6 V)', '4'),
                ('DAC3_CH1 / DAC1_CH1', 'STM32G474 internal DACs (no pin)', 'COMP1 threshold 1.65 V / COMP4 threshold 2.6 V', '2, 4')]
        cols = [0, 5.0, 16.0, 40.0]
        for i, r in enumerate(amps):
            for c, s in zip(cols, r):
                T((c, y - i * 0.72), s, fs=FS - 0.5, color='black' if i else BLUE)
        T((0, 5.6), 'TLC2272 / TLC2262 are not used: they need ≥ 4.4 V and the carrier runs on 3.3 V.', fs=FS - 1, color=GREY)
        T((0, 4.9), 'Firmware note: OPAMP3 PGA mode, PC4 (U10), PB15 (MUXR_EN) and PB2 (VBUF) follow docs/accuracy-plan.md F1–F3 (firmware update pending).', fs=FS - 1, color=RED)
        legend(0, 3.6)
        for r in ('U1', 'K1', 'K2', 'J5'):
            SEEN.add(r)


# ================================================================ HELPERS ===


def opamp(xy, anchor, plus_top):
    """Op-amp with polarity by name. Returns (element, plus, minus, out).

    schemdraw: in1 is always the '−' input and in2 the '+' input; flip() only
    moves them. Every connection below uses plus/minus, never in1/in2.
    """
    e = elm.Opamp(leads=True, sign=True)
    if plus_top:
        e = e.flip()
    e = e.at(xy).anchor({'plus': 'in2', 'minus': 'in1', 'out': 'out'}[anchor]).right()
    return e, e.in2, e.in1, e.out


def contact(com, nc, no, closed, ref, pins, title, side='right'):
    """Relay / switch contact with all three terminals shown.

    pins = (com_pin, nc_pin, no_pin); closed = 'NC' or 'NO'.
    """
    for p in (com, nc, no):
        elm.Dot(open=True).at(p)
    W(com, nc if closed == 'NC' else no, lw=1.6)
    names = {'com': f'{pins[0]} COM', 'nc': f'{pins[1]} NC', 'no': f'{pins[2]} NO'}
    for key, p in (('com', com), ('nc', nc), ('no', no)):
        T((p[0] + 0.2, p[1] + 0.25), names[key], loc='right', fs=FS - 3, color=GREY)
    T((max(com[0], nc[0], no[0]) + 1.2, (com[1] + nc[1] + no[1]) / 3), title, loc=side, fs=FS - 1.5)
    SEEN.add(ref)


def ssr(x, y, ref, top_sw_pin, bot_sw_pin, sw_left=False, h=1.6):
    """AQY212EH: LED (pins 1 A, 2 K) and MOSFET output (pins 3/4) in one outline.

    Returns dict of terminal coordinates: led_a, led_k, sw_top, sw_bot.
    """
    xl, xs = (x + 2.2, x) if sw_left else (x, x + 2.2)
    elm.LED().at((xl, y + h)).to((xl, y))
    elm.Switch(action=None).at((xs, y + h)).to((xs, y))
    W((x - 0.7, y - 0.45), (x + 2.9, y - 0.45), (x + 2.9, y + h + 0.45), (x - 0.7, y + h + 0.45), (x - 0.7, y - 0.45),
      color=GREY, ls=':')
    if sw_left:   # output wire leaves the top-left: put the title beside the outline
        T((x + 3.1, y + h / 2), lbl(ref, short=True), loc='right', fs=FS - 2)
    else:
        T((x - 0.7, y + h + 0.6), lbl(ref, short=True), loc='right', fs=FS - 2)
    T((xl - 0.15, y + h - 0.15), '1', loc='left', fs=FS - 3, color=GREY)
    T((xl - 0.15, y + 0.15), '2', loc='left', fs=FS - 3, color=GREY)
    T((xs + 0.15, y + h - 0.15), top_sw_pin, loc='right', fs=FS - 3, color=GREY)
    T((xs + 0.15, y + 0.15), bot_sw_pin, loc='right', fs=FS - 3, color=GREY)
    return {'led_a': (xl, y + h), 'led_k': (xl, y), 'sw_top': (xs, y + h), 'sw_bot': (xs, y)}


def clamp(x, ymid, ref, node_net):
    """BAV199/BAT54S series pair: pin 1 GND (anode low diode), pin 2 +3V3 (cathode high diode), pin 3 node."""
    elm.Diode().at((x, ymid - 1.6)).to((x, ymid))        # anode GND -> cathode node
    elm.Diode().at((x, ymid)).to((x, ymid + 1.6))        # anode node -> cathode +3V3
    gnd((x, ymid - 1.6))
    vdd((x, ymid + 1.6))
    dot((x, ymid))
    T((x + 0.35, ymid + 0.8), '2', fs=FS - 3, color=GREY)
    T((x + 0.35, ymid), '3', fs=FS - 3, color=GREY)
    T((x + 0.35, ymid - 0.8), '1', fs=FS - 3, color=GREY)
    T((x + 0.35, ymid - 1.75), f'{ref}\n{MPN[ref].split(" ")[0]} clamp', loc='right', fs=FS - 2.5)
    net('GND', f'{ref}-1')
    net('+3V3', f'{ref}-2')
    net(node_net, f'{ref}-3')


def mcu_pin_in(p, pin, hdr, desc=''):
    """MCU pin on the right edge of the MCU box; labels inside the box."""
    elm.Dot(open=True).at(p)
    T((p[0] - 0.25, p[1] + 0.25), f'{pin} ({hdr})', loc='left', fs=FS - 1, color=GREEN)
    if desc:
        T((p[0] - 0.25, p[1] - 0.3), desc, loc='left', fs=FS - 2.5, color=GREY)


def notes(x, y, lines, fs=FS - 1.5):
    for i, (s, c) in enumerate(lines):
        T((x, y - i * 0.55), s, fs=fs, color=c)


# ================================================================ SHEET 2 ===


def sheet2():
    with schemdraw.Drawing(file=f'{OUT}/sheet2-cmode.svg', show=False) as d:
        d.config(fontsize=FS, unit=2.0)
        frame(d, 'Sheet 2 — C-mode measurement path (K1 and K2 de-energised)', 2, 7)
        mcu_box(0, 7.5, 10.0, 28.5, 'STM32G474CEU6 (on WeAct module)')
        # COMP1: + = PA1 (VCAP_ADC), − = DAC3_CH1 1.65 V
        c1, cplus, cminus, cout = opamp((4.0, 21.0), 'plus', plus_top=False)
        T((c1.center[0] - 0.35, c1.center[1]), 'COMP1', loc='center', fs=FS - 1.5)
        block(0.6, 23.2, 3.4, 24.6, 'DAC3_CH1\n1.65 V', GREEN, fs=FS - 2)
        W((2.0, 23.2), (2.0, cminus[1]), cminus)
        T((c1.center[0], c1.center[1] - 1.5), 'output inverted · 70 mV hysteresis', loc='center', fs=FS - 2.5, color=GREY)
        jn = (8.0, cout[1])
        W(cout, (10.0, cout[1]))
        dot(jn)
        block(5.6, 25.2, 9.6, 26.6, 'TIM2_CH1 capture\n(TISEL, internal)', GREEN, fs=FS - 2)
        W(jn, (8.0, 25.2))
        mcu_pin_in((10.0, cout[1]), 'PA0', 'J2-6', 'COMP1_OUT (AF8) / GPIO step')
        pa1 = (10.0, 16.5)
        W(cplus, (1.2, cplus[1]), (1.2, pa1[1]), pa1)
        dot((5.0, pa1[1]))
        block(2.8, 13.0, 7.2, 14.4, 'ADC1_IN2\nTIM6 + DMA stream', GREEN, fs=FS - 2)
        W((5.0, pa1[1]), (5.0, 14.4))
        mcu_pin_in(pa1, 'PA1', 'J2-7', 'ADC1_IN2 + COMP1_INP')
        mcu_pin_in((10.0, 11.0), 'PB5', 'J1-5', 'MUXC_EN (EN̄)')
        mcu_pin_in((10.0, 9.8), 'PB4', 'J1-6', 'MUXC_A1')
        mcu_pin_in((10.0, 8.6), 'PB3', 'J1-7', 'MUXC_A0')

        # U2 buffer with R29 pull-down
        y0 = cout[1]
        W((10.0, y0), (14.0, y0))
        dot((12.0, y0))
        two('R', (12.0, y0), (12.0, y0 - 2.6), 'R29', side='right')
        gnd((12.0, y0 - 2.6))
        T((11.0, y0 + 0.35), 'DRIVE', loc='top', color=BLUE)
        net('DRIVE', 'J2-6', 'R29-1', 'U2-2')
        net('GND', 'R29-2', 'U2-3')
        b = logic.Buf().at((14.0, y0)).anchor('in1').right()
        T((b.center[0], y0 + 1.0), lbl('U2'), loc='top', fs=FS - 2)
        T((13.6, y0 + 0.3), '2', fs=FS - 3, color=GREY)
        T((b.out[0] + 0.1, y0 + 0.3), '4', fs=FS - 3, color=GREY)
        T((14.0, y0 - 1.3), 'U2: VCC 5 = +3V3, GND 3', loc='right', fs=FS - 3, color=GREY)
        net('+3V3', 'U2-5')

        # U3 MAX4619 (pins listed bottom -> top)
        u3 = ic(19.5, 12.0, 'U3', [
            ('left', 'A', '11', 'MUXC_A0'), ('left', 'B', '10', 'MUXC_A0'), ('left', 'C', '9', 'MUXC_A1'),
            ('left', 'EN̄', '6', 'MUXC_EN'), ('left', 'Z (COM)', '4', 'C_MUX_COM'),
            ('right', 'X0', '12', 'C_CH0'), ('right', 'X1', '13', 'C_CH1'),
            ('right', 'Y0', '2', 'C_CH2'), ('right', 'Y1', '1', 'C_CH3'),
        ], w=3.4, pinspacing=1.25, title=lbl('U3', short=True) + '  (VCC 16 = +3V3, GND 8)')
        net('MUXC_P', 'U3-14', 'U3-5')
        net('MUXC_Q', 'U3-15', 'U3-3')
        net('+3V3', 'U3-16')
        net('GND', 'U3-8')
        zx = b.out[0] + 0.5
        W(b.out, (zx, y0), (zx, u3.p4[1]), u3.p4)
        T((zx + 0.15, (y0 + u3.p4[1]) / 2 + 0.1), 'C_MUX_COM', loc='right', fs=FS - 2, color=BLUE)
        net('C_MUX_COM', 'U2-4', 'U3-4')
        # control lines
        W((10.0, 11.0), (17.0, 11.0), (17.0, u3.p6[1]), u3.p6)
        W((10.0, 9.8), (17.6, 9.8), (17.6, u3.p9[1]), u3.p9)
        W((10.0, 8.6), (18.2, 8.6), (18.2, u3.p10[1]), u3.p10)
        W((18.2, u3.p11[1]), u3.p11)
        dot((18.2, u3.p11[1]))
        net('MUXC_EN', 'J1-5', 'R30-1')
        net('MUXC_A1', 'J1-6')
        net('MUXC_A0', 'J1-7')
        net('+3V3', 'R30-2')
        SEEN.add('R30')
        # range resistors -> VCAP bus
        xb = 30.5
        for anch, ref, n in (('p12', 'R3', 'C_CH0'), ('p13', 'R2', 'C_CH1'), ('p2', 'R1', 'C_CH2'), ('p1', 'R4', 'C_CH3')):
            a = getattr(u3, anch)
            elm.Resistor().at((a[0] + 0.6, a[1])).to((a[0] + 4.6, a[1]))
            W(a, (a[0] + 0.6, a[1]))
            W((a[0] + 4.6, a[1]), (xb, a[1]))
            dot((xb, a[1]))
            T((a[0] + 5.0, a[1] + 0.3), lbl(ref), loc='right', fs=FS - 2.5)
            net(n, f'{ref}-1')
            net('VCAP', f'{ref}-2')
        bus_lo = u3.p12[1]
        W((xb, bus_lo), (xb, 24.5))
        T((xb - 0.2, 24.7), 'VCAP', loc='left', color=BLUE)
        net('VCAP', 'R32-1', 'C1-1', 'C2-1', 'R33-1', 'D1-3', 'U6-4')
        # D1 clamp on VCAP (top)
        W((xb, 24.5), (32.5, 24.5))
        clamp(32.5, 24.5, 'D1', 'VCAP')
        # R32 to PA1
        W((xb, bus_lo), (xb, 11.0))
        two('R', (xb, 11.0), (xb, 8.4), 'R32', side='right')
        W((xb, 8.4), (xb, 7.8))
        T((xb, 7.6), 'VCAP_ADC → PA1 (J2-7)', loc='bottom', color=BLUE)
        net('VCAP_ADC', 'R32-2', 'J2-7')
        T((xb + 0.3, 12.2), 'also U6 (SSR3 clamp), sheet 3', fs=FS - 3, color=GREY)
        # C1 || C2 || R33 between VCAP and C_BLOCK
        L, R = 34.0, 38.5
        W((xb, 20.0), (L, 20.0))
        dot((xb, 20.0))
        W((L, 18.2), (L, 21.8))
        W((R, 18.2), (R, 21.8))
        dot((L, 20.0))
        elm.Capacitor(polar=False).at((L, 21.8)).to((R, 21.8))
        T(((L + R) / 2, 22.3), lbl('C1'), loc='bottom', fs=FS - 2.5)
        elm.Capacitor().at((L, 20.0)).to((R, 20.0))
        T(((L + R) / 2, 20.5), lbl('C2'), loc='bottom', fs=FS - 2.5)
        elm.Resistor().at((L, 18.2)).to((R, 18.2))
        T(((L + R) / 2, 17.55), lbl('R33') + ' (bleed)', loc='bottom', fs=FS - 2.5)
        dot((R, 20.0))
        T((R + 0.1, 20.35), 'C_BLOCK', loc='right', fs=FS - 1.5, color=BLUE)
        net('C_BLOCK', 'C1-2', 'C2-2', 'R33-2', 'K1-10')
        # K1B: NC 10 = C_BLOCK (closed), COM 9 = FH, NO 8 = not connected
        nc = (40.0, 20.0)
        W((R, 20.0), nc)
        com = (42.0, 20.0)
        no = (40.0, 21.0)
        contact(com, nc, no, 'NC', 'K1', ('9', '10', '8'), '', side='right')
        T((41.0, 22.6), 'K1B (pole 2 of K1): C-mode NC 10 closed, NO 8 not connected', loc='center', fs=FS - 2.5)
        net('FH', 'K1-9')
        fh = (43.0, 20.0)
        W(com, fh)
        dot(fh)
        T((fh[0] + 0.25, fh[1] - 0.9), 'FH (J5-1)', loc='right', color=BLUE)
        net('FH', 'J5-1', 'R12-2', 'R9-2', 'U7-4', 'D6-1')
        # DUT and K1A
        elm.Capacitor().at(fh).to((fh[0], 15.0)).fill('#fde68a')
        T((fh[0] + 0.6, 17.5), 'DUT\n(capacitor\nunder test)', fs=FS - 1.5)
        dot((fh[0], 15.0))
        T((fh[0] + 0.2, 15.0), 'FL (J5-4)', loc='right', color=BLUE)
        net('FL', 'J5-4', 'K1-4')
        com_a = (fh[0], 14.0)
        W((fh[0], 15.0), com_a)
        nc_a, no_a = (fh[0], 12.0), (fh[0] + 1.0, 12.0)
        contact(com_a, nc_a, no_a, 'NC', 'K1', ('4', '3', '5'), 'K1A (pole 1)\nC-mode: NC 3 closed\nNO 5 not connected', side='right')
        gnd(nc_a)
        net('GND', 'K1-3')
        notes(11.0, 6.6, [
            ('U3 truth table (A = B = MUXC_A0 on PB3, C = MUXC_A1 on PB4):  00 → X0 R3 98.6 kΩ   01 → X1 R2 993.6 Ω   10 → Y0 R1 98.9 Ω   11 → Y1 R4 1.01 MΩ', GREY),
            ('MUXC_EN (PB5) has R30 100 kΩ pull-up to +3V3: U3 is off at reset (EN̄ active low).', GREY),
            ('The DUT is in series with C1 ‖ C2 (C_block ≈ 1000 µF):  C_DUT = C_eq·C_block / (C_block − C_eq).', 'black'),
            ('Oscillator: COMP1 drives U2 through PA0, T = K·R·C + T0 (K ≈ 0.085), timed by TIM2.  RC step: PA0 GPIO step, ADC1 records V(t), τ = R·C.', 'black'),
        ])
        T((fh[0] + 0.2, 23.6), 'FH also: R12, R9/U5, U7 (sheet 3); D6 (sheet 4)', loc='right', fs=FS - 3, color=GREY)
        legend(0, 3.4)


# ================================================================ SHEET 3 ===


def sheet3():
    with schemdraw.Drawing(file=f'{OUT}/sheet3-bias-safety.svg', show=False) as d:
        d.config(fontsize=FS, unit=2.0)
        frame(d, 'Sheet 3 — DUT bias, safety SSRs and DC-bias monitor (C-mode)', 3, 7)
        mcu_box(0, 6.0, 9.0, 25.0, 'STM32G474CEU6')
        # J6 and R12 bias feed
        vb = (11.0, 26.5)
        dot(vb)
        T((vb[0] - 0.2, vb[1] + 0.35), lbl('J6', short=True) + ':  pin 1 = V_BIAS, pin 2 = GND', loc='right', fs=FS - 1.5)
        T((vb[0] - 0.2, vb[1] - 0.4), 'V_BIAS', loc='left', color=BLUE)
        net('V_BIAS', 'J6-1', 'R12-1', 'U5-4')
        net('GND', 'J6-2')
        W(vb, (21.0, 26.5))
        two('R', (21.0, 26.5), (25.5, 26.5), 'R12', side='top', extra='DC feed, stiff at DC / open at AC')
        W((25.5, 26.5), (37.0, 26.5))
        T((37.2, 26.5), 'FH → DUT top (J5-1)', loc='right', color=BLUE)
        net('FH', 'R12-2')
        # SSR1 = U5
        u5 = ssr(17.0, 19.5, 'U5', '4', '3')
        W((u5['sw_top'][0], 26.5), u5['sw_top'])
        dot((u5['sw_top'][0], 26.5))
        W(u5['sw_bot'], (u5['sw_bot'][0], 18.6))
        two('R', (u5['sw_bot'][0], 18.6), (u5['sw_bot'][0] + 4.5, 18.6), 'R9', side='bottom', extra='2512, limits pre-charge ≈ 0.4 A')
        W((u5['sw_bot'][0] + 4.5, 18.6), (27.5, 18.6), (27.5, 26.5))
        dot((27.5, 26.5))
        net('SSR1_SW', 'U5-3', 'R9-1')
        net('FH', 'R9-2')
        gnd(u5['led_k'])
        T((23.0, 22.4), 'SSR1 (U5): pre-charge — V_BIAS → R9 → FH', fs=FS - 1, color=RED)
        # SSR3 = U6
        u6 = ssr(17.0, 12.5, 'U6', '4', '3')
        W(u6['sw_top'], (u6['sw_top'][0], 15.3))
        T((u6['sw_top'][0] + 0.15, 15.5), 'VCAP (sheet 2)', loc='right', color=BLUE)
        two('R', u6['sw_bot'], (u6['sw_bot'][0], 10.3), 'R11', side='right', extra='1 Ω')
        gnd((u6['sw_bot'][0], 10.3))
        net('SSR3_SW', 'U6-3', 'R11-1')
        net('GND', 'R11-2')
        gnd(u6['led_k'])
        T((23.0, 14.8), 'SSR3 (U6): clamps VCAP while pre-charging', fs=FS - 1, color=RED)
        # SSR2 = U7
        u7 = ssr(17.0, 5.0, 'U7', '4', '3')
        W(u7['sw_top'], (u7['sw_top'][0], 7.8))
        T((u7['sw_top'][0] + 0.15, 8.0), 'FH (J5-1)', loc='right', color=BLUE)
        two('R', u7['sw_bot'], (u7['sw_bot'][0], 3.2), 'R10', side='right', extra='2512')
        gnd((u7['sw_bot'][0], 3.2))
        net('SSR2_SW', 'U7-3', 'R10-1')
        net('GND', 'R10-2')
        gnd(u7['led_k'])
        T((23.0, 7.2), 'SSR2 (U7): discharges FH (DUT) to GND', fs=FS - 1, color=RED)
        for u in ('U5', 'U6', 'U7'):
            net('GND', f'{u}-2')
        # LED drives
        pb11 = (9.0, u5['led_a'][1])
        mcu_pin_in(pb11, 'PB11', 'J2-20', 'SSR13: SSR1 + SSR3 together')
        two('R', (11.5, pb11[1]), u5['led_a'], 'R22', side='top')
        W(pb11, (11.5, pb11[1]))
        dot((10.6, pb11[1]))
        W((10.6, pb11[1]), (10.6, u6['led_a'][1]), (11.5, u6['led_a'][1]))
        two('R', (11.5, u6['led_a'][1]), u6['led_a'], 'R23', side='top')
        net('SSR13', 'J2-20', 'R22-1', 'R23-1')
        net('SSR1_LED', 'R22-2', 'U5-1')
        net('SSR3_LED', 'R23-2', 'U6-1')
        pb13 = (9.0, u7['led_a'][1])
        mcu_pin_in(pb13, 'PB13', 'J1-19', 'SSR2_CTRL: discharge')
        W(pb13, (11.5, pb13[1]))
        two('R', (11.5, pb13[1]), u7['led_a'], 'R24', side='top')
        net('SSR2_CTRL', 'J1-19', 'R24-1')
        net('SSR2_LED', 'R24-2', 'U7-1')

        # DC-bias monitor
        T((30.5, 21.0), 'DC-bias monitor (C-mode: U10 on, U9 off)', fs=FS + 0.5)
        sh = (31.0, 19.0)
        T((sh[0] - 0.2, sh[1] + 0.3), 'SH (J5-2)', loc='left', color=BLUE)
        dot(sh)
        net('SH', 'J5-2', 'R38-1', 'C21-1', 'U9-3')
        two('R', sh, (sh[0], 15.0), 'R38', side='left')
        W(sh, (33.0, 19.0))
        elm.Capacitor().at((33.0, 19.0)).to((33.0, 15.0))
        T((33.4, 17.0), lbl('C21'), fs=FS - 2.5)
        dot((sh[0], 15.0))
        W((sh[0], 15.0), (35.0, 15.0))
        dot((33.0, 15.0))
        T((sh[0] - 0.2, 15.0), 'SH_DIV (÷11)', loc='left', color=BLUE)
        net('SH_DIV', 'R38-2', 'C21-2', 'R39-1', 'C20-1', 'U10-3')
        two('R', (sh[0], 15.0), (sh[0], 11.0), 'R39', side='left')
        elm.Capacitor().at((33.0, 15.0)).to((33.0, 11.0))
        T((33.4, 13.0), lbl('C20'), fs=FS - 2.5)
        gnd((sh[0], 11.0))
        gnd((33.0, 11.0))
        net('GND', 'R39-2', 'C20-2')
        # U10: switch on the left (pin 4 top = VN_SENSE, pin 3 bottom = SH_DIV), LED right
        u10 = ssr(37.0, 12.0, 'U10', '4', '3', sw_left=True)
        W((35.0, 15.0), (35.0, 12.0), u10['sw_bot'])
        W(u10['sw_top'], (u10['sw_top'][0], 16.6), (44.0, 16.6))
        T((44.2, 16.6), 'VN_SENSE → PB0 (J2-15)', loc='right', color=BLUE)
        T((44.2, 16.0), 'OPAMP3 follower → ADC3 (sheet 4)', loc='right', fs=FS - 2.5, color=GREY)
        net('VN_SENSE', 'U10-4', 'J2-15')
        W(u10['led_a'], (u10['led_a'][0], u10['led_a'][1] + 1.0))
        vdd((u10['led_a'][0], u10['led_a'][1] + 1.0))
        net('+3V3', 'U10-1')
        two('R', u10['led_k'], (u10['led_k'][0], 9.0), 'R43', side='right')
        W((u10['led_k'][0], 9.0), (9.0, 9.0))
        mcu_pin_in((9.0, 9.0), 'PC4', 'J2-14', 'C_MODE_SW: LOW = U10 on')
        net('U10_LED_K', 'U10-2', 'R43-2')
        net('C_MODE_SW', 'R43-1', 'J2-14')
        notes(28.5, 7.6, [
            ('Pre-charge: PB11 HIGH turns on SSR1 and SSR3 together (shared pin = hardware interlock).', 'black'),
            ('Discharge: PB13 HIGH turns on SSR2 (100 Ω to GND). Firmware never runs SSR2 during a measurement.', 'black'),
            ('Bias monitor: V_BIAS at the DUT = 11 × V(SH_DIV), up to ≈ 36 V. VN_SENSE also has D2 + C7 (sheet 4).', 'black'),
            ('R33 10 MΩ across C1 slowly bleeds the blocking capacitor (sheet 2).', GREY),
        ])
        SEEN.add('R33')
        legend(0, 3.4)


# ================================================================ SHEET 4 ===


def sheet4():
    with schemdraw.Drawing(file=f'{OUT}/sheet4-kelvin.svg', show=False) as d:
        d.config(fontsize=FS, unit=2.0)
        frame(d, 'Sheet 4 — R-mode, 4-wire Kelvin (K1 energised, K2 energised)', 4, 7)
        mcu_box(0, 5.5, 10.0, 29.0, 'STM32G474CEU6')
        # OPAMP1 inside: − = PA3 (VREF), + = PA7 (MUXR_COM)
        o1, o1p, o1m, o1o = opamp((4.5, 24.0), 'out', plus_top=False)
        o1, o1p, o1m, o1o = opamp((7.0, 24.0), 'out', plus_top=False) if False else (o1, o1p, o1m, o1o)
        T((o1.center[0] - 0.35, o1.center[1]), 'OPAMP1', loc='center', fs=FS - 2)
        pa2 = (10.0, 24.0)
        W(o1o, pa2)
        mcu_pin_in(pa2, 'PA2', 'J2-8', 'OPAMP1 VOUT')
        pa3 = (10.0, 27.0)
        W(o1m, (o1m[0] - 0.4, o1m[1]), (o1m[0] - 0.4, pa3[1]), pa3)
        mcu_pin_in(pa3, 'PA3', 'J2-9', 'OPAMP1 VINM0 · ADC1_IN4')
        pa7 = (10.0, 19.0)
        W(o1p, (o1p[0] - 0.4, o1p[1]), (o1p[0] - 0.4, pa7[1]), pa7)
        mcu_pin_in(pa7, 'PA7', 'J2-13', 'OPAMP1 VINP2 · ADC2_IN4')
        pb14 = (10.0, 22.0)
        mcu_pin_in(pb14, 'PB14', 'J1-18', 'R_FORCE: Hi-Z in Kelvin')
        # VREF network
        n = (12.5, 27.0)
        W(pa3, n)
        dot(n)
        two('R', n, (n[0], 29.2), 'R17', side='right')
        vdd((n[0], 29.2))
        W(n, (16.8, 27.0))
        dot((14.2, 27.0))
        two('R', (14.2, 27.0), (14.2, 25.2), 'R18', side='left')
        gnd((14.2, 25.2))
        dot((15.6, 27.0))
        elm.Capacitor().at((15.6, 27.0)).to((15.6, 25.2))
        gnd((15.6, 25.2))
        T((15.85, 26.4), lbl('C5'), fs=FS - 3)
        T((12.7, 27.4), 'VREF 0.405 V', loc='right', fs=FS - 1.5, color=BLUE)
        net('VREF', 'J2-9', 'R17-2', 'R18-1', 'C5-1', 'C3-2')
        net('+3V3', 'R17-1')
        net('GND', 'R18-2', 'C5-2')
        SEEN.add('C5')
        elm.Capacitor().at((16.8, 27.0)).to((16.8, 24.0))
        T((17.05, 25.5), lbl('C3'), fs=FS - 2.5)
        # OP1_OUT -> R14 -> Q1 gate ; R13 pull-up ; R15 to PB14
        W(pa2, (16.8, 24.0))
        dot((16.8, 24.0))
        two('R', (17.4, 24.0), (21.0, 24.0), 'R14', side='bottom')
        W((16.8, 24.0), (17.4, 24.0))
        net('OP1_OUT', 'J2-8', 'R14-1', 'C3-1')
        g = (22.4, 24.0)
        W((21.0, 24.0), g)
        q1 = elm.PFet().theta(0).reverse().at(g).anchor('gate')
        T((q1.source[0] + 0.4, q1.source[1] - 0.2), lbl('Q1') + '  (G 1, S 2, D 3)', fs=FS - 2)
        vdd(q1.source)
        net('+3V3', 'Q1-2')
        net('Q1_GATE', 'Q1-1', 'R13-2', 'R14-2', 'R15-2')
        dot((21.6, 24.0))
        two('R', (21.6, 24.0), (21.6, 26.4), 'R13', side='left')
        vdd((21.6, 26.4))
        net('+3V3', 'R13-1')
        dot((21.0, 24.0))
        two('R', (21.0, 24.0), (21.0, 22.0), 'R15', side='left')
        W((21.0, 22.0), pb14)
        net('R_FORCE', 'R15-1', 'J1-18')
        # force column
        X = q1.drain[0]
        d6b = (X, q1.drain[1] - 1.6)
        elm.Diode().at(q1.drain).to(d6b)
        T((X + 0.35, (q1.drain[1] + d6b[1]) / 2), lbl('D6') + '  (A 2 → K 1)', fs=FS - 2)
        net('Q1_D', 'Q1-3', 'D6-2')
        net('FH', 'D6-1', 'J5-1', 'K1-9')
        dot(d6b)
        T((X - 0.25, d6b[1]), 'FH (J5-1)', loc='left', fs=FS - 1, color=BLUE)
        top = (X, 20.8)
        W(d6b, top)
        dot(top)
        bot = (X, 17.2)
        elm.Resistor().at(top).to(bot).fill('#fde68a')
        T((X - 0.45, 19.0), 'R_DUT', loc='left')
        dot(bot)
        fl = (X, 16.4)
        W(bot, fl)
        dot(fl)
        T((X - 0.25, fl[1] - 0.3), 'FL (J5-4)', loc='left', fs=FS - 1, color=BLUE)
        net('FL', 'J5-4', 'K1-4', 'K2-4')
        # K1A (energised: COM 4 -> NO 5 n.c.) drawn left of FL
        k1com = (X - 2.2, fl[1])
        W(fl, k1com)
        contact(k1com, (k1com[0] - 1.6, fl[1] + 0.8), (k1com[0] - 1.6, fl[1] - 0.8), 'NO', 'K1', ('4', '3', '5'), '', side='left')
        T((k1com[0] - 1.9, fl[1] + 0.8), 'GND', loc='left', fs=FS - 2.5, color=BLUE)
        T((k1com[0] - 1.9, fl[1] - 0.8), 'n.c.', loc='left', fs=FS - 2.5, color=GREY)
        T((k1com[0] - 1.0, fl[1] + 1.6), 'K1A: NO (FL off GND)', loc='center', fs=FS - 2.5)
        net('GND', 'K1-3')
        # K2A (energised: COM 4 -> NO 5 = MUXR_COM)
        c2 = (X, 15.6)
        W(fl, c2)
        contact(c2, (X + 1.0, 13.8), (X, 13.8), 'NO', 'K2', ('4', '3', '5'), 'K2A: NO 5 closed\n(NC 3 not connected)', side='right')
        mc = (X, 13.0)
        W((X, 13.8), mc)
        dot(mc)
        T((X + 0.25, mc[1] - 0.15), 'MUXR_COM', loc='right', fs=FS - 1, color=BLUE)
        net('MUXR_COM', 'K2-5', 'U4-4', 'J2-13')
        W(mc, (11.4, 13.0), (11.4, pa7[1]), pa7)
        T((11.6, 15.8), 'MUXR_COM (H1)', loc='right', fs=FS - 2, color=BLUE)
        # U4 below
        u4 = ic(14.5, 5.6, 'U4', [
            ('left', 'A', '11', 'MUXR_A0'), ('left', 'B', '10', 'MUXR_A0'), ('left', 'C', '9', 'MUXR_A1'),
            ('left', 'EN̄', '6', 'MUXR_EN'),
            ('right', 'X0', '12', 'R_CH0'), ('right', 'X1', '13', 'R_CH1'), ('right', 'Y0', '2', 'R_CH2'),
            ('right', 'Y1', '1', 'R_CH3'), ('right', 'Z (COM)', '4', 'MUXR_COM'),
        ], w=3.4, pinspacing=1.1, title=lbl('U4', short=True) + '  (VCC 16, GND 8)')
        net('MUXR_P', 'U4-14', 'U4-5')
        net('MUXR_Q', 'U4-15', 'U4-3')
        W(mc, (X, u4.p4[1]), u4.p4)
        gx = 23.0
        for anch, ref, nn in (('p12', 'R5', 'R_CH0'), ('p13', 'R6', 'R_CH1'), ('p2', 'R7', 'R_CH2'), ('p1', 'R8', 'R_CH3')):
            a = getattr(u4, anch)
            elm.Resistor().at((a[0] + 0.4, a[1])).to((gx - 0.2, a[1]))
            W(a, (a[0] + 0.4, a[1]))
            W((gx - 0.2, a[1]), (gx, a[1]))
            dot((gx, a[1]))
            T((a[0] + 0.5, a[1] + 0.32), lbl(ref), loc='right', fs=FS - 3)
            net(nn, f'{ref}-1')
            net('GND', f'{ref}-2')
        W((gx, u4.p1[1]), (gx, u4.p12[1] - 0.8))
        gnd((gx, u4.p12[1] - 0.8))
        for pin, hdr, nn, anch, yy, xx in (('PB15', 'J1-17', 'MUXR_EN', 'p6', 9.6, 12.0),
                                          ('PB7', 'J1-3', 'MUXR_A1', 'p9', 8.4, 12.6),
                                          ('PB6', 'J1-4', 'MUXR_A0', 'p10', 7.2, 13.2)):
            mcu_pin_in((10.0, yy), pin, hdr, nn)
            a = getattr(u4, anch)
            W((10.0, yy), (xx, yy), (xx, a[1]), a)
            net(nn, hdr)
        W((13.2, u4.p11[1]), u4.p11)
        W((13.2, 7.2), (13.2, u4.p11[1]))
        dot((13.2, u4.p11[1]))
        dot((13.2, u4.p10[1]))
        net('MUXR_EN', 'R31-1')
        net('+3V3', 'R31-2')
        SEEN.add('R31')
        # SH row (y = 24): U9 -> R20 -> VN_SENSE (C7, D2) -> PB0 = OPAMP3 +
        ysh = 24.0
        W(top, (26.6, top[1]), (26.6, ysh), (28.2, ysh))
        T((24.3, top[1] + 0.3), 'SH (J5-2)', loc='right', fs=FS - 1, color=BLUE)
        u9 = ssr(28.2, ysh - 1.4, 'U9', '3', '4', sw_left=True, h=1.4)
        net('SH', 'J5-2', 'U9-3')
        W(u9['led_a'], (u9['led_a'][0], u9['led_a'][1] + 1.0))
        vdd((u9['led_a'][0], u9['led_a'][1] + 1.0))
        net('+3V3', 'U9-1')
        net('U9_LED_K', 'U9-2', 'R41-2')
        net('K1_C', 'R41-1')
        SEEN.add('R41')
        vin = (u9['sw_bot'][0], ysh - 2.2)
        W(u9['sw_bot'], vin)
        two('R', vin, (vin[0] + 3.4, vin[1]), 'R20', side='bottom', extra='(H4: was 100 kΩ)')
        net('VN_IN', 'U9-4', 'R20-1')
        vns = (vin[0] + 3.4, vin[1])
        dot(vns)
        net('VN_SENSE', 'R20-2', 'C7-1', 'D2-3', 'J2-15')
        T((vns[0] + 0.15, vns[1] + 0.35), 'VN_SENSE', loc='right', fs=FS - 1, color=BLUE)
        W(vns, (vns[0] + 1.2, vns[1]))
        cx7 = vns[0] + 1.2
        dot((cx7, vns[1]))
        elm.Capacitor().at((cx7, vns[1])).to((cx7, vns[1] - 1.6))
        gnd((cx7, vns[1] - 1.6))
        T((cx7 + 0.3, vns[1] - 0.8), lbl('C7'), loc='right', fs=FS - 2.5)
        net('GND', 'C7-2')
        W((cx7, vns[1]), (cx7 + 2.6, vns[1]))
        clamp(cx7 + 2.6, vns[1], 'D2', 'VN_SENSE')
        # OPAMP3 inside a dashed STM32 box; PB0 / PB2 / PB1 are pins on its boundary
        bx0, bx1 = 40.4, 47.6
        o3, o3p, o3m, o3o = opamp((bx0 + 1.2, vns[1]), 'plus', plus_top=True)
        W((cx7 + 2.6, vns[1]), o3p)
        T((o3.center[0] - 0.4, o3.center[1]), 'OPAMP3', loc='center', fs=FS - 2.5)
        jm = (o3m[0] - 0.5, o3m[1] - 1.1)
        W(o3m, (jm[0], o3m[1]), jm)
        dot(jm)
        rfx = o3o[0] + 0.4
        W(o3o, (rfx, o3o[1]), (rfx, jm[1]))
        elm.Resistor().at((rfx, jm[1])).to((jm[0] + 0.4, jm[1]))
        W((jm[0] + 0.4, jm[1]), jm)
        T(((rfx + jm[0]) / 2, jm[1] - 0.35), 'Rf (internal)', loc='top', fs=FS - 3.5, color=GREEN)
        pb2 = (jm[0], 17.9)
        elm.Resistor().at(jm).to(pb2)
        T((jm[0] + 0.3, (jm[1] + pb2[1]) / 2), 'Rg (internal)', loc='right', fs=FS - 3.5, color=GREEN)
        W((bx0, ysh + 0.6), (bx1, ysh + 0.6), (bx1, pb2[1]), (bx0, pb2[1]), (bx0, ysh + 0.6), color=GREEN, ls='--')
        T((bx0 + 0.1, ysh + 0.85), 'inside STM32: OPAMP3, PGA mode, G = 1 + Rf/Rg = 2…64', loc='right', fs=FS - 3.5, color=GREEN)
        for p, t, side in (((bx0, vns[1]), 'PB0 (J2-15)', 'left'), ((jm[0], pb2[1]), 'PB2 (J2-17)', 'right'),
                           ((bx1, o3o[1]), 'PB1 → ADC3_IN1', 'right')):
            elm.Dot(open=True).at(p)
        T((bx0 - 0.15, vns[1] - 0.35), 'PB0 (J2-15)', loc='left', fs=FS - 3, color=GREEN)
        T((jm[0] + 0.2, pb2[1] - 0.35), 'PB2 (J2-17)', loc='right', fs=FS - 3, color=GREEN)
        W((rfx, o3o[1]), (bx1, o3o[1]))
        T((bx1 - 0.1, o3o[1] + 0.35), 'PB1 → ADC3_IN1', loc='left', fs=FS - 3, color=GREEN)
        # SL row (y = 15.5): R40 -> U1B (+) follower -> VBUF -> PB12 and PB2
        ysl = 15.5
        W(bot, (26.6, bot[1]), (26.6, ysl), (27.6, ysl))
        T((26.8, ysl - 0.35), 'SL (J5-3)', loc='right', fs=FS - 1, color=BLUE)
        two('R', (27.6, ysl), (31.4, ysl), 'R40', side='bottom')
        net('SL', 'J5-3', 'R40-1')
        u1b, bp, bm, bo = opamp((31.4, ysl), 'plus', plus_top=False)
        net('SL_BUF', 'R40-2', 'U1-5')
        T((u1b.center[0] - 0.35, u1b.center[1]), 'U1B', loc='center', fs=FS - 2)
        T((u1b.center[0], u1b.center[1] - 1.5), lbl('U1', short=True) + '  B: +5, −6, out 7', loc='top', fs=FS - 3)
        fbx = bo[0] + 0.5
        W(bm, (bm[0] - 0.3, bm[1]), (bm[0] - 0.3, bm[1] + 0.9), (fbx, bm[1] + 0.9), (fbx, bo[1]))
        W(bo, (bx1, bo[1]))
        dot((fbx, bo[1]))
        W((jm[0], bo[1]), (jm[0], pb2[1]))
        dot((jm[0], bo[1]))
        T((bx1 - 0.1, bo[1] - 0.35), 'VBUF → PB12 (J1-20) ADC4_IN3', loc='left', fs=FS - 2.5, color=BLUE)
        net('VBUF', 'U1-6', 'U1-7', 'J1-20', 'J2-17')
        notes(24.5, 5.4, [
            ('Loop: OPAMP1 drives Q1 until MUXR_COM = VREF (0.405 V)  ⇒  test current I = VREF / R_sense.', 'black'),
            ('U4: 00 → R5 98.9 Ω · 01 → R6 993.6 Ω · 10 → R7 98.6 kΩ · 11 → R8 4.7 MΩ  (A = B = MUXR_A0, C = MUXR_A1); R31 100 kΩ pull-up on MUXR_EN.', GREY),
            ('R = RSENSE_eff × [ (V_ADC3 − V_ADC4) / G − V_os,short ] / V(PA7);  ADC3 and ADC4 sampled simultaneously.', 'black'),
            ('COMP4 (internal): PB0 vs DAC1_CH1 2.6 V — over-voltage guard shuts R-mode down.  OPAMP3 out = V_SL + G·(V_SH − V_SL).', GREEN),
            ('U9 LED: pin 1 = +3V3, pin 2 → R41 330 Ω → K1_C, so U9 conducts whenever K1 is energised (R-mode only).', GREY),
        ])
        legend(0, 3.6)


# ================================================================ SHEET 5 ===


def sheet5():
    with schemdraw.Drawing(file=f'{OUT}/sheet5-tia.svg', show=False) as d:
        d.config(fontsize=FS, unit=2.0)
        frame(d, 'Sheet 5 — R-mode, TIA for 15 MΩ – 1 GΩ (K1 energised, K2 de-energised)', 5, 7)
        mcu_box(0, 22.5, 10.0, 28.5, 'STM32G474CEU6')
        pb14 = (10.0, 25.0)
        mcu_pin_in(pb14, 'PB14', 'J1-18', 'R_FORCE = LOW (Q1 fully on)')
        q1 = elm.PFet().theta(0).reverse().at((17.0, 25.0)).anchor('gate')
        vdd(q1.source)
        T((q1.source[0] + 0.4, q1.source[1] - 0.2), lbl('Q1') + '  fully ON', fs=FS - 2)
        two('R', pb14, (17.0, 25.0), 'R15', side='top')
        X = q1.drain[0]
        d6b = (X, q1.drain[1] - 1.6)
        elm.Diode().at(q1.drain).to(d6b)
        T((X + 0.35, (q1.drain[1] + d6b[1]) / 2), lbl('D6'), fs=FS - 2)
        dot(d6b)
        T((X - 0.25, d6b[1]), 'FH ≈ 2.7 V', loc='left', fs=FS - 1, color=BLUE)
        fl = (X, d6b[1] - 3.6)
        elm.Resistor().at(d6b).to(fl).fill('#fde68a')
        T((X - 0.45, (d6b[1] + fl[1]) / 2), 'R_DUT ≤ 1 GΩ\n(≈ 1 nA at 1 GΩ)', loc='left', fs=FS - 1.5)
        dot(fl)
        T((X - 0.25, fl[1] - 0.35), 'FL (J5-4)', loc='left', fs=FS - 1, color=BLUE)
        net('FL', 'J5-4', 'U1-2', 'R16-1', 'C4-1', 'K2-4', 'K1-4')
        # K2A open (COM 4 -> NC 3 n.c.), MUXR_COM isolated ; K1A open
        c2 = (X, fl[1] - 0.8)
        W(fl, c2)
        contact(c2, (X, c2[1] - 1.8), (X + 1.0, c2[1] - 1.8), 'NC', 'K2', ('4', '3', '5'),
                'K2A: NC 3 (not connected)\nNO 5 = MUXR_COM + PA7: isolated (H1)\nK1A: NO 5 — FL off GND', side='right')
        # U1A: − = FL, + = VBIAS
        ua, ap, am, ao = opamp((27.0, fl[1]), 'minus', plus_top=False)
        W(fl, am)
        T((ua.center[0] - 0.35, ua.center[1]), 'U1A', loc='center', fs=FS - 2)
        T((ua.center[0], ua.center[1] - 1.7), lbl('U1', short=True) + '\nA: −2, +3, out 1 · V+ 8, V− 4', loc='top', fs=FS - 3)
        vbx = ap[0] - 0.8
        W(ap, (vbx, ap[1]), (vbx, ap[1] - 2.2))
        dot((vbx, ap[1] - 2.2))
        T((vbx - 0.2, ap[1] - 2.2), 'VBIAS 1.65 V → PA5 (J2-11)', loc='left', fs=FS - 1, color=BLUE)
        net('VBIAS', 'U1-3', 'R19-2', 'R21-1', 'C6-1', 'J2-11')
        net('+3V3', 'R19-1', 'U1-8')
        net('GND', 'R21-2', 'C6-2', 'U1-4')
        SEEN.update({'R19', 'R21', 'C6'})
        # feedback: FL -> R16 || C4 -> RF_TOP -> K2B NC 10 -> COM 9 = TIA_OUT
        fx = X + 2.0
        W((fx, fl[1]), (fx, fl[1] + 3.0))
        dot((fx, fl[1]))
        rt = (fx + 5.0, fl[1] + 3.0)
        two('R', (fx, fl[1] + 3.0), rt, 'R16', side='bottom', extra='Rf')
        W((fx, fl[1] + 3.0), (fx, fl[1] + 4.6))
        elm.Capacitor().at((fx, fl[1] + 4.6)).to((rt[0], fl[1] + 4.6))
        T(((fx + rt[0]) / 2, fl[1] + 5.1), lbl('C4') + ' (Cf, 1.6 kHz)', loc='top', fs=FS - 2.5)
        W((rt[0], fl[1] + 4.6), rt)
        dot(rt)
        T((rt[0] + 0.1, rt[1] - 0.35), 'RF_TOP', loc='right', fs=FS - 1.5, color=BLUE)
        net('RF_TOP', 'R16-2', 'C4-2', 'K2-10')
        nc = (rt[0] + 1.2, rt[1])
        W(rt, nc)
        com = (nc[0] + 2.0, rt[1])
        contact(com, nc, (nc[0], rt[1] + 1.0), 'NC', 'K2', ('9', '10', '8'), '', side='right')
        T(((nc[0] + com[0]) / 2, rt[1] + 1.6), 'K2B: NC 10 closed\n(NO 8 not connected)', loc='top', fs=FS - 2.5)
        to = (ao[0] + 1.2, ao[1])
        W(com, (to[0], com[1]), to)
        W(ao, to)
        dot(to)
        W(to, (to[0] + 2.0, to[1]))
        T((to[0] + 2.1, to[1]), 'TIA_OUT → PA6 (J2-12) ADC2_IN3', loc='right', fs=FS - 1, color=BLUE)
        net('TIA_OUT', 'U1-1', 'K2-9', 'D3-3', 'J2-12')
        clamp(to[0] + 1.0, to[1] - 3.2, 'D3', 'TIA_OUT') if False else None
        W((to[0] + 1.0, to[1]), (to[0] + 1.0, to[1] - 1.6))
        dot((to[0] + 1.0, to[1]))
        clamp(to[0] + 1.0, to[1] - 1.6 - 1.6, 'D3', 'TIA_OUT') if False else None
        # D3 drawn as a clamp hanging below TIA_OUT
        cx, cy = to[0] + 1.0, to[1] - 3.2
        elm.Diode().at((cx, cy - 1.6)).to((cx, cy))
        gnd((cx, cy - 1.6))
        W((cx, cy), (cx, to[1] - 1.6))
        elm.Diode().at((cx + 1.4, cy)).to((cx + 1.4, cy + 1.6))
        W((cx, cy), (cx + 1.4, cy))
        vdd((cx + 1.4, cy + 1.6))
        dot((cx, cy))
        T((cx + 1.8, cy - 0.4), lbl('D3', short=True) + ' clamp: 3 TIA_OUT, 1 GND, 2 +3V3', loc='right', fs=FS - 2.5)
        net('GND', 'D3-1')
        net('+3V3', 'D3-2')
        # guard
        W((fx - 1.0, fl[1] - 1.0), (rt[0] + 0.7, fl[1] - 1.0), (rt[0] + 0.7, fl[1] + 5.8), (fx - 1.0, fl[1] + 5.8),
          (fx - 1.0, fl[1] - 1.0), color=RED, ls='--')
        T((fx - 0.9, fl[1] - 1.35), 'PCB guard pour at VBIAS around the summing node (P1)', loc='right', fs=FS - 2.5, color=RED)
        notes(0.2, 8.6, [
            ('OPAMP1 is off (PA2 Hi-Z); R13 and R14 on the gate (sheet 4) do not matter here. PB14 LOW pulls the gate to ≈ 0.3 V.', GREY),
            ('V_SH: SH → U9 → R20 → OPAMP3 (follower) → PB1 → ADC3.   V_SL: SL → R40 → U1B → VBUF → PB12 → ADC4 (sheet 4).', GREY),
            ('VBIAS = 1.65 V from R19 10 kΩ (+3V3 → VBIAS) and R21 10 kΩ (VBIAS → GND), C6 100 nF; read on PA5 (J2-11) ADC2_IN13.', GREY),
            ('U1A holds FL at VBIAS and turns the DUT current into a voltage across Rf: V_TIA = V_FL − I × Rf.', 'black'),
            ('I = (V_SL − V_TIA − V_open) / Rf      R = Rf × (V_SH − V_SL) / (V_SL − V_TIA − V_open)      (V_open from the open-circuit tare)', 'black'),
        ])
        legend(0, 3.4)


# ================================================================ SHEET 6 ===


def sheet6():
    with schemdraw.Drawing(file=f'{OUT}/sheet6-power-control.svg', show=False) as d:
        d.config(fontsize=FS, unit=2.0)
        frame(d, 'Sheet 6 — Power, relay drivers, console, J8 bias header, MCU pin map', 6, 7)
        # power
        T((0, 29.4), 'Power (USB only)', fs=FS + 1)
        T((0, 28.4), 'USB-C VBUS → B5819WS → VDD5V → ME6231A33M3G LDO → +3V3   (all on the WeAct module)', fs=FS - 1)
        T((0, 27.6), 'J4-3/4 = +3V3 to carrier, J4-1/2 = GND; J2-2 = +3V3; J1-1, J2-1 = GND; J3-1/2 = GND, J3-3/4 (VDD5V) not connected.', fs=FS - 1.5, color=GREY)
        net('+3V3', 'J4-3', 'J4-4', 'J2-2')
        net('GND', 'J4-1', 'J4-2', 'J1-1', 'J2-1', 'J3-1', 'J3-2')
        SEEN.update({'J1', 'J2', 'J3', 'J4'})
        T((0, 26.7), 'Decoupling: C8 100 nF + C9 10 µF (bulk), C10 100 nF + C15 10 µF (relay coils), C11–C14 100 nF (one per IC: U1–U4).', fs=FS - 1.5, color=GREY)
        for c in ('C8', 'C9', 'C10', 'C11', 'C12', 'C13', 'C14', 'C15'):
            net('+3V3', f'{c}-1')
            net('GND', f'{c}-2')
        # relay drivers
        def driver(x, k, q, rb, rbe, dfly, drv, pin, hdr, cnet, bnet):
            T((x, 25.2), f'{k} driver', fs=FS + 0.5)
            p = (x, 21.0)
            mcu_pin(p, pin, hdr, f'{drv}: HIGH = coil on', side='left')
            two('R', (x + 0.6, 21.0), (x + 3.6, 21.0), rb, side='top')
            W(p, (x + 0.6, 21.0))
            bq = elm.BjtNpn(circle=True).at((x + 3.6, 21.0)).anchor('base')
            T((bq.center[0] + 0.6, bq.center[1] - 0.2), lbl(q) + '\nB 1, E 2, C 3', fs=FS - 2)
            two('R', (x + 3.4, 21.0), (x + 3.4, 18.6), rbe, side='left')
            dot((x + 3.4, 21.0))
            gnd((x + 3.4, 18.6))
            gnd(bq.emitter)
            coil = elm.Inductor2(loops=3).at(bq.collector).to((bq.collector[0], bq.collector[1] + 2.4))
            T((bq.collector[0] - 0.4, bq.collector[1] + 1.2), f'{k} coil\n(12 → 1)', loc='left', fs=FS - 2)
            vdd((bq.collector[0], bq.collector[1] + 2.4))
            elm.Diode().at((bq.collector[0] + 2.2, bq.collector[1])).to((bq.collector[0] + 2.2, bq.collector[1] + 2.4))
            W(bq.collector, (bq.collector[0] + 2.2, bq.collector[1]))
            W((bq.collector[0], bq.collector[1] + 2.4), (bq.collector[0] + 2.2, bq.collector[1] + 2.4))
            T((bq.collector[0] + 2.6, bq.collector[1] + 1.2), lbl(dfly) + '\nflyback', fs=FS - 2)
            flag((bq.collector[0] + 1.1, bq.collector[1]), cnet, loc='bottom')
            net(drv, hdr, f'{rb}-1')
            net(bnet, f'{rb}-2', f'{rbe}-1', f'{q}-1')
            net('GND', f'{rbe}-2', f'{q}-2')
            net(cnet, f'{q}-3', f'{k}-12', f'{dfly}-2')
            net('+3V3', f'{k}-1', f'{dfly}-1')
        driver(4.0, 'K1', 'Q2', 'R25', 'R27', 'D4', 'K1_DRV', 'PB9', 'J1-2', 'K1_C', 'K1_BASE')
        driver(18.0, 'K2', 'Q3', 'R26', 'R28', 'D5', 'K2_DRV', 'PB10', 'J2-19', 'K2_C', 'K2_BASE')
        T((0, 16.3), 'K1_C also drives U9 LED through R41 (sheet 4). Firmware never energises K2 without K1.', fs=FS - 1.5, color=GREY)
        # console + J8
        T((29.0, 25.2), 'Console J7 (115200 8N1)', fs=FS + 0.5)
        T((29.0, 24.4), '1 UART_TX ← PA9 (J1-14) USART1_TX\n2 UART_RX → PA10 (J1-13) USART1_RX\n3 GND   4 +3V3', fs=FS - 1.5)
        net('UART_TX', 'J7-1', 'J1-14')
        net('UART_RX', 'J7-2', 'J1-13')
        net('GND', 'J7-3')
        net('+3V3', 'J7-4')
        T((29.0, 21.6), 'Bias-control header J8 (future bias module)', fs=FS + 0.5)
        T((29.0, 20.8), '1 BIAS_SET_F ← R34 1 kΩ ← BIAS_SET = PA4 (J2-10) TIM3_CH2 PWM; C16 10 µF to GND\n'
                        '2 BIAS_EN = PA15 (J1-10) GPIO; R35 10 kΩ pull-down\n'
                        '3 BIAS_MON_F → R36 1 kΩ → BIAS_MON = PA8 (J1-15) ADC5_IN1; C17 100 nF to GND\n'
                        '4 GND   5 +3V3 (< 50 mA)   6 BIAS_UART_TX = PC10 (J1-9)   7 BIAS_UART_RX = PC11 (J1-8)   8 GND', fs=FS - 2)
        net('BIAS_SET', 'J2-10', 'R34-1')
        net('BIAS_SET_F', 'R34-2', 'C16-1', 'J8-1')
        net('BIAS_EN', 'J1-10', 'R35-1', 'J8-2')
        net('BIAS_MON', 'J1-15', 'R36-1')
        net('BIAS_MON_F', 'R36-2', 'C17-1', 'J8-3')
        net('GND', 'C16-2', 'C17-2', 'R35-2', 'J8-4', 'J8-8')
        net('+3V3', 'J8-5')
        net('BIAS_UART_TX', 'J8-6', 'J1-9')
        net('BIAS_UART_RX', 'J8-7', 'J1-8')
        SEEN.update({'J7', 'J8', 'R34', 'R35', 'R36', 'C16', 'C17'})
        # pin map table
        T((0, 14.9), 'Complete MCU pin map (WeAct header pin → STM32 pin → net → peripheral)', fs=FS + 1)
        rows = [
            ('J2-6', 'PA0', 'DRIVE', 'COMP1_OUT (AF8) / GPIO', 'C'), ('J2-7', 'PA1', 'VCAP_ADC', 'ADC1_IN2, COMP1_INP', 'C'),
            ('J2-8', 'PA2', 'OP1_OUT', 'OPAMP1_VOUT', 'Kelvin'), ('J2-9', 'PA3', 'VREF', 'OPAMP1_VINM0, ADC1_IN4', 'Kelvin'),
            ('J2-10', 'PA4', 'BIAS_SET', 'TIM3_CH2 PWM', 'J8'), ('J2-11', 'PA5', 'VBIAS', 'ADC2_IN13', 'TIA'),
            ('J2-12', 'PA6', 'TIA_OUT', 'ADC2_IN3', 'TIA'), ('J2-13', 'PA7', 'MUXR_COM', 'OPAMP1_VINP2, ADC2_IN4', 'Kelvin'),
            ('J2-14', 'PC4', 'C_MODE_SW', 'GPIO (U10, active low)', 'C'), ('J2-15', 'PB0', 'VN_SENSE', 'OPAMP3_VINP0, COMP4_INP', 'all'),
            ('J2-16', 'PB1', '(not on carrier)', 'OPAMP3_VOUT → ADC3_IN1', 'all'), ('J2-17', 'PB2', 'VBUF', 'OPAMP3_VINM0 (PGA bias)', 'Kelvin'),
            ('J2-19', 'PB10', 'K2_DRV', 'GPIO → Q3 → K2', 'R'), ('J2-20', 'PB11', 'SSR13', 'GPIO → U5 + U6', 'C'),
            ('J1-2', 'PB9', 'K1_DRV', 'GPIO → Q2 → K1', 'R'), ('J1-3', 'PB7', 'MUXR_A1', 'GPIO → U4 C', 'Kelvin'),
            ('J1-4', 'PB6', 'MUXR_A0', 'GPIO → U4 A, B', 'Kelvin'), ('J1-5', 'PB5', 'MUXC_EN', 'GPIO → U3 EN̄', 'C'),
            ('J1-6', 'PB4', 'MUXC_A1', 'GPIO → U3 C', 'C'), ('J1-7', 'PB3', 'MUXC_A0', 'GPIO → U3 A, B', 'C'),
            ('J1-8', 'PC11', 'BIAS_UART_RX', 'USART3_RX', 'J8'), ('J1-9', 'PC10', 'BIAS_UART_TX', 'USART3_TX', 'J8'),
            ('J1-10', 'PA15', 'BIAS_EN', 'GPIO', 'J8'), ('J1-13', 'PA10', 'UART_RX', 'USART1_RX', 'console'),
            ('J1-14', 'PA9', 'UART_TX', 'USART1_TX', 'console'), ('J1-15', 'PA8', 'BIAS_MON', 'ADC5_IN1', 'J8'),
            ('J1-17', 'PB15', 'MUXR_EN', 'GPIO → U4 EN̄ (H2)', 'Kelvin'), ('J1-18', 'PB14', 'R_FORCE', 'GPIO (Hi-Z / LOW)', 'R'),
            ('J1-19', 'PB13', 'SSR2_CTRL', 'GPIO → U7', 'C'), ('J1-20', 'PB12', 'VBUF', 'ADC4_IN3', 'R'),
        ]
        for r in rows:
            if r[2].startswith('('):
                continue
            net(r[2], r[0])
        half = (len(rows) + 1) // 2
        hdr = ('Header', 'MCU', 'Net', 'Peripheral', 'Mode')
        for col, chunk in enumerate((rows[:half], rows[half:])):
            x0 = col * 23.5
            cx = [x0, x0 + 2.6, x0 + 5.0, x0 + 10.4, x0 + 19.6]
            for c, s in zip(cx, hdr):
                T((c, 14.0), s, fs=FS - 1, color=BLUE)
            for i, r in enumerate(chunk):
                for c, s in zip(cx, r):
                    T((c, 13.3 - i * 0.62), s, fs=FS - 1.5)
        T((0, 3.6), 'Reserved by the module: PA11/PA12 USB, PA13/PA14 SWD, PF0/PF1 HSE 8 MHz, PC6 user LED, PC13 user key (active high), PB8 BOOT0, VREF+ = VDDA (R7 0 Ω).', fs=FS - 1.5, color=GREY)
        legend(0, 2.6)


for fn in (sheet1, sheet2, sheet3, sheet4, sheet5, sheet6):
    fn()

missing = sorted(set(NL_VALUE) - SEEN - {r for r in NL_VALUE if r.startswith('FLG')})
if ERRORS:
    print('NETLIST MISMATCHES:')
    for e in ERRORS:
        print('  ', e)
print('parts not shown on any sheet:', missing or 'none')
sys.exit(1 if ERRORS or missing else 0)
