#!/usr/bin/env python3
"""ROM-free test of instructions that run past the end of a 4KB PRG slot.

cyc_codegen compiles such an instruction natively: its operand bytes come from
the board's fixed next slot, or - when no board wiring fixes it - from the
physically following 4KB bank under a run-time guard (cross_ok() in the
generated umbrella). Here an SxROM image runs the three crossing shapes (opcode
at $xFFF with one or two operand bytes in the next slot; opcode at $xFFE with
its high byte in the next slot) in the fixed bank, where the next slot is
always contiguous (guard passes: native, no interpreted ROM), and from three
switchable banks at $BFFF/$BFFE, where $C000 is a different 16KB bank than the
one a block compiled for "the next 4KB" would hold (guard fails: interpreter,
reading the live bytes). A wrongly folded operand changes the stored results,
so native == interpreter == the expected bytes proves both directions, and the
miss log names exactly the guard-failing positions. An NROM-256 image checks
the unguarded case (next slot wired by the board)."""
import argparse
from pathlib import Path
import re
import subprocess

from nested_build import Toolchain, add_arguments


def asm(*chunks):
    out = bytearray()
    for c in chunks:
        out += bytes(c)
    return out


def select_bank(n):
    """Five serial writes to the MMC1 PRG register ($E000)."""
    out = bytearray([0xA9, n])
    for i in range(5):
        out += bytes([0x8D, 0x00, 0xE0])
        if i < 4:
            out += bytes([0x4A])
    return out


def sta(addr):
    return bytes([0x85, addr])


def stx(addr):
    return bytes([0x86, addr])


def build_mmc1():
    banks = [bytearray([0xFF] * 16384) for _ in range(8)]
    for b in (1, 3):
        banks[b][0:3] = bytes([0x06, 0xE8, 0x60])
    banks[2][0:3] = bytes([0x05, 0xE8, 0x60])
    fixed = banks[7]
    # Every 4KB of the fixed bank starts with `04 E8 60`: the operand bytes of a
    # crossing instruction, then INX, then RTS.
    for s in range(4):
        fixed[s * 4096:s * 4096 + 3] = bytes([0x04, 0xE8, 0x60])
    # $E804/$E805/$E806: marker bytes read through the operand $E804 / $E806.
    fixed[0x2804], fixed[0x2805], fixed[0x2806] = 0x5A, 0xA5, 0xC3
    fixed[0x0FFF] = 0xA9                       # $CFFF: LDA #imm, imm = $D000
    fixed[0x1FFE:0x2000] = bytes([0xAD, 0x10])  # $DFFE: LDA $0410 (high byte $E000 = $04)
    fixed[0x2FFF] = 0xAD                       # $EFFF: LDA $E804 (operands $F000/$F001)
    # Switchable bank 6: $BFFF LDA abs, as in the fixed bank (next slot is bank 7).
    banks[6][0x3FFF] = 0xAD
    banks[2][0x3FFF] = 0xAD                    # guard fails: $C000 holds bank 7, not bank 3
    banks[1][0x3FFE:0x4000] = bytes([0xAD, 0x10])
    banks[0][0x3FFF] = 0xA9
    c = bytearray()
    c += bytes([0x78, 0xD8, 0xA2, 0xFF, 0x9A])
    c += bytes([0xA9, 0x77, 0x8D, 0x10, 0x04, 0xA9, 0x88, 0x8D, 0x10, 0x05])
    loop = len(c)
    c += bytes([0xA2, 0x00, 0x20, 0xFF, 0xCF]) + sta(0x00) + stx(0x01)        # JSR $CFFF
    c += bytes([0xA2, 0x00, 0x20, 0xFE, 0xDF]) + sta(0x02) + stx(0x03)        # JSR $DFFE
    c += bytes([0xA2, 0x00, 0x20, 0xFF, 0xEF]) + sta(0x04) + stx(0x05)        # JSR $EFFF
    for n, store, jsr in ((6, 0x06, 0xBFFF), (2, 0x07, 0xBFFF)):
        c += select_bank(n) + bytes([0xA2, 0x00, 0x20, jsr & 0xFF, jsr >> 8]) + sta(store)
    c += select_bank(1) + bytes([0xA2, 0x00, 0x20, 0xFE, 0xBF]) + sta(0x08) + stx(0x09)
    c += select_bank(0) + bytes([0xA2, 0x00, 0x20, 0xFF, 0xBF]) + sta(0x0A) + stx(0x0B)
    c += bytes([0x4C, (0xC010 + loop) & 0xFF, (0xC010 + loop) >> 8])
    fixed[0x10:0x10 + len(c)] = c
    assert 0x10 + len(c) < 0x1000
    fixed[0x08] = 0x40                         # NMI/IRQ: RTI
    fixed[0x3FFA:0x4000] = bytes([0x08, 0xC0, 0x10, 0xC0, 0x08, 0xC0])
    prg = b''.join(bytes(b) for b in banks)
    header = b'NES\x1a' + bytes([8, 0, 0x10, 0]) + bytes(8)
    seeds = ['4k:1C:C010', '4k:1C:C001', '4k:1D:D001', '4k:1D:D002', '4k:1D:DFFE', '4k:1E:E001', '4k:1E:E002',
             '4k:1E:EFFF', '4k:1F:F002',
             '4k:1B:BFFF', '4k:0B:BFFF', '4k:07:BFFE', '4k:03:BFFF',
             '4k:1C:C002']
    # Seeds name 4KB banks: a 16KB bank X at $8000 is 4KB banks 4X..4X+3 (so
    # $B000 of bank 6 is $1B, bank 2 $0B, bank 1 $07, bank 0 $03), and the
    # fixed bank 7 is $1C-$1F.
    expect = {0x00: 0x04, 0x01: 0x01, 0x02: 0x77, 0x03: 0x01, 0x04: 0x5A, 0x05: 0x00,
              0x06: 0x5A, 0x07: 0x5A, 0x08: 0x77, 0x09: 0x01, 0x0A: 0x04, 0x0B: 0x01}
    return header + prg, seeds, expect


def build_nrom():
    prg = bytearray([0xFF] * 32768)
    # $8FFF LDA #imm (imm at $9000), $9FFE LDA $0410 (high byte at $A000), $AFFF
    # every slot is wired by the board: no run-time guard.
    for s in range(8):
        prg[s * 4096:s * 4096 + 3] = bytes([0x04, 0xE8, 0x60])
    prg[0x6804], prg[0x6805] = 0x5A, 0xA5       # $E804 / $E805
    prg[0x0FFF] = 0xA9
    prg[0x1FFE:0x2000] = bytes([0xAD, 0x10])
    prg[0x2FFF] = 0xAD
    prg[0x3FFF] = 0xA9                          # a fourth: $BFFF LDA #imm (imm at $C000)
    prg[0x4FFE:0x5000] = bytes([0xAD, 0x10])   # $CFFE
    prg[0x5FFF] = 0xAD                          # $DFFF: operands $E000/$E001 = $E804
    c = bytearray([0x78, 0xD8, 0xA2, 0xFF, 0x9A])
    c += bytes([0xA9, 0x77, 0x8D, 0x10, 0x04])
    loop = len(c)
    for i, (target, store) in enumerate(((0x8FFF, 0x00), (0x9FFE, 0x02), (0xAFFF, 0x04),
                                         (0xBFFF, 0x06), (0xCFFE, 0x08), (0xDFFF, 0x0A))):
        c += bytes([0xA2, 0x00, 0x20, target & 0xFF, target >> 8]) + sta(store) + stx(store + 1)
    c += bytes([0x4C, (0x8010 + loop) & 0xFF, (0x8010 + loop) >> 8])
    prg[0x10:0x10 + len(c)] = c
    prg[0x08] = 0x40
    prg[0x7FFA:0x8000] = bytes([0x08, 0x80, 0x10, 0x80, 0x08, 0x80])
    header = b'NES\x1a' + bytes([2, 0, 0, 0]) + bytes(8)
    seeds = []   # every slot is board-wired: the static walk follows every transfer
    expect = {0x00: 0x04, 0x01: 0x01, 0x02: 0x77, 0x03: 0x01, 0x04: 0x5A, 0x05: 0x00,
              0x06: 0x04, 0x07: 0x01, 0x08: 0x77, 0x09: 0x01, 0x0A: 0x5A, 0x0B: 0x00}
    return header + bytes(prg), seeds, expect


EXTRAS = r'''
#include "cyc_host_extras.h"
#include "cyc_mod.h"
#include <stdio.h>
static unsigned frame;
static void power(void *ctx) { (void)ctx; frame = 0; }
static void end(void *ctx) {
    (void)ctx;
    if (++frame != 4) return;
    printf("cross-result");
    for (unsigned a = 0; a < 12; a++) printf(" %02X", cyc_mod_peek((uint16_t)a));
    printf("\n");
}
static const CycHostExtras extras = {.power_on=power, .frame_end=end};
const CycHostExtras *cyc_host_extras(void) { return &extras; }
'''


def run_fixture(name, image, seeds, expect, args, tc, root, out, guarded_misses):
    d = out / name
    d.mkdir(parents=True, exist_ok=True)
    (d / 'fixture.nes').write_bytes(image)
    (d / 'seeds.txt').write_text('\n'.join(seeds) + '\n')
    (d / 'extras.c').write_text(EXTRAS, encoding='utf-8')
    (d / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(CrossSlot_{name} C CXX)
set(NESRECOMP_HEADLESS ON)
set(NESRECOMP_RECOMP_UI OFF)
include("{root.as_posix()}/runner/cyc/project.cmake")
nesrecomp_add_cycle_game(cross_{name} HEADLESS
    ROM "{(d / 'fixture.nes').as_posix()}"
    SEED_FILE "{(d / 'seeds.txt').as_posix()}"
    RECOMPILER "{args.recompiler.resolve().as_posix()}"
    HOST_EXTRAS "{(d / 'extras.c').as_posix()}")
''', encoding='utf-8')
    (d / 'logs').mkdir(exist_ok=True)
    tc.build(d, d / 'build', d / 'logs', parallel=4)
    exe = tc.executable(d / 'build', f'cross_{name}')
    want = ' '.join(f'{expect[a]:02X}' for a in range(12))
    for mode in ([], ['--interp-only']):
        label = 'interpreter' if mode else 'native'
        miss = d / f'{label}.miss.txt'
        miss.unlink(missing_ok=True)
        cmd = [str(exe), str(d / 'fixture.nes'), '--frames', '6', '--no-save', *mode]
        if not mode:
            cmd += ['--miss-log', str(miss)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        (d / f'{label}.log').write_text(r.stdout + r.stderr, encoding='utf-8')
        assert r.returncode == 0, r.stdout + r.stderr
        m = re.search(r'cross-result ((?:[0-9A-F]{2} ?)+)', r.stdout)
        assert m and m.group(1).strip() == want, (name, label, m and m.group(1), want)
        if not mode:
            lines = sorted(l.split()[0] for l in miss.read_text().splitlines()
                           if l.startswith('4k:')) if miss.exists() else []
            assert lines == sorted(guarded_misses), (name, 'interpreted ROM positions', lines, guarded_misses)
        print(f'{name} {label}: results {want}', flush=True)
    print(f'{name}: interpreted ROM positions {sorted(guarded_misses)}', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--recompiler', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    add_arguments(ap)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    tc = Toolchain.from_args(args)
    image, seeds, expect = build_mmc1()
    # Only the three switchable-bank crossings the guard rejects reach the
    # interpreter (the bank-6 one passes the guard): $BFFF in banks 0 and 2,
    # $BFFE in bank 1.
    run_fixture('mmc1', image, seeds, expect, args, tc, root, out,
                ['4k:03:BFFF', '4k:07:BFFE', '4k:0B:BFFF'])
    image, seeds, expect = build_nrom()
    run_fixture('nrom', image, seeds, expect, args, tc, root, out, [])


if __name__ == '__main__':
    main()
