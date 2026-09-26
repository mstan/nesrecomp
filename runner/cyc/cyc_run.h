/*
 * cyc_run.h - frame scheduling between recompiled code and the interpreter.
 *
 * Whenever the CPU is between instructions at an address the recompiler
 * compiled, the native code runs; everything else (RAM code, open bus,
 * addresses discovery missed) runs on the interpreter generated from the same
 * templates (cpu6502_interp.c).
 *
 * A frame ends at the first instruction boundary at or after the PPU reports
 * the frame's VBlank. Recompiled code can only yield at instruction
 * boundaries, so native, --interp-only and oracle runs all use that rule and
 * stay comparable cycle for cycle.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool     cyc_run_native;         /* false: interpreter only (default true) */
extern uint64_t cyc_run_native_cycles;  /* CPU cycles performed by recompiled code */
/* Optional: ROM instruction starts that ran interpreted, counted per
 * (PRG bank, slot) because that pair, not the CPU address, is what the
 * recompiler compiles. Indexed cyc_run_miss_index(); the host allocates
 * cyc_run_miss_slots() entries and writes them out as `BB:AAAA` seeds. */
extern uint32_t *cyc_run_miss;
extern uint32_t *cyc_run_ram_miss;      /* optional [0x2000]: RAM instruction starts (always interpreted) */
size_t   cyc_run_miss_slots(void);
unsigned cyc_run_miss_index(uint16_t pc);
/* The bank and CPU address a miss index stands for. */
void     cyc_run_miss_decode(unsigned index, unsigned *bank, uint16_t *addr);
/* Optional [0x800][32]: for each RAM address, a bitset of the opcode bytes
 * that ever began an instruction there. An address with one bit is code whose
 * opcodes are stable and only operands change; several bits mean the byte is
 * an opcode on one pass and something else on another. */
extern uint8_t  *cyc_run_ram_opcodes;

/* Interpreted cycles split by where the instruction began. ROM cycles are
 * addresses discovery missed: seed them (cycle_seed_file) and they compile.
 * RAM cycles are code the program writes at run time, which is not in the ROM
 * image and so cannot be compiled ahead of time at all. */
extern uint64_t cyc_run_interp_rom_cycles;
extern uint64_t cyc_run_interp_ram_cycles;
extern uint64_t cyc_run_interp_other_cycles;   /* $2000-$7FFF: open bus, PRG RAM */

/* CPU power-on (after cyc_power_on). */
void cyc_run_power_on(void);
/* Run until the end of the next frame. */
void cyc_run_frame(void);

#ifdef __cplusplus
}
#endif
