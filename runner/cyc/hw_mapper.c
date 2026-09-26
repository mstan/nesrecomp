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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

enum { MMC1_CTRL_MIRROR = 3, MMC1_CTRL_PRG_MODE = 0x0C, MMC1_CTRL_CHR_4K = 0x10 };

static void mmc1_apply(void)
{
    static const uint8_t mirror[4] = { HW_MIRROR_SCREEN_A, HW_MIRROR_SCREEN_B, HW_MIRROR_VERTICAL,
                                       HW_MIRROR_HORIZONTAL };
    hw_cart.mirroring = mirror[hw_cart.m.ctrl & MMC1_CTRL_MIRROR];

    int prg = hw_cart.m.prg & 0x0F;
    switch ((hw_cart.m.ctrl & MMC1_CTRL_PRG_MODE) >> 2) {
    case 0: case 1: map_prg32(prg >> 1); break;          /* 32KB, ignoring bit 0 */
    case 2: map_prg16(0, 0); map_prg16(1, prg); break;   /* first bank fixed at $8000 */
    default: map_prg16(0, prg); map_prg16(1, -1); break; /* last bank fixed at $C000 */
    }

    if (hw_cart.info.submapper == 5) map_prg32(0);

    if (hw_cart.m.ctrl & MMC1_CTRL_CHR_4K) {
        map_chr4(0, hw_cart.m.chr0 & 0x1F);
        map_chr4(1, hw_cart.m.chr1 & 0x1F);
    } else {
        map_chr8((hw_cart.m.chr0 & 0x1F) >> 1);
    }
    /* Bit 4 of the PRG register disables work RAM on boards that have it. */
    hw_cart.wram_readable = hw_cart.wram_writable = (hw_cart.m.prg & 0x10) == 0;
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
    map_chr2(big / 2 + 0, r[0] >> 1);
    map_chr2(big / 2 + 1, r[1] >> 1);
    map_chr1(small + 0, r[2]);
    map_chr1(small + 1, r[3]);
    map_chr1(small + 2, r[4]);
    map_chr1(small + 3, r[5]);
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
        hw_cart.m.mirror_reg = value;
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

static void mmc3_clock_irq(void)
{
    if (hw_cart.m.irq_counter == 0 || hw_cart.m.irq_reload) hw_cart.m.irq_counter = hw_cart.m.irq_latch;
    else hw_cart.m.irq_counter--;
    if (hw_cart.m.irq_counter == 0 && hw_cart.m.irq_enable) hw_cart.m.irq_out = 1;
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
/* Dispatch                                                                  */
/* ------------------------------------------------------------------------- */

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

uint8_t hw_cart_chr_read(uint16_t addr)
{
    uint8_t value = hw_cart.chr[hw_cart_chr_index(addr)];
    if ((hw_cart.mapper == 9 || hw_cart.mapper == 10) && !hw_cart.m.pattern_pending) {
        hw_cart.m.pattern_pending = 1;
        hw_cart.m.pattern_addr = addr;
    }
    return value;
}

void hw_cart_ppu_rd(bool reading)
{
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

static const struct {
    int         mapper;
    const char *name;
    uint8_t     watch_ppu_addr;
    uint8_t     wram;            /* boards for this mapper carry work RAM */
} MAPPERS[] = {
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
    { 9, "MMC2", 0, 0 },
    { 10, "MMC4", 0, 1 },
    { 232, "Camerica Quattro", 0, 0 },
    { 184, "Sunsoft-1", 0, 0 },
    { 180, "Crazy Climber", 0, 0 },
    { 140, "Jaleco JF-11/14", 0, 0 },
    { 113, "HES", 0, 0 },
    { 94, "UN1ROM", 0, 0 },
    { 87, "J87", 0, 0 },
    { 79, "NINA-003/006", 0, 0 },
    { 76, "Namco 109", 0, 0 },
    { 206, "DxROM", 0, 0 },
    { 75, "VRC1", 0, 0 },
    { 71, "Camerica", 0, 0 },
    { 34, "BNROM / NINA-001", 0, 0 },
    { 13, "CPROM", 0, 0 },
    { 11, "Color Dreams", 0, 0 },
    { 0,  "NROM",  0, 0 },
    { 1,  "MMC1",  0, 1 },
    { 2,  "UxROM", 0, 0 },
    { 3,  "CNROM", 0, 0 },
    { 4,  "MMC3",  1, 1 },
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
    if (hw_cart.mapper!=153 || hw_cart.prg_slots<=64) return true;
    return !((hw_cart.m.reg[0]^hw_cart.m.reg[1])&1) &&
           !((hw_cart.m.reg[0]^hw_cart.m.reg[2])&1) &&
           !((hw_cart.m.reg[0]^hw_cart.m.reg[3])&1);
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
    memset(hw_cart.prg_off, 0, sizeof(hw_cart.prg_off));
    memset(hw_cart.chr_off, 0, sizeof(hw_cart.chr_off));
    int i = mapper_index(hw_cart.mapper);
    hw_cart.watch_ppu_addr = i >= 0 ? MAPPERS[i].watch_ppu_addr : 0;
    hw_cart.watch_cpu = bandai_board() || (vrc24_board() && !vrc2_board()) || hw_cart.mapper == 73 || vrc6_board() || hw_cart.mapper == 85;
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
    switch (hw_cart.mapper) {
    case 16: case 159: case 153: case 157: bandai_apply(); break;
    case 85: hw_cart.m.reg[1]=1; hw_cart.m.reg[2]=2; hw_cart.m.irq_prescaler=341; vrc7_apply(); break;
    case 24: case 26:
        hw_cart.m.vrc6_audio.step[0] = hw_cart.m.vrc6_audio.step[1] = 15;
        hw_cart.m.ctrl = 0x20; hw_cart.m.irq_prescaler = 341; vrc6_apply(); break;
    case 21: case 22: case 23: case 25:
        hw_cart.m.reg[1] = 1; hw_cart.m.irq_prescaler = 341; vrc24_apply(); break;
    case 73: uxrom_reset(); break;
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
    case 206: hw_cart.m.reg[7] = 1; mmc3_apply(); break;
    case 76: uxrom_reset(); hw_cart.m.reg[7] = 1; for (unsigned j = 0; j < 4; ++j) map_chr2(j, 0); break;
    case 94: uxrom_reset(); break;
    case 180: map_prg16(0, 0); map_prg16(1, 0); map_chr8(0); break;
    case 184: nrom_reset(); map_chr4(1, 4); break;
    case 232: map_prg16(0, 0); map_prg16(1, 3); map_chr8(0); break;
    case 1:  mmc1_reset(); break;
    case 2:  uxrom_reset(); break;
    case 3:  cnrom_reset(); break;
    case 4:  mmc3_reset(); break;
    case 7:  axrom_reset(); break;
    case 66: gxrom_reset(); break;
    default: nrom_reset(); break;
    }
}

void hw_cart_cpu_write(uint16_t addr, uint8_t value)
{
    if (bandai_board()) { bandai_write(addr,value); return; }
    if (vrc24_board() && addr >= 0x6000 && addr < 0x8000) {
        if (vrc2_board() && !hw_cart.has_wram) {
            if (addr < 0x7000) hw_cart.m.latch = value & 1;
            return;
        }
        if (!vrc2_board() && hw_cart.wram_len == 2048 && addr >= 0x7000) return;
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
    if (hw_cart.mapper == 79 && (addr & 0xe100) == 0x4100) {
        hw_cart.m.latch = value;
        map_prg32((value >> 3) & 1);
        map_chr8(value & 7);
        return;
    }
    if (hw_cart.mapper == 87 && addr >= 0x6000 && addr < 0x8000) {
        map_chr8(((value & 1) << 1) | ((value & 2) >> 1));
        return;
    }
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
        if (hw_cart.has_wram && hw_cart.wram_writable) hw_cart.wram[(hw_cart.wram_bank + (addr & 0x1FFF)) % hw_cart.wram_len] = value;
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
    case 11: /* Color Dreams; see MAPPERS.md. */
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
    case 206: /* DxROM; see MAPPERS.md. */
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
    case 232: /* Camerica Quattro; see MAPPERS.md. */
        if (addr < 0xc000) hw_cart.m.ctrl = hw_cart.info.submapper == 1 ?
            ((value & 8) | ((value >> 2) & 4)) : (value >> 1) & 12;
        else hw_cart.m.prg = value & 3;
        map_prg16(0, hw_cart.m.ctrl | hw_cart.m.prg);
        map_prg16(1, hw_cart.m.ctrl | 3);
        break;
    case 1:  mmc1_write(addr, value); break;
    case 2:  uxrom_write(value); break;
    case 3:  cnrom_write(value); break;
    case 4:  mmc3_write(addr, value); break;
    case 7:  axrom_write(value); break;
    case 66: gxrom_write(value); break;
    default: break;              /* NROM: the ROM ignores writes */
    }
}

bool hw_cart_cpu_read(uint16_t addr, uint8_t *value)
{
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
    if (hw_cart.mapper == 4) mmc3_ppu_addr(vbus);
    else if (hw_cart.mapper==153 || hw_cart.mapper==157) bandai_ppu_addr(vbus);
}

bool hw_cart_irq(void) { return hw_cart.m.irq_out != 0; }

void hw_cart_cpu_clock(void)
{
    if (bandai_board()) { bandai_clock(); return; }
    if (hw_cart.mapper==85) vrc7_audio_clock();
    if (vrc6_board()) vrc6_audio_clock();
    if (hw_cart.mapper == 73) vrc3_clock();
    else if (hw_cart.watch_cpu) vrc_irq_clock();
}

/* ------------------------------------------------------------------------- */
/* Cartridge nametables and expansion audio. */
uint16_t hw_cart_nt_a10(uint16_t addr) { return (vrc6_nt_bank((addr >> 10) & 3) & 1) << 10; }
bool hw_cart_nt_read(uint16_t addr, bool read_bus, uint8_t *value)
{
    if (!vrc6_board() || !(hw_cart.m.ctrl & 0x10)) return false;
    if (read_bus) {
        unsigned index = (vrc6_nt_bank((addr >> 10) & 3) % hw_cart.chr_pages)*1024 + (addr & 1023);
        *value = hw_cart.chr[hw_cart.chr_ram ? index % hw_cart.chr_len : index];
    }
    return true;
}
bool hw_cart_nt_write(uint16_t addr, uint8_t value)
{
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
    if (hw_cart.mapper==85) return -(double)hw_cart.m.vrc7_output / 32768.0;
    return vrc6_board() ? -(double)vrc6_audio_dac() * (0.1488 / 15.0) : 0;
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
    if (hw_cart.mapper == 9 || hw_cart.mapper == 10) {
        acc = acc * 131 + hw_cart.m.pattern_pending;
        acc = acc * 131 + hw_cart.m.pattern_addr;
    }
    if (bandai_board() || vrc24_board() || hw_cart.mapper == 73 || vrc6_board() || hw_cart.mapper == 85) {
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
    if (hw_cart.mapper==85) acc=vrc7_sound_hash(acc);
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
    if (bandai_board() || vrc24_board() || hw_cart.mapper == 73 || vrc6_board() || hw_cart.mapper == 85) {
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
