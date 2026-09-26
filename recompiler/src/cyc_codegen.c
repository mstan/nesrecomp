/*
 * cyc_codegen.c - cycle-accurate code generation (NESRecomp --cycle-accurate)
 *
 * The regular code generator (code_generator.c) maps 6502 subroutines onto C
 * functions and advances time in whole instructions. That model cannot express
 * what hardware test ROMs measure: which address is on the bus during each CPU
 * cycle, when the interrupt lines are polled inside an instruction, and which
 * cycles a DMC/OAM DMA is allowed to steal.
 *
 * This generator describes every 6502 instruction as the sequence of CPU
 * cycles it performs on the bus (runner/cyc/cpu6502.h: cpu_read, cpu_write,
 * with interrupt polls and the instruction's last cycle marked), and emits
 * those templates twice:
 *
 *   - generated/<prefix>_cyc.c: one labeled block per ROM instruction reached
 *     by discovery, with the address, opcode and operands folded to
 *     constants, and branches, jumps and subroutine calls with static
 *     targets compiled to gotos;
 *   - runner/cyc/cpu6502_interp.c (--emit-cycle-interpreter): the
 *     interpreter for everything else, the same templates with operands read
 *     at run time.
 *
 * Because both come from the templates below, recompiled and interpreted
 * execution cannot disagree about an instruction. Both hand over only at
 * instruction boundaries; interrupt entry, BRK and jams are runtime sequences
 * in runner/cyc/cpu6502.c that either can call.
 *
 * The templates follow the NMOS 6502 cycle tables including undocumented
 * opcodes, and were checked cycle for cycle against TriCNES's 6502 (the
 * oracle in runner/cyc/oracle).
 *
 * Banked mappers. Folding a ROM byte to a constant assumes the byte at that
 * CPU address is known, which on a banked cartridge it is not: $8000-$FFFF is
 * four 8KB slots whose contents a game changes at run time. So a block is
 * generated per (bank, slot) pair rather than per address, execution enters
 * one only through a dispatch that checks which bank the mapper has there
 * (hw_prg_bank), and a write that can reach the mapper's registers ends the
 * block so the next instruction is dispatched afresh. Control flow that
 * leaves the slot it started in returns to the scheduler for the same reason,
 * unless the target slot's bank is fixed by the board. Everything the static
 * walk cannot resolve - an entry point reached only through a bank switch -
 * comes from a run, the way RTS and JMP-indirect targets already do
 * (--miss-log, which reports the bank it saw).
 */
#include "cyc_codegen.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <direct.h>
#  define cyc_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define cyc_mkdir(p) mkdir((p), 0755)
#endif

#include "cpu6502_decoder.h"

/* A compiled block is valid for one PRG bank at one CPU address, because the
 * bytes at an address in $8000-$FFFF depend on which bank the mapper has
 * there. 8KB is the finest granularity any supported mapper switches, so the
 * CPU's $8000-$FFFF is four slots and a block is identified by (bank, slot,
 * offset in slot). NROM is the degenerate case: every slot's bank is fixed.
 *
 * One C function per 1KB keeps individual functions small, and one
 * translation unit per bank keeps them compilable in parallel (see
 * SPLITGEN_MIGRATION.md for the same split in the function-level output). */
#define SLOT_SHIFT      13
#define SLOT_SIZE       (1u << SLOT_SHIFT)
#define SLOT_COUNT      4
#define CHUNK_SHIFT     10
#define CHUNKS_PER_SLOT (SLOT_SIZE >> CHUNK_SHIFT)

/* ------------------------------------------------------------------------ */
/* Instruction templates                                                    */
/* ------------------------------------------------------------------------ */

typedef enum {
    K_IMPLIED,  /* dummy read of the next byte, then the operation */
    K_IMM,      /* operand read, then the operation on v */
    K_READ,     /* addressing cycles, then the operation on the value read (v) */
    K_RMW,      /* addressing, read v, write v back, write the result r */
    K_WRITE,    /* addressing, then a write */
    K_SH,       /* SHA/SHS/SHX/SHY: a write whose value and address depend on
                   the address high byte, unless a DMA takes the fix-up cycle */
    K_BRANCH,
    K_JMP,
    K_JMPIND,
    K_JSR,
    K_RTS,
    K_RTI,
    K_PHA,
    K_PHP,
    K_PLA,
    K_PLP,
    K_BRK,
    K_HLT,
} Kind;

typedef struct {
    Kind        kind;
    AddrMode    am;
    const char *name;
    /* K_IMPLIED:        statements.
     * K_IMM, K_READ:    statements using v, the byte read.
     * K_RMW:            statements setting r from v, before the final write.
     * K_WRITE:          the value written.
     * K_BRANCH:         the condition for taking the branch. */
    const char *body;
    const char *post;   /* K_RMW: statements after the final write, using r */
} OpDef;

#define IMPL(n, b)          { K_IMPLIED, AM_IMP, n, b, NULL }
#define IMM(n, b)           { K_IMM, AM_IMM, n, b, NULL }
#define READ(n, am, b)      { K_READ, am, n, b, NULL }
#define RMW(n, am, b, post) { K_RMW, am, n, b, post }
#define WRITE(n, am, v)     { K_WRITE, am, n, v, NULL }
#define SH(n, am)           { K_SH, am, n, NULL, NULL }
#define BRANCH(n, c)        { K_BRANCH, AM_REL, n, c, NULL }
#define HLT                 { K_HLT, AM_IMP, "HLT", NULL, NULL }

#define ORA_V "cpu.a |= v; cpu_nz(cpu.a);"
#define AND_V "cpu.a &= v; cpu_nz(cpu.a);"
#define EOR_V "cpu.a ^= v; cpu_nz(cpu.a);"
#define ADC_V "cpu_adc(v);"
#define SBC_V "cpu_sbc(v);"
#define CMP_V "cpu_cmp(cpu.a, v);"
#define CPX_V "cpu_cmp(cpu.x, v);"
#define CPY_V "cpu_cmp(cpu.y, v);"
#define LDA_V "cpu.a = v; cpu_nz(v);"
#define LDX_V "cpu.x = v; cpu_nz(v);"
#define LDY_V "cpu.y = v; cpu_nz(v);"
#define LAX_V "cpu.a = cpu.x = v; cpu_nz(v);"
#define BIT_V "cpu_bit(v);"
#define NOP_V "(void)v;"
/* LAS ($BB): A, X and S all take v & S. */
#define LAS_V "cpu.a = cpu.x = cpu.s = (uint8_t)(v & cpu.s); cpu_nz(cpu.x);"
/* ANC ($0B, $2B): AND, with C copied from N. */
#define ANC_V "cpu.a &= v; cpu_nz(cpu.a); cpu.c = cpu.n;"

#define ASL_R "r = cpu_asl(v);"
#define LSR_R "r = cpu_lsr(v);"
#define ROL_R "r = cpu_rol(v);"
#define ROR_R "r = cpu_ror(v);"
#define INC_R "r = (uint8_t)(v + 1); cpu_nz(r);"
#define DEC_R "r = (uint8_t)(v - 1); cpu_nz(r);"
#define SLO(am) RMW("SLO", am, ASL_R, "cpu.a |= r; cpu_nz(cpu.a);")
#define RLA(am) RMW("RLA", am, ROL_R, "cpu.a &= r; cpu_nz(cpu.a);")
#define SRE(am) RMW("SRE", am, LSR_R, "cpu.a ^= r; cpu_nz(cpu.a);")
#define RRA(am) RMW("RRA", am, ROR_R, "cpu_adc(r);")
#define DCP(am) RMW("DCP", am, "r = (uint8_t)(v - 1);", "cpu_cmp(cpu.a, r);")
#define ISC(am) RMW("ISC", am, "r = (uint8_t)(v + 1);", "cpu_sbc(r);")

static const OpDef OPS[256] = {
    [0x00] = { K_BRK, AM_IMP, "BRK", NULL, NULL },
    [0x01] = READ("ORA", AM_INDX, ORA_V),
    [0x02] = HLT,
    [0x03] = SLO(AM_INDX),
    [0x04] = READ("NOP", AM_ZP, NOP_V),
    [0x05] = READ("ORA", AM_ZP, ORA_V),
    [0x06] = RMW("ASL", AM_ZP, ASL_R, NULL),
    [0x07] = SLO(AM_ZP),
    [0x08] = { K_PHP, AM_IMP, "PHP", NULL, NULL },
    [0x09] = IMM("ORA", ORA_V),
    [0x0A] = IMPL("ASL", "cpu.a = cpu_asl(cpu.a);"),
    [0x0B] = IMM("ANC", ANC_V),
    [0x0C] = READ("NOP", AM_ABS, NOP_V),
    [0x0D] = READ("ORA", AM_ABS, ORA_V),
    [0x0E] = RMW("ASL", AM_ABS, ASL_R, NULL),
    [0x0F] = SLO(AM_ABS),

    [0x10] = BRANCH("BPL", "!cpu.n"),
    [0x11] = READ("ORA", AM_INDY, ORA_V),
    [0x12] = HLT,
    [0x13] = SLO(AM_INDY),
    [0x14] = READ("NOP", AM_ZPX, NOP_V),
    [0x15] = READ("ORA", AM_ZPX, ORA_V),
    [0x16] = RMW("ASL", AM_ZPX, ASL_R, NULL),
    [0x17] = SLO(AM_ZPX),
    [0x18] = IMPL("CLC", "cpu.c = 0;"),
    [0x19] = READ("ORA", AM_ABSY, ORA_V),
    [0x1A] = IMPL("NOP", ""),
    [0x1B] = SLO(AM_ABSY),
    [0x1C] = READ("NOP", AM_ABSX, NOP_V),
    [0x1D] = READ("ORA", AM_ABSX, ORA_V),
    [0x1E] = RMW("ASL", AM_ABSX, ASL_R, NULL),
    [0x1F] = SLO(AM_ABSX),

    [0x20] = { K_JSR, AM_ABS, "JSR", NULL, NULL },
    [0x21] = READ("AND", AM_INDX, AND_V),
    [0x22] = HLT,
    [0x23] = RLA(AM_INDX),
    [0x24] = READ("BIT", AM_ZP, BIT_V),
    [0x25] = READ("AND", AM_ZP, AND_V),
    [0x26] = RMW("ROL", AM_ZP, ROL_R, NULL),
    [0x27] = RLA(AM_ZP),
    [0x28] = { K_PLP, AM_IMP, "PLP", NULL, NULL },
    [0x29] = IMM("AND", AND_V),
    [0x2A] = IMPL("ROL", "cpu.a = cpu_rol(cpu.a);"),
    [0x2B] = IMM("ANC", ANC_V),
    [0x2C] = READ("BIT", AM_ABS, BIT_V),
    [0x2D] = READ("AND", AM_ABS, AND_V),
    [0x2E] = RMW("ROL", AM_ABS, ROL_R, NULL),
    [0x2F] = RLA(AM_ABS),

    [0x30] = BRANCH("BMI", "cpu.n"),
    [0x31] = READ("AND", AM_INDY, AND_V),
    [0x32] = HLT,
    [0x33] = RLA(AM_INDY),
    [0x34] = READ("NOP", AM_ZPX, NOP_V),
    [0x35] = READ("AND", AM_ZPX, AND_V),
    [0x36] = RMW("ROL", AM_ZPX, ROL_R, NULL),
    [0x37] = RLA(AM_ZPX),
    [0x38] = IMPL("SEC", "cpu.c = 1;"),
    [0x39] = READ("AND", AM_ABSY, AND_V),
    [0x3A] = IMPL("NOP", ""),
    [0x3B] = RLA(AM_ABSY),
    [0x3C] = READ("NOP", AM_ABSX, NOP_V),
    [0x3D] = READ("AND", AM_ABSX, AND_V),
    [0x3E] = RMW("ROL", AM_ABSX, ROL_R, NULL),
    [0x3F] = RLA(AM_ABSX),

    [0x40] = { K_RTI, AM_IMP, "RTI", NULL, NULL },
    [0x41] = READ("EOR", AM_INDX, EOR_V),
    [0x42] = HLT,
    [0x43] = SRE(AM_INDX),
    [0x44] = READ("NOP", AM_ZP, NOP_V),
    [0x45] = READ("EOR", AM_ZP, EOR_V),
    [0x46] = RMW("LSR", AM_ZP, LSR_R, NULL),
    [0x47] = SRE(AM_ZP),
    [0x48] = { K_PHA, AM_IMP, "PHA", NULL, NULL },
    [0x49] = IMM("EOR", EOR_V),
    [0x4A] = IMPL("LSR", "cpu.a = cpu_lsr(cpu.a);"),
    [0x4B] = IMM("ASR", "cpu.a = cpu_lsr((uint8_t)(cpu.a & v));"),
    [0x4C] = { K_JMP, AM_ABS, "JMP", NULL, NULL },
    [0x4D] = READ("EOR", AM_ABS, EOR_V),
    [0x4E] = RMW("LSR", AM_ABS, LSR_R, NULL),
    [0x4F] = SRE(AM_ABS),

    [0x50] = BRANCH("BVC", "!cpu.v"),
    [0x51] = READ("EOR", AM_INDY, EOR_V),
    [0x52] = HLT,
    [0x53] = SRE(AM_INDY),
    [0x54] = READ("NOP", AM_ZPX, NOP_V),
    [0x55] = READ("EOR", AM_ZPX, EOR_V),
    [0x56] = RMW("LSR", AM_ZPX, LSR_R, NULL),
    [0x57] = SRE(AM_ZPX),
    [0x58] = IMPL("CLI", "cpu.i = 0;"),
    [0x59] = READ("EOR", AM_ABSY, EOR_V),
    [0x5A] = IMPL("NOP", ""),
    [0x5B] = SRE(AM_ABSY),
    [0x5C] = READ("NOP", AM_ABSX, NOP_V),
    [0x5D] = READ("EOR", AM_ABSX, EOR_V),
    [0x5E] = RMW("LSR", AM_ABSX, LSR_R, NULL),
    [0x5F] = SRE(AM_ABSX),

    [0x60] = { K_RTS, AM_IMP, "RTS", NULL, NULL },
    [0x61] = READ("ADC", AM_INDX, ADC_V),
    [0x62] = HLT,
    [0x63] = RRA(AM_INDX),
    [0x64] = READ("NOP", AM_ZP, NOP_V),
    [0x65] = READ("ADC", AM_ZP, ADC_V),
    [0x66] = RMW("ROR", AM_ZP, ROR_R, NULL),
    [0x67] = RRA(AM_ZP),
    [0x68] = { K_PLA, AM_IMP, "PLA", NULL, NULL },
    [0x69] = IMM("ADC", ADC_V),
    [0x6A] = IMPL("ROR", "cpu.a = cpu_ror(cpu.a);"),
    /* ARR: AND then ROR, with C from bit 6 and V from bit 6 xor bit 5. */
    [0x6B] = IMM("ARR", "cpu.a = cpu_ror((uint8_t)(cpu.a & v)); cpu.c = (cpu.a >> 6) & 1; "
                        "cpu.v = ((cpu.a >> 5) ^ (cpu.a >> 6)) & 1;"),
    [0x6C] = { K_JMPIND, AM_IND, "JMP", NULL, NULL },
    [0x6D] = READ("ADC", AM_ABS, ADC_V),
    [0x6E] = RMW("ROR", AM_ABS, ROR_R, NULL),
    [0x6F] = RRA(AM_ABS),

    [0x70] = BRANCH("BVS", "cpu.v"),
    [0x71] = READ("ADC", AM_INDY, ADC_V),
    [0x72] = HLT,
    [0x73] = RRA(AM_INDY),
    [0x74] = READ("NOP", AM_ZPX, NOP_V),
    [0x75] = READ("ADC", AM_ZPX, ADC_V),
    [0x76] = RMW("ROR", AM_ZPX, ROR_R, NULL),
    [0x77] = RRA(AM_ZPX),
    [0x78] = IMPL("SEI", "cpu.i = 1;"),
    [0x79] = READ("ADC", AM_ABSY, ADC_V),
    [0x7A] = IMPL("NOP", ""),
    [0x7B] = RRA(AM_ABSY),
    [0x7C] = READ("NOP", AM_ABSX, NOP_V),
    [0x7D] = READ("ADC", AM_ABSX, ADC_V),
    [0x7E] = RMW("ROR", AM_ABSX, ROR_R, NULL),
    [0x7F] = RRA(AM_ABSX),

    [0x80] = IMM("NOP", NOP_V),
    [0x81] = WRITE("STA", AM_INDX, "cpu.a"),
    [0x82] = IMM("NOP", NOP_V),
    [0x83] = WRITE("SAX", AM_INDX, "(uint8_t)(cpu.a & cpu.x)"),
    [0x84] = WRITE("STY", AM_ZP, "cpu.y"),
    [0x85] = WRITE("STA", AM_ZP, "cpu.a"),
    [0x86] = WRITE("STX", AM_ZP, "cpu.x"),
    [0x87] = WRITE("SAX", AM_ZP, "(uint8_t)(cpu.a & cpu.x)"),
    [0x88] = IMPL("DEY", "cpu.y--; cpu_nz(cpu.y);"),
    [0x89] = IMM("NOP", NOP_V),
    [0x8A] = IMPL("TXA", "cpu.a = cpu.x; cpu_nz(cpu.a);"),
    /* ANE: A = (A | magic) & X & v, with the magic constant taken as $FF. */
    [0x8B] = IMM("ANE", "cpu.a = (uint8_t)(cpu.x & v); cpu_nz(cpu.a);"),
    [0x8C] = WRITE("STY", AM_ABS, "cpu.y"),
    [0x8D] = WRITE("STA", AM_ABS, "cpu.a"),
    [0x8E] = WRITE("STX", AM_ABS, "cpu.x"),
    [0x8F] = WRITE("SAX", AM_ABS, "(uint8_t)(cpu.a & cpu.x)"),

    [0x90] = BRANCH("BCC", "!cpu.c"),
    [0x91] = WRITE("STA", AM_INDY, "cpu.a"),
    [0x92] = HLT,
    [0x93] = SH("SHA", AM_INDY),
    [0x94] = WRITE("STY", AM_ZPX, "cpu.y"),
    [0x95] = WRITE("STA", AM_ZPX, "cpu.a"),
    [0x96] = WRITE("STX", AM_ZPY, "cpu.x"),
    [0x97] = WRITE("SAX", AM_ZPY, "(uint8_t)(cpu.a & cpu.x)"),
    [0x98] = IMPL("TYA", "cpu.a = cpu.y; cpu_nz(cpu.a);"),
    [0x99] = WRITE("STA", AM_ABSY, "cpu.a"),
    [0x9A] = IMPL("TXS", "cpu.s = cpu.x;"),
    [0x9B] = SH("SHS", AM_ABSY),
    [0x9C] = SH("SHY", AM_ABSX),
    [0x9D] = WRITE("STA", AM_ABSX, "cpu.a"),
    [0x9E] = SH("SHX", AM_ABSY),
    [0x9F] = SH("SHA", AM_ABSY),

    [0xA0] = IMM("LDY", LDY_V),
    [0xA1] = READ("LDA", AM_INDX, LDA_V),
    [0xA2] = IMM("LDX", LDX_V),
    [0xA3] = READ("LAX", AM_INDX, LAX_V),
    [0xA4] = READ("LDY", AM_ZP, LDY_V),
    [0xA5] = READ("LDA", AM_ZP, LDA_V),
    [0xA6] = READ("LDX", AM_ZP, LDX_V),
    [0xA7] = READ("LAX", AM_ZP, LAX_V),
    [0xA8] = IMPL("TAY", "cpu.y = cpu.a; cpu_nz(cpu.y);"),
    [0xA9] = IMM("LDA", LDA_V),
    [0xAA] = IMPL("TAX", "cpu.x = cpu.a; cpu_nz(cpu.x);"),
    /* LXA: A = (A | magic) & v, X = A, magic taken as $FF. */
    [0xAB] = IMM("LXA", LAX_V),
    [0xAC] = READ("LDY", AM_ABS, LDY_V),
    [0xAD] = READ("LDA", AM_ABS, LDA_V),
    [0xAE] = READ("LDX", AM_ABS, LDX_V),
    [0xAF] = READ("LAX", AM_ABS, LAX_V),

    [0xB0] = BRANCH("BCS", "cpu.c"),
    [0xB1] = READ("LDA", AM_INDY, LDA_V),
    [0xB2] = HLT,
    [0xB3] = READ("LAX", AM_INDY, LAX_V),
    [0xB4] = READ("LDY", AM_ZPX, LDY_V),
    [0xB5] = READ("LDA", AM_ZPX, LDA_V),
    [0xB6] = READ("LDX", AM_ZPY, LDX_V),
    [0xB7] = READ("LAX", AM_ZPY, LAX_V),
    [0xB8] = IMPL("CLV", "cpu.v = 0;"),
    [0xB9] = READ("LDA", AM_ABSY, LDA_V),
    [0xBA] = IMPL("TSX", "cpu.x = cpu.s; cpu_nz(cpu.x);"),
    [0xBB] = READ("LAS", AM_ABSY, LAS_V),
    [0xBC] = READ("LDY", AM_ABSX, LDY_V),
    [0xBD] = READ("LDA", AM_ABSX, LDA_V),
    [0xBE] = READ("LDX", AM_ABSY, LDX_V),
    [0xBF] = READ("LAX", AM_ABSY, LAX_V),

    [0xC0] = IMM("CPY", CPY_V),
    [0xC1] = READ("CMP", AM_INDX, CMP_V),
    [0xC2] = IMM("NOP", NOP_V),
    [0xC3] = DCP(AM_INDX),
    [0xC4] = READ("CPY", AM_ZP, CPY_V),
    [0xC5] = READ("CMP", AM_ZP, CMP_V),
    [0xC6] = RMW("DEC", AM_ZP, DEC_R, NULL),
    [0xC7] = DCP(AM_ZP),
    [0xC8] = IMPL("INY", "cpu.y++; cpu_nz(cpu.y);"),
    [0xC9] = IMM("CMP", CMP_V),
    [0xCA] = IMPL("DEX", "cpu.x--; cpu_nz(cpu.x);"),
    /* AXS: X = (A & X) - v, flags as for CMP. */
    [0xCB] = IMM("AXS", "cpu.c = (uint8_t)(cpu.a & cpu.x) >= v; cpu.x = (uint8_t)((cpu.a & cpu.x) - v); "
                        "cpu_nz(cpu.x);"),
    [0xCC] = READ("CPY", AM_ABS, CPY_V),
    [0xCD] = READ("CMP", AM_ABS, CMP_V),
    [0xCE] = RMW("DEC", AM_ABS, DEC_R, NULL),
    [0xCF] = DCP(AM_ABS),

    [0xD0] = BRANCH("BNE", "!cpu.z"),
    [0xD1] = READ("CMP", AM_INDY, CMP_V),
    [0xD2] = HLT,
    [0xD3] = DCP(AM_INDY),
    [0xD4] = READ("NOP", AM_ZPX, NOP_V),
    [0xD5] = READ("CMP", AM_ZPX, CMP_V),
    [0xD6] = RMW("DEC", AM_ZPX, DEC_R, NULL),
    [0xD7] = DCP(AM_ZPX),
    [0xD8] = IMPL("CLD", "cpu.d = 0;"),
    [0xD9] = READ("CMP", AM_ABSY, CMP_V),
    [0xDA] = IMPL("NOP", ""),
    [0xDB] = DCP(AM_ABSY),
    [0xDC] = READ("NOP", AM_ABSX, NOP_V),
    [0xDD] = READ("CMP", AM_ABSX, CMP_V),
    [0xDE] = RMW("DEC", AM_ABSX, DEC_R, NULL),
    [0xDF] = DCP(AM_ABSX),

    [0xE0] = IMM("CPX", CPX_V),
    [0xE1] = READ("SBC", AM_INDX, SBC_V),
    [0xE2] = IMM("NOP", NOP_V),
    [0xE3] = ISC(AM_INDX),
    [0xE4] = READ("CPX", AM_ZP, CPX_V),
    [0xE5] = READ("SBC", AM_ZP, SBC_V),
    [0xE6] = RMW("INC", AM_ZP, INC_R, NULL),
    [0xE7] = ISC(AM_ZP),
    [0xE8] = IMPL("INX", "cpu.x++; cpu_nz(cpu.x);"),
    [0xE9] = IMM("SBC", SBC_V),
    [0xEA] = IMPL("NOP", ""),
    [0xEB] = IMM("SBC", SBC_V),
    [0xEC] = READ("CPX", AM_ABS, CPX_V),
    [0xED] = READ("SBC", AM_ABS, SBC_V),
    [0xEE] = RMW("INC", AM_ABS, INC_R, NULL),
    [0xEF] = ISC(AM_ABS),

    [0xF0] = BRANCH("BEQ", "cpu.z"),
    [0xF1] = READ("SBC", AM_INDY, SBC_V),
    [0xF2] = HLT,
    [0xF3] = ISC(AM_INDY),
    [0xF4] = READ("NOP", AM_ZPX, NOP_V),
    [0xF5] = READ("SBC", AM_ZPX, SBC_V),
    [0xF6] = RMW("INC", AM_ZPX, INC_R, NULL),
    [0xF7] = ISC(AM_ZPX),
    [0xF8] = IMPL("SED", "cpu.d = 1;"),
    [0xF9] = READ("SBC", AM_ABSY, SBC_V),
    [0xFA] = IMPL("NOP", ""),
    [0xFB] = ISC(AM_ABSY),
    [0xFC] = READ("NOP", AM_ABSX, NOP_V),
    [0xFD] = READ("SBC", AM_ABSX, SBC_V),
    [0xFE] = RMW("INC", AM_ABSX, INC_R, NULL),
    [0xFF] = ISC(AM_ABSX),
};

static int am_size(AddrMode am) {
    switch (am) {
    case AM_IMP: case AM_ACC: return 1;
    case AM_ABS: case AM_ABSX: case AM_ABSY: case AM_IND: return 3;
    default: return 2;
    }
}

/* Bytes the CPU consumes from the instruction stream before the operation
 * itself (BRK skips a padding byte and resumes two bytes on). */
static int op_length(uint8_t opcode) {
    const OpDef *d = &OPS[opcode];
    if (d->kind == K_BRK) return 2;
    return am_size(d->am);
}

/* ------------------------------------------------------------------------ */
/* ROM access and discovery                                                 */
/* ------------------------------------------------------------------------ */

/* A position: a PRG bank, the CPU slot it is mapped in, and the offset within
 * the bank. This, not a CPU address, is what a compiled block is keyed on. */
typedef struct {
    uint32_t bank, slot, k;
} Pos;

#define POS_NONE 0xFFFFFFFFu
#define POS_PACK(bank, slot, k) (((bank) << 15) | ((slot) << SLOT_SHIFT) | (k))

typedef struct {
    const NESRom *rom;
    uint32_t prg_len;              /* the image's own length */
    uint32_t banks;                /* 8KB banks, rounded up to a power of two */
    int      mapper;
    bool     banked;               /* the mapper can move PRG under the CPU */
    /* The bank a slot is wired to permanently, or -1 when a game can switch
     * it; and the bank a slot holds at power-on. */
    int      fixed[SLOT_COUNT];
    int      power_on[SLOT_COUNT];
    /* Indexed by POS_PACK: instruction starts compiled here, and positions
     * discovery has already visited. */
    uint8_t *is_insn;
    uint8_t *seen;
} Program;

static size_t pos_space(const Program *p) { return (size_t)p->banks * SLOT_COUNT * SLOT_SIZE; }

/* The PRG byte at an offset, with the padding past the end of a ROM whose
 * length is not a power of two reading as zero, exactly as hw_machine.c
 * loads it. */
static uint8_t prg_byte(const Program *p, uint32_t bank, uint32_t k) {
    uint32_t off = (bank << SLOT_SHIFT) | (k & (SLOT_SIZE - 1));
    return off < p->prg_len ? p->rom->prg_data[off] : 0;
}

static uint16_t pos_addr(const Pos *at) {
    return (uint16_t)(0x8000 + (at->slot << SLOT_SHIFT) + at->k);
}

/* Which bank a slot holds. Only what a board wires that way counts as fixed:
 * a mode bit a game can flip does not, because a block compiled for the wrong
 * bank is simply never entered (the dispatch checks the live mapping) while a
 * wrong assumption would also fold reads from another slot to stale bytes.
 * MMC1 has no permanently fixed slots: mode 2 switches $C000-$FFFF, and
 * modes 0/1 switch all of PRG. Its reset mapping is only a discovery seed. */
static int fixed_bank_for(int mapper, uint32_t banks, uint32_t slot) {
    switch (mapper) {
    case 13: return (int)(slot & (banks - 1));
    case 71: return slot >= 2 ? (int)(banks - 2 + slot - 2) : -1;
    case 75: return slot == 3 ? (int)(banks - 1) : -1;
    case 206: return slot >= 2 ? (int)(banks - 2 + slot - 2) : -1;
    case 76: return slot >= 2 ? (int)(banks - 2 + slot - 2) : -1;
    case 87: return (int)(slot & (banks - 1));
    case 94: return slot >= 2 ? (int)(banks - 2 + slot - 2) : -1;
    case 0: case 3:  return (int)(slot & (banks - 1));            /* wired straight through */
    case 2:  return slot >= 2 ? (int)(banks - 2 + (slot - 2)) : -1;  /* last 16KB fixed */
    case 1:  return -1;
    case 4:  return slot == 3 ? (int)(banks - 1) : -1;            /* $E000 is hardwired */
    default: return -1;                                           /* AxROM, GxROM */
    }
}

/* The configuration a cold console comes up in; hw_mapper.c's reset paths. */
static int power_on_bank_for(int mapper, uint32_t banks, uint32_t slot) {
    switch (mapper) {
    case 71: return slot >= 2 ? (int)(banks - 2 + slot - 2) : (int)slot;
    case 75: return slot == 3 ? (int)(banks - 1) : (int)slot;
    case 206: return slot >= 2 ? (int)(banks - 2 + slot - 2) : (int)slot;
    case 76: return slot >= 2 ? (int)(banks - 2 + slot - 2) : (int)slot;
    case 94: return slot >= 2 ? (int)(banks - 2 + slot - 2) : (int)slot;
    case 1:  return slot >= 2 ? (int)(banks - 2 + (slot - 2)) : (int)slot;  /* mode 3 */
    case 2:  return slot >= 2 ? (int)(banks - 2 + (slot - 2)) : (int)slot;
    case 4:  return slot == 2 ? (int)(banks - 2) : slot == 3 ? (int)(banks - 1) : (int)slot;
    default: return (int)(slot & (banks - 1));
    }
}

/* Whether every byte of the instruction is a byte this block knows: inside
 * $8000-$FFFF (the program counter wraps from $FFFF to $0000) and inside the
 * one slot whose bank the block was generated for. An instruction that
 * reaches into the next slot reads a byte from a bank chosen at run time, so
 * neither its operand nor its length can be folded; the interpreter runs it. */
static bool fits_in_slot(const Pos *at, int len) {
    uint16_t addr = pos_addr(at);
    if ((uint32_t)addr + (uint32_t)len - 1 > 0xFFFF) return false;
    return at->k + (uint32_t)len <= SLOT_SIZE;
}

/* Where control goes when it reaches CPU address addr from a block generated
 * for `from`. Within the same slot the bank carries over; into another slot it
 * is only known when that slot is fixed. Everything else is left to run-time
 * discovery (--miss-log), which reports the bank it actually saw. */
static bool target_pos(const Program *p, const Pos *from, uint16_t addr, Pos *out) {
    if (addr < 0x8000) return false;
    uint32_t slot = (addr >> SLOT_SHIFT) & (SLOT_COUNT - 1);
    uint32_t k = addr & (SLOT_SIZE - 1);
    if (slot == from->slot) {
        out->bank = from->bank;
    } else if (p->fixed[slot] >= 0) {
        out->bank = (uint32_t)p->fixed[slot];
    } else {
        return false;
    }
    out->slot = slot;
    out->k = k;
    return true;
}

static void discover(Program *p, const uint32_t *seeds, int seed_count) {
    size_t cap = pos_space(p);
    uint32_t *work = (uint32_t *)malloc(sizeof(uint32_t) * cap);
    size_t top = 0;
    for (int i = 0; i < seed_count; i++)
        if (seeds[i] != POS_NONE) work[top++] = seeds[i];
    while (top > 0) {
        uint32_t packed = work[--top];
        if (p->seen[packed]) continue;
        p->seen[packed] = 1;
        Pos at = { packed >> 15, (packed >> SLOT_SHIFT) & (SLOT_COUNT - 1), packed & (SLOT_SIZE - 1) };
        uint8_t opcode = prg_byte(p, at.bank, at.k);
        const OpDef *d = &OPS[opcode];
        int len = op_length(opcode);
        if (!fits_in_slot(&at, len)) continue;
        p->is_insn[packed] = 1;
        uint16_t addr = pos_addr(&at);
        uint16_t next = (uint16_t)(addr + len);
        uint16_t succ[2];
        int n = 0;
        switch (d->kind) {
        case K_BRANCH: {
            int8_t rel = (int8_t)prg_byte(p, at.bank, at.k + 1);
            succ[n++] = next;
            succ[n++] = (uint16_t)(next + rel);
            break;
        }
        case K_JMP:
            succ[n++] = (uint16_t)(prg_byte(p, at.bank, at.k + 1) | prg_byte(p, at.bank, at.k + 2) << 8);
            break;
        case K_JSR:
            succ[n++] = (uint16_t)(prg_byte(p, at.bank, at.k + 1) | prg_byte(p, at.bank, at.k + 2) << 8);
            succ[n++] = next;  /* return address */
            break;
        case K_JMPIND: case K_RTS: case K_RTI: case K_HLT:
            break;
        case K_BRK:
            succ[n++] = next;  /* RTI from the handler resumes here */
            break;
        default:
            succ[n++] = next;
            break;
        }
        for (int i = 0; i < n; i++) {
            Pos to;
            if (!target_pos(p, &at, succ[i], &to)) continue;
            uint32_t t = POS_PACK(to.bank, to.slot, to.k);
            if (!p->seen[t] && top < cap) work[top++] = t;
        }
    }
    free(work);
}

/* ------------------------------------------------------------------------ */
/* Emission                                                                 */
/* ------------------------------------------------------------------------ */

/* Templates are written once and emitted in two modes. Compiled (p != NULL):
 * the instruction address P and its operand bytes are constants. Interpreter
 * (p == NULL): the instruction address is the local pc and operand bytes are
 * read into b1/b2 at run time. */
typedef struct {
    FILE          *f;
    const Program *p;
    Pos            at;          /* bank, slot and offset of the instruction */
    uint32_t       chunk;       /* 1KB chunk within the slot being emitted */
    uint16_t       P;           /* the instruction's CPU address (compiled) */
    uint8_t        opcode;
} Emit;

#define INTERP(e) ((e)->p == NULL)

static void out(Emit *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(e->f, fmt, ap);
    va_end(ap);
}

/* Body line, indented. */
static void ln(Emit *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("        ", e->f);
    vfprintf(e->f, fmt, ap);
    fputc('\n', e->f);
    va_end(ap);
}

/* Short-lived formatted strings for building statements. */
static const char *str(const char *fmt, ...) {
    static char bufs[32][128];
    static unsigned slot;
    char *b = bufs[slot++ % 32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(bufs[0]), fmt, ap);
    va_end(ap);
    return b;
}

static const char *flags(int f) {
    switch (f) {
    case 0: return "0";
    case 1: return "CYC_POLL";
    case 4: return "CYC_DONE";
    case 1 | 4: return "CYC_POLL | CYC_DONE";
    case 2 | 4: return "CYC_POLL_BRANCH | CYC_DONE";
    default: return "?";
    }
}
#define POLL 1
#define POLL_BRANCH 2
#define DONE 4

/* A read of a constant address: PRG ROM bytes fold to constants. */
/* The ROM byte a block knows is at a CPU address: inside the block's own slot
 * the bank is the block's own, and another slot's byte is known only when that
 * slot is fixed. Returns false when the byte is whatever a run-time bank holds
 * - a dummy read of the next slot, say - and the access cannot be folded. */
static bool known_byte(const Emit *e, uint16_t addr, uint8_t *out) {
    Pos to;
    if (!target_pos(e->p, &e->at, addr, &to)) return false;
    *out = prg_byte(e->p, to.bank, to.k);
    return true;
}

static const char *read_const(Emit *e, uint16_t addr, int f) {
    uint8_t v;
    if (!INTERP(e) && known_byte(e, addr, &v))
        return str("cpu_read_rom(0x%04X, 0x%02X, %s)", addr, v, flags(f));
    return str("cpu_read(0x%04X, %s)", addr, flags(f));
}

/* Read instruction byte k (1 or 2, or the byte after a one-byte
 * instruction): a statement that leaves the value in operand(e, k). */
static void read_operand(Emit *e, int k, int f) {
    if (INTERP(e))
        ln(e, "b%d = cpu_read((uint16_t)(pc + %d), %s);", k, k, flags(f));
    else
        ln(e, "%s;", read_const(e, (uint16_t)(e->P + k), f));
}

/* The 16-bit operand of a 3-byte instruction, as a value. */
static uint16_t insn_operand16(const Emit *e) {
    return (uint16_t)(prg_byte(e->p, e->at.bank, e->at.k + 1) | prg_byte(e->p, e->at.bank, e->at.k + 2) << 8);
}

/* An instruction's own operand bytes. fits_in_slot() kept the whole
 * instruction inside one slot, so these are always the block's own bank. */
static const char *operand(Emit *e, int k) {
    if (INTERP(e)) return k == 1 ? "b1" : "b2";
    return str("0x%02X", prg_byte(e->p, e->at.bank, e->at.k + (uint32_t)k));
}

static const char *operand16(Emit *e) {
    if (INTERP(e)) return "(uint16_t)(b1 | b2 << 8)";
    return str("0x%04X", insn_operand16(e));
}

/* The address of the instruction's k-th byte. */
static const char *pc_plus(Emit *e, int k) {
    if (INTERP(e)) return str("(uint16_t)(pc + %d)", k);
    return str("0x%04X", (uint16_t)(e->P + k));
}

/* Whether a target is a label in the C function being emitted. It has to be a
 * compiled instruction of the same bank and slot (another one is another
 * translation unit) and in the same 1KB chunk (another one is another
 * function); anything else goes back to the scheduler, which looks up the
 * bank the mapper has there now. */
static bool is_label(Emit *e, uint16_t addr) {
    Pos to;
    if (!target_pos(e->p, &e->at, addr, &to)) return false;
    if (to.bank != e->at.bank || to.slot != e->at.slot) return false;
    if ((to.k >> CHUNK_SHIFT) != e->chunk) return false;
    return e->p->is_insn[POS_PACK(to.bank, to.slot, to.k)] != 0;
}

/* Continue at a constant address (compiled mode). */
static const char *jump_const(Emit *e, uint16_t target) {
    if (is_label(e, target)) return str("goto L_%04X;", target);
    return str("{ cpu.pc = 0x%04X; return; }", target);
}

/* Continue at the next instruction. */
static void go_next(Emit *e) {
    int len = op_length(e->opcode);
    if (INTERP(e))
        ln(e, "cpu.pc = (uint16_t)(pc + %d);", len);
    else
        ln(e, "%s", jump_const(e, (uint16_t)(e->P + len)));
}

/* Continue at the address in the C expression pc_expr. */
static void go_expr(Emit *e, const char *pc_expr) {
    ln(e, "cpu.pc = %s;", pc_expr);
    if (!INTERP(e)) ln(e, "return;");
}

static char index_reg(AddrMode am) {
    return (am == AM_ZPY || am == AM_ABSY || am == AM_INDY) ? 'y' : 'x';
}

/* ---- writes that can move PRG banks ----
 *
 * A write to a mapper register can change which bank
 * backs the addresses this block folded to constants. The instructions after
 * such a write are only correct for the mapping the block was generated for,
 * so the block ends there and the scheduler re-dispatches on the live
 * mapping - which is also what makes the folding safe in the first place.
 *
 * Most writes cannot reach that far and continue in place. */
typedef enum { WR_NEVER, WR_MAYBE, WR_ALWAYS } WriteReach;

static unsigned mapper_write_floor(int mapper) {
    switch (mapper) {
    /* Low-address register apertures are added with their boards. */
    case 34: return 0x7ffd;
    case 79: return 0x4100;
    case 113: return 0x4100;
    case 140: return 0x6000;
    default: return 0x8000;
    }
}

static WriteReach write_reach(Emit *e, AddrMode am) {
    if (INTERP(e) || !e->p->banked) return WR_NEVER;
    unsigned floor = mapper_write_floor(e->p->mapper);
    switch (am) {
    case AM_ZP: case AM_ZPX: case AM_ZPY:
        return WR_NEVER;                     /* the effective address is one byte */
    case AM_ABS:
        return insn_operand16(e) >= floor ? WR_ALWAYS : WR_NEVER;
    case AM_ABSX: case AM_ABSY: {
        uint16_t base = insn_operand16(e);
        if ((unsigned)base + 255 < floor) return WR_NEVER;
        if (base >= floor && base <= 0xFF00) return WR_ALWAYS;
        return WR_MAYBE; /* crosses the register aperture or wraps */
    }
    default:
        return WR_MAYBE;                     /* (zp,X) and (zp),Y: a run-time pointer */
    }
}

/* Emitted after the write, once the instruction's effects are complete.
 * Returns true when it emitted the block's exit, so the caller skips its own
 * continuation. */
static bool bank_exit(Emit *e, WriteReach reach) {
    if (reach == WR_NEVER) return false;
    uint16_t next = (uint16_t)(e->P + op_length(e->opcode));
    if (reach == WR_ALWAYS) {
        ln(e, "cpu.pc = 0x%04X; return;   /* wrote the mapper: re-dispatch */", next);
        return true;
    }
    ln(e, "if (ea >= 0x%04X) { cpu.pc = 0x%04X; return; }   /* wrote the mapper */",
       mapper_write_floor(e->p->mapper), next);
    return false;
}

/* Addressing cycles of read, write, read-modify-write and SH instructions.
 * Declares ea (and base for the indexed absolute modes). An index that
 * carries into the high byte costs a cycle that reads the uncorrected
 * address; read instructions spend it only when there is a carry, the others
 * always. For SH instructions, ignore_h records whether a DMA held the CPU on
 * that cycle. */
static void addressing(Emit *e, AddrMode am, bool always_fix, bool sh) {
    char r = index_reg(am);
    switch (am) {
    case AM_ZP:
        read_operand(e, 1, 0);
        ln(e, "uint16_t ea = %s;", operand(e, 1));
        return;
    case AM_ZPX:
    case AM_ZPY:
        read_operand(e, 1, 0);
        ln(e, "cpu_read(%s, 0);", operand(e, 1));
        ln(e, "uint16_t ea = (uint8_t)(%s + cpu.%c);", operand(e, 1), r);
        return;
    case AM_ABS:
        read_operand(e, 1, 0);
        read_operand(e, 2, 0);
        ln(e, "uint16_t ea = %s;", operand16(e));
        return;
    case AM_INDX:
        read_operand(e, 1, 0);
        ln(e, "cpu_read(%s, 0);", operand(e, 1));
        ln(e, "uint8_t lo = cpu_read((uint8_t)(%s + cpu.x), 0);", operand(e, 1));
        ln(e, "uint8_t hi = cpu_read((uint8_t)(%s + cpu.x + 1), 0);", operand(e, 1));
        ln(e, "uint16_t ea = (uint16_t)(lo | hi << 8);");
        return;
    case AM_ABSX:
    case AM_ABSY:
        read_operand(e, 1, 0);
        read_operand(e, 2, 0);
        ln(e, "uint16_t base = %s, ea = (uint16_t)(base + cpu.%c);", operand16(e), r);
        break;
    case AM_INDY:
        read_operand(e, 1, 0);
        ln(e, "uint8_t lo = cpu_read(%s, 0);", operand(e, 1));
        ln(e, "uint8_t hi = cpu_read((uint8_t)(%s + 1), 0);", operand(e, 1));
        ln(e, "uint16_t base = (uint16_t)(lo | hi << 8), ea = (uint16_t)(base + cpu.y);");
        break;
    default:
        fprintf(stderr, "[cyc] unexpected addressing mode %d for $%02X\n", am, e->opcode);
        exit(1);
    }
    if (always_fix)
        ln(e, "cpu_read((uint16_t)((base & 0xFF00) | (ea & 0xFF)), 0);");
    else
        ln(e, "if ((ea ^ base) & 0xFF00) cpu_read((uint16_t)(ea - 0x100), 0);");
    if (sh) ln(e, "bool ignore_h = hw_dma_stalls != 0;");
}

static void emit_implied(Emit *e, const OpDef *d) {
    read_operand(e, 1, POLL | DONE);
    if (d->body[0]) ln(e, "%s", d->body);
    go_next(e);
}

static void emit_imm(Emit *e, const OpDef *d) {
    if (INTERP(e))
        ln(e, "uint8_t v = cpu_read((uint16_t)(pc + 1), CYC_POLL | CYC_DONE);");
    else
        ln(e, "uint8_t v = %s;", read_const(e, (uint16_t)(e->P + 1), POLL | DONE));
    ln(e, "%s", d->body);
    go_next(e);
}

static void emit_read(Emit *e, const OpDef *d) {
    addressing(e, d->am, false, false);
    ln(e, "uint8_t v = cpu_read(ea, CYC_POLL | CYC_DONE);");
    ln(e, "%s", d->body);
    go_next(e);
}

static void emit_write(Emit *e, const OpDef *d) {
    WriteReach reach = write_reach(e, d->am);
    addressing(e, d->am, true, false);
    ln(e, "cpu_write(ea, %s, CYC_POLL | CYC_DONE);", d->body);
    if (!bank_exit(e, reach)) go_next(e);
}

static void emit_rmw(Emit *e, const OpDef *d) {
    WriteReach reach = write_reach(e, d->am);
    addressing(e, d->am, true, false);
    ln(e, "uint8_t v = cpu_read(ea, 0), r;");
    ln(e, "cpu_write(ea, v, 0);");
    ln(e, "%s", d->body);
    ln(e, "cpu_write(ea, r, CYC_POLL | CYC_DONE);");
    if (d->post) ln(e, "%s", d->post);
    if (!bank_exit(e, reach)) go_next(e);
}

/* SHA/SHS/SHX/SHY. The value is ANDed with the address high byte + 1, and
 * when indexing carried, the written address's high byte is ANDed with the
 * index register; a DMA on the carry fix-up cycle drops the AND with the high
 * byte. SHA and SHS use A & (X | $F5), the magic constant as measured on
 * 2A03s. */
static void emit_sh(Emit *e, const OpDef *d) {
    char fix = d->name[2] == 'Y' ? 'y' : 'x';
    WriteReach reach = write_reach(e, d->am);
    addressing(e, d->am, true, true);
    ln(e, "uint8_t h = ignore_h ? 0xFF : (uint8_t)((base >> 8) + 1);");
    ln(e, "if ((ea ^ base) & 0xFF00) ea = (uint16_t)((ea & 0xFF) | ((ea >> 8) & cpu.%c) << 8);", fix);
    if (!strcmp(d->name, "SHS")) ln(e, "cpu.s = (uint8_t)(cpu.a & cpu.x);");
    const char *value = fix == 'y' ? "(uint8_t)(cpu.y & h)"
                      : !strcmp(d->name, "SHX") ? "(uint8_t)(cpu.x & h)"
                      : "(uint8_t)(cpu.a & (cpu.x | 0xF5) & h)";
    ln(e, "cpu_write(ea, %s, CYC_POLL | CYC_DONE);", value);
    if (!bank_exit(e, reach)) go_next(e);
}

/* The negation of a branch condition ("cpu.z" or "!cpu.z"). */
static const char *not_taken(const OpDef *d) {
    return d->body[0] == '!' ? d->body + 1 : str("!%s", d->body);
}

/* Not taken: one cycle. Taken: a second cycle without a poll, and a third,
 * polling but keeping an IRQ the first poll latched, only when the target is
 * on another page. */
static void emit_branch(Emit *e, const OpDef *d) {
    if (INTERP(e)) {
        ln(e, "if (%s) { cpu_read((uint16_t)(pc + 1), CYC_POLL | CYC_DONE); cpu.pc = (uint16_t)(pc + 2); return; }",
           not_taken(d));
        ln(e, "b1 = cpu_read((uint16_t)(pc + 1), CYC_POLL);");
        ln(e, "uint16_t next = (uint16_t)(pc + 2), target = (uint16_t)(next + (int8_t)b1);");
        ln(e, "if (((target ^ next) & 0xFF00) == 0) {");
        ln(e, "    cpu_read(next, CYC_DONE);");
        ln(e, "} else {");
        ln(e, "    cpu_read(next, 0);");
        ln(e, "    cpu_read((uint16_t)((next & 0xFF00) | (target & 0xFF)), CYC_POLL_BRANCH | CYC_DONE);");
        ln(e, "}");
        ln(e, "cpu.pc = target;");
        return;
    }
    uint16_t next = (uint16_t)(e->P + 2);
    uint16_t target = (uint16_t)(next + (int8_t)prg_byte(e->p, e->at.bank, e->at.k + 1));
    uint16_t same_page = (uint16_t)((next & 0xFF00) | (target & 0xFF));
    ln(e, "if (%s) { %s; %s }", not_taken(d), read_const(e, (uint16_t)(e->P + 1), POLL | DONE), jump_const(e, next));
    read_operand(e, 1, POLL);
    if ((target ^ next) & 0xFF00) {
        ln(e, "%s;", read_const(e, next, 0));
        ln(e, "%s;", read_const(e, same_page, POLL_BRANCH | DONE));
    } else {
        ln(e, "%s;", read_const(e, next, DONE));
    }
    ln(e, "%s", jump_const(e, target));
}

static void emit_jmp(Emit *e) {
    read_operand(e, 1, 0);
    read_operand(e, 2, POLL | DONE);
    if (INTERP(e))
        ln(e, "cpu.pc = %s;", operand16(e));
    else
        ln(e, "%s", jump_const(e, insn_operand16(e)));
}

/* JMP ($xxFF) reads the high byte from $xx00. */
static void emit_jmp_ind(Emit *e) {
    read_operand(e, 1, 0);
    read_operand(e, 2, 0);
    if (INTERP(e)) {
        ln(e, "uint16_t ptr = %s;", operand16(e));
        ln(e, "uint8_t lo = cpu_read(ptr, 0);");
        ln(e, "uint8_t hi = cpu_read((uint16_t)((ptr & 0xFF00) | ((ptr + 1) & 0xFF)), CYC_POLL | CYC_DONE);");
    } else {
        uint16_t ptr = insn_operand16(e);
        ln(e, "uint8_t lo = %s;", read_const(e, ptr, 0));
        ln(e, "uint8_t hi = %s;", read_const(e, (uint16_t)((ptr & 0xFF00) | ((ptr + 1) & 0xFF)), POLL | DONE));
    }
    go_expr(e, "(uint16_t)(lo | hi << 8)");
}

/* JSR: operand low byte, a read of the stack, pushes of the address of the
 * JSR's last byte, then the operand high byte. */
static void emit_jsr(Emit *e) {
    read_operand(e, 1, 0);
    ln(e, "cpu_read((uint16_t)(0x100 | cpu.s), 0);");
    if (INTERP(e)) {
        ln(e, "cpu_write((uint16_t)(0x100 | cpu.s), (uint8_t)((pc + 2) >> 8), 0);");
        ln(e, "cpu_write((uint16_t)(0x100 | (uint8_t)(cpu.s - 1)), (uint8_t)(pc + 2), 0);");
    } else {
        uint16_t ret = (uint16_t)(e->P + 2);
        ln(e, "cpu_write((uint16_t)(0x100 | cpu.s), 0x%02X, 0);", ret >> 8);
        ln(e, "cpu_write((uint16_t)(0x100 | (uint8_t)(cpu.s - 1)), 0x%02X, 0);", ret & 0xFF);
    }
    read_operand(e, 2, POLL | DONE);
    ln(e, "cpu.s = (uint8_t)(cpu.s - 2);");
    if (INTERP(e))
        ln(e, "cpu.pc = %s;", operand16(e));
    else
        ln(e, "%s", jump_const(e, insn_operand16(e)));
}

static void emit_rts(Emit *e) {
    read_operand(e, 1, 0);
    ln(e, "cpu_read((uint16_t)(0x100 | cpu.s), 0);");
    ln(e, "uint8_t lo = cpu_read((uint16_t)(0x100 | (uint8_t)(cpu.s + 1)), 0);");
    ln(e, "uint8_t hi = cpu_read((uint16_t)(0x100 | (uint8_t)(cpu.s + 2)), 0);");
    ln(e, "cpu_read((uint16_t)(lo | hi << 8), CYC_POLL | CYC_DONE);");
    ln(e, "cpu.s = (uint8_t)(cpu.s + 2);");
    go_expr(e, "(uint16_t)((lo | hi << 8) + 1)");
}

static void emit_rti(Emit *e) {
    read_operand(e, 1, 0);
    ln(e, "cpu_read((uint16_t)(0x100 | cpu.s), 0);");
    ln(e, "cpu_set_p(cpu_read((uint16_t)(0x100 | (uint8_t)(cpu.s + 1)), 0));");
    ln(e, "uint8_t lo = cpu_read((uint16_t)(0x100 | (uint8_t)(cpu.s + 2)), 0);");
    ln(e, "uint8_t hi = cpu_read((uint16_t)(0x100 | (uint8_t)(cpu.s + 3)), CYC_POLL | CYC_DONE);");
    ln(e, "cpu.s = (uint8_t)(cpu.s + 3);");
    go_expr(e, "(uint16_t)(lo | hi << 8)");
}

static void emit_stack(Emit *e, const OpDef *d) {
    read_operand(e, 1, 0);
    switch (d->kind) {
    case K_PHA:
        ln(e, "cpu_write((uint16_t)(0x100 | cpu.s), cpu.a, CYC_POLL | CYC_DONE);");
        ln(e, "cpu.s--;");
        break;
    case K_PHP:
        ln(e, "cpu_write((uint16_t)(0x100 | cpu.s), cpu_get_p(1), CYC_POLL | CYC_DONE);");
        ln(e, "cpu.s--;");
        break;
    case K_PLA:
        ln(e, "cpu_read((uint16_t)(0x100 | cpu.s), 0);");
        ln(e, "cpu.s++;");
        ln(e, "cpu.a = cpu_read((uint16_t)(0x100 | cpu.s), CYC_POLL | CYC_DONE);");
        ln(e, "cpu_nz(cpu.a);");
        break;
    case K_PLP:
        ln(e, "cpu_read((uint16_t)(0x100 | cpu.s), 0);");
        ln(e, "cpu.s++;");
        ln(e, "cpu_set_p(cpu_read((uint16_t)(0x100 | cpu.s), CYC_POLL | CYC_DONE));");
        break;
    default:
        break;
    }
    go_next(e);
}

/* The cycles after the opcode fetch. */
static void emit_body(Emit *e) {
    const OpDef *d = &OPS[e->opcode];
    switch (d->kind) {
    case K_IMPLIED: emit_implied(e, d); break;
    case K_IMM:     emit_imm(e, d); break;
    case K_READ:    emit_read(e, d); break;
    case K_WRITE:   emit_write(e, d); break;
    case K_SH:      emit_sh(e, d); break;
    case K_RMW:     emit_rmw(e, d); break;
    case K_BRANCH:  emit_branch(e, d); break;
    case K_JMP:     emit_jmp(e); break;
    case K_JMPIND:  emit_jmp_ind(e); break;
    case K_JSR:     emit_jsr(e); break;
    case K_RTS:     emit_rts(e); break;
    case K_RTI:     emit_rti(e); break;
    case K_PHA: case K_PHP: case K_PLA: case K_PLP: emit_stack(e, d); break;
    case K_BRK:
        if (!INTERP(e)) ln(e, "cpu.pc = 0x%04X;", e->P);
        ln(e, "cpu_interrupt(true);");
        if (!INTERP(e)) ln(e, "return;");
        break;
    case K_HLT:
        if (!INTERP(e)) ln(e, "cpu.pc = 0x%04X;", e->P);
        ln(e, "cpu_jam();");
        if (!INTERP(e)) ln(e, "return;");
        break;
    }
}

static const char *mode_suffix(AddrMode am) {
    switch (am) {
    case AM_IMM: return " #";
    case AM_ZP: return " zp";
    case AM_ZPX: return " zp,X";
    case AM_ZPY: return " zp,Y";
    case AM_ABS: return " abs";
    case AM_ABSX: return " abs,X";
    case AM_ABSY: return " abs,Y";
    case AM_IND: return " (abs)";
    case AM_INDX: return " (zp,X)";
    case AM_INDY: return " (zp),Y";
    default: return "";
    }
}

static void emit_instruction(Emit *e) {
    const OpDef *d = &OPS[e->opcode];
    out(e, "L_%04X: /* %02X %s%s */\n", e->P, e->opcode, d->name, mode_suffix(d->am));
    out(e, "    if (hw_frame_done) { cpu.pc = 0x%04X; return; }\n", e->P);
    out(e, "    if (cpu_fetch_rom(0x%04X, 0x%02X)) { cpu.pc = 0x%04X; cpu_interrupt(false); return; }\n", e->P,
        e->opcode, e->P);
    out(e, "    {\n");
    emit_body(e);
    out(e, "    }\n");
}

static FILE *open_out(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[cyc] cannot write %s\n", path);
        exit(1);
    }
    return f;
}

/* Is anything compiled for this bank in this slot? */
static bool slot_used(const Program *p, uint32_t bank, uint32_t slot) {
    const uint8_t *bits = &p->is_insn[POS_PACK(bank, slot, 0)];
    for (uint32_t k = 0; k < SLOT_SIZE; k++)
        if (bits[k]) return true;
    return false;
}

/* One translation unit per PRG bank: for each slot that bank is compiled in,
 * a bitmap of instruction starts and a table of 1KB chunk functions, then the
 * chunks themselves. Splitting by bank keeps the units independent and
 * compilable in parallel, as SPLITGEN_MIGRATION.md describes for the
 * function-level output. */
static void emit_bank_file(const Program *p, const char *path, uint32_t bank) {
    FILE *f = open_out(path);
    fprintf(f,
        "/* %s - generated by NESRecomp --cycle-accurate. DO NOT EDIT.\n"
        " * PRG bank %u ($%06X-$%06X), one label per instruction, one\n"
        " * cpu_read/cpu_write per CPU cycle. A block is only entered while this\n"
        " * bank is the one mapped at its address; see recompiler/src/cyc_codegen.c. */\n"
        "#include \"cyc_recomp.h\"\n\n",
        path, bank, bank << SLOT_SHIFT, ((bank + 1) << SLOT_SHIFT) - 1);

    Emit e = {0};
    e.f = f;
    e.p = p;
    e.at.bank = bank;

    for (uint32_t slot = 0; slot < SLOT_COUNT; slot++) {
        if (!slot_used(p, bank, slot)) continue;
        const uint8_t *insn = &p->is_insn[POS_PACK(bank, slot, 0)];
        uint16_t base_addr = (uint16_t)(0x8000 + (slot << SLOT_SHIFT));

        bool used[CHUNKS_PER_SLOT] = { false };
        for (uint32_t k = 0; k < SLOT_SIZE; k++)
            if (insn[k]) used[k >> CHUNK_SHIFT] = true;

        for (uint32_t c = 0; c < CHUNKS_PER_SLOT; c++)
            if (used[c])
                fprintf(f, "static void chunk_s%u_%04X(void);\n", slot,
                        (unsigned)(base_addr + (c << CHUNK_SHIFT)));

        fprintf(f, "\nconst uint8_t cyc_bits_b%02X_s%u[%u] = {", bank, slot, SLOT_SIZE / 8);
        for (uint32_t i = 0; i < SLOT_SIZE / 8; i++) {
            uint8_t bits = 0;
            for (uint32_t b = 0; b < 8; b++)
                if (insn[i * 8 + b]) bits |= (uint8_t)(1u << b);
            fprintf(f, "%s0x%02X,", (i % 16) ? "" : "\n    ", bits);
        }
        fprintf(f, "\n};\n\nvoid (*const cyc_chunks_b%02X_s%u[%u])(void) = {\n", bank, slot, CHUNKS_PER_SLOT);
        for (uint32_t c = 0; c < CHUNKS_PER_SLOT; c++) {
            if (used[c])
                fprintf(f, "    chunk_s%u_%04X,\n", slot, (unsigned)(base_addr + (c << CHUNK_SHIFT)));
            else
                fprintf(f, "    0,\n");
        }
        fprintf(f, "};\n\n");

        e.at.slot = slot;
        for (uint32_t c = 0; c < CHUNKS_PER_SLOT; c++) {
            if (!used[c]) continue;
            e.chunk = c;
            uint32_t k0 = c << CHUNK_SHIFT;
            fprintf(f, "static void chunk_s%u_%04X(void) {\n    switch (cpu.pc) {\n", slot,
                    (unsigned)(base_addr + k0));
            for (uint32_t k = k0; k < k0 + (1u << CHUNK_SHIFT); k++)
                if (insn[k])
                    fprintf(f, "    case 0x%04X: goto L_%04X;\n", (unsigned)(base_addr + k),
                            (unsigned)(base_addr + k));
            fprintf(f, "    default: return;\n    }\n");
            for (uint32_t k = k0; k < k0 + (1u << CHUNK_SHIFT); k++) {
                if (!insn[k]) continue;
                e.at.k = k;
                e.P = (uint16_t)(base_addr + k);
                e.opcode = prg_byte(p, bank, k);
                emit_instruction(&e);
            }
            fprintf(f, "}\n\n");
        }
    }
    fclose(f);
}

/* The umbrella: the table that maps (bank, slot) to a compiled view, and the
 * two entry points the scheduler uses. */
static void emit_umbrella(const Program *p, const char *path, const char *prefix) {
    FILE *f = open_out(path);
    fprintf(f,
        "/* %s - generated by NESRecomp --cycle-accurate. DO NOT EDIT.\n"
        " * Dispatch for the per-bank translation units next to this file.\n"
        " * See recompiler/src/cyc_codegen.c. */\n"
        "#include \"cyc_recomp.h\"\n\n", path);

    for (uint32_t bank = 0; bank < p->banks; bank++)
        for (uint32_t slot = 0; slot < SLOT_COUNT; slot++)
            if (slot_used(p, bank, slot))
                fprintf(f, "extern const uint8_t cyc_bits_b%02X_s%u[%u];\n"
                           "extern void (*const cyc_chunks_b%02X_s%u[%u])(void);\n",
                        bank, slot, SLOT_SIZE / 8, bank, slot, CHUNKS_PER_SLOT);

    fprintf(f, "\n#define CYC_BANKS %uu\n\nstatic const CycNativeView VIEWS[CYC_BANKS][%u] = {\n", p->banks,
            SLOT_COUNT);
    for (uint32_t bank = 0; bank < p->banks; bank++) {
        fprintf(f, "    {");
        for (uint32_t slot = 0; slot < SLOT_COUNT; slot++) {
            if (slot_used(p, bank, slot))
                fprintf(f, " { cyc_bits_b%02X_s%u, cyc_chunks_b%02X_s%u },", bank, slot, bank, slot);
            else
                fprintf(f, " { 0, 0 },");
        }
        fprintf(f, " },\n");
    }
    fprintf(f, "};\n\n");

    uint32_t prg_hash = 2166136261u;
    for (uint32_t i = 0; i < p->prg_len; i++) prg_hash = (prg_hash ^ p->rom->prg_data[i]) * 16777619u;
    fprintf(f,
        "const char *cyc_native_program_name = \"%s\";\n"
        "const uint32_t cyc_native_prg_hash = 0x%08Xu;  /* FNV-1a of the PRG ROM compiled */\n\n"
        "/* Which compiled view covers a CPU address right now: the bank the\n"
        " * cartridge has in that address's 8KB slot decides, because the bytes\n"
        " * there - and so the block compiled from them - depend on it. */\n"
        "static const CycNativeView *view_at(uint16_t addr, unsigned *out_k) {\n"
        "    unsigned slot = (addr >> %d) & %u;\n"
        "    unsigned bank = hw_prg_bank(addr);\n"
        "    if (bank >= CYC_BANKS) return 0;\n"
        "    const CycNativeView *v = &VIEWS[bank][slot];\n"
        "    *out_k = addr & %uu;\n"
        "    return v->bits ? v : 0;\n"
        "}\n\n"
        "bool cyc_native_has(uint16_t addr) {\n"
        "    if (addr < 0x8000) return false;\n"
        "    unsigned k;\n"
        "    const CycNativeView *v = view_at(addr, &k);\n"
        "    return v && ((v->bits[k >> 3] >> (k & 7)) & 1);\n"
        "}\n\n"
        "/* Run compiled instructions from cpu.pc until execution leaves compiled\n"
        " * code or the PPU finishes a frame. Called at an instruction boundary.\n"
        " * Each iteration looks the mapping up again: a chunk returns here when\n"
        " * control crosses a slot, a chunk boundary, or a write to the mapper. */\n"
        "void cyc_native_run(void) {\n"
        "    while (!hw_frame_done && !cpu.jammed) {\n"
        "        uint16_t pc = cpu.pc;\n"
        "        if (pc < 0x8000) return;\n"
        "        unsigned k;\n"
        "        const CycNativeView *v = view_at(pc, &k);\n"
        "        if (!v || !((v->bits[k >> 3] >> (k & 7)) & 1)) return;\n"
        "        void (*chunk)(void) = v->chunks[k >> %d];\n"
        "        if (!chunk) return;\n"
        "        chunk();\n"
        "    }\n"
        "}\n",
        prefix, prg_hash, SLOT_SHIFT, SLOT_COUNT - 1, SLOT_SIZE - 1, CHUNK_SHIFT);
    fclose(f);
}

static bool check_table(void) {
    for (int i = 0; i < 256; i++) {
        if (OPS[i].name == NULL) {
            fprintf(stderr, "[cyc] internal error: opcode $%02X has no definition\n", i);
            return false;
        }
    }
    return true;
}

bool cyc_codegen_emit_interpreter(const char *path) {
    if (!check_table()) return false;
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[cyc] cannot write %s\n", path);
        return false;
    }
    fprintf(f,
        "/* cpu6502_interp.c - generated by NESRecomp --emit-cycle-interpreter. DO NOT EDIT.\n"
        " *\n"
        " * The cycle-accurate 6502 interpreter: the instruction templates of\n"
        " * recompiler/src/cyc_codegen.c with operands read at run time. Recompiled\n"
        " * code (<prefix>_cyc.c) is generated from the same templates. Regenerate with\n"
        " *   NESRecomp --emit-cycle-interpreter runner/cyc/cpu6502_interp.c */\n"
        "#include \"cpu6502.h\"\n\n"
        "void cpu_interp_step(void) {\n"
        "    uint16_t pc = cpu.pc;\n"
        "    uint8_t opcode, b1 = 0, b2 = 0;\n"
        "    if (cpu_fetch(pc, &opcode)) { cpu_interrupt(false); return; }\n"
        "    switch (opcode) {\n");
    Emit e = {0};
    e.f = f;
    for (int op = 0; op < 256; op++) {
        const OpDef *d = &OPS[op];
        e.opcode = (uint8_t)op;
        fprintf(f, "    case 0x%02X: { /* %s%s */\n", op, d->name, mode_suffix(d->am));
        emit_body(&e);
        fprintf(f, "        return;\n    }\n");
    }
    fprintf(f, "    }\n    (void)b1; (void)b2;\n}\n");
    fclose(f);
    printf("[NESRecomp] cycle-accurate interpreter -> %s\n", path);
    return true;
}

/* The board names hw_mapper.c implements, for the message and the banner. */
static const char *mapper_name(int mapper) {
    switch (mapper) {
    case 11: return "Color Dreams";
    case 13: return "CPROM";
    case 34: return "BNROM / NINA-001";
    case 71: return "Camerica";
    case 75: return "VRC1";
    case 206: return "DxROM";
    case 76: return "Namco 109";
    case 79: return "NINA-003/006";
    case 87: return "J87";
    case 94: return "UN1ROM";
    case 113: return "HES";
    case 140: return "Jaleco JF-11/14";
    case 0:  return "NROM";
    case 1:  return "MMC1";
    case 2:  return "UxROM";
    case 3:  return "CNROM";
    case 4:  return "MMC3";
    case 7:  return "AxROM";
    case 66: return "GxROM";
    default: return NULL;
    }
}

/* A seed line is `AAAA` or `BB:AAAA`, either with an optional count after it.
 * The bank-less form means the bank the power-on configuration has at that
 * address, which is every bank on NROM and what a hand-written seed means.
 * Returns POS_NONE for a line that is not a seed. */
static uint32_t parse_seed(const Program *p, const char *line) {
    unsigned bank, addr;
    if (line[0] == '#') return POS_NONE;
    if (sscanf(line, "%x:%x", &bank, &addr) == 2) {
        if (addr < 0x8000 || addr > 0xFFFF || bank >= p->banks) return POS_NONE;
    } else if (sscanf(line, "%x", &addr) == 1) {
        if (addr < 0x8000 || addr > 0xFFFF) return POS_NONE;
        bank = (unsigned)p->power_on[(addr >> SLOT_SHIFT) & (SLOT_COUNT - 1)];
    } else {
        return POS_NONE;
    }
    return POS_PACK(bank, (addr >> SLOT_SHIFT) & (SLOT_COUNT - 1), addr & (SLOT_SIZE - 1));
}

/* The three vectors as the bank in slot 3 holds them, seeded at whatever
 * position each target resolves to. A mapper with a fixed $E000 slot has one
 * such bank; one that switches all of $8000-$FFFF (AxROM, GxROM) has no fixed
 * vectors at all, so every bank's are seeded - compiling a block that never
 * runs costs output, not correctness. */
static void seed_vectors(const Program *p, uint32_t bank, uint32_t *seeds, int *n) {
    Pos from = { bank, 3, SLOT_SIZE - 1 };
    for (uint32_t v = 0xFFFA; v <= 0xFFFE; v += 2) {
        uint32_t k = v & (SLOT_SIZE - 1);
        uint16_t target = (uint16_t)(prg_byte(p, bank, k) | prg_byte(p, bank, k + 1) << 8);
        Pos to;
        if (target_pos(p, &from, target, &to)) seeds[(*n)++] = POS_PACK(to.bank, to.slot, to.k);
    }
}

bool cyc_codegen_emit(const NESRom *rom, const GameConfig *cfg, const char *output_prefix) {
    if (rom->nes2 && rom->mapper != 0 && rom->mapper != 1 && rom->mapper != 2 &&
        rom->mapper != 3 && rom->mapper != 4 && rom->mapper != 7 && rom->mapper != 66) {
        fprintf(stderr, "[cyc] NES 2.0 variants of mapper %d are not implemented; see runner/cyc/MAPPERS.md\n", rom->mapper);
        return false;
    }
    const char *board = mapper_name(rom->mapper);
    if (!board) {
        fprintf(stderr, "[cyc] unsupported mapper %d; see runner/cyc/MAPPERS.md\n", rom->mapper);
        return false;
    }
    if (!check_table()) return false;

    Program *p = (Program *)calloc(1, sizeof(Program));
    p->rom = rom;
    p->mapper = rom->mapper;
    p->prg_len = (uint32_t)rom->prg_banks * 0x4000u;
    /* Banks are counted in the padded image, so that a bank number the
     * hardware wraps lands on the same byte here as in hw_mapper.c. */
    p->banks = 1;
    while (p->banks * SLOT_SIZE < p->prg_len) p->banks <<= 1;
    p->banked = rom->mapper != 0;
    bool any_fixed = false;
    for (uint32_t s = 0; s < SLOT_COUNT; s++) {
        p->fixed[s] = fixed_bank_for(rom->mapper, p->banks, s);
        p->power_on[s] = power_on_bank_for(rom->mapper, p->banks, s);
        if (p->fixed[s] >= 0) any_fixed = true;
    }
    p->is_insn = (uint8_t *)calloc(1, pos_space(p));
    p->seen = (uint8_t *)calloc(1, pos_space(p));
    if (!p->is_insn || !p->seen) {
        fprintf(stderr, "[cyc] out of memory for a %u-bank program\n", p->banks);
        return false;
    }

    /* Entry points: the vectors, [functions]/[[extra_func]] addresses, and the
     * seed file. Compiling an address that is never executed is harmless (the
     * block decodes the same constant ROM bytes the interpreter would), so
     * seeds need not be proven instruction starts. */
    size_t seed_cap = (size_t)p->banks * 3 + GAME_CFG_MAX_EXTRA_FUNCS + 0x20000;
    uint32_t *seeds = (uint32_t *)malloc(sizeof(uint32_t) * seed_cap);
    int n = 0;
    if (any_fixed) {
        seed_vectors(p, (uint32_t)(p->fixed[3] >= 0 ? p->fixed[3] : p->power_on[3]), seeds, &n);
    } else {
        for (uint32_t bank = 0; bank < p->banks; bank++) seed_vectors(p, bank, seeds, &n);
    }
    for (int i = 0; i < cfg->extra_func_count; i++) {
        uint16_t addr = cfg->extra_funcs[i].addr;
        if (addr < 0x8000) continue;
        uint32_t slot = (addr >> SLOT_SHIFT) & (SLOT_COUNT - 1);
        seeds[n++] = POS_PACK((uint32_t)p->power_on[slot], slot, addr & (SLOT_SIZE - 1));
    }
    int file_seeds = 0;
    if (cfg->cycle_seed_file[0]) {
        FILE *sf = fopen(cfg->cycle_seed_file, "r");
        if (!sf) {
            fprintf(stderr, "[cyc] note: seed file %s not found (run the host with --miss-log to create it)\n",
                    cfg->cycle_seed_file);
        } else {
            char line[128];
            while (fgets(line, sizeof(line), sf) && (size_t)n + 1 < seed_cap) {
                uint32_t pos = parse_seed(p, line);
                if (pos == POS_NONE) continue;
                seeds[n++] = pos;
                file_seeds++;
            }
            fclose(sf);
        }
    }
    discover(p, seeds, n);
    free(seeds);
    if (file_seeds) printf("[NESRecomp] cycle-accurate: %d seeds from %s\n", file_seeds, cfg->cycle_seed_file);

    uint32_t count = 0, tails = 0;
    for (size_t i = 0; i < pos_space(p); i++) {
        count += p->is_insn[i];
        tails += p->seen[i] && !p->is_insn[i];
    }

    cyc_mkdir("generated");
    char path[512];
    int files = 0;
    for (uint32_t bank = 0; bank < p->banks; bank++) {
        bool any = false;
        for (uint32_t slot = 0; slot < SLOT_COUNT; slot++) any = any || slot_used(p, bank, slot);
        if (!any) continue;
        snprintf(path, sizeof(path), "generated/%s_cyc_b%02X.c", output_prefix, bank);
        emit_bank_file(p, path, bank);
        files++;
    }
    snprintf(path, sizeof(path), "generated/%s_cyc.c", output_prefix);
    emit_umbrella(p, path, output_prefix);

    printf("[NESRecomp] cycle-accurate (%s, %u KB PRG in %u banks of 8KB): %u instructions compiled",
           board, p->prg_len / 1024, p->banks, count);
    if (tails) printf(", %u left to the interpreter (they cross a bank or $FFFF)", tails);
    printf(" -> generated/%s_cyc.c + %d bank file%s\n", output_prefix, files, files == 1 ? "" : "s");
    free(p->is_insn);
    free(p->seen);
    free(p);
    return true;
}
