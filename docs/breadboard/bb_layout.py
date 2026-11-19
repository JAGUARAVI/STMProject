"""Placement of every part on the five breadboards. Jumpers are routed by bb_engine.

B1 DUT, bias & safety, K1     B2 C-mode     B3 sense, TIA, Kelvin force
B4 STM32 module, R-mux, refs  B5 bias-control and console headers
"""
RELAY = {'1': 1, '3': 3, '4': 4, '5': 5, '12': 10, '10': 8, '9': 7, '8': 6}   # G6S-2 pin -> carrier position
BOARD_TITLE = {1: 'DUT, bias & safety, relay K1', 2: 'C-mode (drive, range mux, C_block)',
               3: 'Sense divider, TIA, Kelvin force, relay K2', 4: 'STM32 module, R-mux, references',
               5: 'Bias-control header J8, console J7'}


def place(L):
    two, dip, sip, part, jumper = L.two, L.dip, L.sip, L.part, L.jumper

    # ------------------------------------------------------------- B4 module
    part('J2', {str(n): f'B4:d{n}' for n in range(1, 21)}, 'module')
    part('J4', {'4': 'B4:d21', '3': 'B4:d22', '2': 'B4:d23', '1': 'B4:d24'}, 'module')
    part('J1', {str(n): f'B4:h{n}' for n in range(1, 21)}, 'module')
    part('J3', {'4': 'B4:h21', '3': 'B4:h22', '2': 'B4:h23', '1': 'B4:h24'}, 'module')
    two('R30', 'B4:j5', 'B4:R+:5')
    two('R31', 'B4:a35', 'B4:L+:35')
    two('R17', 'B4:L+:9', 'B4:a9')
    two('C3', 'B4:b8', 'B4:b9', 'cap')
    two('R21', 'B4:a11', 'B4:L-:11')
    two('R18', 'B4:a39', 'B4:L-:39')
    two('C5', 'B4:b39', 'B4:b40', 'cap')
    dip('U4', 4, 30, 16, 'MAX4619')
    two('R8', 'B4:a30', 'B4:L-:30')
    two('R7', 'B4:a31', 'B4:L-:31')
    two('R6', 'B4:j33', 'B4:R-:33')
    two('R5', 'B4:j34', 'B4:R-:34')
    two('C14', 'B4:R+:29', 'B4:R-:29', 'cap')
    two('C8', 'B4:L+:16', 'B4:L-:16', 'cap')
    two('C9', 'B4:R+:12', 'B4:R-:12', 'cap')

    # ---------------------------------------------------------- B5 headers
    sip('J8', 5, 'e', 7, ['8', '7', '6', '5', '4', '3', '2', '1'], 'hdr')
    sip('J7', 5, 'e', 21, ['4', '3', '2', '1'], 'hdr')
    two('R35', 'B5:a13', 'B5:L-:13')
    two('R36', 'B5:b16', 'B5:b12')
    two('C17', 'B5:c12', 'B5:c11', 'cap')
    two('C16', 'B5:d14', 'B5:d15', 'cap')
    two('R34', 'B5:c18', 'B5:c14')
    two('C15', 'B5:L+:28', 'B5:L-:28', 'cap')

    # ------------------------------------------------- B1 DUT, bias, K1
    dip('U7', 1, 1, 4, 'AQY221', rot=True)
    part('J5', {'1': 'B1:a2', '2': 'B1:a4', '3': 'B1:a6', '4': 'B1:a8'}, 'term')
    two('R10', 'B1:c1', 'B1:c13')
    part('J6', {'1': 'B1:a11', '2': 'B1:a13'}, 'term')
    dip('U5', 1, 15, 4, 'AQY221')
    two('R22', 'B1:d19', 'B1:d15')
    two('R9', 'B1:j16', 'B1:j26')
    two('R12', 'B1:i15', 'B1:i26')
    dip('K1', 1, 23, 10, 'G6S-2', RELAY, kind='relay', cols='df')
    two('R24', 'B1:j6', 'B1:j2')
    sip('Q2', 1, 'h', 29, ['3', '1', '2'], 'to92', face='j')
    two('D4', 'B1:j28', 'B1:j29', 'diode')            # band on the +5V half-row 28
    two('R27', 'B1:j30', 'B1:R-:30')
    two('R25', 'B1:i34', 'B1:i30')
    two('C10', 'B1:R+:23', 'B1:R-:23', 'cap')
    jumper('B1:d13', 'B1:L-:16')
    jumper('B1:h26', 'B3:b28')

    # ------------------------------------------------------------ B2 C-mode
    dip('U2', 2, 3, 6, '74LVC1G34', {'1': 1, '2': 2, '3': 3, '4': 4, '5': 6})
    L.keepout[(2, 'R', 4)] = 'unused adapter pin'
    two('R29', 'B2:a4', 'B2:L-:4')
    dip('U3', 2, 9, 16, 'MAX4619')
    two('R4', 'B2:a9', 'B2:a17')
    two('R1', 'B2:b10', 'B2:b17')
    two('R2', 'B2:j12', 'B2:j17')
    two('R3', 'B2:i13', 'B2:i17')
    jumper('B2:e17', 'B2:f17')
    two('R33', 'B2:c17', 'B2:c21')
    dip('U6', 2, 22, 4, 'AQY221', rot=True)
    two('C2', 'B2:b23', 'B2:b21', 'cap')
    two('R11', 'B2:a22', 'B2:a26')
    two('D1A', 'B2:h18', 'B2:h17', 'diode')           # VCAP -> +3V3 clamp (band on row 18)
    two('D1B', 'B2:a32', 'B2:L-:31', 'diode')         # GND -> VCAP clamp (band on a32, the C1 row)
    two('R32', 'B2:g17', 'B2:g21')
    two('R23', 'B2:j27', 'B2:j23')
    two('C1', 'B2:e32', 'B2:f32', 'c1')
    two('C12', 'B2:L+:7', 'B2:L-:7', 'cap')
    two('C13', 'B2:R+:10', 'B2:R-:10', 'cap')

    # ------------------------------------------ B3 sense, TIA, Kelvin force
    dip('U10', 3, 6, 4, 'AQY221', rot=True)
    dip('U9', 3, 10, 4, 'AQY221', rot=True)
    two('R38', 'B3:a10', 'B3:a6')
    two('C21', 'B3:b10', 'B3:b6', 'cap')
    two('R20', 'B3:c11', 'B3:c7')
    two('R39', 'B3:c6', 'B3:c2')
    two('C20', 'B3:d6', 'B3:d2', 'cap')
    two('R43', 'B3:j2', 'B3:j6')
    two('R41', 'B3:j14', 'B3:j10')
    two('D2A', 'B3:h18', 'B3:h17', 'diode')           # VN_SENSE -> +3V3 clamp
    two('D2B', 'B3:f17', 'B3:f19', 'diode')           # GND -> VN_SENSE clamp
    two('C7', 'B3:i17', 'B3:i19', 'cap')
    jumper('B3:d7', 'B3:g17')
    jumper('B3:j17', 'B4:c15')
    sip('Q1', 3, 'c', 22, ['1', '3', '2'], names={'1': 'B', '3': 'C', '2': 'E'})   # BD140: B, C, E top to bottom
    two('R13', 'B3:L+:22', 'B3:a22')
    two('R14', 'B3:g22', 'B3:d22')
    two('R15', 'B3:b26', 'B3:b22')
    two('D6', 'B3:d28', 'B3:d23', 'diode')
    two('R40', 'B3:a25', 'B3:a30')
    sip('D3', 3, 'h', 27, ['1', '2', '3'])
    dip('U1', 3, 30, 8, 'LMC6482', rot=True)
    jumper('B3:d31', 'B3:d32')
    two('R19', 'B3:R+:31', 'B3:j31')
    two('C6', 'B3:i31', 'B3:i30', 'cap')
    two('R16', 'B3:i32', 'B3:i37')
    two('C4', 'B3:h32', 'B3:h37', 'cap')
    dip('K2', 3, 35, 10, 'G6S-2', RELAY, kind='relay', cols='df')
    jumper('B3:j29', 'B3:j33')
    jumper('B3:g33', 'B3:g38')
    jumper('B3:j38', 'B4:c12')
    sip('Q3', 3, 'g', 41, ['3', '1', '2'], 'to92', face='j')
    sip('D5', 3, 'c', 45, ['1', '2', '3'])            # spare BAT54S: 1 anode, 2 unused, 3 cathode
    two('R28', 'B3:j42', 'B3:R-:42')
    two('R26', 'B3:i46', 'B3:i42')
    two('C11', 'B3:L+:31', 'B3:L-:31', 'cap')

    # ------------------------------------------------------------ rail links
    for b in range(1, L.n + 1):
        jumper(f'B{b}:L+:61', f'B{b}:R+:61')
        jumper(f'B{b}:L-:60', f'B{b}:R-:60')
        if b < L.n:
            jumper(f'B{b}:R+:57', f'B{b + 1}:L+:57')
            jumper(f'B{b}:R-:56', f'B{b + 1}:L-:56')
