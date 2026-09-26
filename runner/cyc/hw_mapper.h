/*
 * hw_mapper.h - the cartridge: PRG/CHR address translation, the mapper's
 * registers, the nametable arrangement it selects, and its /IRQ output.
 *
 * The CPU and PPU sides of the machine reach the cartridge only through this
 * header. Everything a mapper can do is expressed as four things:
 *
 *   - where $8000-$FFFF and $6000-$7FFF read from, as an offset per 4KB slot
 *     (hw_cart.prg_off) and the work RAM;
 *   - where the PPU's $0000-$1FFF reads from, as an offset per 1KB page
 *     (hw_cart.chr_off);
 *   - how it drives CIRAM A10, which is the nametable arrangement;
 *   - whether it asserts /IRQ.
 *
 * 4KB PRG slots and 1KB CHR pages are the finest granularity any supported
 * mapper switches, so every mapper is a rule for filling those two tables.
 * The tables are what makes reads a single indexed load on the hot path and
 * what the recompiler dispatches on (a compiled block is valid for one PRG
 * offset at one CPU address), so mappers only ever write them through
 * hw_cart_map_prg8/hw_cart_map_chr1 below.
 *
 * MAPPERS.md lists supported IDs, board variants, references and validation.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How the cartridge drives CIRAM A10 (pin 22 of the 72-pin connector): the
 * nametable arrangement. Four-screen boards leave CIRAM deselected for the
 * upper half and supply their own RAM; the machine stores those extra tables
 * alongside CIRAM when the header selects four-screen wiring. */
typedef enum {
    HW_MIRROR_HORIZONTAL,  /* CIRAM A10 = PPU A11 ("vertical arrangement") */
    HW_MIRROR_VERTICAL,    /* CIRAM A10 = PPU A10 */
    HW_MIRROR_SCREEN_A,    /* one screen: CIRAM A10 low */
    HW_MIRROR_SCREEN_B,    /* one screen: CIRAM A10 high */
} HwMirroring;

/* The mapper numbers hw_mapper.c implements, in the order the table lists
 * them; hw_cart_supports() is what cyc_load_ines() and the recompiler check. */
bool hw_cart_supports(int mapper);
/* A human-readable board name for the run summary ("NROM", "MMC3", ...). */
const char *hw_cart_mapper_name(int mapper);

/* Power-on: the bank configuration a cold console comes up in, and the
 * mapper's registers cleared. Called by cyc_power_on(). */
void hw_cart_power_on(void);

/* A CPU write to $4020-$FFFF: the mapper's registers and its work RAM. */
void hw_cart_cpu_write(uint16_t addr, uint8_t value);
/* A CPU read of $4020-$7FFF (work RAM and mappers that answer there).
 * Returns true and sets *value when the cartridge drives the bus; $8000-$FFFF
 * is read inline through hw_cart.prg_off. */
bool hw_cart_cpu_read(uint16_t addr, uint8_t *value);

/* Every address the PPU puts on its bus: MMC3's IRQ counter clocks on
 * filtered rising edges of A12. Call hw_cart_ppu_addr(), which is free when the loaded
 * mapper does not care. */
void hw_cart_ppu_addr_watched(uint16_t vbus);
/* A real pattern read, including read-triggered bank latches. Debugger peeks
 * use hw_cart_chr_index directly and must not change cartridge state. */
uint8_t hw_cart_chr_read(uint16_t addr);
/* Finish any read-triggered latch when the PPU releases /RD. */
void hw_cart_ppu_rd(bool reading);

/* The mapper's /IRQ output, ORed into the CPU's IRQ input with the 2A03's
 * own frame and DMC interrupts. */
bool hw_cart_irq(void);

/* Comparison (cyc_hw_state_hash / cyc_hw_state_dump). */
uint64_t hw_cart_state_hash(uint64_t h);
void     hw_cart_state_dump(void *file);

#ifdef __cplusplus
}
#endif
