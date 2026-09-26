/*
 * cyc_recomp.h - included by NESRecomp --cycle-accurate output
 * (<prefix>_cyc.c and its per-bank <prefix>_cyc_bNN.c files).
 *
 * Generated code performs each instruction's CPU cycles through cpu6502.h;
 * see recompiler/src/cyc_codegen.c.
 */
#pragma once
#include "cpu6502.h"

/* One compiled view of the cartridge: the instructions of one 8KB PRG bank as
 * mapped in one of the CPU's four 8KB slots. A block folds the ROM bytes at
 * its address to constants, so it is only valid while that bank is the one
 * the mapper has there - which is what the generated dispatch checks, using
 * hw_prg_bank(). NROM has one fixed bank per slot and never leaves it. */
typedef struct {
    const uint8_t *bits;            /* 0x2000 bits: which offsets start an instruction */
    void (*const *chunks)(void);    /* one function per 1KB, 0 where nothing is compiled */
} CycNativeView;

/* Provided by the generated umbrella file. */
extern const char    *cyc_native_program_name;
extern const uint32_t cyc_native_prg_hash;
extern const uint32_t cyc_native_cart_hash;
bool cyc_native_has(uint16_t addr);
void cyc_native_run(void);
