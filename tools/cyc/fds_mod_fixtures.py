#!/usr/bin/env python3
"""A synthetic FDS program for the mod surface tests (tools/cyc/test_cyc_mods.py,
CTest cyc_mods_test): hook sites keyed on the code in RAM, isolated and
committed routine calls, save states with mod records.

No Nintendo code: the BIOS copies disk files from its own ROM into RAM with
ordinary CPU stores (tools/cyc/fds_ramview_fixtures.py) and forwards NMIs
through $DFFA. The game runs one step per NMI:

  frames  0-9   JSR $7000 with overlay A resident: INC $0420, $0421 += 3
  frame  10     load overlay B over $7000 (other code at the same address)
  frames 10-19  JSR $7000 with overlay B: INC $0430
  frame  20     load overlay A again
  frames 20-29  JSR $7000 with overlay A
  every frame   JSR tail ($6400): $0451 = $88 (a site a plugin handles)
  every frame   INC $0460 (the frame count)

Routines a mod calls in isolation or commits:
  calc ($6200)  $0440 = $0441 + ... + $0448 (memory only)
  dev  ($6300)  $0442 = 1, then a store to $2000 (a device register)

Sites (game.toml, written here with their keys): test.ova at $7000 keyed on
overlay A's first 6 bytes (CRC-32), test.tail at $6400 keyed on its bytes.
Writes into --out: bios/disksys.rom + .toml, disk.fds, game.toml, layout.txt.
"""
import argparse
import hashlib
from pathlib import Path
import zlib

from fds_fixtures import amount, disk_info, fds_side, fwnes
from fds_ramview_fixtures import OPS, Asm, bios, disk

OPS['ADC']['absx'] = 0x7D

F_VECTOR, F_MAIN, F_OVA, F_OVB = range(4)
BIOS_LOAD = 0xE100
FRAME = 0x0460


def ova(a):
    a.op('INC', 'abs', 0x0420)
    a.op('LDA', 'abs', 0x0421); a.op('CLC'); a.op('ADC', 'imm', 3); a.op('STA', 'abs', 0x0421)
    a.op('RTS')


def ovb(a):
    a.op('INC', 'abs', 0x0430)
    a.op('NOP'); a.op('NOP')
    a.op('RTS')


def main_file():
    a = Asm(0x6000)
    a.label('start')
    a.op('LDA', 'imm', F_OVA); a.op('JSR', 'abs', BIOS_LOAD)       # overlay A at $7000
    a.op('LDA', 'imm', 0x80); a.op('STA', 'abs', 0x2000)        # NMIs on
    a.label('idle'); a.op('JMP', 'abs', 'idle')
    a.label('nmi')
    a.op('PHA'); a.op('TXA'); a.op('PHA'); a.op('TYA'); a.op('PHA')
    a.op('LDA', 'abs', FRAME)
    a.op('CMP', 'imm', 10); a.op('BNE', 'rel', 'not10')
    a.op('LDA', 'imm', F_OVB); a.op('JSR', 'abs', BIOS_LOAD)
    a.op('JMP', 'abs', 'run')
    a.label('not10')
    a.op('CMP', 'imm', 20); a.op('BNE', 'rel', 'run')
    a.op('LDA', 'imm', F_OVA); a.op('JSR', 'abs', BIOS_LOAD)
    a.label('run')
    a.op('LDA', 'abs', FRAME); a.op('CMP', 'imm', 30); a.op('BCS', 'rel', 'skip')
    a.op('JSR', 'abs', 0x7000)
    a.label('skip')
    a.op('JSR', 'abs', 0x6500)                   # qualified writable-operand fixture
    a.op('LDX', 'imm', 3)                        # the tail site sees X = 3
    a.op('JSR', 'abs', 0x6400)
    a.op('INC', 'abs', FRAME)
    a.op('PLA'); a.op('TAY'); a.op('PLA'); a.op('TAX'); a.op('PLA')
    a.op('RTI')
    a.raw([0xEA] * (0x6200 - a.pc))
    a.label('calc')                              # $0440 = sum $0441..$0448
    a.op('LDA', 'imm', 0); a.op('LDX', 'imm', 0)
    a.label('calc_loop')
    a.op('CLC'); a.op('ADC', 'absx', 0x0441); a.op('INX'); a.op('CPX', 'imm', 8); a.op('BNE', 'rel', 'calc_loop')
    a.op('STA', 'abs', 0x0440)
    a.op('RTS')
    a.raw([0xEA] * (0x6300 - a.pc))
    a.label('dev')
    a.op('LDA', 'imm', 1); a.op('STA', 'abs', 0x0442)
    a.op('LDA', 'imm', 0x80); a.op('STA', 'abs', 0x2000)
    a.op('RTS')
    a.raw([0xEA] * (0x6400 - a.pc))
    a.label('tail')
    a.op('LDA', 'imm', 0x88); a.op('STA', 'abs', 0x0451)
    a.op('RTS')
    a.raw([0xEA] * (0x6500 - a.pc))
    a.label('view')
    a.raw([0xA9,0x35,0x8D,0x4A,0x04,0xEE,0x4A,0x04,0x0E,0x4A,0x04,0xAD,0x4A,0x04,0x60])
    a.raw([0xEA] * (0x6600 - a.pc))
    a.label('nested')
    a.op('LDX', 'imm', 3); a.op('LDY', 'imm', 6)
    a.label('nested_loop')
    a.op('JSR', 'abs', 0x6400)
    a.op('DEY'); a.op('BNE', 'rel', 'nested_loop')
    a.op('RTS')
    return a


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out
    (out / 'bios').mkdir(parents=True, exist_ok=True)
    main_asm = main_file()
    main_bytes = main_asm.resolve()
    a = Asm(0x7000); ova(a); ova_bytes = a.resolve()
    b = Asm(0x7000); ovb(b); ovb_bytes = b.resolve()
    nmi, start = main_asm.labels['nmi'], main_asm.labels['start']
    vectors = b''.join(v.to_bytes(2, 'little') for v in (nmi, nmi, nmi, start, start))
    table = [(F_VECTOR, b'VECTORS ', 0xDFF6, vectors), (F_MAIN, b'MAIN    ', 0x6000, main_bytes),
             (F_OVA, b'OVERLAYA', 0x7000, ova_bytes), (F_OVB, b'OVERLAYB', 0x7000, ovb_bytes)]
    rom = bios(table)
    (out / 'bios' / 'disksys.rom').write_bytes(rom)
    (out / 'bios' / 'disksys.toml').write_text(
        '# Synthetic test BIOS (tools/cyc/fds_mod_fixtures.py), not disksys.rom.\n[program]\n'
        f'name = "cyc mod test BIOS"\nsize = {len(rom)}\ncrc32 = "0x{zlib.crc32(rom):08X}"\n'
        f'sha1 = "{hashlib.sha1(rom).hexdigest()}"\n', newline='\n')
    (out / 'disk.fds').write_bytes(disk(table))
    tail = main_asm.labels['tail']
    tail_bytes = main_bytes[tail - 0x6000:tail - 0x6000 + 5]
    (out / 'game.toml').write_text(
        '[game]\noutput_prefix = "mods"\ncycle_accurate = true\nfds = true\n\n'
        '[fds]\nimage = "disk.fds"\nbios = "bios/disksys.rom"\n\n'
        '[[mod_function_hook]]\nid = "test.ova"\naddr = 0x7000\n'
        f'length = 6\ncrc32 = 0x{zlib.crc32(ova_bytes[:6]):08X}\n\n'
        '[[mod_function_hook]]\nid = "test.tail"\n'
        f'addr = 0x{tail:04X}\nbytes = "{" ".join(f"{x:02X}" for x in tail_bytes)}"\n', newline='\n')
    (out / 'layout.txt').write_text(''.join(f'{n} {main_asm.labels[n]:04X}\n' for n in ('calc', 'dev', 'tail')),
                                    newline='\n')
    print(f'mod fixtures -> {out}')


if __name__ == '__main__':
    main()
