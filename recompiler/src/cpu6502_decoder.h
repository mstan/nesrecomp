/*
 * cpu6502_decoder.h — 6502 opcode table
 */
#pragma once
#include <stdint.h>

typedef enum {
    MN_ADC, MN_AND, MN_ASL, MN_BCC, MN_BCS, MN_BEQ, MN_BIT, MN_BMI,
    MN_BNE, MN_BPL, MN_BRK, MN_BVC, MN_BVS, MN_CLC, MN_CLD, MN_CLI,
    MN_CLV, MN_CMP, MN_CPX, MN_CPY, MN_DEC, MN_DEX, MN_DEY, MN_EOR,
    MN_INC, MN_INX, MN_INY, MN_JMP, MN_JSR, MN_LDA, MN_LDX, MN_LDY,
    MN_LSR, MN_NOP, MN_ORA, MN_PHA, MN_PHP, MN_PLA, MN_PLP, MN_ROL,
    MN_ROR, MN_RTI, MN_RTS, MN_SBC, MN_SEC, MN_SED, MN_SEI, MN_STA,
    MN_STX, MN_STY, MN_TAX, MN_TAY, MN_TSX, MN_TXA, MN_TXS, MN_TYA,
    MN_LAX,     /* Undocumented: LDA+LDX combined (A=X=mem) */
    MN_ILLEGAL  /* Invalid/undocumented opcode */
} OpMnemonic;

typedef enum {
    AM_IMP,     /* Implied         — no operand */
    AM_ACC,     /* Accumulator     — ASL A */
    AM_IMM,     /* Immediate       — LDA #$nn */
    AM_ZP,      /* Zero Page       — LDA $nn */
    AM_ZPX,     /* Zero Page,X     — LDA $nn,X */
    AM_ZPY,     /* Zero Page,Y     — LDA $nn,Y */
    AM_REL,     /* Relative        — BNE $±nn */
    AM_ABS,     /* Absolute        — LDA $nnnn */
    AM_ABSX,    /* Absolute,X      — LDA $nnnn,X */
    AM_ABSY,    /* Absolute,Y      — LDA $nnnn,Y */
    AM_IND,     /* Indirect        — JMP ($nnnn) */
    AM_INDX,    /* (Indirect,X)    — LDA ($nn,X) */
    AM_INDY     /* (Indirect),Y    — LDA ($nn),Y */
} AddrMode;

typedef struct {
    OpMnemonic mnemonic;
    AddrMode   addr_mode;
    int        size;        /* Instruction size in bytes (1, 2, or 3) */
    int        cycles;      /* Base cycle count (not including page-cross penalties) */
} OpcodeEntry;

/* 256-entry opcode table */
extern const OpcodeEntry g_opcode_table[256];

const char *mnemonic_name(OpMnemonic mn);
const char *addrmode_name(AddrMode am);
