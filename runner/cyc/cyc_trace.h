/*
 * cyc_trace.h - observable per-cycle trace for comparing two NES models.
 *
 * Records only what the hardware makes visible, so that two independent
 * implementations (NESRecomp's CPU and hardware, and the TriCNES oracle) can
 * be compared without sharing internal state:
 *
 *   - each CPU cycle: the bus access (address, value, read or write), the
 *     data bus left behind, and whether the cycle completed an instruction;
 *   - each DMA cycle: the DMA's own access, or only the fact of a halt (the
 *     address a halted CPU holds is not compared, see cyc_trace.c);
 *   - each instruction start: PC and the registers.
 *
 * Records fold into cyc_trace_hash; hosts write it once per frame, and with
 * cyc_trace_file set every record is also printed.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool     cyc_trace_enabled;
extern uint64_t cyc_trace_hash;
extern void    *cyc_trace_file; /* FILE *, or NULL */

void cyc_trace_instruction(uint16_t pc, uint8_t a, uint8_t x, uint8_t y, uint8_t s, uint8_t p);
/* A bus access. Only the first access of a cycle is recorded. */
void cyc_trace_access(uint16_t addr, uint8_t value, bool write);
/* The current cycle belongs to a DMA; halt = the DMA only held the CPU. */
void cyc_trace_dma(bool halt);
/* End of a CPU or DMA cycle. */
void cyc_trace_cycle_end(bool instruction_done, uint8_t data_bus);

/* Optional observer of every completed cycle while tracing is enabled, for
 * co-simulation harnesses (tools/cyc/mister_apu). */
typedef struct {
    uint32_t index;      /* cycles since power-on (as in --trace-out) */
    uint16_t addr;       /* the access; for a halted DMA cycle, the address repeated */
    uint8_t  value;
    uint8_t  data_bus;   /* left on the bus */
    char     kind;       /* 'R', 'W', or '-' for no access */
    bool     dma, halt, done;
} CycTraceCycle;
extern void (*cyc_trace_cycle_hook)(const CycTraceCycle *cycle);

uint64_t cyc_trace_mix(uint64_t h, uint64_t v);

/* The CPU cycle number the trace is recording, counted the same way by every
 * implementation that writes a trace, so it is the one clock two of them can
 * be compared on. Each machine's own cycle counter is incremented at its own
 * point in a cycle and two are not directly comparable; this is. It only
 * advances while cyc_trace_enabled, so debug output that uses it needs a
 * --hash-out or --trace-out run. */
extern uint32_t cyc_trace_cycle;

/* The memory and picture hash both machines report (cyc_mem_state_hash).
 * chr_ram is NULL for CHR ROM and wram for a board without work RAM; palette
 * entries are compared on their 6 bits. Only memories a program can observe
 * belong here: the mapper's registers are internal state and go in
 * cyc_hw_state_hash, which is compared between runs of one implementation. */
uint64_t cyc_mem_hash(uint64_t cycles, const uint8_t *ram, const uint8_t *ciram, size_t ciram_len, const uint8_t *oam,
                      const uint8_t *palette, const uint8_t *chr_ram, size_t chr_ram_len,
                      const uint8_t *wram, size_t wram_len, const uint16_t *frame_index);
void cyc_mem_dump(void *file, uint64_t cycles, const uint8_t *ram, const uint8_t *ciram, size_t ciram_len, const uint8_t *oam,
                  const uint8_t *palette, const uint8_t *chr_ram, size_t chr_ram_len, const uint8_t *wram,
                  size_t wram_len, const uint16_t *frame_index);

#ifdef __cplusplus
}
#endif
