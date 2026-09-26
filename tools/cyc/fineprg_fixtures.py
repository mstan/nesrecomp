"""Independent 4 KiB windows, execution handoff, and cross-window data loads."""


def fineprg_fixtures():
    for slot in range(8):
        for alias in (False, True) if slot == 1 else (False,):
            start = 0x8080 + slot * 4096
            register = (0x5000 if alias else 0x5ff8) + slot
            code = bytes([0x78, 0xd8, 0xa9, 5, 0x8d, register & 255, register >> 8])
            loop = start + len(code) + 2
            prg = bytearray()
            for bank in range(32):
                block = bytearray([bank])*4096
                program = code + bytes([0xa9, bank, 0x4c, loop & 255, loop >> 8])
                block[0x80:0x80+len(program)] = program
                block[-6:] = bytes([start & 255, start >> 8])*3
                prg.extend(block)
            header = b'NES\x1a' + bytes([8,0,0xf0,0x10]) + bytes(8)
            seeds = ''.join(f'4k:{bank:02x}:{start:04x}\n' for bank in range(32))
            yield f'fine31_slot{slot}' + ('_alias' if alias else ''), header+prg, seeds, 'A=05'

    # An old 8 KiB view would incorrectly fold the load as data from its own
    # PRG bank. The odd 4 KiB half is independently switchable.
    start=0x8080
    code=bytes([0x78,0xd8,0xa9,5,0x8d,0xf9,0x5f,0xad,0x00,0x90,
                0x4c,0x8a,0x80])
    prg=bytearray()
    for bank in range(32):
        block=bytearray([bank])*4096
        block[0x80:0x80+len(code)]=code
        block[-6:]=bytes([0x80,0x80])*3
        prg.extend(block)
    header=b'NES\x1a'+bytes([8,0,0xf0,0x10])+bytes(8)
    yield 'fine31_other_half', header+prg, '4k:00:8080\n', 'A=05'

    # NES 2.0 can represent a single 4 KiB PRG chip; its vectors must mirror
    # into $FFFA-$FFFF rather than landing in allocation padding.
    prg=bytearray([0xea])*4096
    prg[:5]=bytes([0xa9,0x63,0x4c,0x02,0xf0])
    prg[-6:]=bytes([0,0xf0])*3
    header=b'NES\x1a'+bytes([12*4,0,0xf0,0x18,0,15,0,7])+bytes(4)
    yield 'fine31_4k_image', header+prg, '4k:00:f000\n', 'A=63'
