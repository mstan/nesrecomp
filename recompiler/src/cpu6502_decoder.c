/*
 * cpu6502_decoder.c — 256-entry 6502 opcode table with cycle counts
 * Valid 6502 has ~151 opcodes across 13 addressing modes.
 * Undocumented/illegal opcodes have CORRECT sizes matching NMOS 6502 behavior.
 * MN_LAX: undocumented LDA+LDX combo, emitted with behavior.
 * Other illegals: MN_ILLEGAL with correct size (treated as sized NOP in codegen).
 *
 * Cycle counts are BASE cycles (not including page-cross penalties for
 * indexed reads or branch-taken penalties).  These are used by the runner's
 * maybe_trigger_vblank() to simulate NMI timing.
 *
 * Use data_region in game.cfg to exclude data areas from pointer scanning
 * rather than relying on incorrect opcode sizes as a workaround.
 */
#include "cpu6502_decoder.h"

/*                              mnemonic     mode    sz  cyc */
const OpcodeEntry g_opcode_table[256] = {
    /* 0x00 */ {MN_BRK,     AM_IMP,   1, 7},
    /* 0x01 */ {MN_ORA,     AM_INDX,  2, 6},
    /* 0x02 */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* KIL */
    /* 0x03 */ {MN_ILLEGAL, AM_INDX,  2, 8},      /* SLO (zp,X) */
    /* 0x04 */ {MN_ILLEGAL, AM_ZP,    2, 3},      /* DOP zp */
    /* 0x05 */ {MN_ORA,     AM_ZP,    2, 3},
    /* 0x06 */ {MN_ASL,     AM_ZP,    2, 5},
    /* 0x07 */ {MN_ILLEGAL, AM_ZP,    2, 5},      /* SLO zp */
    /* 0x08 */ {MN_PHP,     AM_IMP,   1, 3},
    /* 0x09 */ {MN_ORA,     AM_IMM,   2, 2},
    /* 0x0A */ {MN_ASL,     AM_ACC,   1, 2},
    /* 0x0B */ {MN_ILLEGAL, AM_IMM,   2, 2},      /* AAC #imm */
    /* 0x0C */ {MN_ILLEGAL, AM_ABS,   3, 4},      /* TOP abs */
    /* 0x0D */ {MN_ORA,     AM_ABS,   3, 4},
    /* 0x0E */ {MN_ASL,     AM_ABS,   3, 6},
    /* 0x0F */ {MN_ILLEGAL, AM_ABS,   3, 6},      /* SLO abs */
    /* 0x10 */ {MN_BPL,     AM_REL,   2, 2},
    /* 0x11 */ {MN_ORA,     AM_INDY,  2, 5},
    /* 0x12 */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* KIL */
    /* 0x13 */ {MN_ILLEGAL, AM_INDY,  2, 8},      /* SLO (zp),Y */
    /* 0x14 */ {MN_ILLEGAL, AM_ZPX,   2, 4},      /* DOP zp,X */
    /* 0x15 */ {MN_ORA,     AM_ZPX,   2, 4},
    /* 0x16 */ {MN_ASL,     AM_ZPX,   2, 6},
    /* 0x17 */ {MN_ILLEGAL, AM_ZPX,   2, 6},      /* SLO zp,X */
    /* 0x18 */ {MN_CLC,     AM_IMP,   1, 2},
    /* 0x19 */ {MN_ORA,     AM_ABSY,  3, 4},
    /* 0x1A */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* NOP (1-byte) */
    /* 0x1B */ {MN_ILLEGAL, AM_ABSY,  3, 7},      /* SLO abs,Y */
    /* 0x1C */ {MN_ILLEGAL, AM_ABSX,  3, 4},      /* TOP abs,X */
    /* 0x1D */ {MN_ORA,     AM_ABSX,  3, 4},
    /* 0x1E */ {MN_ASL,     AM_ABSX,  3, 7},
    /* 0x1F */ {MN_ILLEGAL, AM_ABSX,  3, 7},      /* SLO abs,X */
    /* 0x20 */ {MN_JSR,     AM_ABS,   3, 6},
    /* 0x21 */ {MN_AND,     AM_INDX,  2, 6},
    /* 0x22 */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* KIL */
    /* 0x23 */ {MN_ILLEGAL, AM_INDX,  2, 8},      /* RLA (zp,X) */
    /* 0x24 */ {MN_BIT,     AM_ZP,    2, 3},
    /* 0x25 */ {MN_AND,     AM_ZP,    2, 3},
    /* 0x26 */ {MN_ROL,     AM_ZP,    2, 5},
    /* 0x27 */ {MN_ILLEGAL, AM_ZP,    2, 5},      /* RLA zp */
    /* 0x28 */ {MN_PLP,     AM_IMP,   1, 4},
    /* 0x29 */ {MN_AND,     AM_IMM,   2, 2},
    /* 0x2A */ {MN_ROL,     AM_ACC,   1, 2},
    /* 0x2B */ {MN_ILLEGAL, AM_IMM,   2, 2},      /* AAC #imm */
    /* 0x2C */ {MN_BIT,     AM_ABS,   3, 4},
    /* 0x2D */ {MN_AND,     AM_ABS,   3, 4},
    /* 0x2E */ {MN_ROL,     AM_ABS,   3, 6},
    /* 0x2F */ {MN_ILLEGAL, AM_ABS,   3, 6},      /* RLA abs */
    /* 0x30 */ {MN_BMI,     AM_REL,   2, 2},
    /* 0x31 */ {MN_AND,     AM_INDY,  2, 5},
    /* 0x32 */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* KIL */
    /* 0x33 */ {MN_ILLEGAL, AM_INDY,  2, 8},      /* RLA (zp),Y */
    /* 0x34 */ {MN_ILLEGAL, AM_ZPX,   2, 4},      /* DOP zp,X */
    /* 0x35 */ {MN_AND,     AM_ZPX,   2, 4},
    /* 0x36 */ {MN_ROL,     AM_ZPX,   2, 6},
    /* 0x37 */ {MN_ILLEGAL, AM_ZPX,   2, 6},      /* RLA zp,X */
    /* 0x38 */ {MN_SEC,     AM_IMP,   1, 2},
    /* 0x39 */ {MN_AND,     AM_ABSY,  3, 4},
    /* 0x3A */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* NOP */
    /* 0x3B */ {MN_ILLEGAL, AM_ABSY,  3, 7},      /* RLA abs,Y */
    /* 0x3C */ {MN_ILLEGAL, AM_ABSX,  3, 4},      /* TOP abs,X */
    /* 0x3D */ {MN_AND,     AM_ABSX,  3, 4},
    /* 0x3E */ {MN_ROL,     AM_ABSX,  3, 7},
    /* 0x3F */ {MN_ILLEGAL, AM_ABSX,  3, 7},      /* RLA abs,X */
    /* 0x40 */ {MN_RTI,     AM_IMP,   1, 6},
    /* 0x41 */ {MN_EOR,     AM_INDX,  2, 6},
    /* 0x42 */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* KIL */
    /* 0x43 */ {MN_ILLEGAL, AM_INDX,  2, 8},      /* SRE (zp,X) */
    /* 0x44 */ {MN_ILLEGAL, AM_ZP,    2, 3},      /* DOP zp */
    /* 0x45 */ {MN_EOR,     AM_ZP,    2, 3},
    /* 0x46 */ {MN_LSR,     AM_ZP,    2, 5},
    /* 0x47 */ {MN_ILLEGAL, AM_ZP,    2, 5},      /* SRE zp */
    /* 0x48 */ {MN_PHA,     AM_IMP,   1, 3},
    /* 0x49 */ {MN_EOR,     AM_IMM,   2, 2},
    /* 0x4A */ {MN_LSR,     AM_ACC,   1, 2},
    /* 0x4B */ {MN_ILLEGAL, AM_IMM,   2, 2},      /* ALR #imm */
    /* 0x4C */ {MN_JMP,     AM_ABS,   3, 3},
    /* 0x4D */ {MN_EOR,     AM_ABS,   3, 4},
    /* 0x4E */ {MN_LSR,     AM_ABS,   3, 6},
    /* 0x4F */ {MN_ILLEGAL, AM_ABS,   3, 6},      /* SRE abs */
    /* 0x50 */ {MN_BVC,     AM_REL,   2, 2},
    /* 0x51 */ {MN_EOR,     AM_INDY,  2, 5},
    /* 0x52 */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* KIL */
    /* 0x53 */ {MN_ILLEGAL, AM_INDY,  2, 8},      /* SRE (zp),Y */
    /* 0x54 */ {MN_ILLEGAL, AM_ZPX,   2, 4},      /* DOP zp,X */
    /* 0x55 */ {MN_EOR,     AM_ZPX,   2, 4},
    /* 0x56 */ {MN_LSR,     AM_ZPX,   2, 6},
    /* 0x57 */ {MN_ILLEGAL, AM_ZPX,   2, 6},      /* SRE zp,X */
    /* 0x58 */ {MN_CLI,     AM_IMP,   1, 2},
    /* 0x59 */ {MN_EOR,     AM_ABSY,  3, 4},
    /* 0x5A */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* NOP */
    /* 0x5B */ {MN_ILLEGAL, AM_ABSY,  3, 7},      /* SRE abs,Y */
    /* 0x5C */ {MN_ILLEGAL, AM_ABSX,  3, 4},      /* TOP abs,X */
    /* 0x5D */ {MN_EOR,     AM_ABSX,  3, 4},
    /* 0x5E */ {MN_LSR,     AM_ABSX,  3, 7},
    /* 0x5F */ {MN_ILLEGAL, AM_ABSX,  3, 7},      /* SRE abs,X */
    /* 0x60 */ {MN_RTS,     AM_IMP,   1, 6},
    /* 0x61 */ {MN_ADC,     AM_INDX,  2, 6},
    /* 0x62 */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* KIL */
    /* 0x63 */ {MN_ILLEGAL, AM_INDX,  2, 8},      /* RRA (zp,X) */
    /* 0x64 */ {MN_ILLEGAL, AM_ZP,    2, 3},      /* DOP zp */
    /* 0x65 */ {MN_ADC,     AM_ZP,    2, 3},
    /* 0x66 */ {MN_ROR,     AM_ZP,    2, 5},
    /* 0x67 */ {MN_ILLEGAL, AM_ZP,    2, 5},      /* RRA zp */
    /* 0x68 */ {MN_PLA,     AM_IMP,   1, 4},
    /* 0x69 */ {MN_ADC,     AM_IMM,   2, 2},
    /* 0x6A */ {MN_ROR,     AM_ACC,   1, 2},
    /* 0x6B */ {MN_ILLEGAL, AM_IMM,   2, 2},      /* ARR #imm */
    /* 0x6C */ {MN_JMP,     AM_IND,   3, 5},
    /* 0x6D */ {MN_ADC,     AM_ABS,   3, 4},
    /* 0x6E */ {MN_ROR,     AM_ABS,   3, 6},
    /* 0x6F */ {MN_ILLEGAL, AM_ABS,   3, 6},      /* RRA abs */
    /* 0x70 */ {MN_BVS,     AM_REL,   2, 2},
    /* 0x71 */ {MN_ADC,     AM_INDY,  2, 5},
    /* 0x72 */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* KIL */
    /* 0x73 */ {MN_ILLEGAL, AM_INDY,  2, 8},      /* RRA (zp),Y */
    /* 0x74 */ {MN_ILLEGAL, AM_ZPX,   2, 4},      /* DOP zp,X */
    /* 0x75 */ {MN_ADC,     AM_ZPX,   2, 4},
    /* 0x76 */ {MN_ROR,     AM_ZPX,   2, 6},
    /* 0x77 */ {MN_ILLEGAL, AM_ZPX,   2, 6},      /* RRA zp,X */
    /* 0x78 */ {MN_SEI,     AM_IMP,   1, 2},
    /* 0x79 */ {MN_ADC,     AM_ABSY,  3, 4},
    /* 0x7A */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* NOP */
    /* 0x7B */ {MN_ILLEGAL, AM_ABSY,  3, 7},      /* RRA abs,Y */
    /* 0x7C */ {MN_ILLEGAL, AM_ABSX,  3, 4},      /* TOP abs,X */
    /* 0x7D */ {MN_ADC,     AM_ABSX,  3, 4},
    /* 0x7E */ {MN_ROR,     AM_ABSX,  3, 7},
    /* 0x7F */ {MN_ILLEGAL, AM_ABSX,  3, 7},      /* RRA abs,X */
    /* 0x80 */ {MN_ILLEGAL, AM_IMM,   2, 2},      /* DOP #imm */
    /* 0x81 */ {MN_STA,     AM_INDX,  2, 6},
    /* 0x82 */ {MN_ILLEGAL, AM_IMM,   2, 2},      /* DOP #imm */
    /* 0x83 */ {MN_ILLEGAL, AM_INDX,  2, 6},      /* SAX (zp,X) */
    /* 0x84 */ {MN_STY,     AM_ZP,    2, 3},
    /* 0x85 */ {MN_STA,     AM_ZP,    2, 3},
    /* 0x86 */ {MN_STX,     AM_ZP,    2, 3},
    /* 0x87 */ {MN_ILLEGAL, AM_ZP,    2, 3},      /* SAX zp */
    /* 0x88 */ {MN_DEY,     AM_IMP,   1, 2},
    /* 0x89 */ {MN_ILLEGAL, AM_IMM,   2, 2},      /* DOP #imm */
    /* 0x8A */ {MN_TXA,     AM_IMP,   1, 2},
    /* 0x8B */ {MN_ILLEGAL, AM_IMM,   2, 2},      /* XAA #imm */
    /* 0x8C */ {MN_STY,     AM_ABS,   3, 4},
    /* 0x8D */ {MN_STA,     AM_ABS,   3, 4},
    /* 0x8E */ {MN_STX,     AM_ABS,   3, 4},
    /* 0x8F */ {MN_ILLEGAL, AM_ABS,   3, 4},      /* SAX abs */
    /* 0x90 */ {MN_BCC,     AM_REL,   2, 2},
    /* 0x91 */ {MN_STA,     AM_INDY,  2, 6},
    /* 0x92 */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* KIL */
    /* 0x93 */ {MN_ILLEGAL, AM_INDY,  2, 6},      /* SHA (zp),Y */
    /* 0x94 */ {MN_STY,     AM_ZPX,   2, 4},
    /* 0x95 */ {MN_STA,     AM_ZPX,   2, 4},
    /* 0x96 */ {MN_STX,     AM_ZPY,   2, 4},
    /* 0x97 */ {MN_ILLEGAL, AM_ZPY,   2, 4},      /* SAX zp,Y */
    /* 0x98 */ {MN_TYA,     AM_IMP,   1, 2},
    /* 0x99 */ {MN_STA,     AM_ABSY,  3, 5},
    /* 0x9A */ {MN_TXS,     AM_IMP,   1, 2},
    /* 0x9B */ {MN_ILLEGAL, AM_ABSY,  3, 5},      /* TAS abs,Y */
    /* 0x9C */ {MN_ILLEGAL, AM_ABSX,  3, 5},      /* SHY abs,X */
    /* 0x9D */ {MN_STA,     AM_ABSX,  3, 5},
    /* 0x9E */ {MN_ILLEGAL, AM_ABSY,  3, 5},      /* SHX abs,Y */
    /* 0x9F */ {MN_ILLEGAL, AM_ABSY,  3, 5},      /* SHA abs,Y */
    /* 0xA0 */ {MN_LDY,     AM_IMM,   2, 2},
    /* 0xA1 */ {MN_LDA,     AM_INDX,  2, 6},
    /* 0xA2 */ {MN_LDX,     AM_IMM,   2, 2},
    /* 0xA3 */ {MN_LAX,     AM_INDX,  2, 6},      /* LAX (zp,X) */
    /* 0xA4 */ {MN_LDY,     AM_ZP,    2, 3},
    /* 0xA5 */ {MN_LDA,     AM_ZP,    2, 3},
    /* 0xA6 */ {MN_LDX,     AM_ZP,    2, 3},
    /* 0xA7 */ {MN_LAX,     AM_ZP,    2, 3},      /* LAX zp */
    /* 0xA8 */ {MN_TAY,     AM_IMP,   1, 2},
    /* 0xA9 */ {MN_LDA,     AM_IMM,   2, 2},
    /* 0xAA */ {MN_TAX,     AM_IMP,   1, 2},
    /* 0xAB */ {MN_LAX,     AM_IMM,   2, 2},      /* LAX #imm */
    /* 0xAC */ {MN_LDY,     AM_ABS,   3, 4},
    /* 0xAD */ {MN_LDA,     AM_ABS,   3, 4},
    /* 0xAE */ {MN_LDX,     AM_ABS,   3, 4},
    /* 0xAF */ {MN_LAX,     AM_ABS,   3, 4},      /* LAX abs */
    /* 0xB0 */ {MN_BCS,     AM_REL,   2, 2},
    /* 0xB1 */ {MN_LDA,     AM_INDY,  2, 5},
    /* 0xB2 */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* KIL */
    /* 0xB3 */ {MN_LAX,     AM_INDY,  2, 5},      /* LAX (zp),Y */
    /* 0xB4 */ {MN_LDY,     AM_ZPX,   2, 4},
    /* 0xB5 */ {MN_LDA,     AM_ZPX,   2, 4},
    /* 0xB6 */ {MN_LDX,     AM_ZPY,   2, 4},
    /* 0xB7 */ {MN_LAX,     AM_ZPY,   2, 4},      /* LAX zp,Y */
    /* 0xB8 */ {MN_CLV,     AM_IMP,   1, 2},
    /* 0xB9 */ {MN_LDA,     AM_ABSY,  3, 4},
    /* 0xBA */ {MN_TSX,     AM_IMP,   1, 2},
    /* 0xBB */ {MN_ILLEGAL, AM_ABSY,  3, 4},      /* LAS abs,Y */
    /* 0xBC */ {MN_LDY,     AM_ABSX,  3, 4},
    /* 0xBD */ {MN_LDA,     AM_ABSX,  3, 4},
    /* 0xBE */ {MN_LDX,     AM_ABSY,  3, 4},
    /* 0xBF */ {MN_LAX,     AM_ABSY,  3, 4},      /* LAX abs,Y */
    /* 0xC0 */ {MN_CPY,     AM_IMM,   2, 2},
    /* 0xC1 */ {MN_CMP,     AM_INDX,  2, 6},
    /* 0xC2 */ {MN_ILLEGAL, AM_IMM,   2, 2},      /* DOP #imm */
    /* 0xC3 */ {MN_ILLEGAL, AM_INDX,  2, 8},      /* DCP (zp,X) */
    /* 0xC4 */ {MN_CPY,     AM_ZP,    2, 3},
    /* 0xC5 */ {MN_CMP,     AM_ZP,    2, 3},
    /* 0xC6 */ {MN_DEC,     AM_ZP,    2, 5},
    /* 0xC7 */ {MN_ILLEGAL, AM_ZP,    2, 5},      /* DCP zp */
    /* 0xC8 */ {MN_INY,     AM_IMP,   1, 2},
    /* 0xC9 */ {MN_CMP,     AM_IMM,   2, 2},
    /* 0xCA */ {MN_DEX,     AM_IMP,   1, 2},
    /* 0xCB */ {MN_ILLEGAL, AM_IMM,   2, 2},      /* AXS #imm */
    /* 0xCC */ {MN_CPY,     AM_ABS,   3, 4},
    /* 0xCD */ {MN_CMP,     AM_ABS,   3, 4},
    /* 0xCE */ {MN_DEC,     AM_ABS,   3, 6},
    /* 0xCF */ {MN_ILLEGAL, AM_ABS,   3, 6},      /* DCP abs */
    /* 0xD0 */ {MN_BNE,     AM_REL,   2, 2},
    /* 0xD1 */ {MN_CMP,     AM_INDY,  2, 5},
    /* 0xD2 */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* KIL */
    /* 0xD3 */ {MN_ILLEGAL, AM_INDY,  2, 8},      /* DCP (zp),Y */
    /* 0xD4 */ {MN_ILLEGAL, AM_ZPX,   2, 4},      /* DOP zp,X */
    /* 0xD5 */ {MN_CMP,     AM_ZPX,   2, 4},
    /* 0xD6 */ {MN_DEC,     AM_ZPX,   2, 6},
    /* 0xD7 */ {MN_ILLEGAL, AM_ZPX,   2, 6},      /* DCP zp,X */
    /* 0xD8 */ {MN_CLD,     AM_IMP,   1, 2},
    /* 0xD9 */ {MN_CMP,     AM_ABSY,  3, 4},
    /* 0xDA */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* NOP */
    /* 0xDB */ {MN_ILLEGAL, AM_ABSY,  3, 7},      /* DCP abs,Y */
    /* 0xDC */ {MN_ILLEGAL, AM_ABSX,  3, 4},      /* TOP abs,X */
    /* 0xDD */ {MN_CMP,     AM_ABSX,  3, 4},
    /* 0xDE */ {MN_DEC,     AM_ABSX,  3, 7},
    /* 0xDF */ {MN_ILLEGAL, AM_ABSX,  3, 7},      /* DCP abs,X */
    /* 0xE0 */ {MN_CPX,     AM_IMM,   2, 2},
    /* 0xE1 */ {MN_SBC,     AM_INDX,  2, 6},
    /* 0xE2 */ {MN_ILLEGAL, AM_IMM,   2, 2},      /* DOP #imm */
    /* 0xE3 */ {MN_ILLEGAL, AM_INDX,  2, 8},      /* ISC (zp,X) */
    /* 0xE4 */ {MN_CPX,     AM_ZP,    2, 3},
    /* 0xE5 */ {MN_SBC,     AM_ZP,    2, 3},
    /* 0xE6 */ {MN_INC,     AM_ZP,    2, 5},
    /* 0xE7 */ {MN_ILLEGAL, AM_ZP,    2, 5},      /* ISC zp */
    /* 0xE8 */ {MN_INX,     AM_IMP,   1, 2},
    /* 0xE9 */ {MN_SBC,     AM_IMM,   2, 2},
    /* 0xEA */ {MN_NOP,     AM_IMP,   1, 2},
    /* 0xEB */ {MN_ILLEGAL, AM_IMM,   2, 2},      /* SBC #imm (mirror) */
    /* 0xEC */ {MN_CPX,     AM_ABS,   3, 4},
    /* 0xED */ {MN_SBC,     AM_ABS,   3, 4},
    /* 0xEE */ {MN_INC,     AM_ABS,   3, 6},
    /* 0xEF */ {MN_ILLEGAL, AM_ABS,   3, 6},      /* ISC abs */
    /* 0xF0 */ {MN_BEQ,     AM_REL,   2, 2},
    /* 0xF1 */ {MN_SBC,     AM_INDY,  2, 5},
    /* 0xF2 */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* KIL */
    /* 0xF3 */ {MN_ILLEGAL, AM_INDY,  2, 8},      /* ISC (zp),Y */
    /* 0xF4 */ {MN_ILLEGAL, AM_ZPX,   2, 4},      /* DOP zp,X */
    /* 0xF5 */ {MN_SBC,     AM_ZPX,   2, 4},
    /* 0xF6 */ {MN_INC,     AM_ZPX,   2, 6},
    /* 0xF7 */ {MN_ILLEGAL, AM_ZPX,   2, 6},      /* ISC zp,X */
    /* 0xF8 */ {MN_SED,     AM_IMP,   1, 2},
    /* 0xF9 */ {MN_SBC,     AM_ABSY,  3, 4},
    /* 0xFA */ {MN_ILLEGAL, AM_IMP,   1, 2},      /* NOP */
    /* 0xFB */ {MN_ILLEGAL, AM_ABSY,  3, 7},      /* ISC abs,Y */
    /* 0xFC */ {MN_ILLEGAL, AM_ABSX,  3, 4},      /* TOP abs,X */
    /* 0xFD */ {MN_SBC,     AM_ABSX,  3, 4},
    /* 0xFE */ {MN_INC,     AM_ABSX,  3, 7},
    /* 0xFF */ {MN_ILLEGAL, AM_ABSX,  3, 7},      /* ISC abs,X */
};

const char *mnemonic_name(OpMnemonic mn) {
    static const char *names[] = {
        "ADC","AND","ASL","BCC","BCS","BEQ","BIT","BMI",
        "BNE","BPL","BRK","BVC","BVS","CLC","CLD","CLI",
        "CLV","CMP","CPX","CPY","DEC","DEX","DEY","EOR",
        "INC","INX","INY","JMP","JSR","LDA","LDX","LDY",
        "LSR","NOP","ORA","PHA","PHP","PLA","PLP","ROL",
        "ROR","RTI","RTS","SBC","SEC","SED","SEI","STA",
        "STX","STY","TAX","TAY","TSX","TXA","TXS","TYA",
        "LAX","???"
    };
    if (mn > MN_ILLEGAL) mn = MN_ILLEGAL;
    return names[mn];
}

const char *addrmode_name(AddrMode am) {
    static const char *names[] = {
        "IMP","ACC","IMM","ZP","ZPX","ZPY","REL","ABS","ABSX","ABSY","IND","INDX","INDY"
    };
    return names[am];
}
