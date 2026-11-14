"""Accuracy-plan diagrams for the ESWCap Rev C carrier (explicit coordinates)."""
import sys
import schemdraw
import schemdraw.elements as elm
import schemdraw.logic as logic

schemdraw.use('svg')
OUTDIR = sys.argv[1]
RED = '#b91c1c'
FS = 11


def W(d, *pts, **kw):
    """Polyline through absolute points."""
    for a, b in zip(pts, pts[1:]):
        elm.Line(**kw).at(a).to(b)


def dot(p, text=None, loc='right', color=None, **kw):
    e = elm.Dot().at(p)
    if text:
        e.label(text, loc=loc, color=color, **kw)
    return e


ALIGN = {'right': ('left', 'center'), 'left': ('right', 'center'),
         'top': ('center', 'bottom'), 'bottom': ('center', 'top'),
         'center': ('center', 'center')}


def note(p, text, loc='right', color=None, fs=FS):
    h, v = ALIGN[loc]
    elm.Label().at(p).label(text, color=color, fontsize=fs, halign=h, valign=v)


def force_column(d, x, y_top, dut_label, *, q1_note):
    """+3V3 -> Q1 -> D6 -> FH -> DUT -> bottom node. Returns key points."""
    elm.Vdd().at((x, y_top)).label('+3V3')
    q1 = elm.PFet().theta(0).reverse().at((x, y_top)).anchor('source')
    note((x + 0.3, y_top - 0.75), 'Q1 AO3401' + q1_note)
    elm.Diode().at(q1.drain).to((x, q1.drain[1] - 1.5)).label('D6 1N4148', loc='bottom')
    fh_top = (x, q1.drain[1] - 1.5)
    note((x - 0.3, fh_top[1] - 0.3), 'FH (J5-1)', loc='left', fs=10)
    dut_top = (x, fh_top[1] - 0.8)
    W(d, fh_top, dut_top)
    dot(dut_top)
    dut_bot = (x, dut_top[1] - 2.4)
    elm.Resistor().at(dut_top).to(dut_bot).fill('#fde68a').label(dut_label, loc='bottom')
    dot(dut_bot)
    return q1, dut_top, dut_bot


# --------------------------------------------------------------------------
# 1. Kelvin
# --------------------------------------------------------------------------
with schemdraw.Drawing(file=f'{OUTDIR}/accuracy-kelvin.svg', show=False) as d:
    d.config(fontsize=FS, unit=2.0)
    note((0, 15.2), 'R-mode, 4-wire Kelvin — proposed (changes in red)', fs=14)
    X = 9
    q1, dtop, dbot = force_column(d, X, 13.5, 'DUT', q1_note='')

    # FL -> K2A -> MUXR_COM -> U4 -> Rsense -> GND
    fl = (X, dbot[1] - 0.8)
    W(d, dbot, fl)
    note((X - 0.3, fl[1] + 0.3), 'FL (J5-4)', loc='left', fs=10)
    mc = (X, fl[1] - 1.6)
    elm.Switch().at(fl).to(mc).label('K2A  (closed)', loc='bottom')
    dot(mc)
    note((X + 0.3, mc[1] + 0.05), 'MUXR_COM', loc='right', fs=10)
    u4b = (X, mc[1] - 1.6)
    elm.Switch().at(mc).to(u4b).label('U4 MAX4619\nRon ≈ 10–20 Ω', loc='bottom')
    rsb = (X, u4b[1] - 2.0)
    elm.Resistor().at(u4b).to(rsb).label('R5–R8  0.1 %\n100 / 1k / 100k / 4.7M', loc='bottom')
    elm.Ground().at(rsb)

    # OPAMP1: - = VREF (top), + = MUXR_COM (bottom), out -> R14 -> gate
    op1 = elm.Opamp(leads=True).at((q1.gate[0] - 1.9, q1.gate[1])).anchor('out').right()
    note((op1.center[0] - 0.55, op1.center[1]), 'OPAMP1\n(int.)', loc='center', fs=10)
    elm.Resistor().at((q1.gate[0] - 1.9, q1.gate[1])).to(q1.gate).label('R14 1k', loc='top')
    W(d, op1.in1, (op1.in1[0] - 0.8, op1.in1[1]))
    dot((op1.in1[0] - 0.8, op1.in1[1]), 'VREF 0.405 V  (PA3)\nR17 7.15k / R18 1k', loc='left')
    xr = op1.in2[0] - 0.5
    W(d, op1.in2, (xr, op1.in2[1]), (xr, mc[1]), mc, color=RED)
    note((xr + 0.2, mc[1] + 0.45), 'PA7 (J2-13)  —  H1: moved from FL to MUXR_COM', color=RED, fs=10)

    # SH -> U9 -> R20 -> VN_SENSE -> OPAMP3(+); PGA bias on PB2
    sh = (X + 1.6, dtop[1])
    W(d, dtop, sh)
    dot(sh, 'SH (J5-2)', loc='top')
    u9e = (sh[0] + 2.0, sh[1])
    elm.Switch().at(sh).to(u9e).label('U9 AQY212 (on)', loc='bottom', fontsize=10)
    vns = (u9e[0] + 2.2, sh[1])
    elm.Resistor().at(u9e).to(vns).label('R20 1k (was 100k) — H4', loc='top', color=RED, fontsize=10)
    dot(vns)
    note((vns[0], vns[1] - 0.25), 'VN_SENSE', loc='bottom', fs=10)
    c7b = (vns[0], vns[1] - 1.4)
    elm.Capacitor().at(vns).to(c7b)
    note((vns[0] + 0.45, vns[1] - 0.7), 'C7 1nF', loc='right', fs=10)
    elm.Ground().at(c7b)
    op3_in = (vns[0] + 2.4, vns[1])
    W(d, vns, op3_in)
    note((op3_in[0] - 0.6, op3_in[1] + 0.05), 'PB0', loc='top', fs=9)
    op3 = elm.Opamp(leads=True, sign=True).flip().at(op3_in).anchor('in2').right()
    note((op3.center[0] - 0.4, op3.center[1]), 'OPAMP3\nPGA ×1…×64', loc='center', fs=9)
    W(d, op3.out, (op3.out[0] + 0.6, op3.out[1]))
    dot((op3.out[0] + 0.6, op3.out[1]), 'PB1 → ADC3_IN1\n= V_SL + G·(V_SH − V_SL)', loc='right')

    # SL -> R40 -> U1B (+) follower -> VBUF -> PB12 and PB2
    sl = (X + 1.6, dbot[1])
    W(d, dbot, sl)
    dot(sl, 'SL (J5-3)', loc='bottom')
    r40e = (sl[0] + 2.2, sl[1])
    elm.Resistor().at(sl).to(r40e).label('R40 100k', loc='top', fontsize=10)
    u1b = elm.Opamp(leads=True, sign=True).flip().at(r40e).anchor('in2').right()
    note((u1b.center[0] - 0.4, u1b.center[1]), 'U1B\nLMC6482', loc='center', fs=9)
    vb_fb = (u1b.out[0] + 0.5, u1b.out[1])
    W(d, u1b.out, vb_fb)
    dot(vb_fb)
    W(d, u1b.in1, (u1b.in1[0] - 0.3, u1b.in1[1]), (u1b.in1[0] - 0.3, u1b.in1[1] - 0.9),
      (vb_fb[0], u1b.in1[1] - 0.9), vb_fb)
    xb = vb_fb[0]
    vbuf = vb_fb
    note((xb + 0.25, vbuf[1] - 0.25), 'VBUF', loc='right', fs=10)
    W(d, vbuf, (op3.out[0] + 0.6, vbuf[1]))
    dot((op3.out[0] + 0.6, vbuf[1]), 'PB12 → ADC4_IN3\n(sampled with ADC3: F4)', loc='right')
    W(d, vbuf, (xb, op3.in1[1]), op3.in1, color=RED)
    note((xb + 0.15, (vbuf[1] + op3.in1[1]) / 2), 'PB2 (J2-17)\nH3: new', loc='right', color=RED, fs=10)

    note((0, -1.2), 'R = RSENSE_eff × [ (V_ADC3 − V_ADC4) / G − V_os,short ] / V_PA7', fs=12)
    note((0, -1.9), 'H2: MUXR_EN moves PB2 → PB15 (J1-17).   Both op-amps self-calibrated at boot (F2).', fs=11)

# --------------------------------------------------------------------------
# 2. TIA
# --------------------------------------------------------------------------
with schemdraw.Drawing(file=f'{OUTDIR}/accuracy-tia.svg', show=False) as d:
    d.config(fontsize=FS, unit=2.0)
    note((0, 15.2), 'R-mode, TIA (10 MΩ – 1 GΩ) — proposed (changes in red)', fs=14)
    X = 9
    q1, dtop, dbot = force_column(d, X, 13.5, 'DUT\n≤ 1 GΩ', q1_note='\nfully on')
    # gate pulled low by R_FORCE
    elm.Resistor().at(q1.gate).to((q1.gate[0] - 2.2, q1.gate[1])).label('R15 10k', loc='top')
    dot((q1.gate[0] - 2.2, q1.gate[1]), 'R_FORCE (PB14) = LOW', loc='left')

    # FL = summing node -> U1A (-)
    fl = (X, dbot[1] - 0.8)
    W(d, dbot, fl)
    dot(fl)
    note((X - 0.3, fl[1] + 0.3), 'FL (J5-4)', loc='left', fs=10)
    # K2A open: MUXR_COM + PA7 are now off the summing node
    mc = (X, fl[1] - 1.6)
    elm.Switch().at(fl).to(mc).label('K2A (open)', loc='bottom')
    dot(mc)
    W(d, mc, (X - 3.0, mc[1]), color=RED)
    note((X - 3.0, mc[1] + 0.05), 'MUXR_COM + PA7 (H1)\nisolated from FL in TIA', loc='left', color=RED, fs=10)

    # U1A: - = FL (top), + = VBIAS (bottom)
    ua_in = (X + 6.0, fl[1])
    W(d, fl, ua_in)
    u1a = elm.Opamp(leads=True, sign=True).at(ua_in).anchor('in1').right()
    note((u1a.center[0] - 0.4, u1a.center[1]), 'U1A\nLMC6482', loc='center', fs=9)
    vb = (u1a.in2[0] - 0.6, u1a.in2[1])
    W(d, u1a.in2, vb)
    vbn = (vb[0], vb[1] - 1.2)
    W(d, vb, vbn)
    dot(vbn, 'VBIAS 1.65 V (PA5)\nR19/R21 10k, C6', loc='left')
    tia = (u1a.out[0] + 0.8, u1a.out[1])
    W(d, u1a.out, tia)
    dot(tia, 'TIA_OUT → PA6 (ADC2_IN3)', loc='right')
    # feedback: FL -> Rf || Cf -> RF_TOP -> K2B -> TIA_OUT
    fb_y = fl[1] + 2.6
    fx0 = X + 2.6
    W(d, (fx0, fl[1]), (fx0, fb_y))
    dot((fx0, fl[1]))
    rf_e = (fx0 + 2.6, fb_y)
    elm.Resistor().at((fx0, fb_y)).to(rf_e).label('R16 10M 0.1 %', loc='top')
    W(d, (fx0, fb_y), (fx0, fb_y + 1.0))
    elm.Capacitor().at((fx0, fb_y + 1.0)).to((rf_e[0], fb_y + 1.0)).label('C4 10pF', loc='top')
    W(d, (rf_e[0], fb_y + 1.0), rf_e)
    dot(rf_e)
    note((rf_e[0], rf_e[1] - 0.3), 'RF_TOP', loc='bottom', fs=10)
    k2b_e = (tia[0], fb_y)
    elm.Switch().at(rf_e).to(k2b_e).label('K2B (closed)', loc='top')
    W(d, k2b_e, tia)
    # guard ring (VBIAS) around FL / Rf / U1A input
    gx0, gx1 = fx0 - 0.9, rf_e[0] + 0.6
    gy0, gy1 = fl[1] - 0.6, fb_y + 1.7
    W(d, (gx0, gy0), (gx1, gy0), (gx1, gy1), (gx0, gy1), (gx0, gy0), color=RED, ls='--')
    note((gx0, gy1 + 0.25), 'P1: guard ring at VBIAS (both layers, vias ~2 mm)', loc='right', color=RED, fs=10)
    W(d, (gx0, gy0), (gx0, vbn[1]), vbn, color=RED, ls='--')

    # SL -> R40 -> U1B -> VBUF (used as V_SL), SH -> U9 -> R20 -> OPAMP3 follower
    note((0, dbot[1] - 5.6),
         'V_SH: SH → U9 → R20 → OPAMP3 follower → PB1.   V_SL: SL → R40 → U1B → VBUF → PB12.', fs=11)
    note((0, dbot[1] - 6.4),
         'R = Rf × (V_SH − V_SL) / (V_SL − V_TIA − V_open)      (F6: V_SL replaces the VBIAS reading; V_open from `ropen`)',
         fs=12)

# --------------------------------------------------------------------------
# 3. C-mode
# --------------------------------------------------------------------------
with schemdraw.Drawing(file=f'{OUTDIR}/accuracy-cmode.svg', show=False) as d:
    d.config(fontsize=FS, unit=2.0)
    note((0, 13.2), 'C-mode — proposed (changes in red)', fs=14)
    y = 10.0
    # PA0 -> U2 -> U3 -> R1..R4 -> VCAP
    dot((0, y))
    note((0, y + 0.35), 'PA0 (J2-6)\nCOMP1_OUT / GPIO', loc='top', fs=10)
    W(d, (0, y), (1.8, y))
    b = logic.Buf().at((1.8, y)).anchor('in1').right()
    note((b.center[0], b.center[1] + 0.6), 'U2 74LVC1G34', loc='top', fs=10)
    elm.Resistor().at((1.0, y)).to((1.0, y - 1.6))
    note((1.25, y - 0.8), 'R29 10k', loc='right', fs=10)
    elm.Ground().at((1.0, y - 1.6))
    dot((1.0, y))
    u3s = (b.out[0] + 0.3, y)
    W(d, b.out, u3s)
    u3e = (u3s[0] + 1.8, y)
    elm.Switch().at(u3s).to(u3e).label('U3 MAX4619', loc='bottom', fontsize=10)
    vcap = (u3e[0] + 2.4, y)
    elm.Resistor().at(u3e).to(vcap).label('R1–R4\n100 / 1k / 100k / 1M', loc='top', fontsize=10)
    dot(vcap)
    note((vcap[0] + 0.25, vcap[1] - 0.35), 'VCAP', loc='right', fs=10)
    # VCAP -> R32 -> PA1 ; D1 clamp
    pa1 = (vcap[0], vcap[1] - 2.0)
    elm.Resistor().at(vcap).to(pa1)
    note((vcap[0] - 0.3, vcap[1] - 1.0), 'R32 1k', loc='left', fs=10)
    dot(pa1)
    note((pa1[0], pa1[1] - 0.3), 'PA1: ADC1_IN2 + COMP1 IN+\nCOMP1 IN− = DAC3 1.65 V, 70 mV hysteresis\nCOMP1 → TIM2 (internal) and → PA0', loc='bottom', fs=10)
    d1 = (vcap[0] + 1.6, vcap[1] - 1.0)
    W(d, vcap, (d1[0], vcap[1]))
    elm.Diode().at((d1[0], vcap[1])).to((d1[0], vcap[1] - 1.4)).reverse()
    note((d1[0] + 0.3, vcap[1] - 0.7), 'D1 BAV199 clamp\n(was BAT54S) H5', loc='right', color=RED, fs=10)
    elm.Ground().at((d1[0], vcap[1] - 1.4))
    # VCAP -> C1 || C2 (|| R33) -> C_BLOCK -> K1B -> FH
    cb_s = (vcap[0] + 4.4, y)
    W(d, (d1[0], vcap[1]), cb_s)
    cb_e = (cb_s[0] + 2.0, y)
    elm.Capacitor().at(cb_s).to(cb_e).label('C1 1000µF BP', loc='top', fontsize=10)
    W(d, cb_s, (cb_s[0], y + 1.2))
    W(d, cb_e, (cb_e[0], y + 1.2))
    elm.Capacitor().at((cb_s[0], y + 1.2)).to((cb_e[0], y + 1.2)).label('C2 68nF', loc='top', fontsize=10)
    W(d, cb_s, (cb_s[0], y - 1.2))
    W(d, cb_e, (cb_e[0], y - 1.2))
    elm.Resistor().at((cb_s[0], y - 1.2)).to((cb_e[0], y - 1.2)).label('R33 10M (was 1M) H6', loc='bottom', color=RED, fontsize=10)
    dot(cb_e)
    note((cb_e[0], y + 0.25), 'C_BLOCK', loc='right', fs=9)
    fh = (cb_e[0] + 2.2, y)
    elm.Switch().at(cb_e).to(fh).label('K1B (NC, closed)', loc='bottom', fontsize=10)
    dot(fh)
    note((fh[0] + 0.15, fh[1] + 0.3), 'FH / SH', loc='top', fs=10)
    dutb = (fh[0], fh[1] - 2.4)
    elm.Resistor().at(fh).to(dutb).fill('#fde68a').label('DUT', loc='bottom')  # placeholder, relabelled below
    dot(dutb)
    note((dutb[0] - 0.3, dutb[1]), 'FL / SL', loc='left', fs=10)
    k1a = (dutb[0], dutb[1] - 1.4)
    elm.Switch().at(dutb).to(k1a)
    note((dutb[0] + 0.5, dutb[1] - 0.7), 'K1A (NC, closed)', loc='right', fs=10)
    elm.Ground().at(k1a)
    # bias: V_BIAS -> R12 -> FH ; SSR1 + R9 ; SSR2 + R10
    vb = (fh[0] + 4.5, fh[1] + 1.4)
    W(d, fh, (fh[0], vb[1]))
    elm.Resistor().at((fh[0], vb[1])).to(vb).label('R12 4.8M', loc='top', fontsize=10)
    dot(vb, 'V_BIAS 0–20 V (J6)', loc='right')
    note((fh[0] + 0.4, fh[1] + 2.4), 'SSR1+R9 47Ω pre-charge, SSR2+R10 discharge, SSR3 VCAP clamp (unchanged)', fs=9)
    # bias monitor: SH -> R38 10M -> SH_DIV -> R39 1M -> GND ; U10 -> VN_SENSE -> OPAMP3
    shx = fh[0] + 2.8
    W(d, fh, (shx, fh[1]))
    dot((shx, fh[1]))
    shd = (shx, fh[1] - 2.0)
    elm.Resistor().at((shx, fh[1])).to(shd)
    note((shx - 0.3, fh[1] - 1.0), 'R38 10M', loc='left', fs=10)
    dot(shd)
    r39b = (shx, shd[1] - 1.6)
    elm.Resistor().at(shd).to(r39b)
    note((shx + 0.4, shd[1] - 0.8), 'R39 1M', loc='right', fs=10)
    elm.Ground().at(r39b)
    note((shx + 0.3, shd[1] + 0.75), 'SH_DIV (÷11)', loc='right', fs=10)
    u10e = (shx + 3.0, shd[1])
    elm.Switch().at((shx, shd[1])).to(u10e)
    note(((shx + u10e[0]) / 2, shd[1] - 0.45), 'U10 (on in C)', loc='bottom', fs=10)
    dot(u10e, 'VN_SENSE → OPAMP3 follower → PB1\nF8: DC-bias monitor (up to 36 V)', loc='right', color=RED)

    note((0, 3.0), 'F7 `cbcal`: short J5 on the 100 Ω range → τ / R = C_block (measured, not assumed).', fs=11)
    note((0, 2.3), 'P2: ground-plane cutout under J5, C1/C2, VCAP, SH_DIV.   U9 open in C-mode (SH → R20 branch isolated).', fs=11)
