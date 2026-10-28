/*
 * hw_mapper.c - the mapper chips, as rules for filling the PRG and CHR bank
 * tables described in hw_mapper.h.
 *
 * Mapper behavior, unlike the 2A03 and 2C02 timing in hw_apu.c and hw_ppu.c,
 * is documented: these chips are small synchronous logic whose registers and
 * bank arithmetic the nesdev wiki describes completely. Each section below
 * says which document it follows. The one part that is a timing question
 * rather than a lookup is MMC3's IRQ counter, which clocks off the PPU's A12
 * line; see mmc3_ppu_addr().
 *
 * Only one cartridge exists at a time, like the rest of the machine.
 */
#include "hw_internal.h"

#include "cyc_trace.h"
#include "hw_fds.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MMC1_CTRL_MIRROR = 3, MMC1_CTRL_PRG_MODE = 0x0C, MMC1_CTRL_CHR_4K = 0x10 };

bool hw_code_cart_watch;

bool hw_cart_code_ram_supported(void)
{
    /* Fixed8 KiB SNROM aperture only. Banked SOROM/SXROM/SZROM, extended
     * submappers and other boards keep the existing interpreter contract. */
    return hw_cart.mapper == 1 && hw_cart.info.submapper == 0 &&
           hw_cart.has_wram && hw_cart.wram_len == 8192 && hw_cart.chr_ram &&
           hw_cart.chr_pages <= 8 && hw_cart.prg_slots <= 64;
}

bool hw_cart_code_ram_ready(void)
{
    if (!hw_cart_code_ram_supported() || !hw_cart.wram_readable || hw_cart.wram_bank)
        return false;
    /* Both4 KiB CHR selectors must leave SRAM /CE enabled. Merely checking
     * the current A12-selected gate is unsafe within a folded instruction. */
    unsigned chr = hw_cart.m.chr0;
    if (hw_cart.m.ctrl & MMC1_CTRL_CHR_4K) chr |= hw_cart.m.chr1;
    return !(hw_cart.m.prg & 16) && !(chr & 16);
}

/* ------------------------------------------------------------------------- */
/* Bank tables                                                               */
/* ------------------------------------------------------------------------- */

/* Map a 4KB PRG bank into one of eight CPU slots ($8000 through $F000). Negative bank numbers count from the end of the ROM (-1 = last,
 * -2 = second last), which is how the fixed slots of MMC1, MMC3 and UxROM are
 * specified. Bank numbers past the end of the ROM wrap, as a board's missing
 * address lines make them: prg_slots is a power of two, and two's complement
 * makes the same mask serve both.
 *
 * Mappers must not write prg_off directly. Everything downstream - the read
 * fast path, the state hash, and the recompiler's dispatch, which needs the
 * PRG offset a compiled block was generated for - reads these tables. */
static void map_prg4(unsigned slot, int bank)
{
    unsigned n = hw_cart.prg_slots;
    hw_cart.prg_off[slot & 7] = ((unsigned)bank & (n - 1)) * 0x1000u;
#if NESRECOMP_NATIVE_PRG_GUARDS
    hw_native_interval.prg[slot & 7] = hw_cart.prg + hw_cart.prg_off[slot & 7];
#endif
}

static void map_prg8(unsigned slot, int bank)
{
    map_prg4(slot * 2, bank * 2);
    map_prg4(slot * 2 + 1, bank * 2 + 1);
}

static void map_prg16(unsigned slot, int bank)
{
    map_prg8(slot * 2, bank * 2);
    map_prg8(slot * 2 + 1, bank * 2 + 1);
}

static void map_prg32(int bank)
{
    for (unsigned i = 0; i < 4; i++) map_prg8(i, bank * 4 + (int)i);
}

/* The same for CHR, in 1KB pages. */
static void map_chr1(unsigned page, int bank)
{
    unsigned n = hw_cart.chr_pages;
    hw_cart.chr_off[page & 7] = (uint32_t)(((unsigned)bank & (n - 1)) * 0x400u);
}

static void map_chr2(unsigned page, int bank)
{
    map_chr1(page * 2, bank * 2);
    map_chr1(page * 2 + 1, bank * 2 + 1);
}

static void map_chr4(unsigned page, int bank)
{
    for (unsigned i = 0; i < 4; i++) map_chr1(page * 4 + i, bank * 4 + (int)i);
}

static void map_chr8(int bank)
{
    for (unsigned i = 0; i < 8; i++) map_chr1(i, bank * 8 + (int)i);
}

/* ------------------------------------------------------------------------- */
/* Mapper 0: NROM                                                           */
/* ------------------------------------------------------------------------- */
/* No logic at all: the PRG ROM is wired to $8000-$FFFF (a 16KB ROM appears
 * twice, which map_prg32's wrap does), CHR to the PPU's $0000-$1FFF, and
 * CIRAM A10 to a solder pad. */

static void nrom_reset(void)
{
    map_prg32(0);
    map_chr8(0);
}

/* ------------------------------------------------------------------------- */
/* Mapper 2: UxROM                                                          */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, UxROM: a write anywhere in $8000-$FFFF latches the 16KB bank
 * at $8000; $C000 is fixed to the last bank. UNROM's latch is 3 bits wide and
 * UOROM's 4, but the bank wrap handles a ROM smaller than the write. */

static void uxrom_reset(void)
{
    map_prg16(0, 0);
    map_prg16(1, -1);
    map_chr8(0);
}

static void uxrom_write(uint8_t value)
{
    hw_cart.m.latch = value;
    map_prg16(0, value);
}

/* ------------------------------------------------------------------------- */
/* Mapper 3: CNROM                                                          */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, CNROM: PRG is fixed, and a write latches the 8KB CHR bank. */

static void cnrom_reset(void)
{
    map_prg32(0);
    map_chr8(0);
}

static void cnrom_write(uint8_t value)
{
    hw_cart.m.latch = value;
    map_chr8(value);
}

/* ------------------------------------------------------------------------- */
/* Mapper 7: AxROM                                                          */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, AxROM: bits 0-2 select a 32KB PRG bank and bit 4 picks which
 * single nametable the board mirrors, so there is no fixed PRG slot at all. */

static void axrom_reset(void)
{
    map_prg32(0);
    map_chr8(0);
    hw_cart.mirroring = HW_MIRROR_SCREEN_A;
}

static void axrom_write(uint8_t value)
{
    hw_cart.m.latch = value;
    map_prg32(value & 7);
    hw_cart.mirroring = (value & 0x10) ? HW_MIRROR_SCREEN_B : HW_MIRROR_SCREEN_A;
}

/* ------------------------------------------------------------------------- */
/* Mapper 66: GxROM                                                         */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, GxROM: one latch holding a 32KB PRG bank in bits 4-5 and an
 * 8KB CHR bank in bits 0-1. Like AxROM, nothing is fixed. */

static void gxrom_reset(void)
{
    map_prg32(0);
    map_chr8(0);
}

static void gxrom_write(uint8_t value)
{
    hw_cart.m.latch = value;
    map_prg32((value >> 4) & 3);
    map_chr8(value & 3);
}

/* ------------------------------------------------------------------------- */
/* Mapper 1: MMC1                                                           */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, MMC1. $8000-$FFFF is a serial port: each write shifts bit 0
 * into a 5-bit register, and the fifth write commits it to the register the
 * address's bits 13-14 select. A write with bit 7 set clears the shift
 * register and sets the PRG mode to 3 (16KB at $8000, last bank fixed at
 * $C000), which is how a game gets a known state at reset.
 *
 * The chip only accepts one write per two CPU cycles: an RMW instruction's
 * two writes (games use DEC/INC on a ROM address deliberately) count as one.
 * hw.cycles is the CPU cycle counter, DMA cycles included. */

/* CHR A16/A15/A14 are also CPU memory signals on SxROM. Their source
 * changes with PPU A12 even when /RD is high or rendering is disabled. */
static void mmc1_apply(void)
{
    static const uint8_t mirror[4] = { HW_MIRROR_SCREEN_A, HW_MIRROR_SCREEN_B,
        HW_MIRROR_VERTICAL, HW_MIRROR_HORIZONTAL };
    hw_cart.mirroring = hw_cart.info.submapper==7 ?
        (hw_cart.info.vertical ? HW_MIRROR_VERTICAL : HW_MIRROR_HORIZONTAL) :
        mirror[hw_cart.m.ctrl & MMC1_CTRL_MIRROR];
    unsigned chr=(hw_cart.m.ctrl&MMC1_CTRL_CHR_4K) && hw_cart.m.a12 ?
        hw_cart.m.chr1 : hw_cart.m.chr0;
    unsigned outer=hw_cart.prg_slots>64 ? chr&16 : 0;
    unsigned prg=hw_cart.m.prg&15;
    bool early=hw_cart.mapper==155 || hw_cart.info.submapper==3;
    unsigned first=early && (hw_cart.m.prg&16) ? prg&8 : 0;
    unsigned last=early && (hw_cart.m.prg&16) ? (prg&8)|7 : 15;
    switch ((hw_cart.m.ctrl & MMC1_CTRL_PRG_MODE) >> 2) {
    case 0: case 1: map_prg32((outer|prg)>>1); break;
    case 2: map_prg16(0,outer|first); map_prg16(1,outer|prg); break;
    default: map_prg16(0,outer|prg); map_prg16(1,outer|last); break;
    }
    if (hw_cart.info.submapper==5) map_prg32(0);
    if (hw_cart.m.ctrl & MMC1_CTRL_CHR_4K) {
        map_chr4(0,hw_cart.m.chr0); map_chr4(1,hw_cart.m.chr1);
    } else map_chr8(hw_cart.m.chr0>>1);
    /* SXROM uses C as RAM A13 and D as A14; SOROM uses D as A13.
     * SZROM instead uses E and has 16-64 KiB CHR. Save layout is physical
     * chip order: volatile chip 0, battery-backed chip 1 on SOROM/SZROM. */
    unsigned bank=hw_cart.chr_pages>8 && hw_cart.wram_len>=16384 ? (chr>>4)&1 :
        hw_cart.wram_len==32768 ? (chr>>2)&3 : hw_cart.wram_len==16384 ? (chr>>3)&1 : 0;
    hw_cart.wram_bank=bank*8192;
    bool disabled=!early && (hw_cart.m.prg&16)!=0;
    if (hw_cart.chr_pages<=8 && hw_cart.prg_slots<=64 && hw_cart.wram_len==8192)
        disabled|=(chr&16)!=0; /* SNROM's additional RAM /CE */
    hw_cart.wram_readable=hw_cart.wram_writable=!disabled;
}

static void mmc1_reset(void)
{
    hw_cart.m.shift = hw_cart.m.shift_count = 0;
    hw_cart.m.ctrl = 0x0C;   /* PRG mode 3: the state a bit-7 write leaves */
    hw_cart.m.chr0 = hw_cart.m.chr1 = hw_cart.m.prg = 0;
    mmc1_apply();
}

static void mmc1_write(uint16_t addr, uint8_t value)
{
    /* One write per two CPU cycles; the second write of an RMW is ignored. */
    if (hw.cycles == hw_cart.m.last_write_cycle + 1) return;
    hw_cart.m.last_write_cycle = hw.cycles;

    if (value & 0x80) {
        hw_cart.m.shift = hw_cart.m.shift_count = 0;
        hw_cart.m.ctrl |= MMC1_CTRL_PRG_MODE;
        mmc1_apply();
        return;
    }
    hw_cart.m.shift = (uint8_t)(hw_cart.m.shift >> 1 | (value & 1) << 4);
    if (++hw_cart.m.shift_count < 5) return;
    uint8_t v = hw_cart.m.shift;
    hw_cart.m.shift = hw_cart.m.shift_count = 0;
    switch ((addr >> 13) & 3) {
    case 0: hw_cart.m.ctrl = v; break;
    case 1: hw_cart.m.chr0 = v; break;
    case 2: hw_cart.m.chr1 = v; break;
    default: hw_cart.m.prg = v; break;
    }
    mmc1_apply();
}

/* ------------------------------------------------------------------------- */
/* Mapper 4: MMC3                                                           */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, MMC3. Eight bank registers behind a select latch, a mirroring
 * bit, a work RAM protect byte, and a scanline counter that drives /IRQ. */

/* nesdev wiki, TQROM: CHR A16 (bank bit 6) selects the 8 KiB CHR RAM chip
 * instead of the CHR ROM, which then sees bank bits 0-5 (at most 64 KiB).
 * The RAM decodes A10-A12 only, so its 1 KiB page is the bank's low 3 bits. */
static void mmc3_chr1(unsigned page, unsigned bank)
{
    bool ram = hw_cart.mapper == 119 && (bank & 0x40);
    if (hw_cart.mapper == 119 && !ram) bank &= 0x3f;
    hw_cart.chr_write[page] = hw_cart.mapper == 119 ? ram : hw_cart.chr_ram;
    if (ram) hw_cart.chr_off[page] = hw_cart.chr_ram_base + (bank & 7) * 0x400u;
    else map_chr1(page, (int)bank);
}

/* Namco 118 boards NAMCOT-3433/3453 (mappers 88, 154) wire PPU A12 to CHR
 * A16: the left pattern table reads the first 64 KiB, the right the second
 * (nesdev wiki, INES Mapper 088). */
static unsigned namco108_chr_a16(unsigned page, unsigned bank)
{
    if (hw_cart.mapper != 88 && hw_cart.mapper != 154) return bank;
    return (bank & 0x3f) | (page >= 4 ? 0x40 : 0);
}

static void mmc3_apply(void)
{
    const uint8_t *r = hw_cart.m.reg;
    if (hw_cart.m.bank_select & 0x40) {
        map_prg8(0, -2);
        map_prg8(2, r[6]);
    } else {
        map_prg8(0, r[6]);
        map_prg8(2, -2);
    }
    map_prg8(1, r[7]);
    map_prg8(3, -1);
    if (hw_cart.mapper == 206 && hw_cart.info.submapper == 1) map_prg32(0);

    /* Bit 7 swaps the 2KB and 1KB halves of the pattern tables (it inverts
     * CHR A12). The 2KB banks ignore the low bit of their register. */
    unsigned big = (hw_cart.m.bank_select & 0x80) ? 4 : 0;   /* page of the 2KB pair */
    unsigned small = big ^ 4;
    for (unsigned i = 0; i < 4; ++i) {
        mmc3_chr1(big + i, namco108_chr_a16(big + i, (r[i >> 1] & 0xfe) | (i & 1)));
        mmc3_chr1(small + i, namco108_chr_a16(small + i, r[2 + i]));
    }
}

static void mmc3_reset(void)
{
    hw_cart.m.bank_select = 0;
    memset(hw_cart.m.reg, 0, sizeof(hw_cart.m.reg));
    hw_cart.m.reg[6] = 0;
    hw_cart.m.reg[7] = 1;
    hw_cart.m.mirror_reg = 0;
    hw_cart.m.ram_protect = 0;
    hw_cart.m.irq_latch = hw_cart.m.irq_counter = 0;
    hw_cart.m.irq_reload = hw_cart.m.irq_enable = hw_cart.m.irq_out = 0;
    hw_cart.m.a12 = 0;
    hw_cart.m.a12_low_cycle = 0;
    /* The MMC3 comes up with nothing selected; a game sets everything it uses
     * before enabling rendering. Work RAM starts accessible so a board that
     * has it works before the game writes $A001 (which every game that uses
     * work RAM does). */
    hw_cart.wram_readable = hw_cart.wram_writable = 1;
    mmc3_apply();
    hw_cart.mirroring = HW_MIRROR_VERTICAL;
}

static void mmc3_write(uint16_t addr, uint8_t value)
{
    switch (addr & 0xE001) {
    case 0x8000:
        hw_cart.m.bank_select = value;
        mmc3_apply();
        break;
    case 0x8001:
        hw_cart.m.reg[hw_cart.m.bank_select & 7] = value;
        mmc3_apply();
        break;
    case 0xA000:
        /* TxSROM wires CHR A17 to CIRAM A10 instead; see txsrom_ciram_a10. */
        hw_cart.m.mirror_reg = value;
        if (hw_cart.mapper != 118)
            hw_cart.mirroring = (value & 1) ? HW_MIRROR_HORIZONTAL : HW_MIRROR_VERTICAL;
        break;
    case 0xA001:
        hw_cart.m.ram_protect = value;
        hw_cart.wram_readable = (value & 0x80) != 0;
        hw_cart.wram_writable = (value & 0x80) != 0 && (value & 0x40) == 0;
        break;
    case 0xC000:
        hw_cart.m.irq_latch = value;
        break;
    case 0xC001:
        /* Reload: the counter is cleared and reloaded on the next A12 rise. */
        hw_cart.m.irq_counter = 0;
        hw_cart.m.irq_reload = 1;
        break;
    case 0xE000:
        hw_cart.m.irq_enable = 0;
        hw_cart.m.irq_out = 0;   /* acknowledges a pending IRQ */
        break;
    default:
        hw_cart.m.irq_enable = 1;
        break;
    }
}

/* One clock of the scanline counter (nesdev wiki, MMC3, "IRQ counter"):
 * reload when the counter is zero or a reload was requested, otherwise
 * decrement, and assert /IRQ when the result is zero and IRQs are enabled.
 * This is the MMC3B/C behavior. MMC3A instead only asserts when the counter
 * was nonzero before the clock or a reload was requested, so a latch of 0
 * produces one IRQ rather than one per clock; boards with an A revision are
 * not identifiable from an iNES header, and no supported game needs it. */
/* CYC_MMC3_TRACE=1 prints every clock of the counter, with the trace cycle
 * (comparable across implementations, see cyc_trace.h) and the PPU position.
 * tric_prelude.inc prints the same line, so the oracle's counter and this one
 * can be diffed directly; that is how the A12 sample point was checked. */
static void mmc3_trace_clock(void)
{
    static int on = -1;
    if (on < 0) on = getenv("CYC_MMC3_TRACE") != NULL;
    if (!on) return;
    fprintf(stderr, "MMC3 clk tc=%u sl=%u dot=%u ctr=%02X out=%u vbus=%04X\n", cyc_trace_cycle, ppu.scanline,
            ppu.dot, hw_cart.m.irq_counter, hw_cart.m.irq_out, ppu.vbus);
}

/* Taito TC0690 (mapper 48) asserts later than the MMC3 would; m.latch counts
 * the delay (hw_taito.inc). nesdev says "about 4 CPU cycles", but with 4 the
 * Flintstones status-bar split lands mid-scanline and flickers (seen in owner
 * playtest). A sweep over its route renders the split identically and cleanly
 * for 20-24 cycles and glitches below 18 or above 26; 22 is the middle, and is
 * also the value Mesen2 tuned for Flintstones and Captain Saver. */
enum { TC0690_IRQ_DELAY = 22 };

static void mmc3_clock_irq(void)
{
    if (hw_cart.m.irq_counter == 0 || hw_cart.m.irq_reload) hw_cart.m.irq_counter = hw_cart.m.irq_latch;
    else hw_cart.m.irq_counter--;
    if (hw_cart.m.irq_counter == 0 && hw_cart.m.irq_enable) {
        if (hw_cart.mapper != 48) hw_cart.m.irq_out = 1;
        else if (!hw_cart.m.latch && !hw_cart.m.irq_out) hw_cart.m.latch = TC0690_IRQ_DELAY;
    }
    hw_cart.m.irq_reload = 0;
    mmc3_trace_clock();
}

/* The MMC3 has no scanline input: it counts rising edges of the PPU's A12,
 * which goes high once per scanline when rendering fetches from the half of
 * the pattern tables that the background does not use. A12 also toggles every
 * few dots within a tile fetch, so the chip low-pass filters the line: an
 * edge counts only when A12 has been low for at least three CPU cycles
 * (nesdev wiki, MMC3, "IRQ counter"; Mesen filters on the same three CPU
 * cycles, NES_MiSTer's MMC3.sv on 16 PPU cycles - the gaps a rendering PPU
 * produces are either ~4 dots or ~64, so any threshold between them behaves
 * the same).
 *
 * A12 is a direct pin, not multiplexed through the cartridge's address latch,
 * so the mapper sees it change as soon as the PPU drives a new address, which
 * is in the first half of a dot; hw_ppu.c offers it once per dot, there. */
enum { MMC3_A12_FILTER_CYCLES = 3 };

static void mmc3_ppu_addr(uint16_t vbus)
{
    if (!(vbus & 0x1000)) {
        if (hw_cart.m.a12) {
            hw_cart.m.a12 = 0;
            hw_cart.m.a12_low_cycle = hw.cycles;
        }
        return;
    }
    if (!hw_cart.m.a12) {
        hw_cart.m.a12 = 1;
        if (hw.cycles - hw_cart.m.a12_low_cycle >= MMC3_A12_FILTER_CYCLES) mmc3_clock_irq();
    }
}

/* ------------------------------------------------------------------------- */
/* Mapper 32: Irem G-101                                                    */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, INES Mapper 032. Registers decode A15-A12 ($B000 also A2-A0):
 * $8000 and $A000 select 8 KiB PRG banks (5 bits); $9000 bit 1 swaps the
 * $8000 bank with the fixed second-to-last bank at $C000, bit 0 selects
 * mirroring (1 = horizontal); $B000-$B007 select the eight 1 KiB CHR banks.
 * Submapper 1 (Major League) ties CIRAM A10 high and fixes the PRG mode. */
static void irem_g101_apply(void)
{
    bool swap = (hw_cart.m.ctrl & 2) && hw_cart.info.submapper != 1;
    map_prg8(swap ? 2 : 0, hw_cart.m.reg[0] & 0x1f);
    map_prg8(swap ? 0 : 2, -2);
    map_prg8(1, hw_cart.m.reg[1] & 0x1f);
    map_prg8(3, -1);
    hw_cart.mirroring = hw_cart.info.submapper == 1 ? HW_MIRROR_SCREEN_B :
        (hw_cart.m.ctrl & 1) ? HW_MIRROR_HORIZONTAL : HW_MIRROR_VERTICAL;
}

static void irem_g101_write(uint16_t addr, uint8_t value)
{
    switch (addr & 0xf000) {
    case 0x8000: hw_cart.m.reg[0] = value; break;
    case 0x9000: hw_cart.m.ctrl = value; break;
    case 0xa000: hw_cart.m.reg[1] = value; break;
    case 0xb000: map_chr1(addr & 7, value); return;
    default: return;
    }
    irem_g101_apply();
}

/* ------------------------------------------------------------------------- */
/* Mapper 65: Irem H3001                                                    */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, INES Mapper 065. $8000 and $A000 select 8 KiB PRG banks;
 * $9000 bit 7 moves the $8000 bank to $C000 and bank $3E to $8000 ($C000 is
 * otherwise $3E; $E000 is always $3F). $9001 bits 7-6: 00 vertical, 10
 * horizontal, x1 one-screen A. $B000-$B007 select the eight 1 KiB CHR banks.
 * IRQ: $9005/$9006 set the 16-bit reload value, a $9004 write copies it into
 * the counter, $9003 bit 7 enables; either write acknowledges. The enabled
 * counter decrements every CPU cycle and asserts on reaching 0, where it
 * stops. Power-on PRG registers are $00/$01 ("games do rely on this"). The
 * $C000 bank register some documents (and Mesen2) describe does not exist. */
static void irem_h3001_apply(void)
{
    bool swap = (hw_cart.m.ctrl & 0x80) != 0;
    map_prg8(swap ? 2 : 0, hw_cart.m.reg[0]);
    map_prg8(swap ? 0 : 2, 0x3e);
    map_prg8(1, hw_cart.m.reg[1]);
    map_prg8(3, 0x3f);
    unsigned m = hw_cart.m.mirror_reg >> 6;
    hw_cart.mirroring = m == 0 ? HW_MIRROR_VERTICAL : m == 2 ? HW_MIRROR_HORIZONTAL : HW_MIRROR_SCREEN_A;
}

static void irem_h3001_write(uint16_t addr, uint8_t value)
{
    switch (addr & 0xf000) {
    case 0x8000: hw_cart.m.reg[0] = value; break;
    case 0xa000: hw_cart.m.reg[1] = value; break;
    case 0xb000: map_chr1(addr & 7, value); return;
    case 0x9000:
        switch (addr & 7) {
        case 0: hw_cart.m.ctrl = value; break;
        case 1: hw_cart.m.mirror_reg = value; break;
        case 3: hw_cart.m.irq_enable = value >> 7; hw_cart.m.irq_out = 0; return;
        case 4: hw_cart.m.irq_counter16 = hw_cart.m.irq_latch16; hw_cart.m.irq_out = 0; return;
        case 5: hw_cart.m.irq_latch16 = (uint16_t)((hw_cart.m.irq_latch16 & 0x00ff) | (value << 8)); return;
        case 6: hw_cart.m.irq_latch16 = (uint16_t)((hw_cart.m.irq_latch16 & 0xff00) | value); return;
        default: return;
        }
        break;
    default: return;
    }
    irem_h3001_apply();
}

static void irem_h3001_clock(void)
{
    if (hw_cart.m.irq_enable && hw_cart.m.irq_counter16 && --hw_cart.m.irq_counter16 == 0) hw_cart.m.irq_out = 1;
}

/* ------------------------------------------------------------------------- */
/* Mapper 77: Irem LROG017 (Napoleon Senki)                                 */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, INES Mapper 077. [CCCC PPPP] at $8000-$FFFF with AND bus
 * conflicts: a 32 KiB PRG bank and a 2 KiB CHR ROM bank at $0000. CHR RAM is
 * fixed at $0800-$1FFF (the RAM chip after the ROM), and the four nametables
 * are RAM too (four_screen). */
static void irem77_chr(unsigned bank)
{
    for (unsigned page = 0; page < 8; ++page) {
        if (page < 2) { map_chr1(page, (int)(bank * 2 + page)); hw_cart.chr_write[page] = 0; }
        else { hw_cart.chr_off[page] = hw_cart.chr_ram_base + (page - 2) * 0x400u; hw_cart.chr_write[page] = 1; }
    }
}

/* ------------------------------------------------------------------------- */
/* Mapper 96: Bandai Oeka Kids                                              */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, INES Mapper 096. [.... .CPP] at $8000-$FFFF with AND bus
 * conflicts: a 32 KiB PRG bank and the 16 KiB outer CHR RAM bank. The inner
 * 4 KiB bank at $0000 is PPU A9-A8, latched when the PPU address moves into
 * $2xxx from anywhere else (whoever drives it); $1000 holds inner bank 3. */
static void oeka_apply(void)
{
    map_chr4(0, (hw_cart.m.ctrl & 4) | hw_cart.m.chr0);
    map_chr4(1, (hw_cart.m.ctrl & 4) | 3);
}

static void oeka_ppu_addr(uint16_t vbus)
{
    if ((hw_cart.m.pattern_addr & 0x3000) != 0x2000 && (vbus & 0x3000) == 0x2000) {
        hw_cart.m.chr0 = (vbus >> 8) & 3;
        oeka_apply();
    }
    hw_cart.m.pattern_addr = vbus;
}

/* ------------------------------------------------------------------------- */
/* Mapper 228: Active Enterprises (Action 52, Cheetahmen II)                */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, INES Mapper 228. A write to $8000-$FFFF latches the address
 * (1.MHHPPP PPS.CCCC) and D0-D1: A13 mirroring (1 = horizontal), A11-A12 the
 * 512 KiB PRG chip, A6-A10 its 16 KiB page, A5 16 KiB mode (the page in both
 * halves; else the even/odd pair), A0-A3:D0-D1 the 8 KiB CHR bank. The
 * 1.5 MiB Action 52 has chips 0, 1 and 3, stored in that order: selecting
 * chip 2 reads open bus. The documented $4020-$5FFF nibble RAM is absent on
 * both cartridges and is not modeled. */
static void action52_apply(void)
{
    uint16_t a = (uint16_t)(hw_cart.m.chr1 << 8 | hw_cart.m.latch);
    unsigned chip = (a >> 11) & 3, page = (a >> 6) & 31;
    bool three_chips = hw_cart.prg_len == 0x180000;
    bool open = three_chips && chip == 2;
    if (three_chips && chip == 3) chip = 2;
    unsigned bank = chip * 32 + page;
    if (a & 0x20) { map_prg16(0, (int)bank); map_prg16(1, (int)bank); }
    else map_prg16(0, (int)(bank & ~1u)), map_prg16(1, (int)(bank | 1));
    if (open) for (unsigned slot = 0; slot < 8; ++slot) hw_cart.prg_off[slot] |= MMC5_PRG_OPEN;
    map_chr8((int)((a & 15) << 2 | (hw_cart.m.chr0 & 3)));
    hw_cart.mirroring = (a & 0x2000) ? HW_MIRROR_HORIZONTAL : HW_MIRROR_VERTICAL;
}

static void action52_write(uint16_t addr, uint8_t value)
{
    hw_cart.m.latch = (uint8_t)addr;
    hw_cart.m.chr1 = (uint8_t)(addr >> 8);
    hw_cart.m.chr0 = value & 3;
    action52_apply();
}

/* ------------------------------------------------------------------------- */
/* Mapper 118: TxSROM (TKSROM/TLSROM)                                       */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, TxSROM: an MMC3 whose CHR A17 output drives CIRAM A10 in place
 * of the chip's mirroring output. The MMC3 decodes CHR from PPU A12-A10 alone,
 * so a nametable address selects bit 7 of whichever register the matching
 * pattern page uses: R0/R1 for the 2 KiB half, R2-R5 for the 1 KiB half,
 * swapped by $8000 bit 7. Registers mapped to the other half are ignored.
 * $A000 has no effect. The register value is used before any ROM-size wrap:
 * A17 exists on the connector even when a smaller CHR ROM ignores it. */
static uint16_t txsrom_ciram_a10(uint16_t addr)
{
    unsigned page = ((addr >> 10) & 7) ^ ((hw_cart.m.bank_select & 0x80) ? 4 : 0);
    uint8_t bank = page < 4 ? hw_cart.m.reg[page >> 1] : hw_cart.m.reg[2 + (page & 3)];
    return (bank & 0x80) ? 0x400 : 0;
}

/* ------------------------------------------------------------------------- */
/* Mapper 20: the FDS RAM Adapter (hw_fds.c)                                 */
/* ------------------------------------------------------------------------- */
/* nesdev wiki, Family Computer Disk System; libretro/Mesen FDS.cpp:10-22. $8000-$DFFF
 * is PRG RAM, tagged so the native dispatch never treats it as ROM (the
 * same tag MMC5's RAM windows use); $E000-$FFFF is the 8 KiB BIOS, the whole
 * of hw_cart.prg, fixed like an NROM bank. $6000-$7FFF is the first 8 KiB of
 * the same RAM, read through hw_cart_cpu_read. */
static void fds_reset(void)
{
    for (unsigned slot = 0; slot < 6; ++slot)
        hw_cart.prg_off[slot] = MMC5_PRG_RAM | (0x2000u + slot * 0x1000u);
    map_prg4(6, 0);
    map_prg4(7, 1);
    map_chr8(0);
    fds_power_on();
}

/* ------------------------------------------------------------------------- */
/* Dispatch                                                                  */
/* ------------------------------------------------------------------------- */

#include "hw_mmc5.inc"

/* MMC2 / MMC4: https://www.nesdev.org/wiki/MMC2 and /MMC4.
 * A triggering read still returns data from the old bank. Only subsequent
 * accesses see the latch's new bank. MMC2 fully decodes the lower trigger;
 * MMC4 ignores A0-A2 on both halves. Power-on latch state is not guaranteed;
 * choose FE consistently in both machines. */
static void mmc2_chr_apply(void)
{
    map_chr4(0, hw_cart.m.reg[hw_cart.m.chr0]);
    map_chr4(1, hw_cart.m.reg[2 + hw_cart.m.chr1]);
}

static void mmc2_write(uint16_t addr, uint8_t value)
{
    unsigned page = addr >> 12;
    if (page == 10) {
        hw_cart.m.prg = value & 15;
        if (hw_cart.mapper == 9) map_prg8(0, hw_cart.m.prg);
        else map_prg16(0, hw_cart.m.prg);
    } else if (page >= 11 && page <= 14) {
        hw_cart.m.reg[page - 11] = value & 31;
        mmc2_chr_apply();
    } else if (page == 15) {
        hw_cart.mirroring = (value & 1) ? HW_MIRROR_HORIZONTAL : HW_MIRROR_VERTICAL;
    }
}

/* Mapper 185: https://www.nesdev.org/wiki/INES_Mapper_185. CNROM whose
 * data bits 0-1 drive CHR ROM chip selects through the copy-protection
 * wiring: submappers 4-7 enable CHR only for chip-select value 0-3. Writes
 * have AND bus conflicts. Submapper 0 follows the nesdev heuristic: CHR is
 * disabled for the fetches of the first two $2007 reads after power-on. A
 * disabled read leaves the address byte on the bus, with D0 pulled high as
 * on the early Mighty Bomb Jack board (other boards vary). */
static bool cnrom185_enabled(void)
{
    unsigned sub = hw_cart.info.submapper;
    return sub ? (hw_cart.m.latch & 3) == sub - 4 : hw_cart.m.data_reads > 2;
}

void hw_cart_ppu_data_read(void)
{
    if (hw_cart.mapper == 185 && hw_cart.m.data_reads < 255) ++hw_cart.m.data_reads;
}

uint8_t hw_cart_chr_read(uint16_t addr)
{
    if (hw_cart.mapper == 185 && !cnrom185_enabled()) return (uint8_t)(addr | 1);
    if (hw_cart.mapper == 93 && !hw_cart.m.ctrl) return (uint8_t)addr;   /* CHR RAM disabled: open bus */
    if (hw_cart.mapper==5) mmc5_ppu_read(addr);
    uint8_t value = hw_cart.chr[hw_cart_chr_index(addr)];
    if ((hw_cart.mapper == 9 || hw_cart.mapper == 10) && !hw_cart.m.pattern_pending) {
        hw_cart.m.pattern_pending = 1;
        hw_cart.m.pattern_addr = addr;
    }
    return value;
}

void hw_cart_ppu_rd(bool reading)
{
    if (hw_cart.mapper==5) { if (!reading) hw_cart.m.mmc5.rd=0; return; }
    if (!reading && hw_cart.m.pattern_pending) {
        uint16_t addr = hw_cart.m.pattern_addr;
        hw_cart.m.pattern_pending = 0;
        unsigned decoded = addr;
        if (hw_cart.mapper == 10 || (addr & 0x1000)) decoded &= 0x1ff8;
        switch (decoded) {
        case 0x0fd8: hw_cart.m.chr0 = 0; break;
        case 0x0fe8: hw_cart.m.chr0 = 1; break;
        case 0x1fd8: hw_cart.m.chr1 = 0; break;
        case 0x1fe8: hw_cart.m.chr1 = 1; break;
        default: return;
        }
        mmc2_chr_apply();
    }
}

#include "hw_vrc.inc"
#include "hw_vrc6.inc"
#include "hw_vrc7.inc"
#include "hw_bandai.inc"
#include "hw_sunsoft.inc"
#include "hw_namco.inc"
#include "hw_jaleco.inc"
#include "hw_tengen.inc"
#include "hw_taito.inc"
#include "hw_taito_x1.inc"

/* ---- sound chip state outside hw_cart (cyc_state.c) ----
 * VRC7's OPLL (emu2413) is audio only: nothing the CPU reads comes from it.
 * An isolated mod call is undone by copying the object back into itself (its
 * internal pointers stay valid); a save state carries VRC7's registers in
 * hw_cart and the chip is rebuilt from them. */
size_t hw_cart_sound_snapshot_size(void) { return vrc7_opll ? sizeof(OPLL) : 0; }
void   hw_cart_sound_snapshot(void *buf) { if (vrc7_opll) memcpy(buf, vrc7_opll, sizeof(OPLL)); }
void   hw_cart_sound_restore(const void *buf) { if (vrc7_opll) memcpy(vrc7_opll, buf, sizeof(OPLL)); }
void   hw_cart_sound_reloaded(void)
{
    if (!vrc7_opll) return;
    uint8_t regs[sizeof(hw_cart.m.vrc7_reg)], address = hw_cart.m.vrc7_address;
    int16_t output = hw_cart.m.vrc7_output;
    uint64_t phase = hw_cart.m.vrc7_phase;
    memcpy(regs, hw_cart.m.vrc7_reg, sizeof(regs));
    vrc7_sound_reset(false);
    memcpy(hw_cart.m.vrc7_reg, regs, sizeof(regs));
    hw_cart.m.vrc7_address = address;
    hw_cart.m.vrc7_output = output;
    hw_cart.m.vrc7_phase = phase;
    for (unsigned r = 0; r < sizeof(regs); ++r)
        if (r < 8 || r == 15 || (r >= 16 && r <= 21) || (r >= 32 && r <= 37) || (r >= 48 && r <= 53))
            OPLL_writeReg(vrc7_opll, r, regs[r]);
}

static const struct {
    int         mapper;
    const char *name;
    uint8_t     watch_ppu_addr;
    uint8_t     wram;            /* boards for this mapper carry work RAM */
} MAPPERS[] = {
    { 20, "FDS RAM Adapter", 0, 1 },
    { 5, "MMC5", 0, 1 },
    { 69, "Sunsoft FME-7 / 5B", 0, 1 },
    { 19, "Namco 163", 0, 0 },
    { 18, "Jaleco SS88006", 0, 0 },
    { 64, "Tengen RAMBO-1", 1, 0 },
    { 158, "Tengen 800037 (RAMBO-1, CHR A17 mirroring)", 1, 0 },
    { 33, "Taito TC0190", 0, 0 },
    { 32, "Irem G-101", 0, 0 },
    { 65, "Irem H3001", 0, 0 },
    { 80, "Taito X1-005", 0, 0 },
    { 207, "Taito X1-005 (CHR mirroring)", 0, 0 },
    { 82, "Taito X1-017", 0, 0 },
    { 552, "Taito X1-017 (NES 2.0)", 0, 0 },
    { 48, "Taito TC0690", 1, 0 },
    { 210, "Namco 175 / 340", 0, 0 },
    { 68, "Sunsoft-4", 0, 1 },
    { 67, "Sunsoft-3", 0, 0 },
    { 41, "Caltron 6-in-1", 0, 0 },
    { 185, "CNROM + copy protection", 0, 0 },
    { 228, "Active Enterprises", 0, 0 },
    { 157, "Bandai Datach", 1, 0 },
    { 153, "Bandai BA-JUMP2", 1, 1 },
    { 16, "Bandai FCG / LZ93D50", 0, 0 },
    { 159, "Bandai LZ93D50 / X24C01", 0, 0 },
    { 85, "VRC7", 0, 1 },
    { 24, "VRC6a", 0, 1 },
    { 26, "VRC6b", 0, 1 },
    { 21, "VRC4a/c", 0, 1 },
    { 22, "VRC2a", 0, 0 },
    { 23, "VRC2b / VRC4e/f", 0, 1 },
    { 25, "VRC2c / VRC4b/d", 0, 1 },
    { 73, "VRC3", 0, 1 },
    { 31, "NSF cartridge", 0, 0 },
    { 40, "NTDEC 2722", 0, 0 },
    { 9, "MMC2", 0, 0 },
    { 10, "MMC4", 0, 1 },
    { 232, "Camerica Quattro", 0, 0 },
    { 184, "Sunsoft-1", 0, 0 },
    { 180, "Crazy Climber", 0, 0 },
    { 70, "Bandai 74161/7432", 0, 0 },
    { 78, "Irem 74HC161 / Jaleco JF-16", 0, 0 },
    { 89, "Sunsoft-2 (Sunsoft-3 board)", 0, 0 },
    { 93, "Sunsoft-2 (Sunsoft-3R board)", 0, 0 },
    { 97, "Irem TAM-S1", 0, 0 },
    { 72, "Jaleco JF-17", 0, 0 },
    { 92, "Jaleco JF-19", 0, 0 },
    { 86, "Jaleco JF-13", 0, 0 },
    { 101, "Jaleco JF-10 (mapper 101)", 0, 0 },
    { 77, "Irem LROG017", 0, 0 },
    { 96, "Bandai Oeka Kids", 1, 0 },
    { 144, "Color Dreams (Death Race)", 0, 0 },
    { 146, "Sachen 3015 / SA-016", 0, 0 },
    { 148, "Sachen SA-008-A / Tengen 800008", 0, 0 },
    { 152, "Bandai 74161/7432 (one-screen)", 0, 0 },
    { 140, "Jaleco JF-11/14", 0, 0 },
    { 113, "HES", 0, 0 },
    { 94, "UN1ROM", 0, 0 },
    { 87, "J87", 0, 0 },
    { 79, "NINA-003/006", 0, 0 },
    { 76, "Namco 109", 0, 0 },
    { 206, "DxROM", 0, 0 },
    { 88, "Namco 118 (CHR A16 = PPU A12)", 0, 0 },
    { 95, "Namco 118 (CHR A15 = CIRAM A10)", 0, 0 },
    { 154, "Namco 118 (CHR A16 = PPU A12, one-screen)", 0, 0 },
    { 75, "VRC1", 0, 0 },
    { 71, "Camerica", 0, 0 },
    { 34, "BNROM / NINA-001", 0, 0 },
    { 13, "CPROM", 0, 0 },
    { 11, "Color Dreams", 0, 0 },
    { 0,  "NROM",  0, 0 },
    { 1,  "MMC1",  1, 1 },
    { 155,"MMC1A", 1, 1 },
    { 2,  "UxROM", 0, 0 },
    { 3,  "CNROM", 0, 0 },
    { 4,  "MMC3",  1, 1 },
    { 118, "TxSROM", 1, 1 },
    { 119, "TQROM", 1, 0 },
    { 7,  "AxROM", 0, 0 },
    { 66, "GxROM", 0, 0 },
};

static int mapper_index(int mapper)
{
    for (size_t i = 0; i < sizeof(MAPPERS) / sizeof(MAPPERS[0]); i++)
        if (MAPPERS[i].mapper == mapper) return (int)i;
    return -1;
}

/* PPU-driven A18 can change DURING an instruction or a DMA stall. Native
 * constants are valid only while all four selected outputs agree. Re-enable
 * native dispatch automatically after software makes the outer bank stable. */
bool hw_prg_is_stable(void)
{
    if ((hw_cart.mapper==1 || hw_cart.mapper==155) && hw_cart.prg_slots>64)
        return !(hw_cart.m.ctrl&16) || !((hw_cart.m.chr0^hw_cart.m.chr1)&16);
    if (hw_cart.mapper!=153 || hw_cart.prg_slots<=64) return true;
    return !((hw_cart.m.reg[0]^hw_cart.m.reg[1])&1) &&
           !((hw_cart.m.reg[0]^hw_cart.m.reg[2])&1) &&
           !((hw_cart.m.reg[0]^hw_cart.m.reg[3])&1);
}

bool hw_prg_is_rom(uint16_t addr)
{
    return addr>=0x8000 && !(hw_cart.prg_off[addr>>12&7]&(MMC5_PRG_RAM|MMC5_PRG_OPEN));
}

bool hw_cart_supports(int mapper) { return mapper_index(mapper) >= 0; }

const char *hw_cart_mapper_name(int mapper)
{
    int i = mapper_index(mapper);
    return i >= 0 ? MAPPERS[i].name : "unsupported";
}

void hw_cart_power_on(void)
{
    memset(&hw_cart.m, 0, sizeof(hw_cart.m));
    if (!hw_cart.info.battery && !hw_cart.info.prg_nvram) memset(hw_cart.exram,0,sizeof(hw_cart.exram));
    memset(hw_cart.prg_off, 0, sizeof(hw_cart.prg_off));
    memset(hw_cart.chr_off, 0, sizeof(hw_cart.chr_off));
    memset(hw_cart.chr_write, hw_cart.chr_ram, sizeof(hw_cart.chr_write));
    hw_cart.chr_ciram = 0;
    int i = mapper_index(hw_cart.mapper);
    hw_cart.watch_ppu_addr = i >= 0 ? MAPPERS[i].watch_ppu_addr : 0;
    hw_cart.watch_cpu = hw_cart.mapper==40 || hw_cart.mapper==69 || hw_cart.mapper==19 || hw_cart.mapper==18 || hw_cart.mapper==48 || hw_cart.mapper==64 || hw_cart.mapper==158 || hw_cart.mapper==65 || hw_cart.mapper==67 || hw_cart.mapper==5 || bandai_board() || (vrc24_board() && !vrc2_board()) || hw_cart.mapper == 73 || vrc6_board() || hw_cart.mapper == 85;
    hw_cart.mirroring = hw_cart.info.vertical ? HW_MIRROR_VERTICAL : HW_MIRROR_HORIZONTAL;
    hw_cart.wram_bank = 0;
    hw_cart.has_wram = hw_cart.info.prg_size ? hw_cart.wram_len != 0 : i >= 0 ? MAPPERS[i].wram : 0;
    hw_cart.wram_readable = hw_cart.wram_writable = hw_cart.has_wram;
    if (hw_cart.mapper == 34 && !hw_cart.info.prg_size) {
        hw_cart.has_wram = hw_cart.chr_pages > 8;
        hw_cart.wram_readable = hw_cart.wram_writable = hw_cart.has_wram;
    }
    if (!hw_cart.info.prg_size) hw_cart.wram_len = hw_cart.has_wram ? 8192 : 0;
    /* Work RAM is uninitialized at power-on like CPU RAM; a battery-backed
     * board would come up with its saved contents, which no run here has. */
    memset(hw_cart.wram, 0, hw_cart.info.prg_nvram ? hw_cart.info.prg_ram : sizeof(hw_cart.wram));

    if (bandai_board() && hw_cart.mapper!=153) hw_cart.has_wram=hw_cart.wram_readable=hw_cart.wram_writable=0;
    for (unsigned chip=0;chip<2;++chip) nes_eeprom_reset(&hw_cart.eeprom[chip]);
    memset(&hw_cart.barcode,0,sizeof(hw_cart.barcode));
    vrc7_sound_reset(true);
    s5b_reset();
    hw_cart.clock_late = hw_cart.mapper == 20;
    switch (hw_cart.mapper) {
    case 20: fds_reset(); break;
    case 5: mmc5_reset(); break;
    case 69: fme7_apply(); break;
    case 19: case 210: hw_cart.m.namco.channel = 7; namco_apply(); break;
    case 18: jaleco_apply(); break;
    case 64: case 158: rambo_apply(); break;
    case 33: case 48: taito_apply(); break;
    case 32: irem_g101_apply(); map_chr8(0); break;
    case 65: hw_cart.m.reg[1] = 1; irem_h3001_apply(); map_chr8(0); break;
    case 80: case 207: case 82: case 552: taito_x1_apply(); break;
    case 68: sunsoft4_apply(); break;
    case 67: uxrom_reset(); break;
    case 41: nrom_reset(); hw_cart.mirroring = HW_MIRROR_VERTICAL; break;
    case 185: nrom_reset(); break;
    /* The games expect $00 written to $8000 at power-on and reset. */
    case 228: action52_write(0x8000, 0); break;
    case 16: case 159: case 153: case 157: bandai_apply(); break;
    case 85: hw_cart.m.reg[1]=1; hw_cart.m.reg[2]=2; hw_cart.m.irq_prescaler=341; vrc7_apply(); break;
    case 24: case 26:
        hw_cart.m.vrc6_audio.step[0] = hw_cart.m.vrc6_audio.step[1] = 15;
        hw_cart.m.ctrl = 0x20; hw_cart.m.irq_prescaler = 341; vrc6_apply(); break;
    case 21: case 22: case 23: case 25:
        hw_cart.m.reg[1] = 1; hw_cart.m.irq_prescaler = 341; vrc24_apply(); break;
    case 73: uxrom_reset(); break;
    case 40:
        map_prg8(0,4); map_prg8(1,5); map_prg8(2,0); map_prg8(3,7); map_chr8(0); break;
    case 31:
        for (unsigned slot = 0; slot < 8; ++slot) map_prg4(slot, slot == 7 ? 255 : 0);
        map_chr8(0); break;
    case 9:
        map_prg8(0, 0); map_prg8(1, -3); map_prg8(2, -2); map_prg8(3, -1);
        hw_cart.m.chr0 = hw_cart.m.chr1 = 1; mmc2_chr_apply(); break;
    case 10:
        uxrom_reset(); hw_cart.m.chr0 = hw_cart.m.chr1 = 1; mmc2_chr_apply(); break;
    case 13: nrom_reset(); map_chr4(1, 0); hw_cart.mirroring = HW_MIRROR_VERTICAL; break;
    case 71: uxrom_reset(); break;
    case 75: nrom_reset(); map_prg8(3, -1); map_chr4(1, 0); hw_cart.mirroring = HW_MIRROR_VERTICAL; break;
    case 206: case 88: case 95: hw_cart.m.reg[7] = 1; mmc3_apply(); break;
    case 154: hw_cart.m.reg[7] = 1; mmc3_apply(); hw_cart.mirroring = HW_MIRROR_SCREEN_A; break;
    case 76: uxrom_reset(); hw_cart.m.reg[7] = 1; for (unsigned j = 0; j < 4; ++j) map_chr2(j, 0); break;
    case 94: uxrom_reset(); break;
    case 180: map_prg16(0, 0); map_prg16(1, 0); map_chr8(0); break;
    case 70: uxrom_reset(); break;
    case 78: case 89: case 72: case 92: uxrom_reset(); break;
    case 93: uxrom_reset(); hw_cart.m.ctrl = 1; break;   /* CHR RAM enabled at power-on */
    case 97: map_prg16(0, -1); map_prg16(1, 0); map_chr8(0); break;
    case 86: case 101: nrom_reset(); break;
    case 77: map_prg32(0); irem77_chr(0); break;
    case 96: map_prg32(0); hw_cart.m.pattern_addr = 0; oeka_apply(); break;
    case 152: uxrom_reset(); hw_cart.mirroring = HW_MIRROR_SCREEN_A; break;
    case 184: nrom_reset(); map_chr4(1, 4); break;
    case 232: map_prg16(0, 0); map_prg16(1, 3); map_chr8(0); break;
    case 1: case 155: mmc1_reset(); break;
    case 2:  uxrom_reset(); break;
    case 3:  cnrom_reset(); break;
    case 4: case 118: case 119: mmc3_reset(); break;
    case 7:  axrom_reset(); break;
    case 66: gxrom_reset(); break;
    default: nrom_reset(); break;
    }
}

void hw_cart_cpu_write(uint16_t addr, uint8_t value)
{
    if (hw_cart.mapper==20) { fds_cpu_write(addr,value); return; }
    /* NTDEC 2722: decoded A15:A13, no bus conflicts. The enable write
     * starts the counter; only $8000 clears it and acknowledges IRQ. */
    if (hw_cart.mapper==40) {
        if (addr>=0x8000) switch (addr&0xe000) {
        case 0x8000: hw_cart.m.irq_enable=hw_cart.m.irq_out=0; hw_cart.m.irq_counter16=0; break;
        case 0xa000:
            if (!hw_cart.m.irq_enable) hw_cart.m.last_write_cycle=hw.cycles+1;
            hw_cart.m.irq_enable=1; break;
        case 0xe000: hw_cart.m.prg=value&7; map_prg8(2,value&7); break;
        }
        return;
    }
    if (hw_cart.mapper==5) { mmc5_write(addr,value); return; }
    if (hw_cart.mapper==69 && addr>=0x8000) { fme7_write(addr,value); return; }
    if (hw_cart.mapper==80 || hw_cart.mapper==207 || hw_cart.mapper==82 || hw_cart.mapper==552) {
        if (addr >= 0x7ef0 && addr <= 0x7eff) taito_x1_write(addr, value);
        else if (addr >= 0x6000 && addr < 0x8000) {
            int i = taito_x1_ram(addr);
            if (i >= 0 && hw_cart.has_wram) hw_cart.wram[(unsigned)i % hw_cart.wram_len] = value;
        }
        return;
    }
    if (hw_cart.mapper==19 || hw_cart.mapper==210) {
        if (addr >= 0x8000) namco_write(addr, value);
        else if (addr >= 0x6000) {
            bool ok = hw_cart.mapper==19 ? namco_write_ram(addr) : hw_cart.wram_writable;
            if (hw_cart.has_wram && ok) hw_cart.wram[(hw_cart.wram_bank + (addr & 0x1FFF)) % hw_cart.wram_len] = value;
        } else if (hw_cart.mapper==19 && addr >= 0x4800) namco_low_write(addr, value);
        return;
    }
    if (bandai_board()) { bandai_write(addr,value); return; }
    if (vrc24_board() && addr >= 0x6000 && addr < 0x8000) {
        if (vrc2_board() && !hw_cart.has_wram) {
            if (addr < 0x7000) hw_cart.m.latch = value & 1;
            return;
        }
        if (!vrc2_board() && hw_cart.wram_len == 2048 && addr >= 0x7000) return;
    }
    /* Mapper 41: https://www.nesdev.org/wiki/INES_Mapper_041. The outer
     * register latches address bits at $6000-$67FF (data ignored): A0-A2 the
     * 32 KiB PRG bank, A3-A4 the outer 32 KiB CHR bank, A5 mirroring
     * (1 = horizontal). A2 also enables the inner 8 KiB CHR select at
     * $8000-$FFFF, which conflicts with the ROM (AND). */
    if (hw_cart.mapper == 41) {
        if ((addr & 0xf800) == 0x6000) {
            hw_cart.m.prg = addr & 7;
            hw_cart.m.chr0 = (addr >> 3) & 3;
            hw_cart.mirroring = (addr & 0x20) ? HW_MIRROR_HORIZONTAL : HW_MIRROR_VERTICAL;
        } else if (addr >= 0x8000 && (hw_cart.m.prg & 4)) {
            hw_cart.m.latch = value & hw_cart_prg_read(addr) & 3;
        } else return;
        map_prg32(hw_cart.m.prg);
        map_chr8(hw_cart.m.chr0 << 2 | hw_cart.m.latch);
        return;
    }
    /* Mapper 31: https://www.nesdev.org/wiki/INES_Mapper_031 */
    if (hw_cart.mapper == 31 && (addr & 0xf000) == 0x5000) {
        hw_cart.m.reg[addr & 7] = value;
        map_prg4(addr & 7, value);
        return;
    }
    if (hw_cart.mapper == 34) {
        if (!(hw_cart.info.prg_size ? nes_cart_nina(&hw_cart.info) : hw_cart.chr_pages > 8)) {
            if (addr >= 0x8000) {
                hw_cart.m.latch = value & hw_cart_prg_read(addr);
                map_prg32(hw_cart.m.latch);
            }
        } else {
            if (addr == 0x7ffd) map_prg32(value & 1);
            if (addr == 0x7ffe) map_chr4(0, value & 15);
            if (addr == 0x7fff) map_chr4(1, value & 15);
        }
    }
    if ((hw_cart.mapper == 79 || hw_cart.mapper == 146) && (addr & 0xe100) == 0x4100) {
        hw_cart.m.latch = value;
        map_prg32((value >> 3) & 1);
        map_chr8(value & 7);
        return;
    }
    if (hw_cart.mapper == 87 && addr >= 0x6000 && addr < 0x8000) {
        map_chr8(((value & 1) << 1) | ((value & 2) >> 1));
        return;
    }
    /* Jaleco JF-13 (mapper 86): $6000-$6FFF [.CPP ..CC] 32 KiB PRG, 8 KiB CHR
     * (bit 6 the high CHR bit); $7000-$7FFF drives the uPD7756 speech chip,
     * which is not modeled; no PRG RAM, no bus conflicts. */
    if (hw_cart.mapper == 86 && addr >= 0x6000 && addr < 0x8000) {
        if (addr < 0x7000) { map_prg32((value >> 4) & 3); map_chr8((value & 3) | ((value >> 4) & 4)); }
        return;
    }
    /* Mapper 101 (a JF-10 misdump): 8 KiB CHR at $6000-$7FFF, bits in order. */
    if (hw_cart.mapper == 101 && addr >= 0x6000 && addr < 0x8000) { map_chr8(value); return; }
    if (hw_cart.mapper == 113 && (addr & 0xe100) == 0x4100) {
        hw_cart.m.latch = value;
        map_prg32((value >> 3) & 7);
        map_chr8((value & 7) | ((value >> 3) & 8));
        hw_cart.mirroring = (value & 0x80) ? HW_MIRROR_VERTICAL : HW_MIRROR_HORIZONTAL;
        return;
    }
    if (hw_cart.mapper == 140 && addr >= 0x6000 && addr < 0x8000) {
        map_prg32((value >> 4) & 3);
        map_chr8(value & 15);
        return;
    }
    if (hw_cart.mapper == 184 && addr >= 0x6000 && addr < 0x8000) {
        map_chr4(0, value & 7);
        map_chr4(1, ((value >> 4) & 3) | 4);
        return;
    }
    if (addr >= 0x6000 && addr < 0x8000) {
        if (hw_cart.has_wram && hw_cart.wram_writable)
            hw_cart_wram_store((hw_cart.wram_bank + (addr & 0x1FFF)) % hw_cart.wram_len, value);
        return;
    }
    if (addr < 0x8000) return;   /* low-address registers were handled above */
    if ((hw_cart.mapper == 2 || hw_cart.mapper == 3 || hw_cart.mapper == 7) &&
        hw_cart.info.submapper == 2) value &= hw_cart_prg_read(addr);
    switch (hw_cart.mapper) {
    case 21: case 22: case 23: case 25: vrc24_write(addr, value); break;
    case 85: vrc7_write(addr,value); break;
    case 24: case 26: vrc6_write(addr, value); break;
    case 73: vrc3_write(addr, value); break;
    case 9: case 10: mmc2_write(addr, value); break;
    case 11: case 144: /* Color Dreams; see MAPPERS.md. */
        /* 144 (Death Race): a resistor on D0 lets the ROM's bit 0 win (nesdev INES Mapper 144). */
        if (hw_cart.mapper == 144) value |= 1;
        value &= hw_cart_prg_read(addr);
        hw_cart.m.latch = value;
        map_prg32(value & 3);
        map_chr8(value >> 4);
        break;
    case 13: /* CPROM; see MAPPERS.md. */
        value &= hw_cart_prg_read(addr);
        hw_cart.m.latch = value;
        map_chr4(1, value & 3);
        break;
    case 71: /* Camerica; see MAPPERS.md. */
        if (addr >= 0xc000) map_prg16(0, value & 15);
        else if ((!hw_cart.info.nes2 && addr >= 0x9000 && addr < 0xa000) ||
                 (hw_cart.info.nes2 && hw_cart.info.submapper == 1 && addr < 0xa000))
            hw_cart.mirroring = (value & 0x10) ? HW_MIRROR_SCREEN_B : HW_MIRROR_SCREEN_A;
        break;
    case 75: /* VRC1; see MAPPERS.md. */
        switch (addr & 0xf000) {
        case 0x8000: map_prg8(0, value & 15); break;
        case 0xa000: map_prg8(1, value & 15); break;
        case 0xc000: map_prg8(2, value & 15); break;
        case 0x9000:
            hw_cart.m.ctrl = value;
            hw_cart.mirroring = (value & 1) ? HW_MIRROR_HORIZONTAL : HW_MIRROR_VERTICAL;
            break;
        case 0xe000: hw_cart.m.chr0 = value & 15; break;
        case 0xf000: hw_cart.m.chr1 = value & 15; break;
        }
        map_chr4(0, hw_cart.m.chr0 | ((hw_cart.m.ctrl & 2) << 3));
        map_chr4(1, hw_cart.m.chr1 | ((hw_cart.m.ctrl & 4) << 2));
        break;
    case 206: case 88: case 95: case 154: /* Namco 108 family; see MAPPERS.md. */
        /* 154's one-screen select answers at all of $8000-$FFFF (nesdev INES Mapper 154). */
        if (hw_cart.mapper == 154) hw_cart.mirroring = (value & 0x40) ? HW_MIRROR_SCREEN_B : HW_MIRROR_SCREEN_A;
        if (addr < 0xa000) {
            if (!(addr & 1)) hw_cart.m.bank_select = value & 7;
            else {
                unsigned r = hw_cart.m.bank_select;
                hw_cart.m.reg[r] = value & (r < 6 ? 63 : 15);
            }
            mmc3_apply(); /* mode bits are absent, so both modes stay zero */
        }
        break;
    case 76: /* Namco 109; see MAPPERS.md. */
        if (addr < 0xa000) {
            if (!(addr & 1)) hw_cart.m.bank_select = value & 7;
            else hw_cart.m.reg[hw_cart.m.bank_select] = value & 63;
            map_prg8(0, hw_cart.m.reg[6] & 15);
            map_prg8(1, hw_cart.m.reg[7] & 15);
            for (unsigned i = 0; i < 4; ++i) map_chr2(i, hw_cart.m.reg[i + 2]);
        }
        break;
    case 94: /* UN1ROM; see MAPPERS.md. */
        value &= hw_cart_prg_read(addr);
        hw_cart.m.latch = value;
        map_prg16(0, (value >> 2) & 7);
        break;
    case 180: /* Crazy Climber; see MAPPERS.md. */
        value &= hw_cart_prg_read(addr);
        hw_cart.m.latch = value;
        map_prg16(1, value & 7);
        break;
    case 78: /* Irem 74HC161 / Jaleco JF-16; see MAPPERS.md. */
        value &= hw_cart_prg_read(addr);
        hw_cart.m.latch = value;
        map_prg16(0, value & 7);
        map_chr8(value >> 4);
        if (hw_cart.info.submapper == 3)
            hw_cart.mirroring = (value & 8) ? HW_MIRROR_VERTICAL : HW_MIRROR_HORIZONTAL;
        else hw_cart.mirroring = (value & 8) ? HW_MIRROR_SCREEN_B : HW_MIRROR_SCREEN_A;
        break;
    case 89: /* Sunsoft-2 on the Sunsoft-3 board; see MAPPERS.md. */
        value &= hw_cart_prg_read(addr);
        hw_cart.m.latch = value;
        map_prg16(0, (value >> 4) & 7);
        map_chr8((value & 7) | ((value >> 4) & 8));
        hw_cart.mirroring = (value & 8) ? HW_MIRROR_SCREEN_B : HW_MIRROR_SCREEN_A;
        break;
    case 93: /* Sunsoft-2 on the Sunsoft-3R board; see MAPPERS.md. */
        value &= hw_cart_prg_read(addr);
        hw_cart.m.latch = value;
        map_prg16(0, (value >> 4) & 7);
        hw_cart.m.ctrl = value & 1;               /* CHR RAM enable */
        for (unsigned i = 0; i < 8; ++i) hw_cart.chr_write[i] = hw_cart.chr_ram && hw_cart.m.ctrl;
        break;
    case 97: /* Irem TAM-S1; see MAPPERS.md. */
        if (addr < 0xc000) {
            hw_cart.m.latch = value;
            map_prg16(1, value & 31);
            hw_cart.mirroring = (value & 0x80) ? HW_MIRROR_VERTICAL : HW_MIRROR_HORIZONTAL;
        }
        break;
    case 77: /* Irem LROG017; see MAPPERS.md. */
        value &= hw_cart_prg_read(addr);
        hw_cart.m.latch = value;
        map_prg32(value & 15);
        irem77_chr(value >> 4);
        break;
    case 96: /* Oeka Kids; see MAPPERS.md. */
        value &= hw_cart_prg_read(addr);
        hw_cart.m.latch = value;
        map_prg32(value & 3);
        hw_cart.m.ctrl = value & 4;
        oeka_apply();
        break;
    case 148: /* Sachen SA-008-A / Tengen 800008; see MAPPERS.md. */
        value &= hw_cart_prg_read(addr);
        hw_cart.m.latch = value;
        map_prg32((value >> 3) & 1);
        map_chr8(value & 7);
        break;
    case 72: case 92: /* Jaleco JF-17 / JF-19; see MAPPERS.md. */
        value &= hw_cart_prg_read(addr);
        if ((value & 0x80) && !(hw_cart.m.latch & 0x80)) map_prg16(hw_cart.mapper == 92 ? 1 : 0, value & 15);
        if ((value & 0x40) && !(hw_cart.m.latch & 0x40)) map_chr8(value & 15);
        hw_cart.m.latch = value;
        break;
    case 70: case 152: /* Bandai 74161/7432; see MAPPERS.md. */
        value &= hw_cart_prg_read(addr);
        hw_cart.m.latch = value;
        map_prg16(0, (value >> 4) & (hw_cart.mapper == 70 ? 15 : 7));
        map_chr8(value & 15);
        if (hw_cart.mapper == 152)
            hw_cart.mirroring = (value & 0x80) ? HW_MIRROR_SCREEN_B : HW_MIRROR_SCREEN_A;
        break;
    case 232: /* Camerica Quattro; see MAPPERS.md. */
        if (addr < 0xc000) hw_cart.m.ctrl = hw_cart.info.submapper == 1 ?
            ((value & 8) | ((value >> 2) & 4)) : (value >> 1) & 12;
        else hw_cart.m.prg = value & 3;
        map_prg16(0, hw_cart.m.ctrl | hw_cart.m.prg);
        map_prg16(1, hw_cart.m.ctrl | 3);
        break;
    case 1: case 155: mmc1_write(addr, value); break;
    case 2:  uxrom_write(value); break;
    case 3:  cnrom_write(value); break;
    case 4: case 118: case 119: mmc3_write(addr, value); break;
    case 68: sunsoft4_write(addr, value); break;
    case 67: sunsoft3_write(addr, value); break;
    case 185: hw_cart.m.latch = value & hw_cart_prg_read(addr); break;
    case 18: jaleco_write(addr, value); break;
    case 64: case 158: rambo_write(addr, value); break;
    case 33: case 48: taito_write(addr, value); break;
    case 32: irem_g101_write(addr, value); break;
    case 65: irem_h3001_write(addr, value); break;
    case 228: action52_write(addr, value); break;
    case 7:  axrom_write(value); break;
    case 66: gxrom_write(value); break;
    default: break;              /* NROM: the ROM ignores writes */
    }
}

bool hw_cart_cpu_read(uint16_t addr, uint8_t *value)
{
    if (hw_cart.mapper==20) return fds_cpu_read(addr,value);
    if (hw_cart.mapper==40 && addr>=0x6000 && addr<0x8000) {
        *value=hw_cart.prg[((12u&(hw_cart.prg_slots-1))*4096)+(addr&8191)]; return true;
    }
    if (hw_cart.mapper==5) return mmc5_read(addr,value);
    if (hw_cart.mapper==69) return addr>=0x6000 && addr<0x8000 && fme7_read(addr,value);
    if (hw_cart.mapper==80 || hw_cart.mapper==207 || hw_cart.mapper==82 || hw_cart.mapper==552) {
        int i = addr >= 0x6000 && addr < 0x8000 ? taito_x1_ram(addr) : -1;
        if (i < 0 || !hw_cart.has_wram) return false;
        *value = hw_cart.wram[(unsigned)i % hw_cart.wram_len];
        return true;
    }
    if (hw_cart.mapper==19 && addr >= 0x4800 && addr < 0x6000) return namco_low_read(addr, value);
    if (bandai_board()) return bandai_read(addr,value);
    if (vrc24_board() && addr >= 0x6000 && addr < 0x8000) {
        if (vrc2_board() && !hw_cart.has_wram) {
            if (addr >= 0x7000) return false;
            *value = (*value & 0xfe) | hw_cart.m.latch;
            return true;
        }
        if (!vrc2_board() && hw_cart.wram_len == 2048 && addr >= 0x7000) return false;
    }
    if (addr >= 0x6000 && addr < 0x8000 && hw_cart.has_wram && hw_cart.wram_readable) {
        *value = hw_cart.wram[(hw_cart.wram_bank + (addr & 0x1FFF)) % hw_cart.wram_len];
        return true;
    }
    return false;
}

void hw_cart_ppu_addr_watched(uint16_t vbus)
{
    if (hw_cart.mapper==1 || hw_cart.mapper==155) {
        unsigned a12=(vbus>>12)&1;
        if (hw_cart.m.a12!=a12) {
            hw_cart.m.a12=(uint8_t)a12;
            /* A12 selects a CHR register for SxROM's CPU-side C/D/E
             * signals (bits 2..4). The CHR windows themselves always use
             * their own registers. Keep the pin for later CPU writes,
             * but rebuild tables only when a selected signal can change.
             * In 8 KiB mode CHR0 supplies both phases. */
            if ((hw_cart.m.ctrl & MMC1_CTRL_CHR_4K) &&
                ((hw_cart.m.chr0 ^ hw_cart.m.chr1) & 0x1C)) mmc1_apply();
        }
    } else if (hw_cart.mapper == 4 || hw_cart.mapper == 118 || hw_cart.mapper == 119 || hw_cart.mapper == 48) mmc3_ppu_addr(vbus);
    else if (hw_cart.mapper==64 || hw_cart.mapper==158) rambo_ppu_addr(vbus);
    else if (hw_cart.mapper==96) oeka_ppu_addr(vbus);
    else if (hw_cart.mapper==153 || hw_cart.mapper==157) bandai_ppu_addr(vbus);
}

bool hw_cart_irq(void) {
    if (hw_cart.mapper==20) return fds_irq();
    if (hw_cart.mapper==5) {
        const Mmc5State *m=&hw_cart.m.mmc5;
        return (m->irq_enable && m->irq_pending) || m->timer_irq || (m->pcm_irq && (m->pcm_control&128));
    }
    return hw_cart.m.irq_out != 0;
}

void hw_cart_cpu_clock_late(void)
{
    if (hw_cart.mapper==20) fds_cpu_clock();
}

void hw_cart_cpu_clock(void)
{
    if (hw_cart.mapper==40) {
        if (hw_cart.m.irq_enable && hw.cycles!=hw_cart.m.last_write_cycle &&
            hw_cart.m.irq_counter16<4096 && ++hw_cart.m.irq_counter16==4096) hw_cart.m.irq_out=1;
        return;
    }
    if (hw_cart.mapper==5) { mmc5_clock(); return; }
    if (hw_cart.mapper==69) { fme7_clock(); return; }
    if (hw_cart.mapper==19) { namco_clock(); return; }
    if (hw_cart.mapper==18) { jaleco_clock(); return; }
    if (hw_cart.mapper==48) { tc0690_cpu_clock(); return; }
    if (hw_cart.mapper==64 || hw_cart.mapper==158) { rambo_cpu_clock(); return; }
    if (hw_cart.mapper==65) { irem_h3001_clock(); return; }
    if (hw_cart.mapper==67) { sunsoft3_clock(); return; }
    if (bandai_board()) { bandai_clock(); return; }
    if (hw_cart.mapper==85) vrc7_audio_clock();
    if (vrc6_board()) vrc6_audio_clock();
    if (hw_cart.mapper == 73) vrc3_clock();
    else if (hw_cart.watch_cpu) vrc_irq_clock();
}

/* ------------------------------------------------------------------------- */
/* Cartridge nametables and expansion audio. */
uint16_t hw_cart_nt_a10(uint16_t addr)
{
    if (hw_cart.mapper == 118) return txsrom_ciram_a10(addr);
    if (hw_cart.mapper == 158) return rambo158_ciram_a10(addr);
    /* NAMCOT-3425 (mapper 95): CHR A15 of R0 (upper nametables) or R1 (lower) is CIRAM A10. */
    if (hw_cart.mapper == 95) return (hw_cart.m.reg[(addr >> 11) & 1] & 0x20) ? 0x400 : 0;
    if (hw_cart.mapper == 19) return namco_nt_a10(addr);
    if (hw_cart.mapper == 207) return x1005_207_ciram_a10(addr);
    return (vrc6_nt_bank((addr >> 10) & 3) & 1) << 10;
}
bool hw_cart_nt_read(uint16_t addr, bool read_bus, uint8_t *value)
{
    if (hw_cart.mapper==5) { if (read_bus) *value=mmc5_nt_read(addr,true); return true; }
    if (hw_cart.mapper==68) {
        if (!(hw_cart.m.ctrl & 0x10)) return false;
        if (read_bus) *value = hw_cart.chr[sunsoft4_nt_index(addr)];
        return true;
    }
    if (hw_cart.mapper==19) {
        if (!namco_nt_is_rom(addr)) return false;
        if (read_bus) *value = hw_cart.chr[namco_nt_index(addr)];
        return true;
    }
    if (!vrc6_board() || !(hw_cart.m.ctrl & 0x10)) return false;
    if (read_bus) {
        unsigned index = (vrc6_nt_bank((addr >> 10) & 3) % hw_cart.chr_pages)*1024 + (addr & 1023);
        *value = hw_cart.chr[hw_cart.chr_ram ? index % hw_cart.chr_len : index];
    }
    return true;
}
bool hw_cart_nt_write(uint16_t addr, uint8_t value)
{
    if (hw_cart.mapper==5) { mmc5_nt_write(addr,value); return true; }
    if (hw_cart.mapper==68) return (hw_cart.m.ctrl & 0x10) != 0; /* ROM: CIRAM deselected */
    if (hw_cart.mapper==19) return namco_nt_is_rom(addr);
    if (!vrc6_board() || !(hw_cart.m.ctrl & 0x10)) return false;
    if (hw_cart.chr_ram) {
        unsigned index = vrc6_nt_bank((addr >> 10) & 3)*1024 + (addr & 1023);
        hw_cart.chr[index % hw_cart.chr_len] = value;
    }
    return true;
}
double hw_cart_audio_level(void)
{
    /* Nominal inverted linear DAC: one 15-level pulse ~= one 2A03 pulse.
     * Cartridge mixer resistor tolerances are not modeled. */
    if (hw_cart.mapper==5) return mmc5_audio();
    if (hw_cart.mapper==85) return -(double)hw_cart.m.vrc7_output / 32768.0;
    if (hw_cart.mapper==69) return s5b_output();
    if (hw_cart.mapper==19) return namco_audio();
    if (hw_cart.mapper==20) return fds_audio_level();
    return vrc6_board() ? -(double)vrc6_audio_dac() * (0.1488 / 15.0) : 0;
}

bool hw_cart_famicom_only(void)
{
    /* Boards whose expansion audio exists only on Famicom hardware: the RAM
     * Adapter (20), Namco 163/129 (19), VRC6 (24, 26), VRC7 (85). None was
     * sold for the NES, and the NES cartridge slot has no audio return, so
     * their sound is only heard through a Famicom. MMC5 (5) and FME-7/5B
     * (69) also have NES boards (NES-ETROM, NES FME-7 titles) and stay NES. */
    int m = hw_cart.mapper;
    return m == 20 || m == 19 || m == 24 || m == 26 || m == 85;
}

/* Comparison                                                                */
/* ------------------------------------------------------------------------- */

uint64_t hw_cart_state_hash(uint64_t h)
{
    uint64_t acc = 0;
    for (int i = 0; i < 8; i++) acc = acc * 131 + hw_cart.prg_off[i];
    for (int i = 0; i < 8; i++) acc = acc * 131 + hw_cart.chr_off[i];
    acc = acc * 131 + hw_cart.mirroring;
    acc = acc * 131 + hw_cart.wram_readable;
    acc = acc * 131 + hw_cart.wram_writable;
    acc = acc * 131 + hw_cart.m.shift;
    acc = acc * 131 + hw_cart.m.shift_count;
    acc = acc * 131 + hw_cart.m.ctrl;
    acc = acc * 131 + hw_cart.m.chr0;
    acc = acc * 131 + hw_cart.m.chr1;
    acc = acc * 131 + hw_cart.m.prg;
    acc = acc * 131 + hw_cart.m.bank_select;
    for (int i = 0; i < 8; i++) acc = acc * 131 + hw_cart.m.reg[i];
    acc = acc * 131 + hw_cart.m.mirror_reg;
    acc = acc * 131 + hw_cart.m.ram_protect;
    acc = acc * 131 + hw_cart.m.irq_latch;
    acc = acc * 131 + hw_cart.m.irq_counter;
    acc = acc * 131 + hw_cart.m.irq_reload;
    acc = acc * 131 + hw_cart.m.irq_enable;
    acc = acc * 131 + hw_cart.m.irq_out;
    acc = acc * 131 + hw_cart.m.a12;
    acc = acc * 131 + hw_cart.m.latch;
    if (hw_cart.mapper == 9 || hw_cart.mapper == 10 || hw_cart.mapper == 96) {
        acc = acc * 131 + hw_cart.m.pattern_pending;
        acc = acc * 131 + hw_cart.m.pattern_addr;
    }
    if (hw_cart.mapper==40 || bandai_board() || vrc24_board() || hw_cart.mapper == 73 || vrc6_board() || hw_cart.mapper == 85 ||
        hw_cart.mapper==80 || hw_cart.mapper==207 || hw_cart.mapper==82 || hw_cart.mapper==552 || hw_cart.mapper==65 || hw_cart.mapper==67) {
        for (unsigned i=0; i<8; ++i) acc = acc*131 + hw_cart.m.vrc_chr[i];
        acc = acc*131 + hw_cart.m.irq_latch16;
        acc = acc*131 + hw_cart.m.irq_counter16;
        acc = acc*131 + hw_cart.m.irq_prescaler;
        acc = acc*131 + hw_cart.m.irq_mode;
        acc = acc*131 + hw_cart.m.last_write_cycle;
    }
    if (vrc6_board()) {
        const HwVrc6Audio *a = &hw_cart.m.vrc6_audio;
        for (unsigned ch=0; ch<3; ++ch) {
            for (unsigned r=0; r<3; ++r) acc=acc*131+a->reg[ch][r];
            acc=acc*131+a->timer[ch]; acc=acc*131+a->step[ch];
        }
        acc=acc*131+a->control; acc=acc*131+a->accumulator;
    }
    if (hw_cart.mapper==5) {
        const uint8_t *bytes=(const uint8_t *)&hw_cart.m.mmc5;
        for (unsigned i=0;i<sizeof(Mmc5State);++i) acc=acc*131+bytes[i];
    }
    if (hw_cart.mapper==85) acc=vrc7_sound_hash(acc);
    if (hw_cart.mapper==20) acc=fds_state_hash(acc);
    if (hw_cart.mapper==19 || hw_cart.mapper==210) {
        const uint8_t *b=(const uint8_t *)&hw_cart.m.namco;
        for (unsigned i=0;i<sizeof(HwNamco);++i) acc=acc*131+b[i];
        acc=acc*131+hw_cart.chr_ciram;
    }
    if (hw_cart.mapper==64 || hw_cart.mapper==158) {
        const uint8_t *b=(const uint8_t *)&hw_cart.m.rambo;
        for (unsigned i=0;i<sizeof(HwRambo);++i) acc=acc*131+b[i];
    }
    if (hw_cart.mapper==18) {
        const uint8_t *b=(const uint8_t *)&hw_cart.m.jaleco;
        for (unsigned i=0;i<sizeof(HwJaleco);++i) acc=acc*131+b[i];
    }
    if (hw_cart.mapper==69) {
        const uint8_t *b=(const uint8_t *)&hw_cart.m.fme7;
        for (unsigned i=0;i<sizeof(HwFme7);++i) acc=acc*131+b[i];
        b=(const uint8_t *)&hw_cart.m.s5b;
        for (unsigned i=0;i<sizeof(Hw5B);++i) acc=acc*131+b[i];
    }
    if (bandai_board()) {
        /* Chip padding starts zero and all fields have deterministic reset. */
        const unsigned char *b=(const unsigned char *)hw_cart.eeprom;
        for (unsigned i=0;i<sizeof(hw_cart.eeprom);++i) acc=acc*131+b[i];
        if (hw_cart.mapper==157) {
            b=(const unsigned char *)&hw_cart.barcode;
            for (unsigned i=0;i<sizeof(hw_cart.barcode);++i) acc=acc*131+b[i];
        }
    }
    /* Work RAM is a memory a program can read back, so it is compared across
     * implementations in cyc_mem_hash, not here. */
    return cyc_trace_mix(h, acc);
}

void hw_cart_state_dump(void *file)
{
    FILE *f = (FILE *)file;
    if (hw_cart.mapper==20) { fds_state_dump(file); fds_audio_state_dump(file); }
    if (hw_cart.mapper==69) {
        const HwFme7 *m=&hw_cart.m.fme7; const Hw5B *s=&hw_cart.m.s5b;
        fprintf(f,"cart.fme7.command %X\ncart.fme7.prg %02X %02X %02X %02X\ncart.fme7.mirror %u\n"
                  "cart.fme7.irq_ctrl %02X\ncart.fme7.counter %04X\n",m->command,m->prg[0],m->prg[1],
                m->prg[2],m->prg[3],m->mirror,m->irq_ctrl,m->counter);
        for (unsigned i=0;i<8;++i) fprintf(f,"cart.fme7.chr[%u] %02X\n",i,m->chr[i]);
        fprintf(f,"cart.5b.address %02X\ncart.5b.env %u up=%u hold=%u count=%u\ncart.5b.lfsr %05X\n",
                s->address,s->env_level,s->env_up,s->env_hold,s->env_count,(unsigned)s->lfsr);
        for (unsigned r=0;r<16;++r) fprintf(f,"cart.5b.reg%X %02X\n",r,s->reg[r]);
    }
    if (hw_cart.mapper==85) {
        fprintf(f,"cart.vrc7.address %02X\ncart.vrc7.phase %llu\ncart.vrc7.output %d\n",
            hw_cart.m.vrc7_address,(unsigned long long)hw_cart.m.vrc7_phase,hw_cart.m.vrc7_output);
        for (unsigned r=0;r<64;++r) fprintf(f,"cart.vrc7.reg%02X %02X\n",r,hw_cart.m.vrc7_reg[r]);
    }
    if (vrc6_board()) {
        const HwVrc6Audio *a=&hw_cart.m.vrc6_audio;
        fprintf(f,"cart.vrc6.control %u\ncart.vrc6.accumulator %u\n",a->control,a->accumulator);
        for (unsigned ch=0;ch<3;++ch)
            fprintf(f,"cart.vrc6.ch%u %02X %02X %02X timer=%u step=%u\n",ch,
                    a->reg[ch][0],a->reg[ch][1],a->reg[ch][2],a->timer[ch],a->step[ch]);
    }
    if (hw_cart.mapper==40 || bandai_board() || vrc24_board() || hw_cart.mapper == 73 || vrc6_board() || hw_cart.mapper == 85 || hw_cart.mapper == 65 || hw_cart.mapper == 67) {
        for (unsigned i=0; i<8; ++i) fprintf(f, "cart.vrc_chr[%u] %03X\n", i, hw_cart.m.vrc_chr[i]);
        fprintf(f, "cart.irq_latch16 %04X\ncart.irq_counter16 %04X\ncart.irq_prescaler %d\ncart.irq_mode %u\n",
                hw_cart.m.irq_latch16, hw_cart.m.irq_counter16, hw_cart.m.irq_prescaler, hw_cart.m.irq_mode);
    }
    if (hw_cart.mapper == 9 || hw_cart.mapper == 10)
        fprintf(f, "cart.pattern_pending %u\ncart.pattern_addr %04X\n",
                hw_cart.m.pattern_pending, hw_cart.m.pattern_addr);
    for (int i = 0; i < 8; i++) fprintf(f, "cart.prg_off[%d] %06X\n", i, hw_cart.prg_off[i]);
    for (int i = 0; i < 8; i++) fprintf(f, "cart.chr_off[%d] %06X\n", i, hw_cart.chr_off[i]);
    fprintf(f, "cart.mirroring %02X\ncart.wram_readable %02X\ncart.wram_writable %02X\n", hw_cart.mirroring,
            hw_cart.wram_readable, hw_cart.wram_writable);
    fprintf(f, "cart.shift %02X\ncart.shift_count %02X\ncart.ctrl %02X\ncart.chr0 %02X\ncart.chr1 %02X\n"
               "cart.prg %02X\n",
            hw_cart.m.shift, hw_cart.m.shift_count, hw_cart.m.ctrl, hw_cart.m.chr0, hw_cart.m.chr1, hw_cart.m.prg);
    fprintf(f, "cart.bank_select %02X\n", hw_cart.m.bank_select);
    for (int i = 0; i < 8; i++) fprintf(f, "cart.reg[%d] %02X\n", i, hw_cart.m.reg[i]);
    fprintf(f, "cart.mirror_reg %02X\ncart.ram_protect %02X\ncart.irq_latch %02X\ncart.irq_counter %02X\n"
               "cart.irq_reload %02X\ncart.irq_enable %02X\ncart.irq_out %02X\ncart.a12 %02X\ncart.latch %02X\n",
            hw_cart.m.mirror_reg, hw_cart.m.ram_protect, hw_cart.m.irq_latch, hw_cart.m.irq_counter,
            hw_cart.m.irq_reload, hw_cart.m.irq_enable, hw_cart.m.irq_out, hw_cart.m.a12, hw_cart.m.latch);
}
