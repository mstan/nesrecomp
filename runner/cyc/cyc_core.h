/*
 * cyc_core.h - host interface to the cycle-accurate NES machine.
 *
 * Two implementations provide it:
 *   - NESRecomp's machine: its CPU (cpu6502.h: recompiled code plus the
 *     generated interpreter) and hardware (hw_machine.c, hw_ppu.c,
 *     hw_apu.c) behind hw.h;
 *   - the TriCNES oracle (tric_core.cpp), used only to check the first.
 * Hosts use only what is declared here, so the same host code drives both.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- machine control ---- */

/* Load an iNES/NES 2.0 image. Returns false on unsupported input; see MAPPERS.md. */
bool cyc_load_ines(const uint8_t *image, size_t size);
/* What CPU RAM holds at power-on. The console leaves no defined state; the
 * default is the pattern AccuracyCoin's power-on page reports from the
 * reference console. Zeros/ones match what other emulators power up with, so
 * a program that reads uninitialized RAM can be compared against them. */
typedef enum { CYC_RAM_PATTERN, CYC_RAM_ZEROS, CYC_RAM_ONES } CycRamInit;
extern CycRamInit cyc_ram_init;

/* Hardware power-on state. ppu_alignment selects the CPU/PPU master clock
 * phase (0-3). The host also powers on the CPU (cyc_run_power_on). */
void cyc_power_on(uint8_t ppu_alignment);
/* FNV-1a 32 of the loaded PRG ROM (matches cyc_native_prg_hash). */
uint32_t cyc_prg_hash(void);
/* Which hardware implementation this is: "nesrecomp" or "tricnes". */
const char *cyc_hw_name(void);

/* Committed nonvolatile bytes, preserved by cyc_power_on. Region 0 is the
 * cartridge; region 1 is Datach's shared 256-byte internal EEPROM.
 * Import only between load/power-on and CPU execution. Length must match. */
/* Datach EAN-8, UPC-A or EAN-13, including a valid check digit. The host
 * chooses swipe speed in CPU cycles per module (1000 is a useful default). */
bool cyc_scan_barcode(const char *digits, unsigned cycles_per_module);
size_t cyc_nvram_size(unsigned region);
bool cyc_nvram_export(unsigned region, void *buffer, size_t size);
bool cyc_nvram_import(unsigned region, const void *buffer, size_t size);

/* ---- host I/O ---- */

/* The 2KB of CPU RAM. */
const uint8_t *cyc_cpu_ram(void);
/* Buttons for controller port 0 or 1 (A B Select Start Up Down Left Right,
 * MSB first), latched by the console when it strobes the port. */
void cyc_set_controller(int port, uint8_t buttons);
/* CPU cycles since power-on, including cycles taken by DMAs. */
uint64_t cyc_cycle_count(void);
/* The picture as 256x240 ARGB8888, and as 9-bit color indices
 * (color | emphasis << 6), as far as the PPU has drawn it. */
const uint32_t *cyc_frame_argb(void);
const uint16_t *cyc_frame_index(void);
/* Audio: signed 16-bit mono at the given rate. cyc_audio_enable returns false
 * if the machine has no audio output. */
bool   cyc_audio_enable(int sample_rate);
size_t cyc_audio_read(int16_t *out, size_t max);

/* ---- comparison ---- */

/* Architectural CPU state at an instruction boundary. */
typedef struct {
    uint16_t pc;
    uint8_t  a, x, y, s, p;          /* p without B and bit 5 */
    uint8_t  do_nmi, do_irq;         /* interrupts the next opcode fetch takes */
} CycCpuState;

/* Implemented by the runtime (cyc_run.c) and by the oracle. */
void cyc_cpu_state(CycCpuState *out);

/* Hash of CPU RAM, CIRAM, OAM, palette RAM (6 bits), CHR RAM, the picture's
 * color indices and the cycle count: comparable between any two NES models
 * (cyc_mem_hash in cyc_trace.c). */
uint64_t cyc_mem_state_hash(void);
/* Hash of the hardware internals; comparable only between runs of the same
 * implementation (cyc_hw_name). */
uint64_t cyc_hw_state_hash(void);
/* The hashed hardware fields by name, one per line, to a FILE *. */
void cyc_hw_state_dump(void *file);
/* What cyc_mem_state_hash covers, as hex lines, to a FILE * (cyc_mem_dump in
 * cyc_trace.c gives both machines the same layout). */
void cyc_mem_state_dump(void *file);

/* ---- oracle only ---- */
void cyc_oracle_run_frame(void);

#ifdef __cplusplus
}
#endif
