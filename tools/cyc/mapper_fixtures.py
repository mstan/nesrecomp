"""Hand-authored mapper fixtures shared by the native/interpreter/oracle test.

Bank-switch programs put a different immediate operand after the write in each
physical bank. Continuing a stale compiled block produces the wrong A register.
No commercial ROM is needed.
"""


def handoff(name, mapper, writes, expected_bank, *, prg_kb=128, chr_kb=8,
            start=0x8000, addressing='absolute'):
    prg = bytearray([0xff]) * (prg_kb * 1024)
    code = bytearray([0x78, 0xd8])
    for address, value in writes:
        if addressing == 'indexed':
            # Cross a page into the mapper register aperture.
            code.extend([0xa2, 1, 0xa9, value, 0x9d,
                         (address - 1) & 255, (address - 1) >> 8])
        elif addressing == 'indirect':
            code.extend([0xa9, address & 255, 0x85, 0x00,
                         0xa9, address >> 8, 0x85, 0x01,
                         0xa0, 0, 0xa9, value, 0x91, 0x00])
        else:
            code.extend([0xa9, value, 0x8d, address & 255, address >> 8])
    end = start + len(code) + 2
    for bank in range(prg_kb // 8):
        program = code + bytes([0xa9, bank, 0x4c, end & 255, end >> 8])
        offset = bank * 8192
        prg[offset:offset + len(program)] = program
        prg[offset + 8192 - 6:offset + 8192] = bytes([start & 255, start >> 8]) * 3
    header = b'NES\x1a' + bytes([prg_kb // 16, chr_kb // 8,
                                (mapper & 15) << 4, mapper & 0xf0]) + bytes(8)
    seeds = ''.join(f'{bank:02x}:{start:04x}\n' for bank in range(prg_kb // 8))
    return name, header + prg + bytes(chr_kb * 1024), seeds, f'A={expected_bank:02X}'


def mapper_fixtures():
    # Add a runtime fixture with each mapper implementation.
    yield handoff('mapper11', 11, [(0xb000, 0x21)], 4, chr_kb=128)
    yield handoff('mapper13', 13, [(0xb000, 3)], 0, prg_kb=32, chr_kb=0)
    yield handoff('mapper34_bnrom', 34, [(0xb000, 2)], 8, chr_kb=0)
    yield handoff('mapper34_bnrom_chrrom', 34, [(0xb000, 2)], 8, chr_kb=8)
    for mode in ('absolute', 'indexed', 'indirect'):
        yield handoff('mapper34_nina_' + mode, 34, [(0x7ffd, 1)], 4,
                      prg_kb=64, chr_kb=64, addressing=mode)
    yield handoff('mapper71', 71, [(0xc100, 3)], 6, chr_kb=0)
    yield handoff('mapper75', 75, [(0x8123, 3)], 3, chr_kb=128)
    yield handoff('mapper206', 206, [(0x8000, 6), (0x8001, 3)], 3, chr_kb=64)
    yield handoff('mapper76', 76, [(0x8000, 6), (0x8001, 3)], 3, chr_kb=128)
    for mode in ('absolute', 'indexed', 'indirect'):
        yield handoff('mapper79_' + mode, 79, [(0x4100, 11)], 4,
                      prg_kb=64, chr_kb=64, addressing=mode)
    yield handoff('mapper87', 87, [(0x6000, 1)], 0, prg_kb=32, chr_kb=32)
    yield handoff('mapper94', 94, [(0xb000, 12)], 6, chr_kb=0)
    for mode in ('absolute', 'indexed', 'indirect'):
        yield handoff('mapper113_' + mode, 113, [(0x4100, 107)], 20,
                      prg_kb=256, chr_kb=128, addressing=mode)
    for mode in ('absolute', 'indexed', 'indirect'):
        yield handoff('mapper140_' + mode, 140, [(0x6000, 0x21)], 8,
                      chr_kb=128, addressing=mode)
    yield handoff('mapper180', 180, [(0xb000, 3)], 6, chr_kb=0, start=0xc000)
    yield handoff('mapper184', 184, [(0x6000, 0x12)], 0, prg_kb=32, chr_kb=32)
    yield handoff('mapper232', 232, [(0x8000, 0x10), (0xc000, 1)], 18, prg_kb=256, chr_kb=0)
    yield from ()
