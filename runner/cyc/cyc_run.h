/*
 * cyc_run.h - frame scheduling between recompiled code and the interpreter.
 *
 * Whenever the CPU is between instructions at an address the recompiler
 * compiled, the native code runs; everything else (RAM code, open bus,
 * addresses discovery missed) runs on the interpreter generated from the same
 * templates (cpu6502_interp.c).
 * A modified cartridge is different: its ROM instructions require a checked
 * native view. Missing coverage is reported before any interpreter fallback.
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
 * cyc_run_miss_slots() entries and writes them out as `4k:BB:AAAA` seeds. */
extern uint32_t *cyc_run_miss;
/* Optional [0x10000]: instruction starts in RAM (always interpreted): CPU RAM
 * $0000-$1FFF, and cartridge RAM from $6000 (the FDS PRG RAM). */
extern uint32_t *cyc_run_ram_miss;
size_t   cyc_run_miss_slots(void);
unsigned cyc_run_miss_index(uint16_t pc);
/* The bank and CPU address a miss index stands for. */
void     cyc_run_miss_decode(unsigned index, unsigned *bank, uint16_t *addr);
/* Optional [0x10000][32]: for each RAM address, a bitset of the opcode bytes
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
extern uint64_t cyc_run_interp_prg_ram_cycles; /* $6000 up, not ROM the compiler folds: cartridge
                                                  RAM (the FDS PRG RAM), or ROM below $8000 */
extern uint64_t cyc_run_interp_other_cycles;   /* $2000-$5FFF: open bus, registers */

/* CPU power-on (after cyc_power_on). */
void cyc_run_power_on(void);
/* Run until the end of the next frame. */
void cyc_run_frame(void);
/* Called at the first instruction boundary after hw_observe_line's dot 0
 * (hw.h), in the middle of cyc_run_frame. */
extern void (*cyc_run_observer)(void);
/* A mod's isolated routine call (cyc_mod.c): dispatch from cpu.pc until the
 * routine returns to stop_pc with S back at entry_s. 0, or CYC_MOD_FAIL_*
 * (cyc_ring.h). */
int cyc_run_isolated(uint16_t stop_pc, uint8_t entry_s);

#ifdef __cplusplus
}
#endif
