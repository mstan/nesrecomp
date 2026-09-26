"""Executable PPU/WRAM contracts with documented expected bytes.

Programs wait through PPU warm-up, exercise $2006/$2007, then render. A=$42
means every assertion passed; A=$EE identifies failure. CHR ROM pages contain
their physical 1 KiB page number. No emulator-generated expectations are used.
"""


def ppu_contract(mapper, prg_kb, chr_kb, operations, suffix=''):
    code = bytearray([0x78, 0xd8, 0xa2, 0xff, 0x9a])
    failures = []

    def store(address, value):
        code.extend([0xa9, value, 0x8d, address & 255, address >> 8])

    def ppu_address(address):
        code.extend([0xad, 0x02, 0x20])
        store(0x2006, address >> 8)
        store(0x2006, address & 255)

    def expect(value):
        code.extend([0xc9, value, 0xf0, 3, 0x4c, 0, 0])
        failures.append(len(code) - 2)

    store(0x2000, 0)
    store(0x2001, 0)
    code.extend([0x2c, 0x02, 0x20, 0x10, 0xfb] * 2)
    for kind, address, value in operations:
        if kind == 'cpu':
            store(address, value)
        elif kind == 'cpu_read':
            code.extend([0xad, address & 255, address >> 8])
            expect(value)
        else:
            ppu_address(address)
            if kind == 'write':
                store(0x2007, value)
            else:
                code.extend([0xad, 0x07, 0x20] * 2)
                expect(value)
    # Leave a visible pattern for the frame-hash comparison after assertions.
    ppu_address(0x3f01)
    store(0x2007, 0x21)
    ppu_address(0)
    store(0x2005, 0)
    store(0x2005, 0)
    store(0x2001, 0x0a)
    end = 0x8000 + len(code) + 2
    code.extend([0xa9, 0x42, 0x4c, end & 255, end >> 8])
    failure = 0x8000 + len(code)
    for offset in failures:
        code[offset:offset+2] = bytes([failure & 255, failure >> 8])
    code.extend([0xa9, 0xee, 0x4c, (failure + 2) & 255, (failure + 2) >> 8])
    assert len(code) < 0x1000  # $B000 remains $FF for bus-conflict-safe writes.
    prg = bytearray([0xff]) * (prg_kb * 1024)
    for bank in range(prg_kb // 8):
        offset = bank * 8192
        prg[offset:offset+len(code)] = code
        prg[offset+8192-6:offset+8192] = bytes([0, 0x80]) * 3
    chr_rom = b''.join(bytes([page & 255]) * 1024 for page in range(chr_kb))
    header = b'NES\x1a' + bytes([prg_kb // 16, chr_kb // 8,
                                (mapper & 15) << 4, mapper & 0xf0]) + bytes(8)
    seeds = ''.join(f'{bank:02x}:8000\n' for bank in range(prg_kb // 8))
    return f'ppu_mapper{mapper}{suffix}', header + prg + chr_rom, seeds, 'final:A=42'


def ppu_fixtures():
    yield ppu_contract(11, 128, 128, [('cpu', 0xb000, 0x21), ('read', 0, 16), ('read', 0x1fff, 23)])
    yield ppu_contract(13, 32, 0, [
        ('cpu', 0xb000, 3), ('write', 0x1000, 0xa5), ('write', 0x0000, 0x5a),
        ('cpu', 0xb000, 1), ('read', 0x1000, 0), ('read', 0, 0x5a),
        ('cpu', 0xb000, 3), ('read', 0x1000, 0xa5),
        ('write', 0x2000, 0x31), ('read', 0x2800, 0x31)])
    yield ppu_contract(34, 64, 64, [
        ('cpu', 0x7ffd, 1), ('cpu_read', 0x7ffd, 1),
        ('cpu', 0x7ffe, 2), ('cpu', 0x7fff, 5),
        ('read', 0, 8), ('read', 0x1000, 20), ('cpu_read', 0x7fff, 5)])
    yield ppu_contract(71, 128, 0, [
        ('cpu', 0x9000, 0), ('write', 0x2000, 0x31), ('read', 0x2c00, 0x31),
        ('cpu', 0x9000, 0x10), ('write', 0x2400, 0x32), ('read', 0x2800, 0x32),
        ('cpu', 0x9000, 0), ('read', 0x2000, 0x31)])
    yield ppu_contract(75, 128, 128, [
        ('cpu', 0xe000, 2), ('cpu', 0xf000, 3), ('cpu', 0x9000, 6),
        ('read', 0, 72), ('read', 0x1000, 76),
        ('write', 0x2000, 0x31), ('read', 0x2800, 0x31),
        ('cpu', 0x9000, 1), ('read', 0, 8), ('read', 0x1000, 12),
        ('read', 0x2400, 0x31)])
    yield ppu_contract(76, 128, 128, [
        ('cpu', 0x8000, 2), ('cpu', 0x8001, 5),
        ('cpu', 0x8000, 5), ('cpu', 0x8001, 8),
        ('read', 0, 10), ('read', 0x0400, 11), ('read', 0x1800, 16)])
    yield ppu_contract(79, 64, 64, [('cpu', 0x4100, 0x0b), ('read', 0, 24), ('read', 0x1fff, 31)])
    yield ppu_contract(87, 32, 32, [
        ('cpu', 0x6000, 1), ('read', 0, 16), ('cpu', 0x7fff, 2), ('read', 0, 8)])
    yield ppu_contract(113, 256, 128, [
        ('cpu', 0x4100, 0xeb), ('read', 0, 88), ('read', 0x1fff, 95),
        ('write', 0x2000, 0x31), ('read', 0x2800, 0x31),
        ('cpu', 0x4100, 0x6b), ('read', 0x2400, 0x31)])
    yield ppu_contract(140, 128, 128, [('cpu', 0x6000, 0x2a), ('read', 0, 80), ('read', 0x1fff, 87)])
    yield ppu_contract(184, 32, 32, [
        ('cpu', 0x6000, 0x12), ('read', 0, 8), ('read', 0x1000, 20),
        ('cpu', 0x7fff, 0), ('read', 0, 0), ('read', 0x1000, 16)])
    yield ppu_contract(206, 128, 64, [
        ('cpu', 0x8000, 0), ('cpu', 0x8001, 0xff),
        ('cpu', 0x8000, 2), ('cpu', 0x8001, 0xc5),
        ('read', 0, 62), ('read', 0x0400, 63), ('read', 0x1000, 5)])
