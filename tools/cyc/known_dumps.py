#!/usr/bin/env python3
"""Generate common/nes_known_dumps.inc: board facts for known dumps whose iNES header cannot
describe the board, keyed by CRC-32 of PRG+CHR, from a pinned Mesen2 MesenNesDB.txt
(NewRisingSun's NES 2.0 header database, NesCartDB and Nestopia).

  python tools/cyc/known_dumps.py --db MesenNesDB.txt --mesen-rev b9fa69ddc6d0

nes_cart_known_dump() applies an entry only to an iNES header (or a NES 2.0 header without
the submapper the board needs) naming the entry's mapper or the mapper it was historically
dumped as, so every rule below is a statement about what such a header leaves out.
"""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# mapper -> (mapper an old header may name instead, rule). Rules return
# (submapper, mirror, sets_ram, prg_ram, prg_nvram) or None to skip the dump.
# mirror: 0 keeps the header, 1 horizontal, 2 vertical.
def kib(field):
    return int(field or 0) * 1024


def namco210(sub, wram, sram, battery, mirror):
    # Namco 175/340 were dumped as mapper 19 before 210 existed; 175 has hardwired
    # mirroring and optional battery RAM, 340 has neither RAM nor a battery.
    if sub not in ('1', '2'): return None
    return int(sub), {'h': 1, 'v': 2}.get(mirror, 0), 1, 0, kib(sram)


def tc0690(sub, wram, sram, battery, mirror):
    # TC0690 dumped as TC0190 (mapper 33). The runtime models submapper 0 only; the one
    # database entry marked submapper 1 (1500E835) has always loaded as 0.
    return 0, 0, 0, 0, 0


def x1005_207(sub, wram, sram, battery, mirror):
    # Fudou Myouou Den: X1-005 with CHR-controlled mirroring, dumped as mapper 80.
    return 0, 0, 0, 0, 0


def cnrom185(sub, wram, sram, battery, mirror):
    # iNES cannot name the CHR-enabling chip select (submappers 4-7).
    if sub not in ('4', '5', '6', '7'): return None
    return int(sub), 0, 0, 0, 0


def bandai16(sub, wram, sram, battery, mirror):
    # FCG-1/2 (submapper 4, registers at $6000) vs LZ93D50 (5, registers at $8000, 24C02
    # EEPROM when battery-backed). Old headers carry neither and often drop the battery bit.
    if sub not in ('4', '5'): return None
    return int(sub), 0, 1, 0, 256 if sub == '5' and battery == '1' else 0


def bandai159(sub, wram, sram, battery, mirror):
    # LZ93D50 with a 24C01 EEPROM (128 bytes), commonly dumped as mapper 16.
    return 0, 0, 1, 0, 128


def datach157(sub, wram, sram, battery, mirror):
    # Datach Joint ROM System: LZ93D50 plus barcode reader, dumped as mapper 16. The cartridge
    # adds a 24C01 (128 bytes) only on battery-backed boards (Battle Rush).
    return 0, 0, 1, 0, 128 if battery == '1' else 0


def bandai152(sub, wram, sram, battery, mirror):
    # Bandai 74161/7432 with mapper-controlled one-screen mirroring, often dumped as mapper
    # 70 (Gegege no Kitarou 2), whose mirroring is soldered.
    return 0, 0, 0, 0, 0


def namco154(sub, wram, sram, battery, mirror):
    # NAMCOT-3453 (Devil Man): mapper 88 plus one-screen control, dumped as 88.
    return 0, 0, 0, 0, 0


def irem78(sub, wram, sram, battery, mirror):
    # Mapper 78: Cosmo Carrier (1, one-screen) vs Holy Diver (3, horizontal/vertical).
    if sub not in ('1', '3'): return None
    return int(sub), 0, 0, 0, 0


def konami_vrc(sub, wram, sram, battery, mirror):
    # VRC2 vs VRC4 address wiring (submappers) and PRG RAM: VRC2 boards have none, only
    # the $6000 microwire latch, which iNES cannot distinguish from 8 KiB of WRAM.
    if sub in ('', '-'): return None
    return int(sub), 0, 1, kib(wram), kib(sram)


def mmc1(sub, wram, sram, battery, mirror):
    # Old MMC1 headers omit the SNROM/SOROM/SXROM RAM wiring. Unknown
    # payloads keep the compatibility allocation; known boards use their
    # actual volatile/battery chip sizes, also defining raw-save size.
    if sub in ('', '-'): return None
    return int(sub), 0, 1, kib(wram), kib(sram)


RULES = {
    210: (19, namco210), 48: (33, tc0690), 207: (80, x1005_207), 185: (185, cnrom185),
    152: (70, bandai152), 154: (88, namco154), 78: (78, irem78),
    16: (16, bandai16), 159: (16, bandai159), 157: (16, datach157),
    21: (21, konami_vrc), 23: (23, konami_vrc), 25: (25, konami_vrc),
    1: (1, mmc1), 155: (155, mmc1),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--db', type=Path, required=True)
    ap.add_argument('--mesen-rev', required=True)
    ap.add_argument('--out', type=Path, default=ROOT / 'common/nes_known_dumps.inc')
    args = ap.parse_args()
    rows = {m: [] for m in RULES}
    for line in args.db.read_text(encoding='utf-8', errors='replace').splitlines():
        f = line.split(',')
        if line.startswith('#') or len(f) < 16 or not f[5].isdigit() or int(f[5]) not in RULES: continue
        mapper = int(f[5])
        entry = RULES[mapper][1](f[15], f[9], f[10], f[11], f[12])
        if entry is not None:
            # Four-screen VRAM is a board fact too; old headers set the bit spuriously (Devil Man).
            rows[mapper].append((int(f[0], 16), mapper, RULES[mapper][0]) + entry + (int(f[12] == '4'),))
    out = [f'/* Generated by tools/cyc/known_dumps.py from Mesen2 {args.mesen_rev[:12]} MesenNesDB.txt;',
           ' * do not edit by hand. { crc, mapper, dumped_as, submapper, mirror, sets_ram, prg_ram, prg_nvram, four_screen } */']
    total = 0
    for mapper, (dumped_as, _) in RULES.items():
        entries = sorted(set(rows[mapper]))
        total += len(entries)
        out.append(f'/* {mapper}' + (f' (dumped as {dumped_as})' if dumped_as != mapper else '') + f': {len(entries)} */')
        out += [f'{{ 0x{c:08X}u, {m}, {d}, {s}, {mi}, {r}, {pr}, {nv}, {fs} }},' for c, m, d, s, mi, r, pr, nv, fs in entries]
    args.out.write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\n')
    print(f'{total} known dumps; wrote {args.out}')


if __name__ == '__main__':
    main()
