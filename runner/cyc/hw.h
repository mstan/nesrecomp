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

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HW_IDLE,   /* no bus access (the cycle before the power-on reset sequence) */
    HW_READ,   /* R/W high; a pending DMA takes the CPU's cycles first */
    HW_WRITE,  /* R/W low; DMAs wait for the next read */
} HwCycleKind;

void    hw_cycle_start(uint16_t addr, HwCycleKind kind);
uint8_t hw_read(uint16_t addr);
/* A read of a byte the recompiler folded to a constant (NROM PRG ROM): the
 * same bus effects as hw_read() on $8000-$FFFF, without decoding. */
uint8_t hw_read_rom(uint16_t addr, uint8_t value);
void    hw_write(uint16_t addr, uint8_t value);
void    hw_cycle_finish(bool instruction_done);

/* CPU cycles DMAs took inside the most recent hw_cycle_start(). */
extern int  hw_dma_stalls;
/* The IRQ input (level-sensitive), as last sampled by the CPU. */
bool hw_irq_line(void);
/* Provided by the CPU (cpu6502.c) and called by the hardware once per CPU
 * cycle, DMA cycles included, at the point where the 6502 samples its NMI
 * input: asserted = /NMI is low. The CPU's edge detector does the rest. */
void cpu_nmi_input(bool asserted);
/* Set by the PPU when it enters VBlank; the scheduler ends a frame at the
 * next instruction boundary and clears it. */
extern bool hw_frame_done;

/* Which 8KB PRG bank the cartridge has at addr ($8000-$FFFF). Recompiled code
 * is generated per (bank, CPU address) pair, because what the bytes at an
 * address are - and so what a block compiled from them does - depends on the
 * bank; this is what its dispatch looks up at each entry. */
unsigned hw_prg_bank(uint16_t addr);

#ifdef __cplusplus
}
#endif
