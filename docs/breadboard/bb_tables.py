"""Parts list, wire list and the guide PDF for the breadboard build pack."""
import csv
import os
import re
import subprocess
from bb_engine import hs, is_rail
from bb_draw import MCU, ly, lx
import bb_layout

PART = {
    'U1': 'LMC6482: SOIC-8 on a SOIC-8 to DIP-8 adapter, or the DIP-8 part LMC6482AIN',
    'U2': '74LVC1G34 (SOT-23-5) on a SOT-23-6 to DIP-6 adapter. Chip pin 5 sits on adapter pin 6; adapter pin 5 is unused',
    'U3': 'MAX4619CPE+ (DIP-16), or the SOIC part on a 0.3 in SOIC-16 to DIP-16 adapter',
    'U4': 'MAX4619CPE+ (DIP-16), or the SOIC part on a 0.3 in SOIC-16 to DIP-16 adapter',
    'K1': 'Omron G6S-2 DC3 on a relay carrier (section 2)', 'K2': 'Omron G6S-2 DC3 on a relay carrier (section 2)',
    'Q1': 'AO3401A on a SOT-23 to 3-pin in-line adapter (1 gate, 2 source, 3 drain)',
    'Q2': 'BC547B, TO-92, flat face towards column a', 'Q3': 'BC547B, TO-92, flat face towards column a',
    'D1': 'BAV199 on a SOT-23 to 3-pin in-line adapter', 'D2': 'BAV199 on a SOT-23 to 3-pin in-line adapter',
    'D3': 'BAT54S on a SOT-23 to 3-pin in-line adapter',
    'D4': '1N4148, band = pin 1', 'D5': '1N4148, band = pin 1', 'D6': '1N4148, band = pin 1',
    'R9': '47 Ω, 2 W', 'R10': '100 Ω, 2 W', 'R11': '1 Ω, ½ W',
    'C1': '1000 µF 35 V bipolar electrolytic, 7.5 mm lead pitch (either way round)',
    'J5': '4-way 5.08 mm screw terminal (DUT)', 'J6': '2-way 5.08 mm screw terminal (V_BIAS 0-20 V)',
    'J7': '1×4 pin header, long pins (console)', 'J8': '1×8 pin header, long pins (bias control)',
}
for u in ('U5', 'U6', 'U7', 'U9', 'U10'):
    PART[u] = 'AQY212EH, DIP-4'
PINNAME = {'Q2': {'3': 'C', '1': 'B', '2': 'E'}, 'Q3': {'3': 'C', '1': 'B', '2': 'E'},
           'Q1': {'1': 'G', '2': 'S', '3': 'D'}, 'D4': {'1': 'K band', '2': 'A'}, 'D5': {'1': 'K band', '2': 'A'},
           'D6': {'1': 'K band', '2': 'A'}}


def desc(L, ref):
    if ref in PART:
        return PART[ref]
    v = L.value.get(ref, '')
    if ref[0] == 'R':
        return f'{v.replace(" ", " Ω, ", 1) if " " in v else v + " Ω"}, ¼ W metal film'.replace('k Ω', ' kΩ').replace('M Ω', ' MΩ')
    if ref[0] == 'C':
        if v.startswith('10uF'):
            return '10 µF ceramic, or tantalum/electrolytic with + on pin 1'
        return v.replace('nF', ' nF').replace('pF', ' pF') + ' ceramic' + (' (C0G/NP0)' if 'pF' in v or v == '1nF' else '')
    return v


def esc(s):
    return str(s).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def refkey(r):
    m = re.match(r'([A-Z]+)(\d+)', r)
    return (m.group(1), int(m.group(2)))


def write(L, wires, cut, OUT, tmp, NB):
    # ------------------------------------------------------------ CSVs
    rows = []
    for ref in sorted(L.parts, key=refkey):
        P = L.parts[ref]
        for p, h in sorted(P['pins'].items(), key=lambda kv: (len(kv[0]), kv[0])):
            net = L.pinnet[(ref, p)]
            name = PINNAME.get(ref, {}).get(p) or (MCU[ref][int(p) - 1] if ref in MCU else L.pinfn.get((ref, p), ''))
            name = '' if name in ('~', f'Pin_{p}') else name
            rows.append([f'B{h[0]}', ref, desc(L, ref) if ref not in MCU else 'WeAct STM32G474 module header',
                         p, name, 'no connection' if net.startswith('unconnected') else net, hs(h)])
    with open(os.path.join(OUT, 'ESWCap-breadboard-parts.csv'), 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['Board', 'Ref', 'Part to fit', 'Pin', 'Pin name', 'Net', 'Hole', 'Done'])
        w.writerows(r + [''] for r in rows)

    def note(j):
        n = []
        if 'lane' in j:
            n.append('route round the bottom end of the module (rows 25-29), not over it')
        if j['mm'] < 4:
            n.append('bare wire staple')
        return '; '.join(n)
    GROUP = {0: 'Rail links', 1: 'Strip to rail', 2: 'On one board', 3: 'Board to board'}
    with open(os.path.join(OUT, 'ESWCap-breadboard-wires.csv'), 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['Wire', 'Group', 'Net', 'From', 'To', 'Run (mm)', 'Wire to use', 'Note', 'Done'])
        for j in wires:
            w.writerow([f'W{j["no"]}', GROUP[j['cls']], j['net'], hs(j['a']), hs(j['b']), round(j['mm']), cut(j), note(j), ''])

    # ------------------------------------------------------------ guide
    def parts_table(b):
        refs = [r for r in L.parts if r not in MCU and list(L.parts[r]['pins'].values())[0][0] == b]
        refs.sort(key=lambda r: (min(ly(h) for h in L.parts[r]['pins'].values()), refkey(r)))
        out = ['<table><tr><th>☐</th><th>Ref</th><th>Part to fit</th><th>Pin → hole (net)</th></tr>']
        for r in refs:
            P = L.parts[r]
            pins = []
            for p, h in sorted(P['pins'].items(), key=lambda kv: (ly(kv[1]), lx(kv[1]))):
                net = L.pinnet[(r, p)]
                nm = PINNAME.get(r, {}).get(p) or L.pinfn.get((r, p), '')
                nm = '' if nm == '~' else nm
                lab = f'{p} {nm}'.strip() if nm and nm != f'Pin_{p}' else p
                pins.append(f'<b>{esc(lab)}</b> → {esc(hs(h))} ({"no connection" if net.startswith("unconnected") else esc(net)})')
            out.append(f'<tr><td>☐</td><td><b>{r}</b></td><td>{esc(desc(L, r))}</td><td>{"; ".join(pins)}</td></tr>')
        out.append('</table>')
        return '\n'.join(out)

    def module_table():
        out = ['<table><tr><th>Row</th><th>Hole</th><th>Header pin</th><th>MCU pin</th><th>Net</th>'
               '<th>Hole</th><th>Header pin</th><th>MCU pin</th><th>Net</th></tr>']
        left, right = {}, {}
        for ref in MCU:
            for p, h in L.parts[ref]['pins'].items():
                net = L.pinnet[(ref, p)]
                net = '—' if net.startswith('unconnected') else net
                (left if h[1] == 'd' else right)[h[2]] = (hs(h), f'P{ref[1]}-{p}', MCU[ref][int(p) - 1], net)
        for r in sorted(left):
            out.append('<tr><td>%d</td>' % r + ''.join(f'<td>{esc(x)}</td>' for x in left[r] + right[r]) + '</tr>')
        out.append('</table>')
        return '\n'.join(out)

    def wire_table(sel):
        out = ['<table><tr><th>☐</th><th>Wire</th><th>Net</th><th>From</th><th>To</th><th>Wire to use</th><th>Note</th></tr>']
        for j in sel:
            out.append(f'<tr><td>☐</td><td><b>W{j["no"]}</b></td><td>{esc(j["net"])}</td><td>{esc(hs(j["a"]))}</td>'
                       f'<td>{esc(hs(j["b"]))}</td><td>{esc(cut(j))}</td><td>{esc(note(j))}</td></tr>')
        out.append('</table>')
        return '\n'.join(out)

    nw = {c: sum(1 for j in wires if j['cls'] == c) for c in range(4)}
    tl = sorted((r for r, P in L.parts.items() if P.get('rot')), key=refkey)
    turned = ', '.join(tl[:-1]) + ' and ' + tl[-1]
    boards = ''.join(
        f'<h3>Board B{b}: {esc(bb_layout.BOARD_TITLE[b])}</h3>{parts_table(b)}'
        f'<p><b>Wires on B{b}</b> (to a rail, or between two holes of this board):</p>'
        f'{wire_table([j for j in wires if j["cls"] in (1, 2) and (j["a"] if not is_rail(j["a"]) else j["b"])[0] == b])}'
        for b in range(1, NB + 1))
    html = f'''<html><head><meta charset="utf-8"><title>ESWCap breadboard layout</title>
<style>
body {{ font-family: "DejaVu Sans", sans-serif; font-size: 9pt; }}
h1 {{ font-size: 16pt; }} h2 {{ font-size: 12.5pt; margin-top: 12pt; }} h3 {{ font-size: 10.5pt; margin-top: 10pt; }}
table {{ border-collapse: collapse; width: 100%; }} td, th {{ border: 1px solid #9ca3af; padding: 2pt 4pt; font-size: 8pt; vertical-align: top; }}
th {{ background: #e5e7eb; }}
</style></head><body>
<h1>ESWCap C+R meter, Rev C.1: breadboard layout on five 830-point boards</h1>
<p>Generated from <code>hardware/kicad/Project.kicad_sch</code>. The layout was checked by program: the breadboard strips plus
the {len(wires)} wires reproduce every net of the schematic, with no shorts. {len(L.parts)} parts
({len(L.parts) - 4} components and the module's four header rows), {nw[0]} rail links, {nw[1]} strip-to-rail wires,
{nw[2]} wires within a board and {nw[3]} wires between boards. The drawings are at the end of this document.</p>

<table style="border:2px solid #b91c1c"><tr><td style="background:#fef2f2;border:2px solid #b91c1c;font-size:9pt"><b>Read before powering anything.</b><br>
1. <b>Firmware:</b> flash the pin-map update (docs/accuracy-plan.md, item F1) before plugging the WeAct module in. The old firmware
drives PB2 and PB15, which this circuit uses for VBUF and the R-mux enable.<br>
2. <b>V_BIAS:</b> leave J6 disconnected until everything else works. Start at 5 V and never exceed 20 V.<br>
3. <b>USB power only.</b> All + rails are +3V3 from the module, all − rails are GND. Never connect 5 V to a rail.<br>
4. <b>Accuracy:</b> a solderless breadboard leaks and adds stray capacitance. Expect the picofarad ranges and resistances
above about 10 MΩ to read wrong. This build is for checking that the circuit works, not for calibration.</td></tr></table>

<h2>1. How to read the layout</h2>
<ul>
<li><b>Boards.</b> Five standard 830-point breadboards side by side with their long edges touching: <b>B1</b> on the left to
<b>B5</b> on the right. Turn every board so that row 1 is at the top and column a is on the left.</li>
<li><b>Holes.</b> <code>B3:d10</code> means board 3, column d, row 10. In each row, holes a to e are joined together (the left
half-row) and holes f to j are joined together (the right half-row). The two halves are not joined to each other.</li>
<li><b>Rails.</b> Each board has four rails: <code>L+</code> and <code>L−</code> on the left, <code>R+</code> and <code>R−</code>
on the right. Every + (red) rail is +3V3 and every − (blue) rail is GND. Go by the + / − marking printed on your board; the order
of the two rails may be the other way round from the drawing. For a rail, any hole on that rail will do; the drawing uses the nearest.</li>
<li><b>Split rails.</b> On many boards each rail is broken in the middle (the red or blue line has a gap near row 31). If yours
are, join the two halves of every rail with a short wire before you start.</li>
<li><b>If a listed hole is covered</b> (for example your adapter board is wider than shown), use any other free hole in the same
half-row. It is the same electrical point.</li>
<li><b>Rows marked "keep empty"</b> hold a pin that must not connect to anything. Put nothing else in that half-row.</li>
<li><b>Wire colours</b> in the drawing only tell the wires apart. Use red for +3V3 and black for GND if you can.</li>
</ul>

<h2>2. Parts to prepare first</h2>
<ul>
<li><b>Relay carriers (K1, K2).</b> The G6S-2 pin rows are 5.08 mm apart, too narrow to straddle the centre gap. Solder each
relay to a 5-row piece of stripboard or perfboard. Add one header pin beside each relay pin, one hole further out on the same row,
and join it to that relay pin. The two header rows are then 10.16 mm apart and plug into columns <b>d</b> and <b>f</b>. Looking
from the top with pin 1 at the top left, the left row is pins 1, (none), 3, 4, 5 and the right row is 12, (none), 10, 9, 8.
Fit no header pin in the "(none)" positions.</li>
<li><b>SOT-23 parts (Q1, D1, D2, D3)</b> go on SOT-23 to 3-pin in-line adapters, which stand upright in one column.
Adapter boards differ: check with a multimeter which adapter pin reaches which chip pad, then plug it so that chip pin 1, 2 and 3
sit in the holes listed for pins 1, 2 and 3.</li>
<li><b>U2</b> goes on a SOT-23-6 to DIP-6 adapter. The chip has five pins: pins 1 to 3 on adapter pins 1 to 3, pin 4 on adapter
pin 4, pin 5 on adapter pin 6.</li>
<li><b>Q2, Q3 (BC547B).</b> With the flat face towards column a, the legs from the top are C, B, E.</li>
<li><b>Chips are shown from above.</b> The yellow pad is pin 1, at the end with the notch. Most chips have pin 1 at the top
left. {turned} are fitted turned half a turn, with pin 1 at the bottom right; follow the drawing.</li>
<li><b>Long resistors.</b> R9, R10 (2 W) and R12 span 8 to 12 rows; bend the leads to suit.</li>
</ul>

<h2>3. Build order</h2>
<ol>
<li>Line up the boards. Fit the rail links W1 to W{nw[0]} (section 6) and bridge any split rails.</li>
<li>Board by board (section 4): chips, adapters, relay carriers, terminals and headers first, then resistors, capacitors and
diodes, then that board's wires. Tick each line.</li>
<li>Fit the board-to-board wires (section 7). Run wires that start beside the module round its bottom end, never over it.</li>
<li>Before fitting the module: check with a multimeter that + rail to − rail does <b>not</b> beep, and spot-check nets using the
net names printed beside the rows in the drawings.</li>
<li>Plug the module in last, USB-C end over row 1 of B4, pins in columns d and h, rows 1 to 24 (section 5).</li>
</ol>

<h2>4. Parts and wires, board by board</h2>
{boards}

<h2>5. STM32 module on B4 (rows 1 to 24, USB-C at row 1)</h2>
<p>Check the pin names printed on the module against this table before plugging it in.</p>
{module_table()}

<h2>6. Rail links</h2>
{wire_table([j for j in wires if j['cls'] == 0])}

<h2>7. Board-to-board wires</h2>
{wire_table([j for j in wires if j['cls'] == 3])}
</body></html>'''
    gp = os.path.join(tmp, 'guide.html')
    open(gp, 'w').write(html)
    subprocess.run(['soffice', '--headless', '--convert-to', 'pdf:writer_web_pdf_Export', '--outdir', tmp, gp],
                   check=True, capture_output=True)
    subprocess.run(['pdfunite', os.path.join(tmp, 'guide.pdf'), os.path.join(tmp, 'p0.pdf'), os.path.join(tmp, 'p1.pdf'),
                    os.path.join(OUT, 'ESWCap-breadboard-layout.pdf')], check=True)
