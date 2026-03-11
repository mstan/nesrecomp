#!/usr/bin/env python3
"""
analyze_rom.py — Print bank map, vector table, JSR count per bank.
Usage: python analyze_rom.py F:/Projects/nesrecomp/baserom.nes
"""
import sys
import struct

def analyze(path):
    with open(path, 'rb') as f:
        data = f.read()

    if data[:4] != b'NES\x1a':
        print("ERROR: Not an iNES file")
        return

    prg_banks = data[4]
    chr_banks = data[5]
    mapper = ((data[6] >> 4) & 0x0F) | (data[7] & 0xF0)
    has_trainer = bool(data[6] & 0x04)

    prg_start = 16 + (512 if has_trainer else 0)
    prg_size = prg_banks * 0x4000
    prg_data = data[prg_start:prg_start + prg_size]

    print(f"ROM: {path}")
    print(f"  File size:  {len(data):,} bytes")
    print(f"  PRG banks:  {prg_banks} x 16KB = {prg_banks*16}KB")
    print(f"  CHR banks:  {chr_banks} x 8KB  (0 = CHR RAM)")
    print(f"  Mapper:     {mapper}")
    print(f"  Trainer:    {has_trainer}")
    print()

    # Read fixed bank vectors
    fixed = prg_data[(prg_banks-1)*0x4000:]
    nmi   = struct.unpack_from('<H', fixed, 0x3FFA)[0]
    reset = struct.unpack_from('<H', fixed, 0x3FFC)[0]
    irq   = struct.unpack_from('<H', fixed, 0x3FFE)[0]
    print(f"Vectors (from fixed bank 15):")
    print(f"  NMI:   ${nmi:04X}")
    print(f"  RESET: ${reset:04X}")
    print(f"  IRQ:   ${irq:04X}")
    print()

    # JSR count per bank
    print("JSR count per PRG bank:")
    for bank in range(prg_banks):
        bank_data = prg_data[bank*0x4000:(bank+1)*0x4000]
        jsr_count = bank_data.count(b'\x20')  # $20 = JSR
        base = 0xC000 if bank == prg_banks-1 else 0x8000
        role = "(fixed)" if bank == prg_banks-1 else "(switchable)"
        print(f"  Bank {bank:2d} @ ${base:04X} {role}: {jsr_count} JSR opcodes")

    print()
    print("File offsets:")
    for bank in range(prg_banks):
        offset = prg_start + bank * 0x4000
        print(f"  Bank {bank:2d}: file offset 0x{offset:05X} ({offset})")


if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'F:/Projects/nesrecomp/baserom.nes'
    analyze(path)
