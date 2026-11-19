"""Breadboard build variant: the parts the team actually has (Nov 2026).

The KiCad schematic stays the PCB reference design. This module edits the parsed
netlist so the breadboard pack is generated, and net-checked, for the circuit that
is really built:

  Q1      AO3401A P-MOSFET  -> BD140 PNP (TO-126). Same three nets: B = Q1_GATE,
          E = +3V3, C = Q1_D. R13 pulls the base up (off), R14 is the base resistor.
  Q2, Q3  BC847             -> 2N2222 NPN (TO-92). Same nets.
  K1, K2  G6S-2 DC3         -> G6S-2 DC5. Coils (pin 1) and the flyback diode
          cathodes move from +3V3 to +5V, taken from the module's 5V pin (P3-3).
  D1, D2  BAV199 (SOT-23)   -> two 1N4148 each: Dn A from the node to +3V3,
          Dn B from GND to the node.
  D5      1N4148            -> the spare BAT54S, one half (pin 1 anode on K2_C,
          pin 3 cathode on +5V, pin 2 unused). Leakage does not matter on a flyback.
  U5-U10  AQY212EH (DIP-4)  -> AQY221R2S (SOP-4) on adapters. Same pinout.

1N4148 used: D1A, D1B, D2A, D2B, D4, D6 = 6.
"""
FIVE_V = '+5V'

VALUES = {'Q1': 'BD140', 'Q2': '2N2222', 'Q3': '2N2222', 'K1': 'G6S-2 DC5', 'K2': 'G6S-2 DC5',
          'D5': 'BAT54S', 'D1A': '1N4148', 'D1B': '1N4148', 'D2A': '1N4148', 'D2B': '1N4148'}
for _u in ('U5', 'U6', 'U7', 'U9', 'U10'):
    VALUES[_u] = 'AQY221R2S'


def apply(value, pinnet, pinfn):
    def setp(ref, pin, net, fn=''):
        pinnet[(ref, pin)] = net
        pinfn[(ref, pin)] = fn

    # relay coils and their flyback diodes on 5 V
    assert pinnet[('K1', '1')] == pinnet[('K2', '1')] == '+3V3'
    setp('K1', '1', FIVE_V, pinfn.get(('K1', '1'), ''))
    setp('K2', '1', FIVE_V, pinfn.get(('K2', '1'), ''))
    assert pinnet[('D4', '1')] == '+3V3'
    setp('D4', '1', FIVE_V, 'K')
    # module 5V pin (P3 pin 3; pin 4 is the same 5 V inside the module and stays empty)
    assert pinnet[('J3', '3')].startswith('unconnected')
    setp('J3', '3', FIVE_V, pinfn.get(('J3', '3'), ''))

    # D5: one half of a BAT54S
    k2c = pinnet.pop(('D5', '2'))
    pinnet.pop(('D5', '1'))
    setp('D5', '1', k2c, 'A1')
    setp('D5', '2', 'unconnected-(D5-K2)', 'K2')
    setp('D5', '3', FIVE_V, 'COM')

    # D1, D2: BAV199 series pair -> two 1N4148 (pin 1 = band/cathode, pin 2 = anode)
    for d in ('D1', 'D2'):
        gnd, vdd, node = (pinnet.pop((d, p)) for p in ('1', '2', '3'))
        assert (gnd, vdd) == ('GND', '+3V3'), (d, gnd, vdd)
        for p in ('1', '2', '3'):
            pinfn.pop((d, p), None)
        value.pop(d, None)
        setp(d + 'A', '1', vdd, 'K')
        setp(d + 'A', '2', node, 'A')
        setp(d + 'B', '1', node, 'K')
        setp(d + 'B', '2', gnd, 'A')

    value.update(VALUES)
