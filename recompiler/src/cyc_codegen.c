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
 * eight 4KB slots whose contents a game changes at run time. So a block is
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
#include "../../common/nes_fds.h"

/* A compiled block is valid for one PRG bank at one CPU address, because the
 * bytes at an address in $8000-$FFFF depend on which bank the mapper has
 * there. 4KB is the finest granularity any supported mapper switches, so the
 * CPU's $8000-$FFFF is four slots and a block is identified by (bank, slot,
 * offset in slot). NROM is the degenerate case: every slot's bank is fixed.
 *
 * One C function per 1KB keeps individual functions small, and one
 * translation unit per bank keeps them compilable in parallel (see
 * SPLITGEN_MIGRATION.md for the same split in the function-level output). */
#define SLOT_SHIFT      12
#define SLOT_SIZE       (1u << SLOT_SHIFT)
#define SLOT_COUNT      8
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
    uint32_t banks;                /* 4KB banks, rounded up to a power of two */
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
    /* Subroutines that take inline argument bytes after the JSR and return
     * past them: bytes per target address, 0 for an ordinary subroutine. */
    uint8_t  inline_jsr[0x10000];
} Program;

/* The FDS BIOS (disksys.rom, CRC32 5E607DCF) passes arguments inline after
 * the JSR: $E844 reads the two bytes after the JSR into its caller and
 * advances that return address by 2, and $E3E7/$E3EA do the same for 2 bytes,
 * or 4 when called with A = $FF. The routines that call them from their own
 * frame with a fixed A are listed; CheckFileCount/AdjustFileCount ($E2B7,
 * $E2BB) and SetFileCount ($E301, $E305) take their count from the caller's A
 * and are left out. Found in the BIOS disassembly; the miss log confirms it
 * (the continuations after `JSR $E7BB` at $EF4C, $EF51, $F0D7, $F0ED and
 * $F42B were the only BIOS instructions interpreted in the SMB2J boot, the
 * no-disk screen and the Otocky side swap before). */
static const struct { uint16_t target; uint8_t bytes; } FDS_BIOS_INLINE_JSR[] = {
    { 0xE7BB, 2 }, { 0xE8D2, 2 }, { 0xE8E1, 2 }, { 0xEBAF, 2 },   /* via $E844 */
    { 0xE1F8, 4 },                                              /* LoadFiles: $E3E7, A = $FF */
    { 0xE237, 4 }, { 0xE239, 4 },                               /* AppendFile / WriteFile: $E3EA, A = $FF */
    { 0xE32A, 2 },                                              /* GetDiskInfo: $E3E7, A = 0 */
};

static size_t pos_space(const Program *p) { return (size_t)p->banks * SLOT_COUNT * SLOT_SIZE; }

/* ---- code in RAM ----
 *
 * Code a program runs from RAM is compiled from images of RAM the compiler
 * knows: each PRG file of an FDS disk at its load address (every side, hidden
 * files and files that overlap each other included), and snapshots a run
 * captured (the host's --capture-log, [game] cycle_capture_file). An image is
 * discovered like ROM, but a compiled view of it folds nothing except its own
 * instructions' bytes, and it is split into views - the instructions static
 * control flow connects within one 1KB chunk, not following calls - so that
 * a store to one routine, or to a variable beside it, leaves the others
 * valid. The runtime (runner/cyc/cyc_ramview.c) enters a view only while RAM
 * holds exactly those bytes. */
typedef struct {
    uint16_t base;             /* CPU address of bytes[0] */
    uint32_t len;
    uint8_t *bytes;
    char     name[96];
    bool     evidence_only;    /* a capture snapshot: compile only what ran */
    bool     disk;             /* a disk file (not a snapshot) */
    uint32_t hash;             /* identity: FNV-1a of base and bytes (CycRamView.image) */
    uint8_t *seed, *seen, *is_insn, *live, *isolate;
    int32_t *root;             /* union-find over instruction positions */
    uint32_t gseed_cursor;     /* global seeds already applied */
} RamImage;

typedef struct {
    const RamImage *img;
    uint32_t  n;               /* instructions */
    uint16_t *pcs;             /* ascending */
    uint16_t *runs;            /* dependency (address, length) pairs */
    uint32_t  run_count;
    uint8_t  *bytes;           /* dependency bytes */
    uint32_t  byte_count;
    uint32_t  hash;
    int       index;           /* in the emitted table */
} RamView;

typedef struct { uint8_t len, b[3]; } RamVariant;

/* A captured snapshot of a 1KB chunk and the instructions that ran in it. */
typedef struct {
    uint16_t base;
    uint8_t  data[1026];
    uint16_t *pcs;
    uint32_t n;
} RamCapGroup;

typedef struct {
    bool       fds;            /* PRG RAM $6000-$DFFF exists */
    RamImage  *img;
    int        nimg, capimg;
    RamView   *view;
    int        nview, capview;
    /* Capture evidence, by CPU address: instruction variants that ran, and
     * bytes a store changed under a validated view. */
    RamVariant *var[0x10000];
    uint8_t     nvar[0x10000];
    uint8_t    *var_on_disk[0x10000];   /* per variant: some disk file holds it exactly */
    /* Stores the capture saw change a byte under a validated view, by
     * address: the values stored. */
    uint8_t    *vol[0x10000];
    uint16_t    nvol[0x10000];
    RamCapGroup *groups;       /* every captured snapshot */
    int          ngroups;
    /* Global seeds: vectors, targets that leave their image, seed-file lines;
     * each applies to every image covering it. */
    uint16_t   *gseed;
    uint32_t    ngseed, capgseed;
    uint8_t     is_gseed[0x10000];
    /* Every byte an emitted view folds, by CPU address (CPU RAM mirrored). */
    uint8_t     code[0x10000];
    uint32_t    code_prefix[0x10001];
    int         hook_errors;   /* [[mod_function_hook]] sites in RAM that cannot fire */
} RamProgram;

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
    /* The FDS RAM Adapter: $8000-$DFFF is PRG RAM (no bank, never compiled),
     * $E000-$FFFF the 8 KiB BIOS, fixed like an NROM bank. */
    if (mapper == 20) return slot >= 6 ? (int)((slot - 6) & (banks - 1)) : -1;
    /* Decode the board once into a slot mask, then calculate its bank. The
     * execution regressions cover this with an optimized compiler too: GCC
     * 13.3 -O3 miscompiled the previous per-case bank returns when inlined
     * into the four-slot initialization loop (even NROM lost its fixed banks). */
    unsigned slots;
    switch (mapper) {
    case 0: case 3: case 13: case 87: case 184: case 185: case 101: slots = 255; break; /* all PRG fixed */
    case 157: case 159: case 16: case 2: case 68: case 33: case 48: case 10: case 22: case 73: case 71: case 76: case 94: case 206: case 88: case 95: case 154: case 70: case 152: case 67: case 78: case 89: case 93: case 72: slots = 240; break; /* last 16 KiB */
    case 9: slots = 252; break;                                  /* last 24 KiB */
    case 85: case 24: case 26: case 4: case 118: case 119: case 69: case 19: case 210: case 18: case 64: case 158: case 65: case 32: case 80: case 207: case 82: case 552: case 21: case 23: case 25: case 75: slots = 192; break;         /* last 8 KiB */
    case 180: case 92: slots = 15; break;                        /* first 16 KiB */
    case 97: slots = 15 | 256; break;                            /* last 16 KiB at $8000 */
    default: slots = 0; break;
    }
    if (!(slots & (1u << slot))) return -1;
    if (slots & 256) return (int)((banks - 4 + slot) & (banks - 1));
    if (slots == 255 || slots == 15) return (int)(slot & (banks - 1));
    return (int)((banks - SLOT_COUNT + slot) & (banks - 1));
}

/* The configuration a cold console comes up in; hw_mapper.c's reset paths. */
static int power_on_bank8_for(int mapper, uint32_t banks, uint32_t slot) {
    switch (mapper) {
    case 40: { static const int b[4]={4,5,0,7}; return (int)(b[slot] & (banks-1)); }
    case 5: return slot==3 ? (int)(banks-1) : (int)(slot&(banks-1));
    case 153: return (int)((slot>=2 ? slot+28 : slot) & (banks-1));
    case 85: return slot == 3 ? (int)(banks - 1) : (int)slot;
    case 24: case 26: return slot == 3 ? (int)(banks - 1) : slot < 2 ? (int)slot : 0;
    case 157: case 159: case 16: case 21: case 22: case 23: case 25: case 73:
        return slot >= 2 ? (int)(banks - 4 + slot) : (int)slot;
    case 9: return slot ? (int)(banks - 4 + slot) : 0;
    case 10: return slot >= 2 ? (int)(banks - 4 + slot) : (int)slot;
    case 71: return slot >= 2 ? (int)(banks - 2 + slot - 2) : (int)slot;
    case 75: return slot == 3 ? (int)(banks - 1) : (int)slot;
    case 206: case 88: case 95: case 154: return slot >= 2 ? (int)(banks - 2 + slot - 2) : (int)slot;
    case 76: return slot >= 2 ? (int)(banks - 2 + slot - 2) : (int)slot;
    case 94: return slot >= 2 ? (int)(banks - 2 + slot - 2) : (int)slot;
    case 70: case 152: return slot >= 2 ? (int)(banks - 2 + slot - 2) : (int)slot;
    case 180: return (int)(slot & 1);
    case 232: return slot < 2 ? (int)slot : (int)(slot + 4);
    case 1: case 155: return (int)((slot>=2 ? slot+28 : slot)&(banks-1)); /* first 256K outer bank */
    case 2: case 68: case 67: case 78: case 89: case 93: case 72: case 92: return slot >= 2 ? (int)(banks - 2 + (slot - 2)) : (int)slot;
    case 97: return slot < 2 ? (int)(banks - 2 + slot) : (int)(slot - 2);
    case 69: case 19: case 210: case 18: case 64: case 158: case 80: case 207: case 82: case 552:
        return slot == 3 ? (int)(banks - 1) : 0;
    case 33: case 48: case 32: return slot >= 2 ? (int)(banks - 4 + slot) : 0;
    case 4: case 118: case 119: case 65: return slot == 2 ? (int)(banks - 2) : slot == 3 ? (int)(banks - 1) : (int)slot;
    default: return (int)(slot & (banks - 1));
    }
}

static int power_on_bank_for(int mapper, uint32_t banks, uint32_t slot) {
    if (mapper == 20) return fixed_bank_for(mapper, banks, slot);
    if (mapper == 31) return slot == 7 ? (int)(255 & (banks-1)) : 0;
    if (banks == 1) return 0;
    return power_on_bank8_for(mapper, banks / 2, slot / 2) * 2 + (slot & 1);
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
        case K_JSR: {
            uint16_t target = (uint16_t)(prg_byte(p, at.bank, at.k + 1) | prg_byte(p, at.bank, at.k + 2) << 8);
            succ[n++] = target;
            succ[n++] = (uint16_t)(next + p->inline_jsr[target]);  /* return address, past inline arguments */
            break;
        }
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
    /* A RAM view (NULL for ROM): its image, its instructions (the labels),
     * the program's folded-byte map (store checks), and whether this
     * instruction reads its operand bytes at run time (they change there). */
    const RamImage   *img;
    const RamView    *view;
    const RamProgram *ram;
    bool              live;
} Emit;

#define INTERP(e) ((e)->p == NULL)
/* Operands read at run time: the interpreter, or a compiled RAM instruction
 * whose operand bytes the program rewrites. Such an instruction is the
 * interpreter's template with its address and opcode constant, and returns to
 * the scheduler when it is done. */
#define LIVE(e) ((e)->p == NULL || (e)->live)

/* Byte k of the instruction being emitted, from ROM or from the RAM image. */
static uint8_t insn_byte(const Emit *e, int k) {
    if (e->img) return e->img->bytes[(uint32_t)(e->P - e->img->base) + (uint32_t)k];
    return prg_byte(e->p, e->at.bank, e->at.k + (uint32_t)k);
}

static bool view_has(const RamView *v, uint16_t addr) {
    uint32_t lo = 0, hi = v->n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (v->pcs[mid] < addr) lo = mid + 1; else hi = mid;
    }
    return lo < v->n && v->pcs[lo] == addr;
}

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
    if (e->view) {
        /* Only this instruction's own bytes, which the view's dependency
         * holds: every other RAM or ROM byte is read at run time. */
        uint16_t k = (uint16_t)(addr - e->P);
        if (e->live || k >= (uint16_t)op_length(e->opcode)) return false;
        *out = insn_byte(e, k);
        return true;
    }
    Pos to;
    if (!target_pos(e->p, &e->at, addr, &to)) return false;
    *out = prg_byte(e->p, to.bank, to.k);
    return true;
}

static const char *read_const(Emit *e, uint16_t addr, int f) {
    uint8_t v;
    if (!LIVE(e) && known_byte(e, addr, &v))
        return str("cpu_read_rom(0x%04X, 0x%02X, %s)", addr, v, flags(f));
    return str("cpu_read(0x%04X, %s)", addr, flags(f));
}

/* Read instruction byte k (1 or 2, or the byte after a one-byte
 * instruction): a statement that leaves the value in operand(e, k). */
static void read_operand(Emit *e, int k, int f) {
    if (LIVE(e))
        ln(e, "b%d = cpu_read((uint16_t)(pc + %d), %s);", k, k, flags(f));
    else
        ln(e, "%s;", read_const(e, (uint16_t)(e->P + k), f));
}

/* The 16-bit operand of a 3-byte instruction, as a value. */
static uint16_t insn_operand16(const Emit *e) {
    return (uint16_t)(insn_byte(e, 1) | insn_byte(e, 2) << 8);
}

/* An instruction's own operand bytes. fits_in_slot() kept the whole
 * instruction inside one slot, so these are always the block's own bank. */
static const char *operand(Emit *e, int k) {
    if (LIVE(e)) return k == 1 ? "b1" : "b2";
    return str("0x%02X", insn_byte(e, k));
}

static const char *operand16(Emit *e) {
    if (LIVE(e)) return "(uint16_t)(b1 | b2 << 8)";
    return str("0x%04X", insn_operand16(e));
}

/* The address of the instruction's k-th byte. */
static const char *pc_plus(Emit *e, int k) {
    if (LIVE(e)) return str("(uint16_t)(pc + %d)", k);
    return str("0x%04X", (uint16_t)(e->P + k));
}

/* Whether a target is a label in the C function being emitted. It has to be a
 * compiled instruction of the same bank and slot (another one is another
 * translation unit) and in the same 1KB chunk (another one is another
 * function); anything else goes back to the scheduler, which looks up the
 * bank the mapper has there now. */
static bool is_label(Emit *e, uint16_t addr) {
    if (e->view) return view_has(e->view, addr);
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
    if (LIVE(e))
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
    case 5: return 0x5000;
    case 16: return 0x6000;
    case 34: return 0x7ffd;
    case 79: case 146: return 0x4100;
    case 31: return 0x5000;
    case 113: return 0x4100;
    case 140: return 0x6000;
    case 86: return 0x6000;
    case 41: return 0x6000;
    case 80: case 207: case 82: case 552: return 0x7ef0;
    default: return 0x8000;
    }
}

static WriteReach write_reach(Emit *e, AddrMode am) {
    /* A RAM view folds no ROM byte, so a bank switch cannot stale it. */
    if (LIVE(e) || e->view || !e->p->banked) return WR_NEVER;
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

/* ---- stores that can change code a RAM view folded ----
 *
 * A compiled RAM view is correct only while RAM holds the bytes it folded.
 * A store that changes one of them - self-modifying code, a routine copied
 * over another, a disk file loaded over code - makes the runtime's write
 * watch set cyc_ram_code_dirty (runner/cyc/cyc_ramview.c), and the block
 * must not run a single folded instruction more: after every store that can
 * reach a byte some view of the program folds, it tests the flag and returns
 * to the scheduler, which validates the next view afresh. A store that can
 * only reach bytes no view folds (the stack page, most variables) continues
 * in place: nothing it changes is folded anywhere. */
static bool code_in(const RamProgram *r, uint32_t lo, uint32_t hi) {
    return r->code_prefix[hi + 1] != r->code_prefix[lo];
}

static bool store_reaches_code(const Emit *e, AddrMode am) {
    const RamProgram *r = e->ram;
    switch (am) {
    case AM_ZP:
        return code_in(r, insn_byte(e, 1), insn_byte(e, 1));
    case AM_ZPX: case AM_ZPY:
        return code_in(r, 0x00, 0xFF);
    case AM_ABS: {
        uint16_t a = insn_operand16(e);
        return code_in(r, a, a);
    }
    case AM_ABSX: case AM_ABSY: {
        uint32_t base = insn_operand16(e), top = base + 0xFF;
        if (top <= 0xFFFF) return code_in(r, base, top);
        return code_in(r, base, 0xFFFF) || code_in(r, 0, top - 0x10000);
    }
    default:
        return code_in(r, 0, 0xFFFF);   /* a run-time pointer */
    }
}

static void ram_store_check(Emit *e, AddrMode am) {
    if (!e->view || e->live || !store_reaches_code(e, am)) return;
    ln(e, "if (cyc_ram_code_dirty) { cpu.pc = 0x%04X; return; }   /* stored to compiled code */",
       (uint16_t)(e->P + op_length(e->opcode)));
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
    if (LIVE(e))
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
    if (!bank_exit(e, reach)) { ram_store_check(e, d->am); go_next(e); }
}

static void emit_rmw(Emit *e, const OpDef *d) {
    WriteReach reach = write_reach(e, d->am);
    addressing(e, d->am, true, false);
    ln(e, "uint8_t v = cpu_read(ea, 0), r;");
    ln(e, "cpu_write(ea, v, 0);");
    ln(e, "%s", d->body);
    ln(e, "cpu_write(ea, r, CYC_POLL | CYC_DONE);");
    if (d->post) ln(e, "%s", d->post);
    if (!bank_exit(e, reach)) { ram_store_check(e, d->am); go_next(e); }
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
    /* The written high byte can be ANDed with the index: any address. */
    if (!bank_exit(e, reach)) { ram_store_check(e, AM_INDY); go_next(e); }
}

/* The negation of a branch condition ("cpu.z" or "!cpu.z"). */
static const char *not_taken(const OpDef *d) {
    return d->body[0] == '!' ? d->body + 1 : str("!%s", d->body);
}

/* Not taken: one cycle. Taken: a second cycle without a poll, and a third,
 * polling but keeping an IRQ the first poll latched, only when the target is
 * on another page. */
static void emit_branch(Emit *e, const OpDef *d) {
    if (LIVE(e)) {
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
    uint16_t target = (uint16_t)(next + (int8_t)insn_byte(e, 1));
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
    if (LIVE(e))
        ln(e, "cpu.pc = %s;", operand16(e));
    else
        ln(e, "%s", jump_const(e, insn_operand16(e)));
}

/* JMP ($xxFF) reads the high byte from $xx00. */
static void emit_jmp_ind(Emit *e) {
    read_operand(e, 1, 0);
    read_operand(e, 2, 0);
    if (LIVE(e)) {
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
    if (LIVE(e)) {
        ln(e, "cpu_write((uint16_t)(0x100 | cpu.s), (uint8_t)((pc + 2) >> 8), 0);");
        ln(e, "cpu_write((uint16_t)(0x100 | (uint8_t)(cpu.s - 1)), (uint8_t)(pc + 2), 0);");
    } else {
        uint16_t ret = (uint16_t)(e->P + 2);
        ln(e, "cpu_write((uint16_t)(0x100 | cpu.s), 0x%02X, 0);", ret >> 8);
        ln(e, "cpu_write((uint16_t)(0x100 | (uint8_t)(cpu.s - 1)), 0x%02X, 0);", ret & 0xFF);
    }
    read_operand(e, 2, POLL | DONE);
    ln(e, "cpu.s = (uint8_t)(cpu.s - 2);");
    if (LIVE(e))
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
    ln(e, "cpu.pc = (uint16_t)((lo | hi << 8) + 1);");
    ln(e, "if (cyc_cpu_rts_observer) cyc_cpu_rts_observer();");
    ln(e, "return;");
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

/* ---- mod hook sites ----
 *
 * game.toml [[mod_function_hook]] entries: addresses where a trusted mod
 * plugin observes (or takes over) a routine of the original program
 * (runner/cyc/cyc_hooks.h). A compiled instruction there first asks whether
 * any hook is armed; if so the block returns to the scheduler at that
 * instruction, which checks the site's content key against what memory holds
 * now, runs the plugins, and dispatches the instruction again (a flag lets it
 * through the second time). Programs without hooks emit nothing. */
static const GameConfig *s_hook_cfg;

/* A site in RAM (CPU RAM, cartridge RAM, the FDS PRG RAM) is keyed on its
 * content; one in ROM on its bank. */
static bool hook_in_ram(int mapper, uint16_t a) {
    return a < 0x8000 || (mapper == 20 && a < 0xE000);
}

/* Does a hook's content key hold the `len` bytes at `bytes`? */
static bool key_holds(const ModHookKey *k, const uint8_t *bytes, uint32_t available) {
    return k->len <= available && nes_crc32(0, bytes, k->len) == k->crc32;
}

/* ... the bytes of ROM bank `bank` from offset `off` in its slot. */
static bool key_holds_rom(const ModHookKey *k, const Program *p, uint32_t bank, uint32_t off) {
    uint8_t b[GAME_CFG_MOD_HOOK_BYTES];
    if (off + k->len > SLOT_SIZE) return false;
    for (uint32_t j = 0; j < k->len; j++) b[j] = prg_byte(p, bank, off + j);
    return key_holds(k, b, k->len);
}

static const ModHookKey *hook_key(int i) {
    static const ModHookKey none;
    return i < GAME_CFG_MAX_MOD_HOOK_KEYS ? &s_hook_cfg->mod_function_hook_keys[i] : &none;
}

/* Does instruction e (at e->P) carry a hook site? In a RAM view only when
 * the image holds the site's content key there: a view of another file at the
 * same address can never be that site. */
static bool hook_site_here(const Emit *e) {
    if (!s_hook_cfg || INTERP(e)) return false;
    for (int i = 0; i < s_hook_cfg->mod_function_hook_count; i++) {
        if (s_hook_cfg->mod_function_hooks[i].addr != e->P) continue;
        const ModHookKey *k = hook_key(i);
        if (!k->len) return true;
        if (!e->img) {
            if (key_holds_rom(k, e->p, e->at.bank, e->at.k)) return true;
            continue;
        }
        uint32_t off = (uint32_t)(e->P - e->img->base);
        if (off < e->img->len && key_holds(k, &e->img->bytes[off], e->img->len - off)) return true;
    }
    return false;
}

static void emit_instruction(Emit *e) {
    const OpDef *d = &OPS[e->opcode];
    out(e, "L_%04X: /* %02X %s%s */\n", e->P, e->opcode, d->name, mode_suffix(d->am));
    out(e, "    if (hw_frame_done) { cpu.pc = 0x%04X; return; }\n", e->P);
    if (hook_site_here(e))
        out(e, "    if (cyc_hook_stop(0x%04X)) { cpu.pc = 0x%04X; return; }   /* mod hook site */\n", e->P, e->P);
    out(e, "    if (cpu_fetch_rom(0x%04X, 0x%02X)) { cpu.pc = 0x%04X; cpu_interrupt(false); return; }\n", e->P,
        e->opcode, e->P);
    out(e, "    {\n");
    if (e->live) ln(e, "const uint16_t pc = 0x%04X; uint8_t b1 = 0, b2 = 0; (void)pc; (void)b1; (void)b2;   /* operands vary */", e->P);
    emit_body(e);
    if (e->live) ln(e, "return;");
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
/* A C string literal. */
static void emit_c_string(FILE *f, const char *s) {
    if (!s || !*s) { fputs("NULL", f); return; }
    fputc('"', f);
    for (; *s; ++s) {
        if (*s == '\\' || *s == '"') fputc('\\', f);
        if ((unsigned char)*s < 32) fprintf(f, "\\%03o", (unsigned char)*s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static void ram_emit_table(FILE *f, const RamProgram *r);

static void emit_umbrella(const Program *p, const char *path, const char *prefix, const CycFdsProgram *fds,
                          const RamProgram *ram, uint8_t console, const char *display_name) {
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
    fprintf(f, "const uint32_t cyc_native_cart_hash = 0x%08Xu;\n", nes_cart_identity(&p->rom->cart));
    fprintf(f, "const uint32_t cyc_native_fds_bios_crc32 = 0x%08Xu;\n", fds ? fds->bios_crc32 : 0u);
    fputs("const char *cyc_native_fds_bios_path = ", f);
    emit_c_string(f, fds ? fds->bios_path : NULL);
    fputs(";\nconst char *cyc_native_fds_image_path = ", f);
    emit_c_string(f, fds ? fds->image_path : NULL);
    fputs(";\nconst char *cyc_native_fds_hle = ", f);
    emit_c_string(f, fds ? fds->hle : NULL);
    fputs(";\nconst char *cyc_native_display_name = ", f);
    emit_c_string(f, display_name && *display_name ? display_name : NULL);
    fputs(";\n", f);
    fprintf(f, "const uint8_t cyc_native_console = %u;  /* game.toml [game] console: %s */\n", console,
            console == 1 ? "nes" : console == 2 ? "famicom" : "the board's default");
    for (uint32_t i = 0; i < p->prg_len; i++) prg_hash = (prg_hash ^ p->rom->prg_data[i]) * 16777619u;
    fprintf(f,
        "const char *cyc_native_program_name = \"%s\";\n"
        "const uint32_t cyc_native_prg_hash = 0x%08Xu;  /* FNV-1a of the PRG ROM compiled */\n\n"
        "/* Which compiled view covers a CPU address right now: the bank the\n"
        " * cartridge has in that address's 4KB slot decides, because the bytes\n"
        " * there - and so the block compiled from them - depend on it. */\n"
        "static const CycNativeView *view_at(uint16_t addr, unsigned *out_k) {\n"
        "    unsigned slot = (addr >> %d) & %u;\n"
        "    unsigned bank = hw_prg_bank4(addr);\n"
        "    if (bank >= CYC_BANKS) return 0;\n"
        "    const CycNativeView *v = &VIEWS[bank][slot];\n"
        "    *out_k = addr & %uu;\n"
        "    return v->bits ? v : 0;\n"
        "}\n\n"
        "bool cyc_native_has(uint16_t addr) {\n"
        "    if (!hw_prg_is_rom(addr) || !hw_prg_is_stable()) return false;\n"
        "    unsigned k;\n"
        "    const CycNativeView *v = view_at(addr, &k);\n"
        "    return v && ((v->bits[k >> 3] >> (k & 7)) & 1);\n"
        "}\n\n"
        "/* Run compiled instructions from cpu.pc until execution leaves compiled\n"
        " * code or the PPU finishes a frame. Called at an instruction boundary.\n"
        " * Each iteration looks the mapping up again: a chunk returns here when\n"
        " * control crosses a slot, a chunk boundary, or a write to the mapper,\n"
        " * and goes back to the scheduler at a mod hook site (cyc_hooks.h). */\n"
        "void cyc_native_run(void) {\n"
        "    while (!hw_frame_done && !cyc_hook_hit && !cpu.jammed) {\n"
        "        uint16_t pc = cpu.pc;\n"
        "        if (!hw_prg_is_rom(pc) || !hw_prg_is_stable()) return;\n"
        "        unsigned k;\n"
        "        const CycNativeView *v = view_at(pc, &k);\n"
        "        if (!v || !((v->bits[k >> 3] >> (k & 7)) & 1)) return;\n"
        "        void (*chunk)(void) = v->chunks[k >> %d];\n"
        "        if (!chunk) return;\n"
        "        chunk();\n"
        "    }\n"
        "}\n",
        prefix, prg_hash, SLOT_SHIFT, SLOT_COUNT - 1, SLOT_SIZE - 1, CHUNK_SHIFT);
    fputc('\n', f);
    ram_emit_table(f, ram);
    /* The mod hook sites (runner/cyc/cyc_hooks.c): the runtime fires a site
     * only while memory holds its content key, and refuses a plugin that
     * registers for a site the program does not declare. */
    int nh = s_hook_cfg ? s_hook_cfg->mod_function_hook_count : 0;
    fprintf(f, "\n/* game.toml [[mod_function_hook]] sites: id, address, content key. */\n"
               "const CycHookSite cyc_native_hook_sites[%d] = {", nh ? nh : 1);
    if (!nh) fprintf(f, " { 0, 0, 0, 0 }");
    for (int i = 0; i < nh; i++) {
        const ModHookKey *k = hook_key(i);
        fprintf(f, "\n    { ");
        emit_c_string(f, k->id[0] ? k->id : NULL);
        fprintf(f, ", 0x%04X, %u, 0x%08Xu },", s_hook_cfg->mod_function_hooks[i].addr, k->len, k->crc32);
    }
    fprintf(f, "\n};\nconst uint32_t cyc_native_hook_site_count = %du;\n", nh);
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
    fprintf(f, "    }\n    (void)b1; (void)b2;\n}\n\n"
               "/* Bytes each opcode takes from the instruction stream (BRK: 2). */\n"
               "const uint8_t cpu6502_op_length[256] = {");
    for (int op = 0; op < 256; op++) fprintf(f, "%s%d,", op % 16 ? " " : "\n    ", op_length((uint8_t)op));
    fprintf(f, "\n};\n");
    fclose(f);
    printf("[NESRecomp] cycle-accurate interpreter -> %s\n", path);
    return true;
}

/* The board names hw_mapper.c implements, for the message and the banner. */
static const char *mapper_name(int mapper) {
    switch (mapper) {
    case 20: return "FDS RAM Adapter";
    case 159: return "Bandai LZ93D50 / X24C01";
    case 5: return "MMC5";
    case 157: return "Bandai Datach";
    case 153: return "Bandai BA-JUMP2";
    case 16: return "Bandai FCG / LZ93D50";
    case 85: return "VRC7";
    case 24: return "VRC6a";
    case 26: return "VRC6b";
    case 21: return "VRC4a/c";
    case 23: return "VRC2b / VRC4e/f";
    case 25: return "VRC2c / VRC4b/d";
    case 22: return "VRC2a";
    case 73: return "VRC3";
    case 31: return "NSF cartridge";
    case 9: return "MMC2";
    case 10: return "MMC4";
    case 11: return "Color Dreams";
    case 13: return "CPROM";
    case 34: return "BNROM / NINA-001";
    case 71: return "Camerica";
    case 75: return "VRC1";
    case 206: return "DxROM";
    case 88: return "Namco 118 (NAMCOT-3433)";
    case 95: return "Namco 118 (NAMCOT-3425)";
    case 154: return "Namco 118 (NAMCOT-3453)";
    case 76: return "Namco 109";
    case 79: return "NINA-003/006";
    case 87: return "J87";
    case 94: return "UN1ROM";
    case 113: return "HES";
    case 140: return "Jaleco JF-11/14";
    case 180: return "Crazy Climber";
    case 70: return "Bandai 74161/7432";
    case 152: return "Bandai 74161/7432 (one-screen)";
    case 184: return "Sunsoft-1";
    case 232: return "Camerica Quattro";
    case 0:  return "NROM";
    case 1:  return "MMC1";
    case 155: return "MMC1A";
    case 40: return "NTDEC 2722";
    case 2:  return "UxROM";
    case 3:  return "CNROM";
    case 4:  return "MMC3";
    case 118: return "TxSROM";
    case 69: return "Sunsoft FME-7 / 5B";
    case 19: return "Namco 163";
    case 185: return "CNROM + copy protection";
    case 18: return "Jaleco SS88006";
    case 64: return "Tengen RAMBO-1";
    case 65: return "Irem H3001";
    case 67: return "Sunsoft-3";
    case 78: return "Irem 74HC161 / Jaleco JF-16";
    case 89: return "Sunsoft-2 (Sunsoft-3 board)";
    case 93: return "Sunsoft-2 (Sunsoft-3R board)";
    case 97: return "Irem TAM-S1";
    case 72: return "Jaleco JF-17";
    case 92: return "Jaleco JF-19";
    case 86: return "Jaleco JF-13";
    case 101: return "Jaleco JF-10 (mapper 101)";
    case 77: return "Irem LROG017";
    case 96: return "Bandai Oeka Kids";
    case 144: return "Color Dreams (Death Race)";
    case 146: return "Sachen 3015 / SA-016";
    case 148: return "Sachen SA-008-A / Tengen 800008";
    case 158: return "Tengen 800037";
    case 33: return "Taito TC0190";
    case 32: return "Irem G-101";
    case 80: return "Taito X1-005";
    case 207: return "Taito X1-005 (CHR mirroring)";
    case 82: return "Taito X1-017";
    case 552: return "Taito X1-017 (NES 2.0)";
    case 48: return "Taito TC0690";
    case 210: return "Namco 175 / 340";
    case 68: return "Sunsoft-4";
    case 41: return "Caltron 6-in-1";
    case 228: return "Active Enterprises";
    case 119: return "TQROM";
    case 7:  return "AxROM";
    case 66: return "GxROM";
    default: return NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* Code in RAM: images, discovery, views                                     */
/* ------------------------------------------------------------------------ */

/* RAM a view may fold: CPU RAM $0000-$07FF except the stack page (a JSR
 * pushes between its own operand fetches), and on the FDS the PRG RAM
 * $6000-$DFFF. Mirrors of CPU RAM are other addresses and are interpreted. */
static bool ram_addr_ok(const RamProgram *r, uint32_t a) {
    if (a < 0x800) return a < 0x100 || a >= 0x200;
    return r->fds && a >= 0x6000 && a < 0xE000;
}

/* Every byte of the instruction in one RAM window, none in the stack page. */
static bool ram_insn_ok(const RamProgram *r, uint16_t pc, int len) {
    uint32_t last = (uint32_t)pc + (uint32_t)len - 1;
    if (!ram_addr_ok(r, pc) || !ram_addr_ok(r, last)) return false;
    if (pc < 0x800) return last < 0x800 && !(pc < 0x200 && last >= 0x100);
    return last < 0xE000;
}

static RamImage *ram_add_image(RamProgram *r, uint16_t base, const uint8_t *bytes, uint32_t len, bool evidence,
                               const char *fmt, ...) {
    if (r->nimg == r->capimg) {
        r->capimg = r->capimg ? r->capimg * 2 : 16;
        r->img = (RamImage *)realloc(r->img, sizeof(RamImage) * (size_t)r->capimg);
    }
    RamImage *im = &r->img[r->nimg++];
    memset(im, 0, sizeof(*im));
    im->base = base;
    im->len = len;
    im->evidence_only = evidence;
    im->bytes = (uint8_t *)malloc(len);
    memcpy(im->bytes, bytes, len);
    im->seed = (uint8_t *)calloc(len, 1);
    im->seen = (uint8_t *)calloc(len, 1);
    im->is_insn = (uint8_t *)calloc(len, 1);
    im->live = (uint8_t *)calloc(len, 1);
    im->isolate = (uint8_t *)calloc(len, 1);
    im->root = (int32_t *)malloc(sizeof(int32_t) * len);
    im->disk = !evidence;
    uint32_t h = 2166136261u;
    h = (h ^ (base & 0xFF)) * 16777619u;
    h = (h ^ (base >> 8)) * 16777619u;
    for (uint32_t k = 0; k < len; ++k) h = (h ^ bytes[k]) * 16777619u;
    im->hash = h;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(im->name, sizeof(im->name), fmt, ap);
    va_end(ap);
    return im;
}

static void ram_add_gseed(RamProgram *r, uint32_t a) {
    if (a > 0xFFFF || !ram_addr_ok(r, a) || r->is_gseed[a]) return;
    r->is_gseed[a] = 1;
    if (r->ngseed == r->capgseed) {
        r->capgseed = r->capgseed ? r->capgseed * 2 : 256;
        r->gseed = (uint16_t *)realloc(r->gseed, sizeof(uint16_t) * r->capgseed);
    }
    r->gseed[r->ngseed++] = (uint16_t)a;
}

static void ram_add_variant(RamProgram *r, uint16_t a, const uint8_t *b, uint8_t len) {
    for (unsigned k = 0; k < r->nvar[a]; ++k)
        if (r->var[a][k].len == len && !memcmp(r->var[a][k].b, b, len)) return;
    if (r->nvar[a] == 255) return;
    r->var[a] = (RamVariant *)realloc(r->var[a], sizeof(RamVariant) * (r->nvar[a] + 1u));
    RamVariant *v = &r->var[a][r->nvar[a]++];
    memset(v, 0, sizeof(*v));
    v->len = len;
    memcpy(v->b, b, len);
}

/* The PRG files of every side of an FDS image, at their load addresses: a
 * file's part in CPU RAM and its part in PRG RAM are separate images (the
 * BIOS writes the rest to registers or to the ROM, which keep nothing). */
static int ram_add_fds_images(RamProgram *r, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[cyc] cannot read the disk image %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size);
    bool ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    const char *ext = strrchr(path, '.');
    bool qd = ext && (!strcmp(ext, ".qd") || !strcmp(ext, ".QD"));
    NesFdsImage img;
    if (!ok || !nes_fds_image(data, (size_t)size, qd ? NES_FDS_QD : NES_FDS_NONE, &img)) {
        fprintf(stderr, "[cyc] %s is not an FDS image\n", path);
        free(data);
        return -1;
    }
    int files = 0;
    for (unsigned side = 0; side < img.sides; ++side) {
        NesFdsSide s;
        NesFdsWalk w;
        if (!nes_fds_side_begin(&img, side, &s, &w)) continue;
        NesFdsFile file;
        while (nes_fds_next_file(&w, &file)) {
            if (file.type != 0 || !file.data || !file.size) continue;
            uint32_t lo = file.load_addr, hi = lo + file.size;
            char name[9];
            for (int k = 0; k < 8; ++k) {
                unsigned char c = (unsigned char)file.name[k];
                name[k] = c >= 0x20 && c < 0x7F && c != '"' && c != '\\' ? (char)c : '.';
            }
            name[8] = 0;
            if (lo < 0x800) {
                uint32_t end = hi < 0x800 ? hi : 0x800;
                ram_add_image(r, (uint16_t)lo, file.data, end - lo, false, "side %u file %u (%s%s) $%04X",
                              side, file.index, name, file.hidden ? ", hidden" : "", (unsigned)lo);
            }
            uint32_t plo = lo > 0x6000 ? lo : 0x6000, phi = hi < 0xE000 ? hi : 0xE000;
            if (r->fds && plo < phi)
                ram_add_image(r, (uint16_t)plo, file.data + (plo - lo), phi - plo, false, "side %u file %u (%s%s) $%04X",
                              side, file.index, name, file.hidden ? ", hidden" : "", (unsigned)plo);
            files++;
        }
    }
    free(data);
    return files;
}

/* ---- the capture file (runner/cyc/cyc_ramview.c writes it) ---- */

static char *ram_read_line(FILE *f) {
    size_t cap = 256, n = 0;
    char *s = (char *)malloc(cap);
    int ch = 0;
    while ((ch = fgetc(f)) != EOF) {
        if (n + 2 > cap) s = (char *)realloc(s, cap *= 2);
        if (ch == '\n') break;
        if (ch != '\r') s[n++] = (char)ch;
    }
    if (ch == EOF && !n) { free(s); return NULL; }
    s[n] = 0;
    return s;
}

static int ram_hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static bool ram_parse_hex(const char *s, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int hi = ram_hex(s[2 * i]), lo = hi < 0 ? -1 : ram_hex(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static int ram_load_captures(RamProgram *r, const char *path, RamCapGroup **groups, int *ngroups) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int cap = 0, insns = 0;
    char *line;
    while ((line = ram_read_line(f)) != NULL) {
        unsigned addr, count, value, image;
        char hex[16], hash[16];
        int used = 0;
        if (sscanf(line, "insn %x %15s %u", &addr, hex, &count) == 3) {
            uint8_t b[3];
            size_t len = strlen(hex) / 2;
            if (addr <= 0xFFFF && len >= 1 && len <= 3 && strlen(hex) == 2 * len && ram_parse_hex(hex, b, len) &&
                (size_t)op_length(b[0]) == len) {
                ram_add_variant(r, (uint16_t)addr, b, (uint8_t)len);
                insns++;
            }
        } else if (sscanf(line, "volatile %x %x %x %u", &addr, &value, &image, &count) == 4) {
            if (addr <= 0xFFFF && value <= 0xFF) {
                bool seen = false;
                for (unsigned k = 0; k < r->nvol[addr]; ++k) seen = seen || r->vol[addr][k] == value;
                if (!seen) {
                    r->vol[addr] = (uint8_t *)realloc(r->vol[addr], r->nvol[addr] + 1u);
                    r->vol[addr][r->nvol[addr]++] = (uint8_t)value;
                }
            }
        } else if (sscanf(line, "code %x %15s %u %n", &addr, hash, &count, &used) == 3 && used) {
            char *data = ram_read_line(f);
            if (data && !strncmp(data, "data ", 5) && strlen(data + 5) == 2 * 1026 && addr <= 0xFFFF && !(addr & 0x3FF)) {
                if (*ngroups == cap) {
                    cap = cap ? cap * 2 : 16;
                    *groups = (RamCapGroup *)realloc(*groups, sizeof(RamCapGroup) * (size_t)cap);
                }
                RamCapGroup *g = &(*groups)[*ngroups];
                memset(g, 0, sizeof(*g));
                g->base = (uint16_t)addr;
                if (ram_parse_hex(data + 5, g->data, 1026)) {
                    g->pcs = (uint16_t *)malloc(sizeof(uint16_t) * 1024);
                    for (char *t = line + used; *t;) {
                        char *end;
                        unsigned long pc = strtoul(t, &end, 16);
                        if (end == t) break;
                        if (pc >= addr && pc < addr + 1024u) g->pcs[g->n++] = (uint16_t)pc;
                        t = *end == ',' ? end + 1 : end;
                    }
                    (*ngroups)++;
                }
            }
            free(data);
        }
        free(line);
    }
    fclose(f);
    return insns;
}

/* ---- what the evidence says about one instruction of one image ----
 *
 * The capture recorded the instruction variants that ran with no view, and
 * the values stores wrote over bytes compiled views fold that no view held
 * there. A stored value a disk file holds at that address is a file loading
 * over another (an overlay), which content-keyed views already handle. Any
 * other is the program rewriting that byte at run time:
 *
 *   - an operand byte (or a variant that ran with operands no disk file has
 *     there): the instruction is compiled to read its operands at run time
 *     (live), so the view does not depend on them at all;
 *   - an opcode byte: the instruction is compiled as this image has it, but
 *     alone (isolated), so the view holding it can go invalid without taking
 *     the code around it along - which also contains data that discovery
 *     decoded as code and the program writes as data. */
static bool ram_held(const RamProgram *r, uint16_t a, uint8_t value) {
    for (int k = 0; k < r->nimg; ++k) {
        const RamImage *im = &r->img[k];
        if (im->disk && a >= im->base && (uint32_t)(a - im->base) < im->len && im->bytes[a - im->base] == value) return true;
    }
    return false;
}

static bool ram_rewritten(const RamProgram *r, uint16_t a) {
    for (unsigned k = 0; k < r->nvol[a]; ++k)
        if (!ram_held(r, a, r->vol[a][k])) return true;
    return false;
}

static void ram_judge(const RamProgram *r, const RamImage *im, uint16_t pc, int len, bool *live, bool *isolate) {
    const uint8_t *b = &im->bytes[pc - im->base];
    bool differs = false, operand = false;
    for (unsigned k = 0; k < r->nvar[pc]; ++k) {
        const RamVariant *v = &r->var[pc][k];
        if (v->b[0] == b[0] && v->len == len && !r->var_on_disk[pc][k] && memcmp(v->b + 1, b + 1, (size_t)len - 1))
            differs = true;
    }
    for (int k = 1; k < len; ++k) operand = operand || ram_rewritten(r, (uint16_t)(pc + k));
    *live = len > 1 && (differs || operand);
    *isolate = ram_rewritten(r, pc);
}

/* Successors of the instruction at pc in image im: n entries of (address,
 * kind) where kind 0 = local flow (fall-through, branch), 1 = a JMP/JSR
 * target (also a global seed: whatever image is resident there then). A live
 * instruction's operand-dependent targets are unknown. */
static int ram_successors(const Program *p, const RamImage *im, uint16_t pc, bool live, uint32_t out[3], int kind[3]) {
    const uint8_t *b = &im->bytes[pc - im->base];
    const OpDef *d = &OPS[b[0]];
    uint16_t next = (uint16_t)(pc + op_length(b[0]));
    int n = 0;
    switch (d->kind) {
    case K_BRANCH:
        out[n] = next; kind[n++] = 0;
        if (!live) { out[n] = (uint16_t)(next + (int8_t)b[1]); kind[n++] = 0; }
        break;
    case K_JMP:
        if (!live) { out[n] = (uint16_t)(b[1] | b[2] << 8); kind[n++] = 1; }
        break;
    case K_JSR: {
        uint16_t target = (uint16_t)(b[1] | b[2] << 8);
        if (!live) { out[n] = target; kind[n++] = 1; }
        out[n] = (uint16_t)(next + (live ? 0 : p->inline_jsr[target])); kind[n++] = 0;
        break;
    }
    case K_JMPIND: case K_RTS: case K_RTI: case K_HLT:
        break;
    default:
        out[n] = next; kind[n++] = 0;
        break;
    }
    return n;
}

/* Discover from the image's pending seeds. Targets outside the image become
 * global seeds (RAM) or ROM seeds (fixed ROM slots, the FDS BIOS). */
static void ram_discover(const Program *p, RamProgram *r, RamImage *im, uint32_t **rom_seeds, int *nrom, int *caprom) {
    uint32_t cap = im->len * 3 + 64, top = 0;
    uint16_t *work = (uint16_t *)malloc(sizeof(uint16_t) * cap);
    for (uint32_t off = 0; off < im->len; ++off)
        if (im->seed[off] && !im->seen[off] && top < cap) work[top++] = (uint16_t)(im->base + off);
    while (top) {
        uint16_t pc = work[--top];
        uint32_t off = (uint32_t)(pc - im->base);
        if (im->seen[off]) continue;
        im->seen[off] = 1;
        uint8_t opcode = im->bytes[off];
        int len = op_length(opcode);
        if (off + (uint32_t)len > im->len || !ram_insn_ok(r, pc, len)) continue;
        bool live, isolate;
        ram_judge(r, im, pc, len, &live, &isolate);
        im->is_insn[off] = 1;
        im->live[off] = live;
        im->isolate[off] = isolate;
        uint32_t succ[3];
        int kind[3];
        int n = ram_successors(p, im, pc, live, succ, kind);
        for (int i = 0; i < n; ++i) {
            uint32_t t = succ[i], toff = t - im->base;
            bool inside = t >= im->base && toff < im->len;
            if (kind[i] == 1 || !inside) ram_add_gseed(r, t);
            if (inside && !im->seen[toff] && (!im->evidence_only || im->seed[toff]) && top < cap) work[top++] = (uint16_t)t;
            if (!inside && t >= 0x8000) {
                uint32_t slot = (t >> SLOT_SHIFT) & (SLOT_COUNT - 1);
                if (p->fixed[slot] >= 0) {
                    if (*nrom == *caprom) {
                        *caprom = *caprom ? *caprom * 2 : 64;
                        *rom_seeds = (uint32_t *)realloc(*rom_seeds, sizeof(uint32_t) * (size_t)*caprom);
                    }
                    (*rom_seeds)[(*nrom)++] = POS_PACK((uint32_t)p->fixed[slot], slot, t & (SLOT_SIZE - 1));
                }
            }
        }
    }
    free(work);
}

static int32_t uf_find(int32_t *root, int32_t x) {
    while (root[x] != x) { root[x] = root[root[x]]; x = root[x]; }
    return x;
}

static uint32_t fnv_bytes(uint32_t h, const void *data, size_t n) {
    const uint8_t *q = (const uint8_t *)data;
    while (n--) h = (h ^ *q++) * 16777619u;
    return h;
}

/* A view's identity: its instructions, which of them are live, and the bytes
 * it folds. Two views equal in these generate the same code. */
static bool view_equal(const RamView *a, const RamView *b) {
    if (a->n != b->n || a->run_count != b->run_count || a->byte_count != b->byte_count) return false;
    if (memcmp(a->pcs, b->pcs, sizeof(uint16_t) * a->n)) return false;
    if (memcmp(a->runs, b->runs, sizeof(uint16_t) * 2 * a->run_count)) return false;
    if (memcmp(a->bytes, b->bytes, a->byte_count)) return false;
    for (uint32_t i = 0; i < a->n; ++i)
        if (a->img->live[a->pcs[i] - a->img->base] != b->img->live[b->pcs[i] - b->img->base]) return false;
    return true;
}

/* Split an image's instructions into views: the ones local control flow
 * connects inside one 1KB chunk (calls are not followed, so a routine keeps
 * its own validity), each folding exactly its instructions' bytes. */
static void ram_make_views(const Program *p, RamProgram *r, RamImage *im, uint32_t *hash_table, uint32_t table_size) {
    for (uint32_t off = 0; off < im->len; ++off) im->root[off] = (int32_t)off;
    for (uint32_t off = 0; off < im->len; ++off) {
        if (!im->is_insn[off]) continue;
        uint16_t pc = (uint16_t)(im->base + off);
        uint32_t succ[3];
        int kind[3];
        int n = ram_successors(p, im, pc, im->live[off] != 0, succ, kind);
        for (int i = 0; i < n; ++i) {
            uint32_t t = succ[i], toff = t - im->base;
            bool call = OPS[im->bytes[off]].kind == K_JSR && kind[i] == 1;
            if (call || t < im->base || toff >= im->len || !im->is_insn[toff] || (t >> CHUNK_SHIFT) != (pc >> CHUNK_SHIFT))
                continue;
            if (im->isolate[off] || im->isolate[toff]) continue;
            int32_t a = uf_find(im->root, (int32_t)off), b = uf_find(im->root, (int32_t)toff);
            if (a != b) im->root[a < b ? b : a] = a < b ? a : b;
        }
    }
    uint32_t *count = (uint32_t *)calloc(im->len, sizeof(uint32_t));
    for (uint32_t off = 0; off < im->len; ++off)
        if (im->is_insn[off]) count[uf_find(im->root, (int32_t)off)]++;
    for (uint32_t rootoff = 0; rootoff < im->len; ++rootoff) {
        if (!count[rootoff]) continue;
        RamView v;
        memset(&v, 0, sizeof(v));
        v.img = im;
        v.pcs = (uint16_t *)malloc(sizeof(uint16_t) * count[rootoff]);
        uint16_t chunk = (uint16_t)((im->base + rootoff) & ~((1u << CHUNK_SHIFT) - 1));
        uint8_t mark[(1u << CHUNK_SHIFT) + 4] = { 0 };
        for (uint32_t off = rootoff; off < im->len && v.n < count[rootoff]; ++off) {
            if (!im->is_insn[off] || (uint32_t)uf_find(im->root, (int32_t)off) != rootoff) continue;
            uint16_t pc = (uint16_t)(im->base + off);
            v.pcs[v.n++] = pc;
            int len = im->live[off] ? 1 : op_length(im->bytes[off]);
            for (int k = 0; k < len; ++k) mark[(uint32_t)(pc - chunk) + (uint32_t)k] = 1;
        }
        v.runs = (uint16_t *)malloc(sizeof(uint16_t) * 2 * ((1u << CHUNK_SHIFT) + 4));
        v.bytes = (uint8_t *)malloc((1u << CHUNK_SHIFT) + 4);
        for (uint32_t k = 0; k < (1u << CHUNK_SHIFT) + 4; ++k) {
            if (!mark[k]) continue;
            uint32_t start = k;
            while (k < (1u << CHUNK_SHIFT) + 4 && mark[k]) {
                v.bytes[v.byte_count++] = im->bytes[chunk + k - im->base];
                ++k;
            }
            v.runs[2 * v.run_count] = (uint16_t)(chunk + start);
            v.runs[2 * v.run_count + 1] = (uint16_t)(k - start);
            v.run_count++;
        }
        uint32_t h = 2166136261u;
        h = fnv_bytes(h, v.pcs, sizeof(uint16_t) * v.n);
        for (uint32_t i = 0; i < v.n; ++i) h = fnv_bytes(h, &im->live[v.pcs[i] - im->base], 1);
        h = fnv_bytes(h, v.runs, sizeof(uint16_t) * 2 * v.run_count);
        v.hash = fnv_bytes(h, v.bytes, v.byte_count);
        /* Keep the first of identical views (the same code in two files). */
        uint32_t slot = v.hash & (table_size - 1);
        bool dup = false;
        while (hash_table[slot]) {
            const RamView *o = &r->view[hash_table[slot] - 1];
            if (o->hash == v.hash && view_equal(o, &v)) { dup = true; break; }
            slot = (slot + 1) & (table_size - 1);
        }
        if (dup) { free(v.pcs); free(v.runs); free(v.bytes); continue; }
        if (r->nview == r->capview) {
            r->capview = r->capview ? r->capview * 2 : 256;
            r->view = (RamView *)realloc(r->view, sizeof(RamView) * (size_t)r->capview);
        }
        v.index = r->nview;
        r->view[r->nview++] = v;
        hash_table[slot] = (uint32_t)r->nview;
    }
    free(count);
}

/* Is the instruction the capture saw at pc (bytes b) what image im holds? */
static bool ram_image_holds(const RamProgram *r, const RamImage *im, uint16_t pc, const uint8_t *b, int len) {
    uint32_t off = (uint32_t)(pc - im->base);
    if (pc < im->base || off + (uint32_t)len > im->len) return false;
    if (im->bytes[off] != b[0]) return false;
    bool live, isolate;
    ram_judge(r, im, pc, len, &live, &isolate);
    return live || !memcmp(&im->bytes[off], b, (size_t)len);
}

/* Build every image, discover it, and cut it into views. ROM targets that RAM
 * code jumps to are appended to rom_seeds for the ROM's own discovery. */
static RamProgram *ram_build(const Program *p, const GameConfig *cfg, const CycFdsProgram *fds, uint32_t **rom_seeds,
                             int *nrom) {
    RamProgram *r = (RamProgram *)calloc(1, sizeof(RamProgram));
    r->fds = p->mapper == 20;
    int caprom = 0, disk_files = 0, cap_images = 0, cap_insns = -1, ngroups = 0;
    *rom_seeds = NULL;
    *nrom = 0;
    if (fds && fds->image_path[0]) disk_files = ram_add_fds_images(r, fds->image_path);
    int disk_images = r->nimg;
    RamCapGroup *groups = NULL;
    if (cfg->cycle_capture_file[0]) {
        cap_insns = ram_load_captures(r, cfg->cycle_capture_file, &groups, &ngroups);
        r->groups = groups;
        r->ngroups = ngroups;
        if (cap_insns < 0)
            fprintf(stderr, "[cyc] note: capture file %s not found (run the host with --capture-log to create it)\n",
                    cfg->cycle_capture_file);
    }
    for (uint32_t a = 0; a < 0x10000; ++a) {
        if (!r->nvar[a]) continue;
        r->var_on_disk[a] = (uint8_t *)calloc(r->nvar[a], 1);
        for (unsigned v = 0; v < r->nvar[a]; ++v)
            for (int k = 0; k < disk_images && !r->var_on_disk[a][v]; ++k) {
                const RamImage *im = &r->img[k];
                uint32_t off = a - im->base;
                if (a >= im->base && off + r->var[a][v].len <= im->len && !memcmp(&im->bytes[off], r->var[a][v].b, r->var[a][v].len))
                    r->var_on_disk[a][v] = 1;
            }
    }
    /* Every snapshot is an image of its own, compiled from what ran in it
     * only: the code may be in no disk file as it ran (copied, generated),
     * or be in one whose views were invalid when it ran (part of a file
     * overwritten, a routine beside rewritten code). Views identical to a
     * disk file's merge. */
    for (int g = 0; g < ngroups; ++g) {
        RamCapGroup *G = &groups[g];
        uint32_t end = G->base < 0x800 ? 0x800u : 0xE000u, len = end - G->base < 1026u ? end - G->base : 1026u;
        if (!ram_addr_ok(r, G->base)) continue;
        RamImage *im = ram_add_image(r, G->base, G->data, len, true, "capture $%04X #%d", G->base, g);
        for (uint32_t i = 0; i < G->n; ++i) im->seed[G->pcs[i] - G->base] = 1;
        cap_images++;
    }
    /* Entry points: the game's vectors ($DFF6/$DFF8/$DFFA NMI, as $0100
     * selects; $DFFC reset; $DFFE IRQ) in every image that holds them, the
     * seed file's RAM addresses, and each captured instruction in every disk
     * image that holds it as it ran. */
    for (int k = 0; k < r->nimg; ++k) {
        const RamImage *im = &r->img[k];
        for (uint32_t v = 0xDFF6; v <= 0xDFFE; v += 2)
            if (v >= im->base && v + 1 < (uint32_t)im->base + im->len)
                ram_add_gseed(r, (uint32_t)(im->bytes[v - im->base] | im->bytes[v + 1 - im->base] << 8));
    }
    if (cfg->cycle_seed_file[0]) {
        FILE *sf = fopen(cfg->cycle_seed_file, "r");
        char line[128];
        while (sf && fgets(line, sizeof(line), sf)) {
            unsigned addr;
            if (line[0] == '#' || strchr(line, ':') || sscanf(line, "%x", &addr) != 1) continue;
            if (addr < 0x8000) ram_add_gseed(r, addr);
        }
        if (sf) fclose(sf);
    }
    for (uint32_t a = 0; a < 0x10000; ++a)
        for (unsigned v = 0; v < r->nvar[a]; ++v)
            for (int k = 0; k < disk_images; ++k)
                if (ram_image_holds(r, &r->img[k], (uint16_t)a, r->var[a][v].b, r->var[a][v].len))
                    r->img[k].seed[a - r->img[k].base] = 1;
    /* Mod hook sites in RAM are entry points of every disk file that holds the
     * site's content key there, and of no other: a file loaded over it at the
     * same address is other code. A site no file holds could never fire. */
    for (int i = 0; s_hook_cfg && i < s_hook_cfg->mod_function_hook_count; ++i) {
        uint16_t a = s_hook_cfg->mod_function_hooks[i].addr;
        if (!hook_in_ram(p->mapper, a)) continue;
        const ModHookKey *key = hook_key(i);
        int held = 0;
        for (int k = 0; k < disk_images && key->len; ++k) {
            RamImage *im = &r->img[k];
            uint32_t off = (uint32_t)(a - im->base);
            if (a < im->base || off >= im->len || !key_holds(key, &im->bytes[off], im->len - off)) continue;
            im->seed[off] = 1;
            held++;
        }
        if (!key->len) {
            fprintf(stderr, "[cyc] [[mod_function_hook]] at RAM address $%04X needs bytes = \"..\": code in RAM is "
                            "keyed on its content\n", a);
            r->hook_errors++;
        } else if (!held && r->fds && a >= 0x6000) {
            fprintf(stderr, "[cyc] [[mod_function_hook]] %s at $%04X: no disk file holds its bytes there\n",
                    key->id[0] ? key->id : "(no id)", a);
            r->hook_errors++;
        } else if (!held) {
            /* Code a run copies there: the interpreter runs it (and the
             * scheduler fires the site) until a capture compiles it. */
            printf("[cyc] note: [[mod_function_hook]] %s at $%04X: no image holds its bytes yet\n",
                   key->id[0] ? key->id : "(no id)", a);
        }
    }
    /* Discover to a fixed point: a target leaving one image is a seed of
     * every image covering it (the code resident there may be any of them). */
    for (;;) {
        uint32_t before = r->ngseed;
        for (int k = 0; k < r->nimg; ++k) {
            RamImage *im = &r->img[k];
            for (; im->gseed_cursor < r->ngseed; ++im->gseed_cursor) {
                uint16_t a = r->gseed[im->gseed_cursor];
                if (!im->evidence_only && a >= im->base && (uint32_t)(a - im->base) < im->len) im->seed[a - im->base] = 1;
            }
            ram_discover(p, r, im, rom_seeds, nrom, &caprom);
        }
        if (r->ngseed == before) break;
    }
    uint32_t table_size = 1;
    uint32_t total = 0;
    for (int k = 0; k < r->nimg; ++k)
        for (uint32_t off = 0; off < r->img[k].len; ++off) total += r->img[k].is_insn[off];
    while (table_size < total * 2 + 16) table_size <<= 1;
    uint32_t *hash_table = (uint32_t *)calloc(table_size, sizeof(uint32_t));
    for (int k = 0; k < r->nimg; ++k) ram_make_views(p, r, &r->img[k], hash_table, table_size);
    free(hash_table);
    /* The folded-byte map, for the store checks. */
    for (int i = 0; i < r->nview; ++i) {
        const RamView *v = &r->view[i];
        for (uint32_t k = 0; k < v->run_count; ++k)
            for (uint32_t j = 0; j < v->runs[2 * k + 1]; ++j) {
                uint16_t a = (uint16_t)(v->runs[2 * k] + j);
                if (a < 0x800) for (uint32_t m = 0; m < 4; ++m) r->code[a | m << 11] = 1;
                else r->code[a] = 1;
            }
    }
    for (uint32_t a = 0; a < 0x10000; ++a) r->code_prefix[a + 1] = r->code_prefix[a] + r->code[a];
    uint32_t instructions = 0;
    for (int i = 0; i < r->nview; ++i) instructions += r->view[i].n;
    if (r->nimg || cap_insns >= 0)
        printf("[NESRecomp] RAM code: %d image%s (%d from %d disk file%s, %d captured snapshot%s), %u instructions "
               "discovered, %d views (%u instructions) after merging identical ones\n",
               r->nimg, r->nimg == 1 ? "" : "s", disk_images, disk_files < 0 ? 0 : disk_files, disk_files == 1 ? "" : "s",
               cap_images, cap_images == 1 ? "" : "s", total, r->nview, instructions);
    if (cap_insns >= 0)
        printf("[NESRecomp] capture file %s: %d instruction variants, %d snapshots\n", cfg->cycle_capture_file,
               cap_insns, ngroups);
    for (int g = 0; g < ngroups; ++g) free(groups[g].pcs);
    free(groups);
    r->groups = NULL;
    r->ngroups = 0;
    return r;
}

/* One translation unit per few thousand instructions: the view functions and
 * their descriptors (runner/cyc/cyc_recomp.h CycRamView). */
static int ram_emit(const Program *p, const RamProgram *r, const char *prefix) {
    int files = 0;
    FILE *f = NULL;
    uint32_t in_file = 0;
    char path[512];
    for (int i = 0; i < r->nview; ++i) {
        const RamView *v = &r->view[i];
        if (!f || in_file > 4000) {
            if (f) fclose(f);
            snprintf(path, sizeof(path), "generated/%s_cyc_r%02X.c", prefix, files++);
            f = open_out(path);
            fprintf(f,
                "/* %s - generated by NESRecomp --cycle-accurate. DO NOT EDIT.\n"
                " * Compiled views of code in RAM: each folds only its own instructions'\n"
                " * bytes (the runs below) and runs only while RAM holds them; see\n"
                " * runner/cyc/cyc_ramview.c and recompiler/src/cyc_codegen.c. */\n"
                "#include \"cyc_recomp.h\"\n\n", path);
            in_file = 0;
        }
        in_file += v->n;
        fprintf(f, "/* view %d: %s, $%04X-$%04X, %u instructions, %u bytes folded */\n", v->index, v->img->name,
                v->pcs[0], v->pcs[v->n - 1], v->n, v->byte_count);
        fprintf(f, "static const uint16_t E%d[%u] = {", v->index, v->n);
        for (uint32_t k = 0; k < v->n; ++k) fprintf(f, "%s0x%04X,", k % 12 ? "" : "\n    ", v->pcs[k]);
        fprintf(f, "\n};\nstatic const uint16_t R%d[%u] = {", v->index, 2 * v->run_count);
        for (uint32_t k = 0; k < v->run_count; ++k)
            fprintf(f, "%s0x%04X, %u,", k % 8 ? " " : "\n    ", v->runs[2 * k], v->runs[2 * k + 1]);
        fprintf(f, "\n};\nstatic const uint8_t B%d[%u] = {", v->index, v->byte_count);
        for (uint32_t k = 0; k < v->byte_count; ++k) fprintf(f, "%s0x%02X,", k % 16 ? "" : "\n    ", v->bytes[k]);
        fprintf(f, "\n};\nstatic void rv%d(void) {\n    switch (cpu.pc) {\n", v->index);
        for (uint32_t k = 0; k < v->n; ++k) fprintf(f, "    case 0x%04X: goto L_%04X;\n", v->pcs[k], v->pcs[k]);
        fprintf(f, "    default: return;\n    }\n");
        Emit e = {0};
        e.f = f;
        e.p = p;
        e.img = v->img;
        e.view = v;
        e.ram = r;
        for (uint32_t k = 0; k < v->n; ++k) {
            e.P = v->pcs[k];
            e.opcode = v->img->bytes[e.P - v->img->base];
            e.live = v->img->live[e.P - v->img->base] != 0;
            emit_instruction(&e);
        }
        fprintf(f, "}\nconst CycRamView cyc_rv%d = { rv%d, E%d, R%d, B%d, %u, %u, 0x%08Xu, 0x%08Xu };\n\n", v->index,
                v->index, v->index, v->index, v->index, v->n, v->run_count, v->hash, v->img->hash);
    }
    if (f) fclose(f);
    /* The views, for tools: which image each came from. */
    snprintf(path, sizeof(path), "generated/%s_cyc_views.txt", prefix);
    FILE *m = open_out(path);
    fprintf(m, "# index hash first last instructions folded-bytes live isolated image-hash image\n");
    for (int i = 0; i < r->nview; ++i) {
        const RamView *v = &r->view[i];
        unsigned live = 0, isolated = 0;
        for (uint32_t k = 0; k < v->n; ++k) {
            live += v->img->live[v->pcs[k] - v->img->base];
            isolated += v->img->isolate[v->pcs[k] - v->img->base];
        }
        fprintf(m, "%d %08X %04X %04X %u %u %u %u %08X %s\n", i, v->hash, v->pcs[0], v->pcs[v->n - 1], v->n,
                v->byte_count, live, isolated, v->img->hash, v->img->name);
    }
    fclose(m);
    return files;
}

static void ram_emit_table(FILE *f, const RamProgram *r) {
    int n = r ? r->nview : 0;
    for (int i = 0; i < n; ++i) fprintf(f, "extern const CycRamView cyc_rv%d;\n", i);
    fprintf(f, "\n/* Compiled views of RAM code (<prefix>_cyc_rNN.c); runner/cyc/cyc_ramview.c. */\n"
               "const CycRamView *const cyc_native_ram_views[%d] = {", n ? n : 1);
    if (!n) fprintf(f, " 0");
    for (int i = 0; i < n; ++i) fprintf(f, "%s&cyc_rv%d,", i % 8 ? " " : "\n    ", i);
    fprintf(f, "\n};\nconst uint32_t cyc_native_ram_view_count = %du;\n", n);
}

static void ram_free(RamProgram *r) {
    if (!r) return;
    for (int k = 0; k < r->nimg; ++k) {
        RamImage *im = &r->img[k];
        free(im->bytes); free(im->seed); free(im->seen); free(im->is_insn); free(im->live); free(im->isolate);
        free(im->root);
    }
    for (int i = 0; i < r->nview; ++i) { free(r->view[i].pcs); free(r->view[i].runs); free(r->view[i].bytes); }
    for (uint32_t a = 0; a < 0x10000; ++a) { free(r->var[a]); free(r->var_on_disk[a]); free(r->vol[a]); }
    free(r->img); free(r->view); free(r->gseed);
    free(r);
}

/* A seed line is `AAAA`, legacy 8 KiB `BB:AAAA`, or `4k:BB:AAAA`,
 * each with an optional count after it.
 * The bank-less form means the bank the power-on configuration has at that
 * address, which is every bank on NROM and what a hand-written seed means.
 * Returns POS_NONE for a line that is not a seed. */
static uint32_t parse_seed(const Program *p, const char *line) {
    unsigned bank, addr;
    if (line[0] == '#') return POS_NONE;
    if (sscanf(line, "4k:%x:%x", &bank, &addr) == 2) {
        if (addr < 0x8000 || addr > 0xFFFF || bank >= p->banks) return POS_NONE;
    } else if (sscanf(line, "%x:%x", &bank, &addr) == 2) {
        if (addr < 0x8000 || addr > 0xFFFF || bank >= (p->banks + 1)/2) return POS_NONE;
        bank = (bank * 2 + ((addr >> 12) & 1)) & (p->banks - 1);
    } else if (sscanf(line, "%x", &addr) == 1) {
        if (addr < 0x8000 || addr > 0xFFFF) return POS_NONE;
        if (p->power_on[(addr >> SLOT_SHIFT) & (SLOT_COUNT - 1)] < 0) return POS_NONE;
        bank = (unsigned)p->power_on[(addr >> SLOT_SHIFT) & (SLOT_COUNT - 1)];
    } else {
        return POS_NONE;
    }
    /* The FDS maps one bank per ROM slot and none in RAM: a seed anywhere else
     * would compile a view the dispatch can never select. */
    if (p->mapper == 20 && p->fixed[(addr >> SLOT_SHIFT) & (SLOT_COUNT - 1)] != (int)bank) return POS_NONE;
    return POS_PACK(bank, (addr >> SLOT_SHIFT) & (SLOT_COUNT - 1), addr & (SLOT_SIZE - 1));
}

/* The three vectors as the bank in slot 3 holds them, seeded at whatever
 * position each target resolves to. A mapper with a fixed $E000 slot has one
 * such bank; one that switches all of $8000-$FFFF (AxROM, GxROM) has no fixed
 * vectors at all, so every bank's are seeded - compiling a block that never
 * runs costs output, not correctness. */
static void seed_vectors(const Program *p, uint32_t bank, uint32_t *seeds, int *n) {
    Pos from = { bank, SLOT_COUNT - 1, SLOT_SIZE - 1 };
    for (uint32_t v = 0xFFFA; v <= 0xFFFE; v += 2) {
        uint32_t k = v & (SLOT_SIZE - 1);
        uint16_t target = (uint16_t)(prg_byte(p, bank, k) | prg_byte(p, bank, k + 1) << 8);
        Pos to;
        if (target_pos(p, &from, target, &to)) seeds[(*n)++] = POS_PACK(to.bank, to.slot, to.k);
    }
}

bool cyc_codegen_emit(const NESRom *rom, const GameConfig *cfg, const char *output_prefix,
                      const CycFdsProgram *fds) {
    if (!nes_cart_variant_supported(&rom->cart)) {
        fprintf(stderr, "[cyc] unsupported cartridge metadata for mapper %d submapper %u\n",
                rom->mapper, rom->cart.submapper);
        return false;
    }
    const char *board = mapper_name(rom->mapper);
    if (!board) {
        fprintf(stderr, "[cyc] unsupported mapper %d; see runner/cyc/MAPPERS.md\n", rom->mapper);
        return false;
    }
    if (!check_table()) return false;
    if (cfg->console > 2) {
        fprintf(stderr, "[cyc] game.toml [game] console must be nes, famicom or default\n");
        return false;
    }

    Program *p = (Program *)calloc(1, sizeof(Program));
    p->rom = rom;
    p->mapper = rom->mapper;
    p->prg_len = rom->cart.prg_size;
    /* Banks are counted in the padded image, so that a bank number the
     * hardware wraps lands on the same byte here as in hw_mapper.c. */
    p->banks = 1;
    while (p->banks * SLOT_SIZE < p->prg_len) p->banks <<= 1;
    /* NROM and the FDS have no bank registers: no write can move their ROM. */
    p->banked = rom->mapper != 0 && rom->mapper != 20;
    bool any_fixed = false;
    for (uint32_t s = 0; s < SLOT_COUNT; s++) {
        p->fixed[s] = fixed_bank_for(rom->mapper, p->banks, s);
        p->power_on[s] = power_on_bank_for(rom->mapper, p->banks, s);
        if ((rom->mapper == 206 && rom->cart.submapper == 1) ||
            (rom->mapper == 1 && rom->cart.submapper == 5))
            p->fixed[s] = p->power_on[s] = s & (p->banks - 1);
        if (p->fixed[s] >= 0) any_fixed = true;
    }
    for (int i = 0; i < cfg->cycle_inline_jsr_count; i++)
        p->inline_jsr[cfg->cycle_inline_jsr[i].target] = cfg->cycle_inline_jsr[i].bytes;
    if (fds && fds->bios_crc32 == 0x5E607DCFu)
        for (size_t i = 0; i < sizeof(FDS_BIOS_INLINE_JSR) / sizeof(FDS_BIOS_INLINE_JSR[0]); i++)
            p->inline_jsr[FDS_BIOS_INLINE_JSR[i].target] = FDS_BIOS_INLINE_JSR[i].bytes;
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
        seed_vectors(p, (uint32_t)(p->fixed[SLOT_COUNT - 1] >= 0 ? p->fixed[SLOT_COUNT - 1] : p->power_on[SLOT_COUNT - 1]), seeds, &n);
    } else {
        for (uint32_t bank = 0; bank < p->banks; bank++) seed_vectors(p, bank, seeds, &n);
    }
    for (int i = 0; i < cfg->extra_func_count; i++) {
        uint16_t addr = cfg->extra_funcs[i].addr;
        if (addr < 0x8000) continue;
        uint32_t slot = (addr >> SLOT_SHIFT) & (SLOT_COUNT - 1);
        if (p->power_on[slot] < 0) continue;
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
    /* Mod hook sites in ROM are entry points: in the bank a fixed slot holds,
     * or in every bank of a switchable slot that holds the site's content key
     * there (which such a site must declare: the address alone names a
     * different routine in each bank). */
    s_hook_cfg = cfg;
    int hook_errors = 0;
    for (int i = 0; i < cfg->mod_function_hook_count; i++) {
        uint16_t addr = cfg->mod_function_hooks[i].addr;
        if (hook_in_ram(p->mapper, addr)) continue;
        const ModHookKey *key = hook_key(i);
        uint32_t slot = (addr >> SLOT_SHIFT) & (SLOT_COUNT - 1), k = addr & (SLOT_SIZE - 1);
        if (p->fixed[slot] < 0 && !key->len) {
            fprintf(stderr, "[cyc] [[mod_function_hook]] at $%04X is in a switchable PRG slot: it needs bytes = \"..\"\n",
                    addr);
            hook_errors++;
            continue;
        }
        int held = 0;
        for (uint32_t bank = 0; bank < p->banks; bank++) {
            if (p->fixed[slot] >= 0 && (int)bank != p->fixed[slot]) continue;
            if (!key_holds_rom(key, p, bank, k)) continue;
            if ((size_t)n + 1 < seed_cap) seeds[n++] = POS_PACK(bank, slot, k);
            held++;
        }
        if (!held) {
            fprintf(stderr, "[cyc] [[mod_function_hook]] %s at $%04X: no PRG bank holds its bytes there\n",
                    key->id[0] ? key->id : "(no id)", addr);
            hook_errors++;
        }
    }
    discover(p, seeds, n);
    free(seeds);
    if (file_seeds) printf("[NESRecomp] cycle-accurate: %d seeds from %s\n", file_seeds, cfg->cycle_seed_file);

    /* Code in RAM (disk files, captured snapshots), and the ROM routines it
     * calls, which the ROM's own control flow may never reach. */
    uint32_t *rom_seeds;
    int rom_seed_count;
    RamProgram *ram = ram_build(p, cfg, fds, &rom_seeds, &rom_seed_count);
    if (rom_seed_count) discover(p, rom_seeds, rom_seed_count);
    free(rom_seeds);
    hook_errors += ram->hook_errors;
    if (hook_errors) {
        fprintf(stderr, "[cyc] %d [[mod_function_hook]] site%s cannot fire; fix game.toml\n", hook_errors,
                hook_errors == 1 ? "" : "s");
        ram_free(ram);
        free(p->is_insn);
        free(p->seen);
        free(p);
        return false;
    }

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
    int ram_files = ram_emit(p, ram, output_prefix);
    snprintf(path, sizeof(path), "generated/%s_cyc.c", output_prefix);
    emit_umbrella(p, path, output_prefix, fds, ram, cfg->console, cfg->display_name);

    printf("[NESRecomp] cycle-accurate (%s, %u KB PRG in %u banks of 4KB): %u instructions compiled",
           board, p->prg_len / 1024, p->banks, count);
    if (tails) printf(", %u left to the interpreter (they cross a bank or $FFFF)", tails);
    printf(" -> generated/%s_cyc.c + %d bank file%s", output_prefix, files, files == 1 ? "" : "s");
    if (ram_files) printf(" + %d RAM view file%s", ram_files, ram_files == 1 ? "" : "s");
    printf("\n");
    ram_free(ram);
    free(p->is_insn);
    free(p->seen);
    free(p);
    return true;
}
