/*
 * hw.h - what the 6502 sees of the rest of the NES, one CPU cycle at a time.
 *
 * NESRecomp's cycle-accurate CPU (cpu6502.h) and the code NESRecomp
 * --cycle-accurate generates talk to the machine only through this interface.
 * It is modeled on the CPU's pins rather than on any emulator's internals:
 * every CPU cycle puts an address and a read/write level on the bus, performs
 * one access, and reports whether the instruction ended. DMAs (the 2A03's
 * sprite and sample transfers) pause the CPU through RDY, which the 6502
 * honors only on read cycles.
 *
 * A CPU cycle is always, in order:
 *
 *   hw_cycle_start(addr, kind)   advance the master clock to the CPU's tick;
 *                                on a read, DMA cycles run first
 *   hw_read / hw_read_rom / hw_write   the access (none for HW_IDLE)
 *   hw_cycle_finish(done)        the rest of the tick; done = the CPU
 *                                completed an instruction this cycle
 *
 * Interrupt polls, when the instruction has one, go between start and the
 * access. The implementation (hw_machine.c, hw_ppu.c, hw_apu.c) is
 * single-instance.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Generated title projects qualify their immutable board once at build time.
 * Standalone multi-board hosts keep runtime qualification. */
#ifndef NESRECOMP_NATIVE_INTERVALS
#define NESRECOMP_NATIVE_INTERVALS 1
#endif
#ifndef NESRECOMP_NATIVE_PRG_GUARDS
#define NESRECOMP_NATIVE_PRG_GUARDS 0
#endif
#ifndef NESRECOMP_ISOLATED_NATIVE
#define NESRECOMP_ISOLATED_NATIVE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HW_IDLE,   /* no bus access (the cycle before the power-on reset sequence) */
    HW_READ,   /* R/W high; a pending DMA takes the CPU's cycles first */
    HW_WRITE,  /* R/W low; DMAs wait for the next read */
    HW_FETCH,  /* opcode read; PAL DMA waits for this instruction boundary */
} HwCycleKind;

void    hw_cycle_start(uint16_t addr, HwCycleKind kind);
uint8_t hw_read(uint16_t addr);
/* A read of a byte the recompiler folded to a constant (NROM PRG ROM): the
 * same bus effects as hw_read() on $8000-$FFFF, without decoding. */
uint8_t hw_read_rom(uint16_t addr, uint8_t value);
/* A validated RAM instruction byte, with ordinary cycle/bus publication but
 * no ROM substitution or cartridge ROM-read observer. */
uint8_t hw_read_code_ram(uint16_t addr, uint8_t value);
void    hw_write(uint16_t addr, uint8_t value);
void    hw_cycle_finish(bool instruction_done);

/* CPU cycles DMAs took inside the most recent bus-cycle start. */
extern int hw_dma_stalls;

/* A derived cursor scoped to one compiled ROM dispatch or qualified SRAM
 * view. Ordinary RAM and folded code accesses consume credit only after a
 * stable, unobserved interval. All device/general accesses flush first.
 * Qualified NESRECOMP_ISOLATED_NATIVE builds also use budget credit for
 * memory-only guest calls, without advancing authoritative device time.
 * This cursor is never serialized; dispatch closes it before host callbacks. */
typedef struct {
    uint8_t *ram;
    unsigned credit, pending;
    uint16_t addr;
    uint8_t value, kind, driven;
    bool enabled;
#if NESRECOMP_NATIVE_PRG_GUARDS
    const uint8_t *prg[8];
    bool checked_prg;
#endif
} HwNativeInterval;
extern HwNativeInterval hw_native_interval;
extern uint8_t hw_code_watch[];
extern uint64_t hw_native_interval_cycles, hw_native_interval_spans;
#if NESRECOMP_ISOLATED_NATIVE
extern uint64_t hw_native_isolated_cycles, hw_native_isolated_spans;
#endif
extern uint64_t ppu_native_sprite_spans, ppu_native_sprite_dots;
extern uint64_t ppu_native_raster_tiles;
extern uint64_t apu_native_event_spans, apu_native_event_cycles;
void hw_native_begin(void);
#if NESRECOMP_NATIVE_PRG_GUARDS
void hw_native_begin_checked_rom(void);
/* Atomic image replacement is observable. A borrowed writable pointer stays
 * untracked until another image is loaded; its chunks may never be cached. */
extern uint64_t hw_prg_guard_epoch;
void hw_prg_guard_write(bool untracked);
void hw_prg_guard_load(void);
const uint8_t *hw_prg_guard_image(size_t *size);
#endif
void hw_native_end(void);
void hw_native_flush(void);
bool hw_native_prepare(void);

static inline bool hw_native_reserve(void)
{
#if NESRECOMP_NATIVE_INTERVALS
    if (!hw_native_interval.enabled) return false;
    if (!hw_native_interval.credit && !hw_native_prepare()) return false;
    --hw_native_interval.credit;
    ++hw_native_interval.pending;
    hw_dma_stalls = 0;
    return true;
#else
    return false;
#endif
}

static inline void hw_native_bus(uint16_t addr, uint8_t value, HwCycleKind kind)
{
    hw_native_interval.addr = addr;
    hw_native_interval.value = value;
    hw_native_interval.kind = (uint8_t)kind;
    if (kind != HW_WRITE) hw_native_interval.driven = 1;
}

/* The IRQ input (level-sensitive), as last sampled by the CPU. */
bool hw_irq_line(void);
/* Provided by the CPU (cpu6502.c) and called by the hardware once per CPU
 * cycle, DMA cycles included, at the point where the 6502 samples its NMI
 * input: asserted = /NMI is low. The CPU's edge detector does the rest. */
void cpu_nmi_input(bool asserted);
/* Set by the PPU when it enters VBlank; the scheduler ends a frame at the
 * next instruction boundary and clears it. */
extern bool hw_frame_done;
/* An observation point inside the frame: when hw_observe_line >= 0 the PPU
 * also stops the scheduler at dot 0 of that scanline (setting hw_frame_done
 * and hw_observe_hit), so a host can look at the machine at the first
 * instruction boundary after it and then let the frame continue. Line 240 is
 * where Mesen ends a frame (libretro/Mesen PPU.cpp Exec, Console.cpp
 * RunSingleFrame), one scanline before VBlank. It changes nothing the machine
 * does. */
extern int  hw_observe_line;
extern bool hw_observe_hit;
/* Set with hw_frame_done when the PPU enters VBlank (the frame's real end),
 * so a stop that was both an observation point and the frame end is not
 * mistaken for an observation alone. */
extern bool hw_frame_end_hit;

/* Legacy 8 KiB identity, retained for older generated programs. It cannot
 * distinguish independently mapped 4 KiB halves; new dispatch uses bank4. */
unsigned hw_prg_bank(uint16_t addr);
/* Exact 4 KiB identity used by current generated dispatch and miss logs. */
unsigned hw_prg_bank4(uint16_t addr);
/* PRG bytes cannot change from PPU activity until the CPU writes a register. */
bool hw_prg_is_stable(void);
bool hw_prg_is_rom(uint16_t addr);
/* Trusted content tools edited PRG; ordinary native ROM dispatch is unsafe.
 * Qualified guards can still admit exact original instruction bytes. */
extern bool hw_prg_modified;
extern bool hw_prg_data_modified;

#ifdef __cplusplus
}
#endif
