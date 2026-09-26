"""NES 2.0 board contracts, with independently specified observable results."""
from mapper_fixtures import handoff
from mapper_ppu_fixtures import ppu_contract


def nes2(case, sub=0, ram=0, chr_ram=0, four=False, exponent=False):
    name, image, seeds, expected = case
    rom = bytearray(image)
    rom[7] |= 8
    rom[8] = sub << 4
    rom[10], rom[11] = ram, chr_ram
    if ram & 0xf0:
        rom[6] |= 2
    if four:
        rom[6] |= 8
    if exponent:
        # These fixtures contain exactly 32 KiB PRG and 8 KiB CHR.
        rom[4], rom[5], rom[9] = 15*4, 13*4, 0xff
    return 'nes2_' + name, bytes(rom), seeds, expected


def variant_fixtures():
    yield nes2(handoff('aladdin', 232, [(0x8000, 8), (0xc000, 1)], 18,
                      prg_kb=256, chr_kb=0), sub=1, chr_ram=7)
    yield nes2(handoff('namco_fixed', 206, [(0x8000, 6), (0x8001, 3)], 0,
                      prg_kb=32, chr_kb=64), sub=1)
    yield nes2(handoff('nina_small_chr', 34, [(0x7ffd, 1)], 4,
                      prg_kb=64, chr_kb=8), sub=1, ram=7)
    yield nes2(handoff('bnrom_large_chr', 34, [(0xb000, 1)], 4,
                      prg_kb=64, chr_kb=16), sub=2)
    yield nes2(handoff('exponent', 0, [], 0, prg_kb=32, chr_kb=8), exponent=True)
    yield nes2(ppu_contract(71, 128, 0, [
        ('cpu', 0x9000, 16), ('write', 0x2000, 0x51), ('write', 0x2800, 0x52),
        ('read', 0x2400, 0x51), ('read', 0x2c00, 0x52)], '_fixed'), chr_ram=7)
    yield nes2(ppu_contract(71, 128, 0, [
        ('cpu', 0x8000, 16), ('write', 0x2000, 0x53), ('read', 0x2c00, 0x53),
        ('cpu', 0x8000, 0), ('write', 0x2000, 0x54),
        ('cpu', 0x8000, 16), ('read', 0x2000, 0x53)], '_firehawk'), sub=1, chr_ram=7)
    yield nes2(ppu_contract(206, 128, 64, [
        ('cpu', 0x6000, 0x57), ('cpu_read', 0x6800, 0x57),
        ('cpu', 0x67ff, 0x58), ('cpu_read', 0x7fff, 0x58)], '_small_wram'), ram=5)
    yield nes2(ppu_contract(206, 128, 64, [
        ('cpu', 0x6000, 0x57), ('cpu', 0x6800, 0x58), ('cpu', 0x7fff, 0x59),
        ('cpu_read', 0x6000, 0x57), ('cpu_read', 0x6800, 0x58),
        ('cpu_read', 0x7fff, 0x59)], '_popils'), ram=0x70)
    yield nes2(ppu_contract(0, 32, 0, [
        ('write', 0x0000, 0x61), ('read', 0x0800, 0x61),
        ('write', 0x07ff, 0x62), ('read', 0x1fff, 0x62)], '_small_chr_ram'), chr_ram=5)
    yield nes2(ppu_contract(4, 128, 8, [
        ('cpu', 0xa000, 1),
        *[('write', 0x2000 + i*1024, 0x31+i) for i in range(4)],
        *[('read', 0x2000 + i*1024, 0x31+i) for i in range(4)],
        ('cpu', 0xa000, 0),
        *[('read', 0x3000 + i*1024, 0x31+i) for i in range(3)]], '_four'), four=True)
