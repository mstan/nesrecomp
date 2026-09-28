#!/usr/bin/env python3
"""A synthetic FDS BIOS that boots the way disksys.rom does, and disks for it:
the boot skip's tests (tools/cyc/test_cyc_fds_boot.py; no Nintendo code or data).

The program stands in for the BIOS at $E000-$FFFF. It keeps the contract of
disksys.rom's boot that the skip reproduces (runner/cyc/cyc_fds_skip.c), so
that a skipped boot must equal its own LLE boot:

  reset   the registers and shadows disksys sets ($2000/$FF, $2001/$FE,
          $4022/$4023, $4025/$FA, $4026/$F9, $4017, $4015, $4080, $408A,
          $0100/$0101), then disk-independent residue the skip has to carry
          over from its pre-run: CPU RAM $0300-$03FF, OAM (through a DMA from
          $0200), CHR RAM $1000-$10FF, two nametables, the palette; background
          rendering on
  wait    until $4032 says a disk is in (the boot's disk wait, where auto
          insert acts), then 20 frames (the "intro")
  load    JSR LoadFiles with two inline pointers (disk ID, file list), as at
          disksys $EF59; LoadFiles follows $E1F8's rules and leaves its
          variables: $00-$03 the pointers, $04 = S - 3, $05 = 2, $06 = 0, $07 =
          the last block code, $08 the boot file code, $09 0 / $FF (last file
          asked for or not), $0A/$0B the last address, $0C/$0D $FFFF, $0E the
          files asked for; $0101 kept; $F9 |= $80; $FE &= $E7 with $2001 for a
          PPU file (the store at mask_store: mid-frame, the 2C02 records an OAM
          row to corrupt); the drive's last $4025 = ($FA & 9) | $26, $4024 as
          disksys's IRQ handler leaves it; returns A = X = 0, Y = files asked for,
          V = 1, Z = 1, C = 0, I = 0. It polls the drive (no IRQs), so its stack
          use differs from disksys's: the tests compare everything but the stack
          page.
  tail    until the drive has run to the end of the side ($4032.1), two
          frames, PPU address traffic ($2006/$2007, as disksys's license
          check), $0102/$0103 = $35/$AC, rendering off, CLI, JMP ($DFFC)

Disks (fwNES, side A of disk 1 unless noted), each a boot file set:
  main.fds     every LoadFiles rule: a PPU file at $2800, a CHR file, a palette
               file ($3F00), PRG files (the game at $6000 with its vectors, one
               overlapping it, one at $E000 = the BIOS ROM, no effect), a CPU
               RAM file at $0200, a requested file at $0100 (dropped: $0000-$01FF),
               one past $FFFF (dropped), one not requested (ID above the boot
               code), a hidden file past the file amount; two sides
  io.fds       a boot file into $2000-$5FFF          (skip refused: registers)
  mirror.fds   a boot file from $0700 into $0800+    (refused: reaches $0000-$01FF)
  empty.fds    a zero-length boot file               (refused: 65536 bytes)
  unproven.fds main.fds with another game name       (refused: no proof record)
The BIOS identity file lists the proof records (hle_boot_proven) of every
disk but unproven.fds, and the boot model (hle_boot_load, hle_boot_load_entry,
hle_boot_jump, hle_boot_mask_store).

The game (main.fds file "GAME", reset $6000): marks $0480 = $C3 and loops.

Writes into --out: bios/boot.rom + bios/boot.toml, the disks, expect.txt.
"""
import argparse
import hashlib
from pathlib import Path
import zlib

import fds_board_fixtures as board
from fds_board_fixtures import Asm
from fds_fixtures import amount, disk_info, fds_side, fwnes, header, payload

board.OPS.update({
    ('LDA', 'indy'): 0xB1, ('STA', 'indy'): 0x91, ('CMP', 'indy'): 0xD1, ('LDA', 'absy'): 0xB9,
    ('STA', 'absy'): 0x99, ('CMP', 'absy'): 0xD9, ('LDY', 'zp'): 0xA4, ('STY', 'zp'): 0x84, ('INC', 'abs'): 0xEE,
    ('DEC', 'zp'): 0xC6, ('ORA', 'imm'): 0x09, ('ORA', 'zp'): 0x05, ('AND', 'zp'): 0x25, ('ADC', 'imm'): 0x69,
    ('ADC', 'zp'): 0x65, ('SBC', 'imm'): 0xE9, ('SEC', ''): 0x38, ('CLC', ''): 0x18, ('TSX', ''): 0xBA,
    ('TAY', ''): 0xA8, ('TYA', ''): 0x98, ('BCS', 'rel'): 0xB0, ('BCC', 'rel'): 0x90, ('BMI', 'rel'): 0x30,
    ('BIT', 'abs'): 0x2C, ('JMP', 'ind'): 0x6C, ('CPX', 'zp'): 0xE4, ('LDA', 'zp'): 0xA5, ('STA', 'zp'): 0x85,
})
board.SIZE.update({'indy': 2, 'absy': 3, 'ind': 3})

NAME, OTHER = b'BOT', b'UNP'
ID = bytes([0xFF] * 6 + [0, 0, 0xFF, 0xFF])   # disk 1 side A, anything else: as disksys $EFF5
MARK = 0x0480
SIG = b'*NINTENDO-HVC*'


def program():
    a = Asm(0xE000)

    def frame():
        a.op('JSR', 'abs', 'frame')

    def fill_nt(hi, value):
        a.op('LDA', 'abs', 0x2002)
        a.op('LDA', 'imm', hi); a.op('STA', 'abs', 0x2006); a.op('LDA', 'imm', 0); a.op('STA', 'abs', 0x2006)
        a.op('LDA', 'imm', value); a.op('LDX', 'imm', 0); a.op('LDY', 'imm', 4)
        lab = f'nt{len(a.code)}'
        a.label(lab); a.op('STA', 'abs', 0x2007); a.op('DEX'); a.op('BNE', 'rel', lab); a.op('DEY')
        a.op('BNE', 'rel', lab)

    a.label('reset')
    a.op('SEI'); a.op('CLD'); a.op('LDX', 'imm', 0xFF); a.op('TXS')
    a.op('LDA', 'imm', 0x10); a.op('STA', 'abs', 0x2000); a.op('STA', 'zp', 0xFF)
    a.op('LDA', 'imm', 0x06); a.op('STA', 'abs', 0x2001); a.op('STA', 'zp', 0xFE)
    a.op('LDX', 'imm', 2)
    a.label('vb'); a.op('LDA', 'abs', 0x2002); a.op('BPL', 'rel', 'vb'); a.op('DEX'); a.op('BNE', 'rel', 'vb')
    a.op('LDA', 'imm', 0); a.op('STA', 'abs', 0x4022); a.op('LDA', 'imm', 0x83); a.op('STA', 'abs', 0x4023)
    a.op('LDA', 'imm', 0x2E); a.op('STA', 'zp', 0xFA); a.op('STA', 'abs', 0x4025)
    a.op('LDA', 'imm', 0xFF); a.op('STA', 'zp', 0xF9); a.op('STA', 'abs', 0x4026)
    a.op('LDA', 'imm', 0xC0); a.op('STA', 'abs', 0x4017); a.op('LDA', 'imm', 0x0F); a.op('STA', 'abs', 0x4015)
    a.op('LDA', 'imm', 0x80); a.op('STA', 'abs', 0x4080); a.op('LDA', 'imm', 0xE8); a.op('STA', 'abs', 0x408A)
    a.op('LDA', 'imm', 0xC0); a.op('STA', 'abs', 0x0100); a.op('LDA', 'imm', 0x80); a.op('STA', 'abs', 0x0101)
    # disk-independent residue
    a.op('LDX', 'imm', 0)
    a.label('r1'); a.op('LDA', 'absx', 'rtab'); a.op('STA', 'absx', 0x0300); a.op('LDA', 'absx', 'otab')
    a.op('STA', 'absx', 0x0200); a.op('INX'); a.op('BNE', 'rel', 'r1')
    a.op('LDA', 'imm', 0); a.op('STA', 'abs', 0x2003); a.op('LDA', 'imm', 2); a.op('STA', 'abs', 0x4014)
    a.op('LDA', 'abs', 0x2002); a.op('LDA', 'imm', 0x10); a.op('STA', 'abs', 0x2006); a.op('LDA', 'imm', 0)
    a.op('STA', 'abs', 0x2006); a.op('LDX', 'imm', 0)
    a.label('r2'); a.op('LDA', 'absx', 'ctab'); a.op('STA', 'abs', 0x2007); a.op('INX'); a.op('BNE', 'rel', 'r2')
    fill_nt(0x20, 0x24)
    fill_nt(0x28, 0x6D)
    a.op('LDA', 'imm', 0x3F); a.op('STA', 'abs', 0x2006); a.op('LDA', 'imm', 0); a.op('STA', 'abs', 0x2006)
    a.op('LDX', 'imm', 0)
    a.label('r3'); a.op('LDA', 'absx', 'ptab'); a.op('STA', 'abs', 0x2007); a.op('INX'); a.op('CPX', 'imm', 0x20)
    a.op('BNE', 'rel', 'r3')
    a.op('LDA', 'abs', 0x2002); a.op('LDA', 'imm', 0); a.op('STA', 'abs', 0x2005); a.op('STA', 'abs', 0x2005)
    a.op('LDA', 'imm', 0x10); a.op('STA', 'abs', 0x2000)
    a.op('LDA', 'imm', 0x0E); a.op('STA', 'abs', 0x2001); a.op('STA', 'zp', 0xFE)
    # the disk wait, then the intro
    a.label('wait'); frame(); a.op('LDA', 'abs', 0x4032); a.op('AND', 'imm', 1); a.op('BNE', 'rel', 'wait')
    a.op('LDY', 'imm', 20)
    a.label('intro'); frame(); a.op('DEY'); a.op('BNE', 'rel', 'intro')
    # the boot load
    a.label('load_call'); a.op('JSR', 'abs', 'loadfiles')
    a.fixups.append(('abs', len(a.code), 'id')); a.raw(b'\0\0')
    a.fixups.append(('abs', len(a.code), 'list')); a.raw(b'\0\0')
    a.op('BNE', 'rel', 'err')
    a.label('tail'); a.op('LDA', 'abs', 0x4032); a.op('AND', 'imm', 2); a.op('BEQ', 'rel', 'tail')
    frame(); frame()
    # PPU address traffic after the load, as disksys's license check has
    # ($F431-$F483: $2006 twice, $2007 reads and writes), so LoadFiles's own
    # $2006/$2007 latches are not what the game starts with
    for hi, lo in ((0x20, 0x00), (0x23, 0xC0)):
        a.op('LDA', 'abs', 0x2002); a.op('LDA', 'imm', hi); a.op('STA', 'abs', 0x2006); a.op('LDA', 'imm', lo)
        a.op('STA', 'abs', 0x2006); a.op('LDA', 'abs', 0x2007); a.op('LDA', 'abs', 0x2007)
    a.op('LDA', 'abs', 0x2002); a.op('LDA', 'imm', 0x23); a.op('STA', 'abs', 0x2006); a.op('LDA', 'imm', 0xF0)
    a.op('STA', 'abs', 0x2006); a.op('LDA', 'imm', 0x55); a.op('STA', 'abs', 0x2007)
    a.op('LDA', 'imm', 0x35); a.op('STA', 'abs', 0x0102); a.op('LDA', 'imm', 0xAC); a.op('STA', 'abs', 0x0103)
    a.op('LDA', 'imm', 0x06); a.op('STA', 'abs', 0x2001)
    a.op('LDA', 'abs', 0x2002); a.op('LDA', 'imm', 0); a.op('STA', 'abs', 0x2005); a.op('STA', 'abs', 0x2005)
    a.op('LDA', 'imm', 0x10); a.op('STA', 'abs', 0x2000)
    a.op('CLI')
    a.label('jump'); a.op('JMP', 'ind', 0xDFFC)
    a.label('err'); a.op('JMP', 'abs', 'err')

    # ---- LoadFiles ($E1F8's contract) ----
    a.label('loadfiles')
    a.op('LDA', 'imm', 0); a.op('STA', 'zp', 0x0E)
    a.op('TSX')
    a.op('LDA', 'absx', 0x0101); a.op('STA', 'zp', 0x05); a.op('LDA', 'absx', 0x0102); a.op('STA', 'zp', 0x06)
    for k in range(4):
        a.op('LDY', 'imm', k + 1); a.op('LDA', 'indy', 0x05); a.op('STA', 'zp', k)
    a.op('CLC'); a.op('LDA', 'zp', 0x05); a.op('ADC', 'imm', 4); a.op('STA', 'absx', 0x0101)
    a.op('LDA', 'zp', 0x06); a.op('ADC', 'imm', 0); a.op('STA', 'absx', 0x0102)
    a.op('TXA'); a.op('SEC'); a.op('SBC', 'imm', 3); a.op('STA', 'zp', 0x04)
    a.op('LDA', 'abs', 0x4032); a.op('AND', 'imm', 1); a.op('BEQ', 'rel', 'present')
    a.op('LDA', 'imm', 1); a.op('RTS')
    a.label('present')
    a.op('LDA', 'abs', 0x0101); a.op('PHA')
    a.op('LDA', 'imm', 2); a.op('STA', 'zp', 0x05)
    a.op('LDA', 'zp', 0xF9); a.op('ORA', 'imm', 0x80); a.op('STA', 'zp', 0xF9); a.op('STA', 'abs', 0x4026)
    a.op('LDA', 'zp', 0xFA); a.op('AND', 'imm', 8); a.op('ORA', 'imm', 0x26); a.op('STA', 'abs', 0x4025)
    a.op('ORA', 'imm', 1); a.op('STA', 'abs', 0x4025); a.op('AND', 'imm', 0xFD); a.op('STA', 'zp', 0xFA)
    a.op('STA', 'abs', 0x4025)
    a.label('ready'); a.op('LDA', 'abs', 0x4032); a.op('AND', 'imm', 2); a.op('BNE', 'rel', 'ready')
    # block 1: the signature, the ID, the boot file code
    a.op('LDA', 'imm', 1); a.op('JSR', 'abs', 'start')
    a.op('LDY', 'imm', 0)
    a.label('sig'); a.op('JSR', 'abs', 'rd'); a.op('CMP', 'absy', 'sigtab'); a.op('BNE', 'rel', 'fail1')
    a.op('INY'); a.op('CPY', 'imm', 14); a.op('BNE', 'rel', 'sig')
    a.op('LDY', 'imm', 0)
    a.label('idl'); a.op('JSR', 'abs', 'rd'); a.op('STA', 'zp', 0x07); a.op('LDA', 'indy', 0x00)
    a.op('CMP', 'imm', 0xFF); a.op('BEQ', 'rel', 'idok'); a.op('CMP', 'zp', 0x07); a.op('BNE', 'rel', 'fail1')
    a.label('idok'); a.op('INY'); a.op('CPY', 'imm', 10); a.op('BNE', 'rel', 'idl')
    a.op('JSR', 'abs', 'rd'); a.op('STA', 'zp', 0x08)
    a.op('LDY', 'imm', 30)
    a.label('skip30'); a.op('JSR', 'abs', 'rd'); a.op('DEY'); a.op('BNE', 'rel', 'skip30')
    a.op('JSR', 'abs', 'end_check')
    # block 2: the file amount
    a.op('LDA', 'imm', 2); a.op('JSR', 'abs', 'start'); a.op('JSR', 'abs', 'rd'); a.op('STA', 'zp', 0x06)
    a.op('JSR', 'abs', 'end_check')
    a.op('LDA', 'zp', 0x06); a.op('BNE', 'rel', 'floop'); a.op('JMP', 'abs', 'none')
    a.label('fail1'); a.op('JMP', 'abs', 'fail')
    a.label('floop')
    a.op('LDA', 'imm', 3); a.op('JSR', 'abs', 'start')
    a.op('JSR', 'abs', 'rd'); a.op('JSR', 'abs', 'rd'); a.op('TAX')
    a.op('LDY', 'imm', 0); a.op('LDA', 'indy', 0x02); a.op('CMP', 'imm', 0xFF); a.op('BNE', 'rel', 'lmode')
    a.op('CPX', 'zp', 0x08); a.op('BEQ', 'rel', 'asked'); a.op('BCS', 'rel', 'notasked'); a.op('JMP', 'abs', 'asked')
    a.label('lmode'); a.op('TXA'); a.op('CMP', 'indy', 0x02); a.op('BEQ', 'rel', 'asked'); a.op('INY')
    a.op('CPY', 'imm', 20); a.op('BEQ', 'rel', 'notasked'); a.op('LDA', 'indy', 0x02); a.op('CMP', 'imm', 0xFF)
    a.op('BNE', 'rel', 'lmode')
    a.label('notasked'); a.op('LDA', 'imm', 0xFF); a.op('JMP', 'abs', 'setflag')
    a.label('asked'); a.op('LDA', 'imm', 0); a.op('INC', 'zp', 0x0E)
    a.label('setflag'); a.op('STA', 'zp', 0x09)
    a.op('LDY', 'imm', 8)
    a.label('nm'); a.op('JSR', 'abs', 'rd'); a.op('DEY'); a.op('BNE', 'rel', 'nm')
    for zp in (0x0A, 0x0B, 0x0C, 0x0D):
        a.op('JSR', 'abs', 'rd'); a.op('STA', 'zp', zp)
    a.op('JSR', 'abs', 'dec_size')
    a.op('JSR', 'abs', 'rd'); a.op('PHA')
    a.op('JSR', 'abs', 'end_check')
    a.op('LDA', 'imm', 4); a.op('JSR', 'abs', 'start')
    a.op('LDY', 'zp', 0x09)
    a.op('PLA'); a.op('BNE', 'rel', 'ppu')
    a.op('CLC'); a.op('LDA', 'zp', 0x0A); a.op('ADC', 'zp', 0x0C); a.op('LDA', 'zp', 0x0B); a.op('ADC', 'zp', 0x0D)
    a.op('BCS', 'rel', 'drop')
    a.op('LDA', 'zp', 0x0B); a.op('CMP', 'imm', 0x20); a.op('BCS', 'rel', 'prg'); a.op('AND', 'imm', 7)
    a.op('CMP', 'imm', 2); a.op('BCS', 'rel', 'prg')
    a.label('drop'); a.op('LDY', 'imm', 0xFF)
    a.label('prg'); a.op('JSR', 'abs', 'rd'); a.op('CPY', 'imm', 0); a.op('BNE', 'rel', 'pn')
    a.op('STA', 'indy', 0x0A); a.op('INC', 'zp', 0x0A); a.op('BNE', 'rel', 'pn'); a.op('INC', 'zp', 0x0B)
    a.label('pn'); a.op('JSR', 'abs', 'dec_size'); a.op('BCS', 'rel', 'prg'); a.op('JMP', 'abs', 'dend')
    a.label('ppu'); a.op('CPY', 'imm', 0); a.op('BNE', 'rel', 'pl')
    a.op('LDA', 'zp', 0xFE); a.op('AND', 'imm', 0xE7); a.op('STA', 'zp', 0xFE)
    a.label('mask_store'); a.op('STA', 'abs', 0x2001)
    a.op('LDA', 'abs', 0x2002); a.op('LDA', 'zp', 0x0B); a.op('STA', 'abs', 0x2006); a.op('LDA', 'zp', 0x0A)
    a.op('STA', 'abs', 0x2006)
    a.label('pl'); a.op('JSR', 'abs', 'rd'); a.op('CPY', 'imm', 0); a.op('BNE', 'rel', 'pn2')
    a.op('STA', 'abs', 0x2007)
    a.label('pn2'); a.op('JSR', 'abs', 'dec_size'); a.op('BCS', 'rel', 'pl')
    a.label('dend'); a.op('LDA', 'zp', 0x09); a.op('BNE', 'rel', 'dnot')
    a.op('JSR', 'abs', 'end_check'); a.op('JMP', 'abs', 'next')
    a.label('dnot'); a.op('JSR', 'abs', 'end_nocheck')
    a.label('next'); a.op('DEC', 'zp', 0x06); a.op('BEQ', 'rel', 'loaded'); a.op('JMP', 'abs', 'floop')
    a.label('loaded')
    a.op('LDA', 'imm', 4); a.op('STA', 'zp', 0x07)
    # $4024 as disksys's IRQ handler leaves it: $FA | $10 in $E706 for a file it
    # asked for, else the first CRC byte of the last block ($E57D)
    a.op('LDA', 'zp', 0x09); a.op('BNE', 'rel', 'wcrc')
    a.op('LDA', 'zp', 0xFA); a.op('AND', 'imm', 8); a.op('ORA', 'imm', 0xF5); a.op('JMP', 'abs', 'wset')
    a.label('wcrc'); a.op('TXA')
    a.label('wset'); a.op('STA', 'abs', 0x4024); a.op('JMP', 'abs', 'finish')
    a.label('none'); a.op('LDA', 'imm', 2); a.op('STA', 'zp', 0x07)
    a.op('LDA', 'zp', 0xFA); a.op('AND', 'imm', 8); a.op('ORA', 'imm', 0xF5); a.op('STA', 'abs', 0x4024)
    a.label('finish')
    a.op('LDA', 'zp', 0xFA); a.op('AND', 'imm', 9); a.op('ORA', 'imm', 0x26); a.op('STA', 'zp', 0xFA)
    a.op('STA', 'abs', 0x4025)
    a.op('PLA'); a.op('STA', 'abs', 0x0101)
    a.op('LDY', 'zp', 0x0E); a.op('LDX', 'imm', 0); a.op('BIT', 'abs', 'c40'); a.op('CLC'); a.op('LDA', 'imm', 0)
    a.op('CLI'); a.op('RTS')
    a.label('fail'); a.op('PLA'); a.op('STA', 'abs', 0x0101); a.op('LDA', 'imm', 0x21); a.op('RTS')

    # ---- the drive, polled ----
    # start: CRC off for more than a byte period (the gap search restarts), CRC
    # on; the byte that ended the gap, then the block code (checked against A)
    a.label('start'); a.op('STA', 'zp', 0x07)
    a.op('LDA', 'zp', 0xFA); a.op('AND', 'imm', 8); a.op('ORA', 'imm', 0x25); a.op('STA', 'abs', 0x4025)
    a.op('LDX', 'imm', 60)
    a.label('gapw'); a.op('DEX'); a.op('BNE', 'rel', 'gapw')
    a.op('LDA', 'zp', 0xFA); a.op('AND', 'imm', 8); a.op('ORA', 'imm', 0x65); a.op('STA', 'abs', 0x4025)
    a.op('JSR', 'abs', 'rd'); a.op('JSR', 'abs', 'rd'); a.op('CMP', 'zp', 0x07); a.op('BNE', 'rel', 'badcode')
    a.op('RTS')
    a.label('badcode'); a.op('PLA'); a.op('PLA'); a.op('JMP', 'abs', 'fail')
    # end_check: CRC byte 1, CRC control on (the drive checks), CRC byte 2, CRC off
    a.label('end_check'); a.op('JSR', 'abs', 'rd')
    a.op('LDA', 'zp', 0xFA); a.op('AND', 'imm', 8); a.op('ORA', 'imm', 0x75); a.op('STA', 'abs', 0x4025)
    a.op('JSR', 'abs', 'rd')
    a.op('LDA', 'zp', 0xFA); a.op('AND', 'imm', 8); a.op('ORA', 'imm', 0x25); a.op('STA', 'abs', 0x4025)
    a.op('RTS')
    # end_nocheck: both CRC bytes without CRC control ($E57A); X = the first
    a.label('end_nocheck'); a.op('JSR', 'abs', 'rd'); a.op('TAX'); a.op('JSR', 'abs', 'rd')
    a.op('LDA', 'zp', 0xFA); a.op('AND', 'imm', 8); a.op('ORA', 'imm', 0x25); a.op('STA', 'abs', 0x4025)
    a.op('RTS')
    a.label('rd'); a.op('LDA', 'abs', 0x4030); a.op('AND', 'imm', 2); a.op('BEQ', 'rel', 'rd')
    a.op('LDA', 'abs', 0x4031); a.op('RTS')
    a.label('dec_size'); a.op('SEC'); a.op('LDA', 'zp', 0x0C); a.op('SBC', 'imm', 1); a.op('STA', 'zp', 0x0C)
    a.op('LDA', 'zp', 0x0D); a.op('SBC', 'imm', 0); a.op('STA', 'zp', 0x0D); a.op('RTS')
    a.label('frame'); a.op('LDA', 'abs', 0x2002)
    a.label('fw'); a.op('LDA', 'abs', 0x2002); a.op('BPL', 'rel', 'fw'); a.op('RTS')
    a.label('nmi'); a.op('RTI')
    a.label('irq'); a.op('RTI')
    a.label('c40'); a.raw(b'\x40')
    a.label('sigtab'); a.raw(SIG)
    a.label('id'); a.raw(ID)
    a.label('list'); a.raw(b'\xff' * 20)
    a.label('rtab'); a.raw(payload(90, 256))
    a.label('otab'); a.raw(payload(91, 256))
    a.label('ctab'); a.raw(payload(92, 256))
    a.label('ptab'); a.raw(bytes(x & 0x3F for x in payload(93, 32)))
    code = a.resolve()
    assert a.origin + len(code) < 0xFFFA, hex(a.origin + len(code))
    rom = bytearray([0xFF]) * 8192
    rom[:len(code)] = code
    for vec, name in ((0x1FFA, 'nmi'), (0x1FFC, 'reset'), (0x1FFE, 'irq')):
        rom[vec:vec + 2] = a.labels[name].to_bytes(2, 'little')
    return bytes(rom), a.labels


def game():
    g = Asm(0x6000)
    g.label('reset'); g.op('SEI'); g.op('LDA', 'imm', 0xC3); g.op('STA', 'abs', MARK)
    g.label('idle'); g.op('JMP', 'abs', 'idle')
    g.label('nmi'); g.op('RTI')
    return g.resolve(), g.labels


def side(files, name=NAME, side_no=0, count=None, hidden=()):
    blocks = [disk_info(name=name, side=side_no, disk=0), amount(len(files) if count is None else count)]
    for n, (fid, fname, addr, ftype, data) in enumerate(list(files) + list(hidden)):
        blocks.append(header(n, fid, fname, addr, len(data), ftype))
        blocks.append(b'\x04' + data)
    return fds_side(blocks)


def disks():
    code, labels = game()
    vectors = b''.join(v.to_bytes(2, 'little') for v in (labels['nmi'], labels['nmi'], labels['nmi'],
                                                        labels['reset'], labels['nmi']))
    license_ = payload(1, 0xE0)
    main = [
        (0x00, b'KYODAKU-', 0x2800, 2, license_),
        (0x01, b'CHRPART ', 0x0400, 1, payload(2, 0x300)),
        (0x02, b'PALETTE ', 0x3F00, 2, bytes(x & 0x3F for x in payload(3, 0x20))),
        (0x03, b'GAME    ', 0x6000, 0, code + payload(4, 0x200 - len(code))),
        (0x04, b'OVERLAP ', 0x6100, 0, payload(5, 0x180)),
        (0x05, b'VECTORS ', 0xDFF6, 0, vectors),
        (0x06, b'ROMWRITE', 0xE000, 0, payload(6, 0x10)),
        (0x07, b'CPURAM  ', 0x0200, 0, payload(7, 0x80)),
        (0x08, b'STACKPG ', 0x0100, 0, payload(8, 0x20)),
        (0x09, b'PASTFFFF', 0xFFF0, 0, payload(9, 0x20)),
        (0x30, b'NOTBOOT ', 0x7000, 0, payload(10, 0x40)),
        (0x0A, b'NTFILE  ', 0x2C40, 2, payload(11, 0x40)),
    ]
    hidden = [(0x01, b'HIDDEN  ', 0x7100, 0, payload(12, 0x20))]
    out = {
        'main': fwnes([side(main, hidden=hidden), side(main[:4], side_no=1)]),
        'io': fwnes([side(main[:4] + [(0x05, b'IOFILE  ', 0x5FF0, 0, payload(13, 0x20))] + main[5:6])]),
        'mirror': fwnes([side(main[:4] + [(0x05, b'MIRROR  ', 0x0700, 0, payload(14, 0x200))] + main[5:6])]),
        'empty': fwnes([side(main[:4] + [(0x07, b'EMPTY   ', 0x7000, 0, b'')] + main[5:6])]),
        'unproven': fwnes([side(main, name=OTHER)]),
    }
    return out


def side0_crc(image):
    off = 16 if image[:4] == b'FDS\x1a' else 0
    return zlib.crc32(image[off:off + 65500])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out
    (out / 'bios').mkdir(parents=True, exist_ok=True)
    rom, labels = program()
    images = disks()
    for name, data in images.items():
        (out / f'{name}.fds').write_bytes(data)
    proofs = 'hle_boot_proven = [' + ', '.join(f'"0x{side0_crc(d):08X}"' for n, d in images.items()
                                               if n != 'unproven') + ']\n'
    (out / 'bios' / 'boot.rom').write_bytes(rom)
    (out / 'bios' / 'boot.toml').write_text(
        '# Synthetic boot-skip test BIOS (tools/cyc/fds_boot_fixtures.py), not disksys.rom.\n[program]\n'
        f'name = "cyc FDS boot test BIOS"\nsize = {len(rom)}\ncrc32 = "0x{zlib.crc32(rom):08X}"\n'
        f'sha1 = "{hashlib.sha1(rom).hexdigest()}"\n'
        '# the boot model (common/nes_fds_hle.h): the boot LoadFiles call, LoadFiles, the jump into\n'
        '# the game, and LoadFiles\'s rendering-off store for a PPU file\n'
        f'hle_boot_load = "0x{labels["load_call"]:04X}"\nhle_boot_load_entry = "0x{labels["loadfiles"]:04X}"\n'
        f'hle_boot_jump = "0x{labels["jump"]:04X}"\nhle_boot_mask_store = "0x{labels["mask_store"]:04X}"\n'
        '# boots shown equivalent to this BIOS\'s (tools/cyc/test_cyc_fds_boot.py)\n' + proofs, newline='\n')
    (out / 'expect.txt').write_text(f'mark {MARK:04X} C3\nwait {labels["wait"]:04X}\n', newline='\n')
    print(f'fds boot fixtures -> {out} (load ${labels["load_call"]:04X}, jump ${labels["jump"]:04X})')


if __name__ == '__main__':
    main()
