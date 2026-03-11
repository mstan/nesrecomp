"""Minimal 6502 disassembler for bank 5 analysis."""

OPCODES = {
    0x00: ("BRK", 1), 0x01: ("ORA (zp,X)", 2), 0x05: ("ORA zp", 2),
    0x06: ("ASL zp", 2), 0x08: ("PHP", 1), 0x09: ("ORA #", 2),
    0x0A: ("ASL A", 1), 0x0D: ("ORA abs", 3), 0x10: ("BPL", 2),
    0x18: ("CLC", 1), 0x20: ("JSR", 3), 0x21: ("AND (zp,X)", 2),
    0x24: ("BIT zp", 2), 0x25: ("AND zp", 2), 0x26: ("ROL zp", 2),
    0x28: ("PLP", 1), 0x29: ("AND #", 2), 0x2A: ("ROL A", 1),
    0x2C: ("BIT abs", 3), 0x2D: ("AND abs", 3), 0x30: ("BMI", 2),
    0x38: ("SEC", 1), 0x40: ("RTI", 1), 0x45: ("EOR zp", 2),
    0x48: ("PHA", 1), 0x49: ("EOR #", 2), 0x4A: ("LSR A", 1),
    0x4C: ("JMP abs", 3), 0x4D: ("EOR abs", 3), 0x50: ("BVC", 2),
    0x51: ("EOR (zp),Y", 2), 0x55: ("EOR zp,X", 2), 0x58: ("CLI", 1),
    0x60: ("RTS", 1), 0x61: ("ADC (zp,X)", 2), 0x65: ("ADC zp", 2),
    0x68: ("PLA", 1), 0x69: ("ADC #", 2), 0x6A: ("ROR A", 1),
    0x6C: ("JMP (abs)", 3), 0x6D: ("ADC abs", 3), 0x70: ("BVS", 2),
    0x71: ("ADC (zp),Y", 2), 0x75: ("ADC zp,X", 2), 0x78: ("SEI", 1),
    0x79: ("ADC abs,Y", 3), 0x7D: ("ADC abs,X", 3),
    0x84: ("STY zp", 2), 0x85: ("STA zp", 2), 0x86: ("STX zp", 2),
    0x88: ("DEY", 1), 0x8A: ("TXA", 1), 0x8C: ("STY abs", 3),
    0x8D: ("STA abs", 3), 0x8E: ("STX abs", 3), 0x90: ("BCC", 2),
    0x91: ("STA (zp),Y", 2), 0x94: ("STY zp,X", 2), 0x95: ("STA zp,X", 2),
    0x98: ("TYA", 1), 0x99: ("STA abs,Y", 3), 0x9A: ("TXS", 1),
    0x9D: ("STA abs,X", 3), 0xA0: ("LDY #", 2), 0xA1: ("LDA (zp,X)", 2),
    0xA2: ("LDX #", 2), 0xA4: ("LDY zp", 2), 0xA5: ("LDA zp", 2),
    0xA6: ("LDX zp", 2), 0xA8: ("TAY", 1), 0xA9: ("LDA #", 2),
    0xAA: ("TAX", 1), 0xAC: ("LDY abs", 3), 0xAD: ("LDA abs", 3),
    0xAE: ("LDX abs", 3), 0xB0: ("BCS", 2), 0xB1: ("LDA (zp),Y", 2),
    0xB4: ("LDY zp,X", 2), 0xB5: ("LDA zp,X", 2), 0xB8: ("CLV", 1),
    0xB9: ("LDA abs,Y", 3), 0xBD: ("LDA abs,X", 3), 0xBE: ("LDX abs,Y", 3),
    0xC0: ("CPY #", 2), 0xC1: ("CMP (zp,X)", 2), 0xC4: ("CPY zp", 2),
    0xC5: ("CMP zp", 2), 0xC6: ("DEC zp", 2), 0xC8: ("INY", 1),
    0xC9: ("CMP #", 2), 0xCA: ("DEX", 1), 0xCC: ("CPY abs", 3),
    0xCD: ("CMP abs", 3), 0xCE: ("DEC abs", 3), 0xD0: ("BNE", 2),
    0xD1: ("CMP (zp),Y", 2), 0xD5: ("CMP zp,X", 2), 0xD8: ("CLD", 1),
    0xD9: ("CMP abs,Y", 3), 0xDD: ("CMP abs,X", 3), 0xDE: ("DEC abs,X", 3),
    0xE0: ("CPX #", 2), 0xE4: ("CPX zp", 2), 0xE5: ("SBC zp", 2),
    0xE6: ("INC zp", 2), 0xE8: ("INX", 1), 0xE9: ("SBC #", 2),
    0xEA: ("NOP", 1), 0xEC: ("CPX abs", 3), 0xED: ("SBC abs", 3),
    0xEE: ("INC abs", 3), 0xF0: ("BEQ", 2), 0xF1: ("SBC (zp),Y", 2),
    0xF5: ("SBC zp,X", 2), 0xF8: ("SED", 1), 0xF9: ("SBC abs,Y", 3),
    0xFD: ("SBC abs,X", 3), 0xFE: ("INC abs,X", 3),
}

data = open('F:/Projects/nesrecomp/banks/bank05.bin','rb').read()
BASE = 0x8000

def dis(start_nes, count=50, label=None):
    if label:
        print(f"\n=== {label} (${start_nes:04X}) ===")
    pc = start_nes - BASE
    for _ in range(count):
        if pc < 0 or pc >= len(data): break
        op = data[pc]
        if op not in OPCODES:
            print(f"  {BASE+pc:04X}: {op:02X}        ??? (unknown opcode ${op:02X})")
            pc += 1
            continue
        mn, sz = OPCODES[op]
        raw = data[pc:pc+sz]
        raw_hex = ' '.join(f'{b:02X}' for b in raw)
        if sz == 1:
            arg = ""
        elif sz == 2:
            v = raw[1]
            if mn[:1] == 'B':  # branch
                target = BASE + pc + 2 + (v if v < 0x80 else v - 0x100)
                arg = f"${target:04X}"
            else:
                arg = f"${v:02X}"
        else:
            v = raw[1] | (raw[2] << 8)
            arg = f"${v:04X}"
        print(f"  {BASE+pc:04X}: {raw_hex:<8}  {mn} {arg}")
        pc += sz
        if op in (0x60, 0x40):  # RTS, RTI — stop
            break
        if op == 0x4C:  # JMP abs — show target then stop
            break

# Jump table
dis(0x8000, 8, "Jump table")

# $8009 -> JMP $800D -> actual func
dis(0x800D, 80, "$800D (via $8009)")

# Key branches from $800D
dis(0x802E, 40, "$802E (BNE branch, $0120 != 0)")
dis(0x8038, 40, "$8038 (BEQ branch, $FA == 0)")
dis(0x8079, 40, "$8079 (BPL branch, $FA positive)")
dis(0x8106, 40, "$8106 (JMP target, $FA negative)")

# $8003 -> JMP $862F -> actual func
dis(0x862F, 80, "$862F (via $8003)")
dis(0x8626, 20, "$8626 (BCS target in $862F)")
