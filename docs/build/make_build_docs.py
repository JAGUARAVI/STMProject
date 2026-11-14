"""Generate the ESWCap Rev C.1 build pack from the verified design data.

    python3 docs/build/make_build_docs.py <board.json> <kicad-netlist.net> <placement.png> <outdir>

board.json comes from the routed PCB (part positions, pad coordinates, pad
nets); the netlist comes from the schematic. Every pad net is checked against
the netlist before anything is written, so the wire list and the wiring map
are the schematic, laid out like the PCB.

Outputs (in outdir):
  ESWCap-wiring-map.drawio   editable diagram, one draw.io layer per circuit block
  ESWCap-wiring-map.pdf      overview page + one page per block
  ESWCap-wire-list.csv       numbered point-to-point wires (+ rail connections)
  ESWCap-build-guide.pdf     instructions (PCB assembly and hand-wired prototype)
"""
import csv
import html
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile

BOARD, NETLIST, PLACEMENT_PNG, OUT = sys.argv[1:5]
os.makedirs(OUT, exist_ok=True)

# ------------------------------------------------------------------ data ---
parts = json.load(open(BOARD))
PART = {p['ref']: p for p in parts}


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


root = sexpr(open(NETLIST).read())
NL = {}
for n in _f(_f(root, 'nets')[0], 'net'):
    for x in _f(n, 'node'):
        NL[(_v(x, 'ref'), _v(x, 'pin'))] = _v(n, 'name').lstrip('/')
NL_VALUE = {_v(c, 'ref'): _v(c, 'value') for c in _f(_f(root, 'components')[0], 'comp')}

# verification: PCB pads == schematic netlist, part for part, pin for pin
bad = []
for p in parts:
    if NL_VALUE.get(p['ref']) != p['value']:
        bad.append(f"{p['ref']} value PCB '{p['value']}' vs schematic '{NL_VALUE.get(p['ref'])}'")
    for q in p['pads']:
        if not q['num']:
            continue
        want = NL.get((p['ref'], q['num']), '')
        if want != q['net']:
            bad.append(f"{p['ref']}-{q['num']}: PCB {q['net']!r} vs schematic {want!r}")
missing = {r for (r, _) in NL} - set(PART) - {r for r in NL_VALUE if r.startswith('FLG')}
if bad or missing:
    print('VERIFICATION FAILED')
    for b in bad:
        print('  ', b)
    print('   parts missing from PCB:', sorted(missing))
    sys.exit(1)

RAILS = {'GND', '+3V3'}
nets = {}
for p in parts:
    for q in p['pads']:
        if q['net'] and not q['net'].startswith('unconnected'):
            nets.setdefault(q['net'], []).append((p['ref'], q['num'], q['x'], q['y']))

BLOCKS = [
    ('Power, relays & control', '#7c3aed', {'K1_DRV', 'K1_BASE', 'K1_C', 'K2_DRV', 'K2_BASE', 'K2_C', 'UART_TX', 'UART_RX',
                                           'BIAS_SET', 'BIAS_SET_F', 'BIAS_EN', 'BIAS_MON', 'BIAS_MON_F',
                                           'BIAS_UART_TX', 'BIAS_UART_RX'}),
    ('C-mode', '#2563eb', {'DRIVE', 'C_MUX_COM', 'MUXC_A0', 'MUXC_A1', 'MUXC_EN', 'MUXC_P', 'MUXC_Q',
                           'C_CH0', 'C_CH1', 'C_CH2', 'C_CH3', 'VCAP', 'VCAP_ADC', 'C_BLOCK'}),
    ('Bias & safety', '#9a3412', {'V_BIAS', 'SSR13', 'SSR1_LED', 'SSR3_LED', 'SSR2_CTRL', 'SSR2_LED', 'SSR1_SW',
                                  'SSR2_SW', 'SSR3_SW', 'FH', 'SH_DIV', 'U10_LED_K', 'C_MODE_SW'}),
    ('R-mode Kelvin', '#047857', {'Q1_GATE', 'Q1_D', 'OP1_OUT', 'VREF', 'R_FORCE', 'MUXR_A0', 'MUXR_A1', 'MUXR_EN',
                                  'MUXR_P', 'MUXR_Q', 'MUXR_COM', 'R_CH0', 'R_CH1', 'R_CH2', 'R_CH3', 'SH', 'VN_IN',
                                  'VN_SENSE', 'SL', 'SL_BUF', 'VBUF', 'U9_LED_K'}),
    ('R-mode TIA', '#ea580c', {'FL', 'RF_TOP', 'TIA_OUT', 'VBIAS'}),
]
SENSITIVE = {'FH', 'FL', 'SH', 'SL', 'SH_DIV', 'SL_BUF', 'VN_IN', 'VN_SENSE', 'VCAP', 'C_BLOCK', 'RF_TOP', 'MUXR_COM', 'VBIAS'}
unassigned = set(nets) - RAILS - set().union(*(b[2] for b in BLOCKS))
assert not unassigned, f'nets without a block: {unassigned}'


def block_of(net):
    for name, color, s in BLOCKS:
        if net in s:
            return name, color
    raise KeyError(net)


def mst(pins):
    """Prim's minimum spanning tree over pin positions -> list of (a, b, length_mm)."""
    if len(pins) < 2:
        return []
    inside, edges = {0}, []
    while len(inside) < len(pins):
        best = None
        for i in inside:
            for j in range(len(pins)):
                if j in inside:
                    continue
                d = math.hypot(pins[i][2] - pins[j][2], pins[i][3] - pins[j][3])
                if best is None or d < best[2]:
                    best = (i, j, d)
        inside.add(best[1])
        edges.append((pins[best[0]], pins[best[1]], best[2]))
    return edges


wires = []      # dicts
n = 0
for bname, color, bset in BLOCKS:
    for net in sorted(bset):
        if net not in nets:
            continue
        for a, b, d in mst(sorted(nets[net])):
            n += 1
            wires.append({'no': n, 'block': bname, 'color': color, 'net': net, 'a': f'{a[0]}-{a[1]}', 'b': f'{b[0]}-{b[1]}',
                          'len_mm': round(d, 1), 'sensitive': net in SENSITIVE,
                          'ax': a[2], 'ay': a[3], 'bx': b[2], 'by': b[3]})
rail_conns = []
for rail in ('GND', '+3V3'):
    for r, pnum, x, y in sorted(nets[rail]):
        rail_conns.append({'rail': rail, 'pin': f'{r}-{pnum}', 'x': x, 'y': y})

# connectivity self-check: the wires + rails must reproduce every net exactly
recon = {}
for w in wires:
    recon.setdefault(w['net'], set()).update({w['a'], w['b']})
for rc in rail_conns:
    recon.setdefault(rc['rail'], set()).add(rc['pin'])
for net, pins in nets.items():
    want = {f'{r}-{p}' for r, p, _, _ in pins}
    got = recon.get(net, set()) if len(pins) > 1 or net in RAILS else want
    assert got == want, (net, want ^ got)
print(f'verified: {len(parts)} parts, {len(nets)} nets, {len(wires)} wires, {len(rail_conns)} rail connections')

# ------------------------------------------------------------ wire list ---
with open(os.path.join(OUT, 'ESWCap-wire-list.csv'), 'w', newline='') as fh:
    w = csv.writer(fh)
    w.writerow(['Wire', 'Block', 'Net', 'From', 'To', 'Length on PCB (mm)', 'Keep short (sensitive analog)', 'Done'])
    for x in wires:
        w.writerow([x['no'], x['block'], x['net'], x['a'], x['b'], x['len_mm'], 'YES' if x['sensitive'] else '', ''])
    for rc in rail_conns:
        w.writerow(['', 'Rail', rc['rail'], rc['pin'], f"{rc['rail']} rail", '', '', ''])

# ---------------------------------------------------------- wiring map ---
S = 16.0           # px per mm
OX, OY = 40, 90    # canvas offset
BW, BH = 111 * S, 85 * S
LEG_X = OX + BW + 40


def X(v):
    return OX + v * S


def Y(v):
    return OY + v * S


SHOW_PIN_NUMBERS = lambda p: len(p['pads']) > 2 or p['ref'][0] == 'D'


def pad_label(p, q):
    if len(p['pads']) == 2 and p['ref'][0] == 'D':
        return 'K' if q['num'] == '1' else 'A'
    return q['num']


def short_value(p):
    v = p['value']
    return v if len(v) <= 12 else v[:11] + '…'


# --- draw.io XML ---
cells = []
cid = [100]


def new_id():
    cid[0] += 1
    return f'c{cid[0]}'


layer_ids = {'Parts': 'L_parts'}
for bname, _, _ in BLOCKS:
    layer_ids[bname] = 'L_' + re.sub(r'\W+', '_', bname)
pad_id = {}

esc = html.escape
cells.append('<mxCell id="0"/>')
for name, lid in layer_ids.items():
    cells.append(f'<mxCell id="{lid}" value="{esc(name)}" parent="0"/>')
cells.append(f'<mxCell id="{new_id()}" value="ESWCap C+R Rev C.1 — wiring map (parts placed as on the PCB; wires = numbered, coloured by block; '
             f'black pins = GND rail, red pins = +3V3 rail)" style="text;fontSize=16;fontStyle=1;align=left;" vertex="1" parent="L_parts">'
             f'<mxGeometry x="{OX}" y="20" width="1700" height="30" as="geometry"/></mxCell>')
cells.append(f'<mxCell id="{new_id()}" value="" style="rounded=0;fillColor=none;strokeColor=#6b7280;dashed=1;" vertex="1" parent="L_parts">'
             f'<mxGeometry x="{OX}" y="{OY}" width="{BW}" height="{BH}" as="geometry"/></mxCell>')
for p in parts:
    x0, y0, x1, y1 = p['bbox']
    style = 'rounded=1;arcSize=6;fillColor=#f3f4f6;strokeColor=#374151;fontSize=8;verticalAlign=top;align=center;spacingTop=1;'
    if p['ref'] in ('J1', 'J2', 'J3', 'J4'):
        style = style.replace('#f3f4f6', '#ecfdf5')
    label = esc(f"{p['ref']}<br>{esc(short_value(p))}")
    cells.append(f'<mxCell id="P_{p["ref"]}" value="{label}" style="{style}html=1;" vertex="1" parent="L_parts">'
                 f'<mxGeometry x="{X(x0):.1f}" y="{Y(y0):.1f}" width="{(x1 - x0) * S:.1f}" height="{(y1 - y0) * S:.1f}" as="geometry"/></mxCell>')
    for q in p['pads']:
        rad = 5 if q['tht'] else 3.5
        fill = '#111827' if q['net'] == 'GND' else '#ef4444' if q['net'] == '+3V3' else '#ffffff'
        pid = f'D_{p["ref"]}_{q["num"] or "x"}'
        pad_id[f'{p["ref"]}-{q["num"]}'] = pid
        lab = esc(pad_label(p, q)) if SHOW_PIN_NUMBERS(p) else ''
        cells.append(f'<mxCell id="{pid}" value="{lab}" style="ellipse;fillColor={fill};strokeColor=#111827;fontSize=7;'
                     f'labelPosition=right;verticalLabelPosition=top;align=left;verticalAlign=bottom;" vertex="1" parent="L_parts">'
                     f'<mxGeometry x="{X(q["x"]) - rad:.1f}" y="{Y(q["y"]) - rad:.1f}" width="{2 * rad}" height="{2 * rad}" as="geometry"/></mxCell>')
for wr in wires:
    lid = layer_ids[wr['block']]
    width = 2.2 if wr['sensitive'] else 1.4
    cells.append(f'<mxCell id="W{wr["no"]}" value="{wr["no"]}" style="endArrow=none;html=1;strokeColor={wr["color"]};strokeWidth={width};'
                 f'fontSize=8;fontColor={wr["color"]};labelBackgroundColor=#ffffff;" edge="1" parent="{lid}" '
                 f'source="{pad_id[wr["a"]]}" target="{pad_id[wr["b"]]}"><mxGeometry relative="1" as="geometry"/></mxCell>')
# legend
ly = OY
legend = [('Legend', '#111827', True)] + [(f'{b[0]} wires', b[1], False) for b in BLOCKS] + [
    ('thick line = sensitive analog: keep short', '#111827', False),
    ('● black pin = connect to GND rail', '#111827', False), ('● red pin = connect to +3V3 rail', '#ef4444', False),
    ('numbers on pins = pin numbers (K/A = diode)', '#111827', False),
    ('wire numbers = ESWCap-wire-list.csv', '#111827', False),
    ('toggle blocks: Extras ▸ Layers (Ctrl+Shift+L)', '#111827', False)]
for text, color, bold in legend:
    cells.append(f'<mxCell id="{new_id()}" value="{esc(text)}" style="text;fontSize=12;fontColor={color};align=left;{"fontStyle=1;" if bold else ""}" '
                 f'vertex="1" parent="L_parts"><mxGeometry x="{LEG_X}" y="{ly}" width="420" height="22" as="geometry"/></mxCell>')
    ly += 26
drawio = ('<mxfile host="ESWCap generator" version="24.0.0"><diagram id="wiring" name="Wiring map">'
          f'<mxGraphModel dx="1600" dy="1000" grid="0" gridSize="10" guides="1" tooltips="1" connect="0" arrows="0" fold="1" '
          f'page="1" pageScale="1" pageWidth="{int(LEG_X + 460)}" pageHeight="{int(OY + BH + 60)}" math="0" shadow="0"><root>'
          + ''.join(cells) + '</root></mxGraphModel></diagram></mxfile>')
open(os.path.join(OUT, 'ESWCap-wiring-map.drawio'), 'w').write(drawio)


# --- SVG rendering of the same geometry (for the PDF) ---
def svg_page(title, focus=None):
    W, H = int(LEG_X + 460), int(OY + BH + 60)
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" font-family="DejaVu Sans, sans-serif">',
         f'<rect width="{W}" height="{H}" fill="white"/>',
         f'<text x="{OX}" y="45" font-size="20" font-weight="bold">{esc(title)}</text>',
         f'<text x="{OX}" y="70" font-size="12" fill="#374151">Parts are placed as on the PCB (top view). Each numbered line is one wire '
         f'(ESWCap-wire-list.csv). Black pins → GND rail, red pins → +3V3 rail.</text>',
         f'<rect x="{OX}" y="{OY}" width="{BW}" height="{BH}" fill="none" stroke="#6b7280" stroke-dasharray="6,4"/>']
    for p in parts:
        x0, y0, x1, y1 = p['bbox']
        fill = '#ecfdf5' if p['ref'] in ('J1', 'J2', 'J3', 'J4') else '#f3f4f6'
        o.append(f'<rect x="{X(x0):.1f}" y="{Y(y0):.1f}" width="{(x1 - x0) * S:.1f}" height="{(y1 - y0) * S:.1f}" rx="3" fill="{fill}" stroke="#374151"/>')
        o.append(f'<text x="{X((x0 + x1) / 2):.1f}" y="{Y(y0) + 10:.1f}" font-size="9" text-anchor="middle" font-weight="bold">{esc(p["ref"])}</text>')
        o.append(f'<text x="{X((x0 + x1) / 2):.1f}" y="{Y(y0) + 19:.1f}" font-size="7" text-anchor="middle" fill="#374151">{esc(short_value(p))}</text>')
    for wr in wires:
        on = focus is None or wr['block'] == focus
        col = wr['color'] if on else '#d1d5db'
        wd = (2.2 if wr['sensitive'] else 1.4) if on else 0.8
        o.append(f'<line x1="{X(wr["ax"]):.1f}" y1="{Y(wr["ay"]):.1f}" x2="{X(wr["bx"]):.1f}" y2="{Y(wr["by"]):.1f}" stroke="{col}" stroke-width="{wd}"/>')
    for p in parts:
        for q in p['pads']:
            rad = 5 if q['tht'] else 3.5
            fill = '#111827' if q['net'] == 'GND' else '#ef4444' if q['net'] == '+3V3' else '#ffffff'
            o.append(f'<circle cx="{X(q["x"]):.1f}" cy="{Y(q["y"]):.1f}" r="{rad}" fill="{fill}" stroke="#111827" stroke-width="0.8"/>')
            if SHOW_PIN_NUMBERS(p):
                o.append(f'<text x="{X(q["x"]) + rad + 1:.1f}" y="{Y(q["y"]) - rad:.1f}" font-size="7" fill="#374151">{esc(pad_label(p, q))}</text>')
    for wr in wires:
        if focus is not None and wr['block'] != focus:
            continue
        mx, my = X((wr['ax'] + wr['bx']) / 2), Y((wr['ay'] + wr['by']) / 2)
        o.append(f'<rect x="{mx - 9:.1f}" y="{my - 7:.1f}" width="18" height="11" fill="white" opacity="0.85"/>')
        o.append(f'<text x="{mx:.1f}" y="{my + 2:.1f}" font-size="8" text-anchor="middle" fill="{wr["color"]}" font-weight="bold">{wr["no"]}</text>')
    ly = OY
    for text, color, bold in legend[:-1]:
        o.append(f'<text x="{LEG_X}" y="{ly + 14}" font-size="13" fill="{color}" {"font-weight=\"bold\"" if bold else ""}>{esc(text)}</text>')
        ly += 26
    if focus:
        ly += 10
        o.append(f'<text x="{LEG_X}" y="{ly + 14}" font-size="13" font-weight="bold">Wires on this page ({esc(focus)}):</text>')
        ly += 24
        for wr in [w for w in wires if w['block'] == focus]:
            if ly > OY + BH - 10:
                o.append(f'<text x="{LEG_X}" y="{ly + 10}" font-size="10" fill="#6b7280">… continued in the wire list</text>')
                break
            mark = ' ⚠' if wr['sensitive'] else ''
            o.append(f'<text x="{LEG_X}" y="{ly + 10}" font-size="10" fill="{wr["color"]}">{wr["no"]:>3}  {esc(wr["a"])} → {esc(wr["b"])}  ({esc(wr["net"])}){mark}</text>')
            ly += 14
    o.append('</svg>')
    return '\n'.join(o)


tmp = tempfile.mkdtemp(prefix='eswcap-build-')
pages = [('ESWCap Rev C.1 — wiring map (all blocks)', None)] + [(f'ESWCap Rev C.1 — wiring map: {b[0]}', b[0]) for b in BLOCKS]
pngs = []
for i, (title, focus) in enumerate(pages):
    svgp = os.path.join(tmp, f'map{i}.svg')
    open(svgp, 'w').write(svg_page(title, focus))
    pngp = os.path.join(tmp, f'map{i}.png')
    subprocess.run(['magick', '-density', '150', svgp, '-background', 'white', '-flatten', '-alpha', 'remove', pngp], check=True)
    pngs.append(pngp)
subprocess.run(['img2pdf', '--pagesize', 'A3^T', '--fit', 'into', *pngs, '-o', os.path.join(OUT, 'ESWCap-wiring-map.pdf')], check=True)
overview_png = pngs[0]

# ----------------------------------------------------------- build guide ---
bom = {}
for p in parts:
    key = (p['value'], p['fp'])
    bom.setdefault(key, []).append(p['ref'])


def refsort(r):
    m = re.match(r'([A-Z]+)(\d+)', r)
    return (m.group(1), int(m.group(2)))


SUB = [
    ('U1', 'LMC6482AIM/NOPB, SOIC-8', 'SOIC-8 → DIP-8 adapter board (keep the SOIC part: 20 fA input current is needed for the TIA)'),
    ('U3, U4', 'MAX4619CSE+, SOIC-16', 'SOIC-16 → DIP-16 adapter, or the DIP-16 version MAX4619CPE+ (same pinout)'),
    ('U2', '74LVC1G34, SOT-23-5', 'SOT-23-5 → DIP adapter'),
    ('Q1', 'AO3401A P-MOSFET, SOT-23', 'SOT-23 → DIP adapter (pins 1 G, 2 S, 3 D)'),
    ('Q2, Q3', 'BC847B NPN, SOT-23', 'BC547B (TO-92). Pin order differs: TO-92 flat face towards you, left→right = C, B, E'),
    ('D1, D2', 'BAV199 low-leakage pair, SOT-23', 'SOT-23 → DIP adapter (do not substitute 1N4148s here: leakage matters on VCAP and VN_SENSE)'),
    ('D3', 'BAT54S Schottky pair, SOT-23', 'SOT-23 → DIP adapter, or two BAT54 / 1N5819 diodes (pin 3 = middle)'),
    ('D4–D6', '1N4148W, SOD-123', '1N4148 (DO-35). Band = cathode = pin 1'),
    ('0805 resistors', 'R1–R8, R12–R43', '¼ W metal-film, same value and tolerance (0.1 % parts for R5–R8, R16, R33, R38)'),
    ('0805 capacitors', 'C2–C21', 'Ceramic, same value: C0G/NP0 for 10 pF, 100 pF, 1 nF; X7R for 68 nF, 100 nF; 10 µF X5R or tantalum'),
    ('R9, R10', '2512', 'R9 47 Ω and R10 100 Ω, 2 W through-hole'),
    ('R11', '1 Ω 0805 (pulse)', '1 Ω, ½ W through-hole'),
    ('U5–U7, U9, U10', 'AQY212EH, DIP-4', 'unchanged (already through-hole)'),
    ('K1, K2', 'G6S-2 DC3', 'unchanged (through-hole, pins on a 2.54 mm grid)'),
    ('C1', '1000 µF 35 V bipolar', 'unchanged (radial)'),
    ('J1–J4, J7, J8, J5, J6', 'headers / terminals', 'unchanged; J5/J6 5.08 mm terminals fit 2.54 mm perfboard'),
]
ORIENT = [
    ('U1, U3, U4 (SOIC)', 'Pin 1 = dot / bevelled edge on the chip ↔ the dot or "1" on the silkscreen'),
    ('U2 (SOT-23-5)', 'Pins 1–3 on one side, 4–5 on the other; match the silkscreen outline'),
    ('Q1, Q2, Q3, D1, D2, D3 (SOT-23)', 'Single pin (pin 3) on one side, two pins on the other; match the outline'),
    ('D4, D5, D6 (SOD-123)', 'Cathode band = pad 1. D4/D5 cathode to +3V3 (flyback); D6 cathode to FH'),
    ('U5, U6, U7, U9, U10 (AQY212EH)', 'Pin 1 = dot/notch = LED anode'),
    ('K1, K2 (G6S-2)', 'Asymmetric pin pattern only fits one way; coil = pins 1 and 12'),
    ('C1 (bipolar)', 'Either way round. Do not use a polarised capacitor here'),
    ('J5 (DUT)', 'Pin 1 = FH (top). Order FH, SH, SL, FL. 2-wire parts: link 1–2 and 3–4'),
    ('J1–J4 (module sockets)', 'J1/J2 pin 1 at the right end, J3/J4 pin 1 at the left; the WeAct module plugs in USB-C facing the right edge'),
]

wire_rows = ''.join(
    f'<tr><td>☐</td><td>{w["no"]}</td><td style="color:{w["color"]}">{esc(w["block"])}</td><td>{esc(w["net"])}</td>'
    f'<td>{esc(w["a"])}</td><td>{esc(w["b"])}</td><td>{w["len_mm"]}</td><td>{"⚠ short" if w["sensitive"] else ""}</td></tr>'
    for w in wires)
rail_rows = ''.join(f'<tr><td>☐</td><td>{esc(r["pin"])}</td><td>{esc(r["rail"])}</td></tr>' for r in rail_conns)
bom_rows = ''.join(
    f'<tr><td>{", ".join(sorted(refs, key=refsort))}</td><td>{esc(v)}</td><td>{esc(fp)}</td><td>{len(refs)}</td></tr>'
    for (v, fp), refs in sorted(bom.items(), key=lambda kv: refsort(sorted(kv[1], key=refsort)[0])))
sub_rows = ''.join(f'<tr><td>{a}</td><td>{esc(b)}</td><td>{esc(c)}</td></tr>' for a, b, c in SUB)
orient_rows = ''.join(f'<tr><td>{esc(a)}</td><td>{esc(b)}</td></tr>' for a, b in ORIENT)
n_sens = sum(1 for w in wires if w['sensitive'])

guide = f'''<html><head><meta charset="utf-8"><title>ESWCap Rev C.1 build guide</title>
<style>
body {{ font-family: "DejaVu Sans", sans-serif; font-size: 9.5pt; }}
h1 {{ font-size: 17pt; }} h2 {{ font-size: 13pt; margin-top: 14pt; }} h3 {{ font-size: 11pt; }}
table {{ border-collapse: collapse; width: 100%; }} td, th {{ border: 1px solid #9ca3af; padding: 2pt 4pt; font-size: 8.5pt; }}
th {{ background: #e5e7eb; }} .stop {{ border: 2px solid #b91c1c; padding: 6pt; background: #fef2f2; }}
</style></head><body>
<h1>ESWCap C+R Meter, carrier board Rev C.1: build guide</h1>
<p>Team Praise Claude · Embedded Systems Workshop · generated 2026-11-14 from the verified design
(<code>hardware/kicad/Project.kicad_sch</code> + <code>Project.kicad_pcb</code>, schematic ↔ PCB parity clean).
{len(parts)} parts, {len(nets)} nets, {len(wires)} point-to-point wires + {len(rail_conns)} rail connections.</p>
<p>Files that come with this guide: <b>ESWCap-wiring-map.drawio</b> (editable, one layer per block),
<b>ESWCap-wiring-map.pdf</b> (printable), <b>ESWCap-wire-list.csv</b> (spreadsheet checklist), and the circuit
diagrams in <code>docs/circuit/ESWCap-circuit-diagrams.pdf</code>.</p>

<table style="border:2px solid #b91c1c"><tr><td style="background:#fef2f2;border:2px solid #b91c1c;font-size:9.5pt"><b>STOP — read before powering anything.</b><br>
1. <b>Firmware:</b> the current firmware drives PB2 HIGH and PB15 LOW at boot. On Rev C.1, PB2 is the output of
U1B (VBUF) and PB15 is the R-mux enable. <b>Do not plug a programmed WeAct module into the assembled board until the
firmware pin-map update (docs/accuracy-plan.md, item F1) is flashed.</b> Tests 1–4 below are done without the module.<br>
2. <b>V_BIAS:</b> leave J6 disconnected until every test passes. Then start at 5 V, never above 20 V.<br>
3. <b>USB power only.</b> The relays draw ≈ 50 mA each from the module's 3.3 V regulator.</td></tr></table>

<h2>1. Parts</h2>
<table><tr><th>References</th><th>Value / part</th><th>Footprint</th><th>Qty</th></tr>{bom_rows}</table>

<h2>2. Route A: soldering onto the PCB</h2>
<ol>
<li>Inspect the bare board. Check for shorts with a multimeter: +3V3 to GND (J4-3 to J4-1) must <b>not</b> beep.</li>
<li>Solder in this order (lowest parts first, so the board sits flat):
 <b>(a)</b> SOIC chips U1, U3, U4, then U2 · <b>(b)</b> SOT-23 parts Q1–Q3, D1–D3 · <b>(c)</b> SOD-123 diodes D4–D6 ·
 <b>(d)</b> 0805 resistors and capacitors · <b>(e)</b> 2512 resistors R9, R10 ·
 <b>(f)</b> AQY212EH SSRs U5–U7, U9, U10 · <b>(g)</b> relays K1, K2 · <b>(h)</b> C1 · <b>(i)</b> J5, J6 terminals ·
 <b>(j)</b> sockets J1–J4, J7 and header J8.</li>
<li>Use the placement map below and match every polarised part to its silkscreen mark (table 2.1).</li>
<li>Wash off the flux around J5, U1, R16/C4, R38/R39 and C1 with isopropyl alcohol and let it dry; flux residue leaks current
 at the 10 MΩ – 1 GΩ level. Do not conformal-coat across those areas.</li>
<li>Tick every part on the BOM table as you fit it.</li>
</ol>
<h3>2.1 Orientation</h3>
<table><tr><th>Parts</th><th>How to orient</th></tr>{orient_rows}</table>
<h3>2.2 Placement map</h3>
<p>See <b>Appendix A</b> (landscape page after the wire list): top side of the PCB with every reference designator.</p>

<h2>3. Route B: hand-wired prototype</h2>
<ul>
<li><b>Use perfboard (2.54 mm), not a solderless breadboard.</b> A breadboard's contact leakage and stray capacitance ruin the
 TIA (≥ 100 MΩ) and pF ranges; perfboard is acceptable for a prototype. Expect the 1 GΩ range to be worse than on the PCB.</li>
<li>Lay the parts out as in the wiring map: it is the PCB placement, so related parts are already next to each other.</li>
<li>Run a <b>GND rail</b> and a <b>+3V3 rail</b> (tinned bus wire) along the board first. Pins drawn black go to GND, red to +3V3
 (rail list at the end).</li>
<li>Then solder the numbered wires from the wire list, ticking each one. Each wire connects two pins of the same net; the list is
 the minimum set of wires that joins every net, so do not add extras.</li>
<li>The {n_sens} wires marked <b>⚠ short</b> carry sensitive analog signals (FH, FL, SH, SL, SH_DIV, VCAP, C_BLOCK, RF_TOP,
 VN_SENSE, MUXR_COM, VBIAS). Keep them short, away from the relay coils, and do not bundle them with the digital wires.</li>
<li>Show one block at a time in draw.io: <i>Extras ▸ Layers</i> (Ctrl+Shift+L), or use the per-block pages of the PDF.</li>
</ul>
<h3>3.1 Through-hole substitutes and adapters</h3>
<table><tr><th>Parts</th><th>On the PCB</th><th>For perfboard</th></tr>{sub_rows}</table>
<h3>3.2 Wiring map</h3>
<p>See <b>Appendix B</b> (the last six A3 pages): an overview with every wire, then one page per block with only that block's wires highlighted and listed. The same drawing is editable in <b>ESWCap-wiring-map.drawio</b>.</p>

<h2>4. Bring-up tests (both routes)</h2>
<ol>
<li><b>Continuity, no power:</b> for every wire in the list, beep from the "From" pin to the "To" pin. +3V3↔GND must not beep.</li>
<li><b>Power without the module:</b> feed 3.3 V from a bench supply into J4-3/4 (+) and J4-1/2 (−), current limit 150 mA.
 Expect a few mA. VREF (U4 side, R17/R18 junction) ≈ 0.405 V, VBIAS (R19/R21 junction) ≈ 1.65 V.</li>
<li><b>Relays without the module:</b> put 3.3 V through 1 kΩ onto J1-2 (K1_DRV): K1 must click; FL (J5-4) must no longer beep
 to GND. Same for J2-19 (K2_DRV) and K2. Never energise K2 without K1 in normal use.</li>
<li><b>SSRs without the module:</b> 3.3 V on J2-20 (SSR13) must make U5 and U6 conduct (V_BIAS↔FH through 47 Ω, VCAP↔GND
 through 1 Ω); 3.3 V on J1-19 must make U7 conduct (FH↔GND through 100 Ω).</li>
<li><b>Module:</b> only after the firmware with the F1 pin-map update is flashed. Plug in, USB power, open the console on J7
 (115200 8N1): expect the banner and <code>SYSCLK 170000000 Hz</code>.</li>
<li><b>First measurements:</b> C-mode with an empty socket (<code>zero</code>), then known capacitors; R-mode only with the
 combined firmware build, starting with a 1 kΩ resistor (V at FL/MUXR_COM ≈ 0.405 V).</li>
<li><b>V_BIAS last:</b> 5 V, then 10 V, then 20 V on J6, with J5 covered.</li>
</ol>

<h2>5. Wire list ({len(wires)} wires)</h2>
<table><tr><th>✓</th><th>#</th><th>Block</th><th>Net</th><th>From</th><th>To</th><th>PCB mm</th><th>Note</th></tr>{wire_rows}</table>
<h3>Rail connections ({len(rail_conns)})</h3>
<table><tr><th>✓</th><th>Pin</th><th>Rail</th></tr>{rail_rows}</table>
</body></html>'''
gpath = os.path.join(tmp, 'ESWCap-build-guide.html')
open(gpath, 'w').write(guide)
subprocess.run(['soffice', '--headless', '--convert-to', 'pdf:writer_web_pdf_Export', '--outdir', tmp, gpath],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
subprocess.run(['img2pdf', '--pagesize', 'A4^T', '--fit', 'into', PLACEMENT_PNG, '-o', os.path.join(tmp, 'appA.pdf')], check=True)
subprocess.run(['pdfunite', os.path.join(tmp, 'ESWCap-build-guide.pdf'), os.path.join(tmp, 'appA.pdf'),
                os.path.join(OUT, 'ESWCap-wiring-map.pdf'), os.path.join(OUT, 'ESWCap-build-guide.pdf')], check=True)
print('wrote', sorted(os.listdir(OUT)))
