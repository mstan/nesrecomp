/*
 * hw_ppu.c - NESRecomp's 2C02 PPU.
 *
 * Clocked by hw_machine.c: ppu_dot() on the master clock tick that starts a
 * dot, ppu_half_dot() two ticks later. Register accesses come from the CPU
 * memory map at the CPU's tick 0 (ppu_read/ppu_write), and some of them run
 * part of the CPU cycle's ticks themselves, because the 2C02 acts on them at
 * points inside the cycle rather than at its start.
 *
 * The model follows the chip's VRAM bus rather than an idealized renderer:
 * addresses go out on AD0-7 and are held by the octal latch while ALE is
 * high, data comes back on the same pins, and the address register the
 * background fetch uses (PAR) is shared with sprite pattern loading. That
 * level is what AccuracyCoin's "Advanced Background Evaluation" and sprite
 * pages measure. Timings that are not in Nintendo or nesdev documentation
 * (the $2007 latch chain, the per-alignment delays of $2001/$2005/$2006,
 * OAM and palette corruption, the OAM address stepping during evaluation)
 * follow measurements published with AccuracyCoin and TriCNES.
 */
#include "hw_internal.h"

#include "hw.h"
#include "cyc_mod.h"
#include "cyc_render.h"
#if NESRECOMP_PPU_HLE
#include "ppu_hle_kernel.h"
#endif
#ifdef NESRECOMP_CYCLE_HDPACK
#include "cyc_hdpack.h"
#endif

#include <stdio.h>
#include <string.h>

HwPpu    ppu;
const char *cyc_ppu_implementation(void) { return NESRECOMP_PPU_HLE ? "HLE" : "LLE"; }
static CycModPpuWriteHook content_write_hook;
void cyc_mod_set_ppu_write_hook(CycModPpuWriteHook hook) { content_write_hook = hook; }
uint16_t hw_frame_index[256 * 240];
CycLine  hw_frame_lines[240];
uint8_t  hw_frame_bg[256 * 240];

const CycLine *cyc_frame_lines(void) { return hw_frame_lines; }
const uint8_t *cyc_frame_bg_opaque(void) { return hw_frame_bg; }

/* cyc_core.h CycLine, at dot 1 of visible line sl. */
static inline void capture_line(int sl)
{
    CycLine *l = &hw_frame_lines[sl];
    l->v = ppu.v;
    l->fine_x = ppu.fine_x;
    l->ctrl = (uint8_t)(ppu.sprite_table << 3 | ppu.bg_table << 4 | ppu.sprite16 << 5);
    l->mask = (uint8_t)(ppu.greyscale | ppu.show_bg8 << 1 | ppu.show_spr8 << 2 | ppu.show_bg << 3 | ppu.show_spr << 4 |
                        ppu.emphasis << 5);
}

/* Dots an open-bus bit of the PPU's CPU-side data bus holds a 1. */
#define IO_DECAY_DOTS 1786830u

enum { COMMIT_NT = 1, COMMIT_AT = 2, COMMIT_LO = 4, COMMIT_HI = 8 };

static inline bool rendering(void) { return ppu.show_bg || ppu.show_spr; }
static inline bool pal_oam_refresh(void) { return hw_pal() && ppu.scanline >= 265 && ppu.scanline < 311; }
static inline bool eval_rendering(void) { return ppu.eval_bg || ppu.eval_spr || pal_oam_refresh(); }
static inline bool render_line(void) { return ppu.scanline < 240 || ppu.scanline == hw_prerender_line(); }

/* ---- CPU-side I/O bus ---- */

static void io_bus_next_deadline(void)
{
    ppu.io_decay_next = UINT64_MAX;
    for (int i = 0; i < 8; i++) {
        uint64_t d = ppu.io_decay_deadline[i];
        if (d > ppu.io_decay_clock && d < ppu.io_decay_next) ppu.io_decay_next = d;
    }
}

static void io_bus_refresh(uint8_t bits)
{
    for (int i = 0; i < 8; i++)
        if (bits & (1 << i)) ppu.io_decay_deadline[i] = ppu.io_decay_clock + IO_DECAY_DOTS;
    io_bus_next_deadline();
}

/* Once per dot while any bit is set: bits whose hold time has run out read
 * 0 again. The hold time only elapses while the bus is not all zero. */
HW_ALWAYS_INLINE void io_bus_decay(void)
{
    if (!ppu.io_bus || ++ppu.io_decay_clock != ppu.io_decay_next) return;
    for (int i = 0; i < 8; i++)
        if (ppu.io_decay_deadline[i] == ppu.io_decay_clock) ppu.io_bus &= (uint8_t)~(1 << i);
    io_bus_next_deadline();
}

/* ---- VRAM bus ---- */

static inline uint16_t ciram_index(void)
{
    /* CIRAM A10 comes from the cartridge (pin 22), not from the PPU. */
    return (uint16_t)((ppu.vbus & 0x300) | ppu.octal_latch | hw_cart_ciram_a10(ppu.vbus));
}

/* A read of the VRAM bus: the latched address, the value driven onto AD0-7
 * (by CIRAM, the cartridge when /RD is low, otherwise the address byte still
 * on the pins), left on the pins. */
static uint8_t vram_fetch(void)
{
    uint8_t value;
    if (ppu.vbus & 0x2000) {
        value = (uint8_t)ppu.vbus;
        uint16_t addr = (uint16_t)((ppu.vbus & 0x3f00) | ppu.octal_latch);
        if (!hw_cart_nt_read(addr, ppu.rd != 0, &value)) value = ppu.ciram[ciram_index()];
    } else {
        uint16_t addr = (uint16_t)(((ppu.vbus & 0x3F00) | ppu.octal_latch) & 0x1FFF);
        if (hw_cart.chr_ciram >> (addr >> 10) & 1) {
            /* Namco 163: CIRAM answers pattern accesses as CHR RAM. */
            uint16_t c = (uint16_t)(hw_cart.chr_off[addr >> 10] | (addr & 0x3ff));
            value = ppu.rd ? ppu.ciram[c] : (uint8_t)ppu.vbus;
            if (ppu.wr) ppu.ciram[c] = (uint8_t)ppu.vbus;
            ppu.vbus = (uint16_t)((ppu.vbus & 0xFF00) | value);
            return value;
        }
        uint32_t a = hw_cart_chr_index(addr);
        value = ppu.rd ? hw_cart_chr_read(addr) : (uint8_t)ppu.vbus;
        if (ppu.wr && hw_cart.chr_write[(addr >> 10) & 7]) hw_cart.chr[a] = (uint8_t)ppu.vbus;
    }
    ppu.vbus = (uint16_t)((ppu.vbus & 0xFF00) | value);
    return value;
}

static void vram_store(uint8_t value)
{
    if ((ppu.vbus & 0x3FFF) >= 0x3F00) {
        ppu.palette[ppu.vbus & ((ppu.vbus & 3) ? 0x1F : 0x0F)] = value & 0x3F;
    } else if (ppu.vbus & 0x2000) {
        uint16_t addr = (uint16_t)((ppu.vbus & 0x3f00) | ppu.octal_latch);
        if (!hw_cart_nt_write(addr, value)) ppu.ciram[ciram_index()] = value;
    } else if (ppu.wr) {
        uint16_t addr = (uint16_t)(((ppu.vbus & 0x3F00) | ppu.octal_latch) & 0x1FFF);
        if (hw_cart.chr_ciram >> (addr >> 10) & 1)
            ppu.ciram[hw_cart.chr_off[addr >> 10] | (addr & 0x3ff)] = value;
        else if (hw_cart.chr_write[(addr >> 10) & 7]) hw_cart.chr[hw_cart_chr_index(addr)] = value;
    }
}

/* ---- scroll ---- */

static void increment_x(void)
{
    if ((ppu.v & 0x1F) == 31) ppu.v = (uint16_t)((ppu.v & 0xFFE0) ^ 0x0400);
    else ppu.v++;
    ppu.v &= 0x7FFF;
}

static void increment_y(void)
{
    if (ppu.copy_v) {
        /* $2006's copy to v landed on the previous dot: the increment sees
         * a mix of the old and new address (AccuracyCoin "t Register
         * Quirks"; modeled as their AND). */
        ppu.v = ppu.w2006_old_v & ppu.w2006_value;
    } else if ((ppu.v & 0x7000) != 0x7000) {
        ppu.v += 0x1000;
    } else {
        ppu.v &= 0x0FFF;
        int y = (ppu.v & 0x03E0) >> 5;
        if (y == 29) {
            y = 0;
            ppu.v ^= 0x0800;
        } else if (y == 31) {
            y = 0;
        } else {
            y++;
        }
        ppu.v = (uint16_t)((ppu.v & 0xFC1F) | (y << 5));
    }
    ppu.v &= 0x7FFF;
}

/* ---- background fetch ---- */

/* The pattern address register's context bits: the pattern table and tile
 * row for a background tile, or the sprite table (8x8), tile bit 0 (8x16)
 * and flipped row for the object being loaded on dots 256-320. */
static void par_context(void)
{
    if (ppu.dot < 256 || ppu.dot > 320) {
        ppu.par_chr = (uint16_t)((ppu.par_chr & 0x0FF8) | (ppu.bg_table ? 0x1000 : 0) | ((ppu.v & 0x7000) >> 12));
        return;
    }
    bool flip = (ppu.load_attr & 0x80) != 0;
    unsigned row = ppu.sprite_row & 7;
    if (flip) row = 7 - row;
    if (!ppu.sprite16) {
        ppu.par_chr = (uint16_t)((ppu.par_chr & 0x0FF8) | (ppu.sprite_table ? 0x1000 : 0) | row);
    } else {
        ppu.par_chr = (uint16_t)((ppu.par_chr & 0x0FE8) | ((ppu.load_tile & 1) ? 0x1000 : 0) | row |
                                 (((ppu.sprite_row & 8) ^ (flip ? 8 : 0)) << 1));
    }
}

/* One dot of the 8-dot fetch: odd steps put an address on the bus, even steps
 * read it back. On dots 257-320 every dot is the nametable read step. */
static void bg_fetch(void)
{
    unsigned step = (ppu.dot >= 257 && ppu.dot <= 320) ? 1 : (ppu.dot + 7) & 7;
    if (ppu.ale && ppu.rd) ppu.octal_latch = (uint8_t)ppu.vbus;
    switch (step) {
    case 0:
        ppu.vbus = (uint16_t)(0x2000 | (ppu.v & 0x0FFF));
        break;
    case 1:
        ppu.vbus = (uint16_t)(0x2000 | (ppu.v & 0x0F00) | ppu.octal_latch);
        ppu.fetch_data = vram_fetch();
        ppu.commit |= COMMIT_NT;
        break;
    case 2:
        ppu.vbus = (uint16_t)(0x23C0 | (ppu.v & 0x0C00) | ((ppu.v >> 4) & 0x38) | ((ppu.v >> 2) & 0x07));
        break;
    case 3:
        ppu.vbus = (uint16_t)(0x2300 | (ppu.v & 0x0C00) | ppu.octal_latch);
        ppu.fetch_data = vram_fetch();
        ppu.commit |= COMMIT_AT;
        break;
    case 4:
        par_context();
        ppu.par_chr &= 0x1FF7;
        ppu.vbus = ppu.par_chr;
        break;
    case 5:
        ppu.vbus = (uint16_t)((ppu.par_chr & 0xFF00) | ppu.octal_latch);
#ifdef NESRECOMP_CYCLE_HDPACK
        { uint16_t address=ppu.vbus;
          ppu.fetch_data=vram_fetch();cyc_hdpack_bg_fetch(address,ppu.fetch_data,false); }
#else
        ppu.fetch_data = vram_fetch();
#endif
        ppu.commit |= COMMIT_LO;
        break;
    case 6:
        par_context();
        ppu.par_chr |= 8;
        ppu.vbus = ppu.par_chr;
        break;
    case 7:
        ppu.vbus = (uint16_t)((ppu.par_chr & 0xFF00) | ppu.octal_latch);
#ifdef NESRECOMP_CYCLE_HDPACK
        { uint16_t address=ppu.vbus;
          ppu.fetch_data=vram_fetch();cyc_hdpack_bg_fetch(address,ppu.fetch_data,true); }
#else
        ppu.fetch_data = vram_fetch();
#endif
        ppu.commit |= COMMIT_HI;
        break;
    }
    if (ppu.ale && !ppu.rd) ppu.octal_latch = (uint8_t)ppu.vbus;
}

/* Dots 337-340 and 0: the two unused nametable fetches, and on dot 0 the
 * pattern address for the first tile. */
static void bg_fetch_tail(void)
{
    if (ppu.rd) ppu.octal_latch = (uint8_t)ppu.vbus;
    if (ppu.dot == 0) {
        par_context();
        ppu.par_chr &= 0x1FF7;
        if (ppu.scanline != hw_prerender_line()) ppu.vbus = ppu.par_chr;
    } else {
        switch (ppu.dot - 337) {
        case 0:
        case 2:
            ppu.vbus = (uint16_t)(0x2000 | (ppu.v & 0x0FFF));
            break;
        case 1:
            ppu.vbus = (uint16_t)(0x2000 | (ppu.v & 0x0FFF));
            ppu.fetch_data = vram_fetch();
            ppu.commit |= COMMIT_NT;
            break;
        case 3:
            ppu.fetch_data = vram_fetch(); /* not committed */
            break;
        }
    }
    if (ppu.ale && !ppu.rd) ppu.octal_latch = (uint8_t)ppu.vbus;
}

/* Second half of a fetch dot: the value read is taken into its register. */
static void bg_commit(void)
{
    if (ppu.commit & COMMIT_NT) {
        ppu.par_chr &= 0x100F;
        if (ppu.dot < 256 || ppu.dot > 320) ppu.par_chr |= (uint16_t)((uint8_t)ppu.vbus << 4);
        else ppu.par_chr |= (uint16_t)(ppu.load_tile << 4);
    }
    if (ppu.commit & COMMIT_AT) {
        uint8_t a = ppu.fetch_data;
        if ((ppu.v & 3) >= 2) a >>= 2;
        if (((ppu.v >> 5) & 3) >= 2) a >>= 4;
        ppu.attribute = a & 3;
    }
    if (ppu.commit & COMMIT_LO) ppu.lo_plane = ppu.fetch_data;
    if (ppu.commit & COMMIT_HI) {
        ppu.hi_plane = ppu.fetch_data;
#if NESRECOMP_PPU_HLE
        ppu.bg_pixels = (ppu.bg_pixels & 0xffff0000u) | ppu_hle_row(ppu.lo_plane, ppu.hi_plane);
#else
        ppu.bg_lo = (uint16_t)((ppu.bg_lo & 0xFF00) | ppu.lo_plane);
        ppu.bg_hi = (uint16_t)((ppu.bg_hi & 0xFF00) | ppu.hi_plane);
#endif
        ppu.attr_latch = ppu.attribute;
#ifdef NESRECOMP_CYCLE_HDPACK
        cyc_hdpack_bg_reload();
#endif
        increment_x();
    }
    ppu.commit = 0;
}

/* ---- sprites ---- */

static inline void oam2_increment(void)
{
    if (!ppu.oam2_full && ++ppu.oam2_addr == 0x20) {
        ppu.oam2_full = 1;
        ppu.oam2_addr = 0;
    }
}

static inline uint8_t flip_bits(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    return (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
}

/* Rendering disabled outside VBlank corrupts one OAM row; which row was
 * recorded when rendering stopped, and the copy happens when it restarts. */
static void corrupt_oam(void)
{
    if (ppu.oamc_index == 0x20) ppu.oamc_index = 0;
    memcpy(&ppu.oam[ppu.oamc_index * 8], &ppu.oam[0], 8);
    ppu.oam2[ppu.oamc_index] = ppu.oam2[0];
}

static inline void oamc_record(bool with_index)
{
    ppu.oamc_disabled = 0;
    ppu.oamc_disabled_now = 0;
    ppu.oamc_pending = 1;
    if (with_index) ppu.oamc_index = ppu.oam2_addr;
}

static void eval_clear(bool prerender)
{
    bool eval = eval_rendering();
    if (ppu.dot & 1) {
        if (!eval) return;
        ppu.oam_buffer_in = prerender ? ppu.oam2[ppu.oam2_addr] : 0xFF;
        if (ppu.dot == 1) {
            ppu.oam2_addr = 0;
            ppu.eval_tick = 0;
            ppu.eval_wrapped = 0;
        }
        if (ppu.oamc_disabled || ppu.oamc_disabled_now) oamc_record(true);
    } else if (ppu.dot == 0) {
        if (eval) oam2_increment();
    } else if (eval) {
        if (!prerender) ppu.oam2[ppu.oam2_addr] = ppu.oam_buffer;
        if (ppu.oamc_disabled) oamc_record(true);
        oam2_increment();
        if (ppu.oamc_disabled_now && ppu.dot == 64) oamc_record(false);
    } else if (ppu.oamc_disabled || ppu.oamc_disabled_now) {
        oamc_record(true);
    }
}

static void eval_objects(bool prerender)
{
    unsigned height = ppu.sprite16 ? 16 : 8;
    if (ppu.dot == 65 && eval_rendering()) {
        ppu.eval_nine = 0;
        ppu.eval_odd_corrupt = 0;
        ppu.eval_wrapped = 0;
    }
    if (!(ppu.instant_bg || ppu.instant_spr || ppu.oamc_disabled_now || pal_oam_refresh())) return;

    if (ppu.dot & 1) {
        ppu.oam_buffer_in = ppu.oam[ppu.oam_addr];
        if (ppu.oamc_disabled_now) {
            /* Rendering stopped on this odd dot: the even dot runs as usual
             * and the OAM address steps once more. */
            ppu.oamc_disabled = 0;
            if (!prerender) ppu.oam_addr++;
            ppu.eval_odd_corrupt = 1;
        }
        return;
    }

    bool step = !ppu.eval_odd_corrupt && !prerender;
    if (!ppu.eval_wrapped) {
        uint8_t before = ppu.oam_addr;
        if (!ppu.oam2_full && !prerender) ppu.oam2[ppu.oam2_addr] = ppu.oam_buffer;
        uint8_t oam2_value = ppu.oam2[ppu.oam2_addr];
        if (ppu.eval_tick == 0) {
            /* Y: in range of this scanline? */
            ppu.sprite_row = (uint16_t)((pal_oam_refresh() ? ppu.scanline : ppu.scanline & 0xFF) - ppu.oam_buffer);
            if (!ppu.eval_nine && !prerender && ppu.sprite_row < height) {
                if (!ppu.oam2_full) {
                    if (!ppu.eval_odd_corrupt) {
                        ppu.oam_addr++;
                        oam2_increment();
                    }
                    /* Sprite 0 hit belongs to whatever object is checked on
                     * dot 66, normally OAM[0]. */
                    if (ppu.dot == 66) ppu.next_has_s0 = 1;
                } else {
                    ppu.eval_nine = 1;
                    ppu.oam_addr++;
                    ppu.overflow = 1;
                }
                ppu.eval_tick++;
            } else {
                if (ppu.dot == 66) ppu.next_has_s0 = 0;
                if (step) {
                    if (ppu.oam2_full && !ppu.eval_nine) {
                        /* The sprite overflow bug: the byte index steps
                         * along with the object index. */
                        ppu.oam_addr += (ppu.oam_addr & 3) == 3 ? 1 : 5;
                    } else {
                        ppu.oam_addr = (uint8_t)((ppu.oam_addr + 4) & 0xFC);
                    }
                }
            }
        } else {
            if (ppu.eval_tick == 3) {
                int row = (int)ppu.scanline - ppu.oam_buffer;
                if (row >= 0 && row < (int)height) {
                    if (step) ppu.oam_addr += ppu.oam2_full ? 4 : 1;
                } else if (!ppu.oam2_full) {
                    if (step) ppu.oam_addr = (uint8_t)((ppu.oam_addr + 1) & 0xFC);
                } else {
                    ppu.oam_addr = (uint8_t)((ppu.oam_addr + 1) & 0xFC);
                }
            } else if (step) {
                ppu.oam_addr++;
            }
            ppu.eval_tick = (ppu.eval_tick + 1) & 3;
            if (!ppu.oam2_full && !prerender) oam2_increment();
        }
        ppu.eval_odd_corrupt = 0;
        if (ppu.oam_addr < before && ppu.oam_addr < 4) ppu.eval_wrapped = 1;
        ppu.oam_buffer_in = oam2_value;
    } else {
        /* The OAM address wrapped during evaluation: secondary OAM is only
         * read from now on. */
        if (step) ppu.oam_addr = (uint8_t)((ppu.oam_addr + 4) & 0xFC);
        ppu.oam_buffer_in = ppu.oam2[ppu.oam2_addr];
    }

    if (ppu.oamc_disabled_now) {
        ppu.oamc_disabled = 0;
        ppu.oamc_pending = 1;
        if ((ppu.oam2_addr & 3) != 0 && !ppu.eval_wrapped && !prerender) {
            ppu.oam2_addr = (uint8_t)((ppu.oam2_addr & 0xFC) + 4);
            if (ppu.oam2_addr == 0x20) {
                ppu.oam2_full = 1;
                ppu.oam2_addr = 0;
            }
        }
        ppu.oamc_index = ppu.dot == 256 ? (ppu.eval_odd_corrupt ? 0 : 1) : ppu.oam2_addr;
    }
    ppu.oamc_disabled_now = 0;
}

/* Dots 257-320: eight objects from secondary OAM, 8 dots each, into the
 * sprite output units, sharing PAR and the VRAM bus with the dummy
 * nametable fetches. */
static void eval_load(void)
{
    bool eval = eval_rendering();
    unsigned height = ppu.sprite16 ? 16 : 8;
    ppu.cur_has_s0 = ppu.next_has_s0;
    if (eval) ppu.oam_addr = 0;
    if (ppu.oamc_disabled) oamc_record(true);
    if (ppu.rd) ppu.octal_latch = (uint8_t)ppu.vbus;

    unsigned slot = ((ppu.dot - 1) & 0x38) >> 3;
    ppu.oam_buffer_in = eval ? ppu.oam2[ppu.oam2_addr] : ppu.oam[ppu.oam_addr];
    if (eval) {
        switch ((ppu.dot - 1) & 7) {
        case 0: /* Y */
            ppu.vbus = (uint16_t)(0x2000 | (ppu.v & 0x0FFF));
            oam2_increment();
            break;
        case 1: /* tile */
            ppu.sprite_row = (uint16_t)((ppu.scanline & 0xFF) - ppu.oam_buffer);
            bg_fetch();
            oam2_increment();
            break;
        case 2: /* attributes */
            ppu.load_tile = ppu.oam_buffer;
            ppu.vbus = (uint16_t)(0x2000 | (ppu.v & 0x0FFF));
            oam2_increment();
            break;
        case 3: /* X */
            ppu.spr_attr[slot] = ppu.oam_buffer;
            ppu.load_attr = ppu.oam_buffer;
            bg_fetch();
            break;
        case 4:
            ppu.spr_x[slot] = ppu.oam_buffer;
            par_context();
            ppu.par_chr &= 0x1FF7;
            ppu.vbus = ppu.par_chr;
            break;
        case 5: {
            par_context();
            ppu.par_chr &= 0x1FF7;
            ppu.vbus = (uint16_t)((ppu.par_chr & 0xFF00) | ppu.octal_latch);
            uint16_t address=ppu.vbus;
            uint8_t bits = vram_fetch();
#ifdef NESRECOMP_CYCLE_HDPACK
            cyc_hdpack_sprite_fetch(slot,address,bits,false);
#else
            (void)address;
#endif
            if (ppu.spr_attr[slot] & 0x40) bits = flip_bits(bits);
            ppu.spr_lo[slot] = ppu.sprite_row < height ? bits : 0;
            break;
        }
        case 6:
            par_context();
            ppu.par_chr |= 8;
            ppu.vbus = ppu.par_chr;
            break;
        case 7: {
            par_context();
            ppu.par_chr |= 8;
            ppu.vbus = (uint16_t)((ppu.par_chr & 0xFF00) | ppu.octal_latch);
            uint16_t address=ppu.vbus;
            uint8_t bits = vram_fetch();
#ifdef NESRECOMP_CYCLE_HDPACK
            cyc_hdpack_sprite_fetch(slot,address,bits,true);
#else
            (void)address;
#endif
            if (ppu.spr_attr[slot] & 0x40) bits = flip_bits(bits);
            ppu.spr_hi[slot] = ppu.sprite_row < height ? bits : 0;
            oam2_increment();
            break;
        }
        }
    }
    if (ppu.ale && !ppu.rd) ppu.octal_latch = (uint8_t)ppu.vbus;
}

static void sprite_evaluation(void)
{
    bool prerender = ppu.scanline == hw_prerender_line();
    if ((ppu.instant_bg || ppu.instant_spr) && ppu.oamc_pending) {
        /* The first evaluated dot after rendering restarts. */
        ppu.oamc_pending = 0;
        if (!ppu.oamc_reenabled) corrupt_oam();
        ppu.oamc_reenabled = 0;
    }
    if (ppu.dot <= 64) {
        eval_clear(prerender);
    } else if (ppu.dot <= 256) {
        eval_objects(prerender);
    } else if (ppu.dot <= 320) {
        eval_load();
    } else {
        if (ppu.oamc_disabled || ppu.oamc_disabled_now) oamc_record(true);
        ppu.oam_buffer_in = rendering() ? ppu.oam2[ppu.oam2_addr] : ppu.oam[ppu.oam_addr];
        if (ppu.dot == 339 && !rendering()) memset(ppu.spr_x, 0, sizeof(ppu.spr_x));
    }
}

static inline bool sprite_units_idle(void)
{
    uint64_t x, lo, hi;
    memcpy(&x, ppu.spr_x, 8);
    memcpy(&lo, ppu.spr_lo, 8);
    memcpy(&hi, ppu.spr_hi, 8);
    return (x | lo | hi) == 0;
}

static void shift_sprites(void)
{
    if (sprite_units_idle()) return;
#if NESRECOMP_PPU_HLE
    unsigned shifted = ppu_hle_sprite_shift(ppu.spr_x, ppu.spr_lo, ppu.spr_hi,
                                            rendering(), ppu.skipped_dot != 0);
#ifdef NESRECOMP_CYCLE_HDPACK
    for (unsigned i = 0; i < 8; ++i)
        if (shifted & (1u << i)) cyc_hdpack_sprite_shift(i);
#else
    (void)shifted;
#endif
#else
    for (int i = 0; i < 8; i++) {
        if (ppu.spr_x[i] > 0 && !ppu.skipped_dot) {
            ppu.spr_x[i]--;
        } else if (rendering()) {
#ifdef NESRECOMP_CYCLE_HDPACK
            cyc_hdpack_sprite_shift((unsigned)i);
#endif
            ppu.spr_lo[i] <<= 1;
            ppu.spr_hi[i] <<= 1;
        }
    }
#endif
}

/* ---- palette corruption ---- */

/* Turning rendering off during the first two dots of a tile fetch with v at
 * $3C00 or above, or a $2006 write leaving palette RAM mid-scanline, writes
 * a pattern into palette RAM that depends on the pixel's color index and
 * v's low nybble. The tables are empirical (AccuracyCoin/TriCNES research);
 * only the consistent behavior at alignment 2 is modeled. */
static void corrupt_palettes(uint8_t color)
{
    if (hw.align != 2) return;
    const uint8_t *p = ppu.palette;
    uint8_t c[32];
    memcpy(c, p, sizeof(c));
    unsigned n = ppu.v & 0xF;
    switch (color) {
    case 0:
        c[n] = (uint8_t)((p[0] & p[n & 0xC]) | (p[0] & p[n]) | (p[n & 0xC] & p[n]));
        break;
    case 1:
        switch (n) {
        case 0x0: c[0x0] = (p[0x1] & p[0xD]) | p[0x0]; c[0x4] = p[0x5]; c[0x8] = p[0x9]; c[0xC] = p[0xD]; break;
        case 0x2:
            c[0x2] = (p[0x2] | p[0xD]) & p[0x3]; c[0x3] = (p[0x1] | p[0x2]) & p[0x3];
            c[0x6] = (p[0x6] | p[0x5]) & p[0x7]; c[0xA] = (p[0xA] | p[0x9]) & p[0xB];
            c[0xE] = p[0xD]; c[0xF] = p[0xD];
            break;
        case 0x3: c[0x3] &= p[0x1] | p[0xD]; c[0xF] = p[0xD]; break;
        case 0x4: c[0x0] = p[0x1]; c[0x4] = (p[0x5] & p[0xD]) | p[0x4]; c[0x8] = p[0x9]; c[0xC] = p[0xD]; break;
        case 0x6:
            c[0x2] = (p[0x2] | p[0x1]) & p[0x3]; c[0x6] = (p[0x6] | p[0x7]) & p[0xD];
            c[0x7] = (p[0x7] | p[0x6]) & p[0x5]; c[0xA] = (p[0xA] | p[0x9]) & p[0xB];
            c[0xE] = p[0xD]; c[0xF] = p[0xD];
            break;
        case 0x7: c[0x7] &= p[0x5] | p[0xD]; c[0xF] = p[0xD]; break;
        case 0x8: c[0x0] = p[0x1]; c[0x4] = p[0x5]; c[0x8] = (p[0x9] & p[0xD]) | p[0x8]; c[0xC] = p[0xD]; break;
        case 0xA:
            c[0x2] = (p[0x2] | p[0x1]) & p[0x3]; c[0x6] = (p[0x6] | p[0xD]) & p[0x7];
            c[0xA] = (p[0xB] | p[0xD]) & p[0xA]; c[0xB] = (p[0x9] | p[0xA]) & p[0xB];
            c[0xE] = p[0xD]; c[0xF] = p[0xD];
            break;
        case 0xB: c[0xB] &= p[0x9] | p[0xD]; c[0xF] = p[0xD]; break;
        case 0xC: c[0x0] = p[0x1]; c[0x4] = p[0x5]; c[0x8] = p[0x9]; c[0xC] = p[0xD]; break;
        case 0xE:
            c[0x2] = (p[0x2] | p[0x1]) & p[0x3]; c[0x6] = (p[0x6] | p[0xD]) & p[0x7];
            c[0xA] = (p[0xA] | p[0x9]) & p[0xB]; c[0xE] = p[0xD]; c[0xF] = p[0xD];
            break;
        case 0xF: c[0xF] = p[0xD]; break;
        default: break; /* 1, 5, 9, D */
        }
        break;
    case 2:
        switch (n) {
        case 0x0: c[0x0] = p[0x0] | (p[0x2] & p[0xE]); c[0x4] = p[0x6]; c[0x8] = p[0xA]; c[0xC] = p[0xE]; break;
        case 0x1:
            c[0x1] = (p[0x2] | p[0x1] | p[0xE]) & (p[0x3] | p[0xE]); c[0x3] = (p[0x2] | p[0xE] | 0x3C) & p[0x3];
            c[0x5] = (p[0x6] | p[0x7]) & p[0x5]; c[0x9] = (p[0xA] | p[0xB]) & p[0x9];
            c[0xD] = p[0xE]; c[0xF] = p[0xE];
            break;
        case 0x3: c[0x3] &= p[0x2] | p[0xE]; c[0xF] = p[0xE]; break;
        case 0x4: c[0x0] = p[0x2]; c[0x4] = p[0x4] | (p[0x6] & p[0xE]); c[0x8] = p[0xA]; c[0xC] = p[0xE]; break;
        case 0x5:
            c[0x1] = (p[0x2] | p[0x1]) & p[0x3]; c[0x5] = (p[0xE] | p[0x6]) & p[0x5];
            c[0x7] = (p[0xE] | p[0x6]) & p[0x7]; c[0xD] = p[0xE]; c[0xF] = p[0xE];
            break;
        case 0x7: c[0x7] &= p[0x6] | p[0xE]; break;
        case 0x8: c[0x0] = p[0x2]; c[0x4] = p[0x6]; c[0x8] = p[0x8] | (p[0xA] & p[0xE]); c[0xC] = p[0xE]; break;
        case 0x9:
            c[0x1] = (p[0x2] | p[0x1]) & p[0x3]; c[0x5] = (p[0x6] | p[0x5]) & p[0x7];
            c[0x9] = (p[0xE] | p[0xA] | 0x01) & p[0x9]; c[0xB] = (p[0xE] | p[0xA] | 0x31) & p[0xB];
            c[0xD] = p[0xE]; c[0xF] = p[0xE];
            break;
        case 0xB: c[0xB] &= p[0xA] | p[0xE]; c[0xF] = p[0xE]; break;
        case 0xC: c[0x0] = p[0x2]; c[0x4] = p[0x6]; c[0x8] = p[0xA]; c[0xC] = p[0xE]; break;
        case 0xD:
            c[0x1] = (p[0x2] | p[0x1]) & p[0x3]; c[0x5] = (p[0x6] | p[0x5]) & p[0x7];
            c[0x9] = (p[0xA] | p[0x9]) & p[0xB]; c[0xD] = p[0xE]; c[0xF] = p[0xE];
            break;
        case 0xF: c[0xF] = p[0xE]; break;
        default: break; /* 2, 6, A, E */
        }
        break;
    case 3:
        switch (n) {
        case 0x0:
            c[0x0] = p[0x3] | (p[0xF] & p[0x0]); c[0x4] &= p[0x7];
            c[0x8] &= p[0x9] | p[0xA] | p[0xB] | p[0xF] | 0x22; c[0xC] = p[0xF];
            break;
        case 0x1: c[0x1] = (p[0x1] | p[0xF]) & p[0x3]; c[0x5] = p[0x7]; c[0x9] = p[0xB]; c[0xD] = p[0xF]; break;
        case 0x2: c[0x2] = (p[0x3] | p[0xF]) & p[0x3]; c[0x6] = p[0x7]; c[0xA] = p[0xB]; c[0xE] = p[0xF]; break;
        case 0x4:
            c[0x0] &= (p[0xF] ^ 0xFF) | p[0x1] | p[0x2] | p[0x3] | 0x07; c[0x4] &= p[0x7] | p[0xF];
            c[0x8] &= p[0xB] | p[0xF] | (p[0xC] ^ 0xFF); c[0xC] = (p[0x7] & p[0xF]) | p[0xC];
            break;
        case 0x5: c[0x1] = p[0x3]; c[0x5] = (p[0x5] | p[0xF]) & p[0x7]; c[0x9] = p[0xB]; c[0xD] = p[0xF]; break;
        case 0x6: c[0x2] = p[0x3]; c[0x6] = (p[0x6] | p[0xF]) & p[0x7]; c[0xA] = p[0xB]; c[0xE] = p[0xF]; break;
        case 0x8:
            c[0x0] &= (p[0xF] ^ 0xFF) | p[0x1] | p[0x2] | p[0x3] | 0x23; c[0x4] = p[0x7];
            c[0x8] &= p[0xB] | p[0xF] | (p[0xC] ^ 0xFF); c[0xC] = (p[0xB] & p[0xF]) | p[0xC];
            break;
        case 0x9: c[0x1] = p[0x3]; c[0x5] = p[0x7]; c[0x9] = (p[0x9] | p[0xF]) & p[0xB]; c[0xD] = p[0xF]; break;
        case 0xA: c[0x2] = p[0x3]; c[0x6] = p[0x7]; c[0xA] = (p[0xA] | p[0xF]) & p[0xB]; c[0xE] = p[0xF]; break;
        case 0xC:
            c[0x0] &= (p[0xF] ^ 0xFF) | p[0x1] | p[0x2] | p[0x3] | 0x37; c[0x4] = p[0x7];
            c[0x8] &= p[0xB] | 0x2F; c[0xC] = p[0xF];
            break;
        case 0xD: c[0x1] = p[0x3]; c[0x5] = p[0x7]; c[0x9] = p[0xB]; c[0xD] = p[0xF]; break;
        case 0xE: c[0x2] = p[0x3]; c[0x6] = p[0x7]; c[0xA] = p[0xB]; c[0xE] = p[0xF]; break;
        default: break; /* 3, 7, B, F */
        }
        break;
    }
    for (int i = 0; i < 32; i++) ppu.palette[i] = c[i] & 0x3F;
}

/* ---- pixel ---- */

static void compute_pixel(void)
{
    uint8_t color = 0, pal = 0;
#ifdef NESRECOMP_CYCLE_HDPACK
    unsigned hd_bg_color=0,hd_bg_palette=0,hd_sprite_color=0;int hd_sprite=-1;
#endif
    if (ppu.show_bg && (ppu.dot > 8 || ppu.show_bg8)) {
        unsigned fx = ppu.fine_x;
#if NESRECOMP_PPU_HLE
        color = (uint8_t)((ppu.bg_pixels >> (30 - 2 * fx)) & 3);
        pal = (uint8_t)((ppu.attr_pixels >> (14 - 2 * fx)) & 3);
#else
        color = (uint8_t)(((ppu.bg_lo >> (15 - fx)) & 1) | (((ppu.bg_hi >> (15 - fx)) & 1) << 1));
        pal = (uint8_t)(((ppu.attr_lo >> (7 - fx)) & 1) | (((ppu.attr_hi >> (7 - fx)) & 1) << 1));
#endif
#ifdef NESRECOMP_CYCLE_HDPACK
        hd_bg_color=color;hd_bg_palette=pal;
#endif
        if (color == 0) pal = 0;
    }
    hw_frame_bg[ppu.scanline * 256 + ppu.dot - 1] = color != 0;
    uint8_t background_addr=(uint8_t)(pal<<2|color);
    if (ppu.show_spr && (ppu.dot > 8 || ppu.show_spr8) && !sprite_units_idle()) {
        int i;
        uint8_t sc = 0;
#if NESRECOMP_PPU_HLE
        unsigned sprites = ppu_hle_sprite_mask(ppu.spr_x, ppu.spr_lo, ppu.spr_hi,
                                                ppu.skipped_dot != 0);
        i = sprites ? (int)ppu_hle_first_sprite(sprites) : 8;
        if (sprites) sc = (uint8_t)((ppu.spr_lo[i] >> 7) | ((ppu.spr_hi[i] >> 7) << 1));
#else
        for (i = 0; i < 8; i++) {
            if (ppu.spr_x[i] != 0 && !ppu.skipped_dot) continue;
            sc = (uint8_t)((ppu.spr_lo[i] >> 7) | ((ppu.spr_hi[i] >> 7) << 1));
            if (sc) break;
        }
#endif
        if (i == 0 && ppu.can_s0hit && ppu.cur_has_s0 && ppu.show_bg && ppu.show_spr && color && sc &&
            (ppu.show_spr8 || ppu.dot > 8) && ppu.dot < 256) {
            ppu.s0hit_pending1 = 1;
            ppu.can_s0hit = 0;
        }
        if (sc && (color == 0 || !(ppu.spr_attr[i] & 0x20))) {
#ifdef NESRECOMP_CYCLE_HDPACK
            hd_sprite=i;hd_sprite_color=sc;
#endif
            color = sc;
            pal = (uint8_t)((ppu.spr_attr[i] & 3) | 4);
        }
    }
    uint8_t addr;
    if (rendering() && ppu.scanline < 240) {
        addr = (uint8_t)(pal << 2 | color);
    } else if ((ppu.v & 0x3F1F) >= 0x3F00) {
        /* Rendering off with v in palette RAM: the backdrop is that entry. */
        addr = (uint8_t)(ppu.v & 0x1F);
        if ((addr & 3) == 0) addr &= 0x0F;
    } else {
        addr = 0;
    }
    if (ppu.palc_disabled || ppu.palc_v_left) {
        ppu.palc_disabled = ppu.palc_v_left = 0;
        corrupt_palettes(color);
    }
    ppu.color[0] = ppu.palette[addr] & 0x3F;
    if(cyc_background_enabled)cyc_background_pipe[0]=ppu.palette[
        rendering()&&ppu.scanline<240?background_addr:addr]&0x3f;
#ifdef NESRECOMP_CYCLE_HDPACK
    cyc_hdpack_pixel(hd_bg_color,hd_bg_palette,hd_sprite,hd_sprite_color);
    if(!rendering())cyc_hdpack_blank(ppu.color[0]);
#endif
}

/* The chosen color reaches the video output three dots later, where greyscale
 * and emphasis are applied, so dots 4-259 of a visible scanline carry pixels
 * 0-255.
 *
 * The odd-frame dot skip does not move this. What an odd frame skips is the
 * last tick of the pre-render scanline - the PPU "jumps directly from
 * (339, 261) to (0, 0)" (nesdev wiki, PPU frame timing), and NES_MiSTer's
 * ppu.sv skips "the *last* cycle of odd frames", armed on the pre-render line
 * - so scanline 0 still runs its own dots and still emits 256 pixels. Nor is
 * the color pipeline displaced by dropping scanline 0's dot 0: that dot only
 * shifts the pipeline and never computes a pixel, so the pixel computed at
 * dot 1 still reaches the output at dot 4 either way. TriCNES shifts scanline
 * 0 one pixel left on odd frames and fills x=255 from the backdrop; that is
 * corrected in the oracle (ORACLE FIX, BACKDROP). */
static void output_pixel(void)
{
    int sl = ppu.scanline, dot = ppu.dot;
    if (dot > 3 && dot <= 259) {
        uint8_t c = ppu.color[3];
        if (ppu.greyscale) c &= 0x30;
        unsigned emphasis = ppu.emphasis;
        if (hw_pal()) emphasis = (emphasis & 4) | ((emphasis & 1) << 1) | ((emphasis & 2) >> 1);
        hw_frame_index[sl * 256 + dot - 4] = (uint16_t)(c | emphasis << 6);
        if(cyc_background_enabled)cyc_render_background_output((unsigned)(sl*256+dot-4),emphasis,ppu.greyscale!=0);
#ifdef NESRECOMP_CYCLE_HDPACK
        cyc_hdpack_output(dot-4,sl);
#endif
        if (hw_zapper.port) hw_zapper_pixel(dot - 4, sl, hw_frame_index[sl * 256 + dot - 4]);
    }
}

/* ---- $2007 access state machine ---- */

/* A CPU read or write of $2007 sets a latch that runs through a chain of
 * half-dot latches, producing the read strobe (PD/RB), the address latch
 * enable and the v increment at a point that depends on where in the dot
 * the CPU access ended. */

/* No access in progress: every latch holds its settled value, and the chain
 * only passes the dot's own read/ALE timing through. Cleared by a CPU access
 * of $2007, recomputed as the chain settles. */
static bool sm_rest;

static bool sm_at_rest(void)
{
    return !(ppu.rd_sr | ppu.wr_sr | ppu.rl[0] | ppu.rl[2] | ppu.rl[4] | ppu.wl[0] | ppu.wl[2] | ppu.wl[4] |
             ppu.pd_rb | ppu.rd_ale | ppu.wr_ale | ppu.db_par | ppu.tstep_latch) &&
           ppu.rl[1] && ppu.rl[3] && ppu.wl[1] && ppu.wl[3];
}

static void data_sm_dot(bool blnk)
{
    ppu.blnk_latch = blnk;
    bool h0 = ((ppu.dot - 1) & 1) != 0;
    ppu.pal_enable = (ppu.vbus & 0x3F00) == 0x3F00 && blnk;
    if (sm_rest) {
        ppu.rd = !blnk && h0;
        hw_cart_ppu_rd(ppu.rd != 0);
        ppu.ale = !blnk && !h0;
        return;
    }
    ppu.rl[0] = ppu.rd_sr;
    ppu.rl[2] = !ppu.rl[1];
    ppu.rl[4] = !ppu.rl[3];
    ppu.pd_rb = ppu.rl[4] && !ppu.rl[2];
    ppu.rd_ale = !ppu.rl[4] && ppu.rl[2];
    ppu.rd = ppu.pd_rb || (!blnk && h0);
    hw_cart_ppu_rd(ppu.rd != 0);
    ppu.wl[0] = ppu.wr_sr;
    ppu.wl[2] = !ppu.wl[1];
    ppu.wl[4] = !ppu.wl[3];
    ppu.wr_ale = !ppu.wl[4] && ppu.wl[2];
    ppu.tstep_latch = ppu.db_par;
    ppu.ale = ppu.rd_ale || ppu.wr_ale || (!blnk && !h0);
    if ((ppu.rd_ale || ppu.wr_ale) && !ppu.rd) {
        ppu.vbus = ppu.v;
        ppu.octal_latch = (uint8_t)ppu.v;
    }
}

static void data_sm_dot_end(void)
{
    if (ppu.pd_rb) {
        ppu.read_buffer = vram_fetch();
        if (ppu.ale) ppu.octal_latch = (uint8_t)ppu.vbus;
    }
}

static void data_sm_half(void)
{
    if (sm_rest) {
        ppu.ale = 0;
        ppu.wr = 0;
        return;
    }
    if (ppu.tstep_latch || ppu.pd_rb) {
        if (!ppu.blnk_latch) increment_y();
        else ppu.v = (uint16_t)((ppu.v + (ppu.inc32 ? 32 : 1)) & 0x7FFF);
    }
    ppu.ale = ppu.rd_ale || ppu.wr_ale;
    if (ppu.pd_rb) {
        ppu.read_buffer = vram_fetch();
        if (ppu.ale) ppu.octal_latch = (uint8_t)ppu.vbus;
    }
    ppu.rl[1] = !ppu.rl[0];
    ppu.rl[3] = !ppu.rl[2];
    if (!ppu.rl[3]) ppu.rd_sr = 0;
    ppu.wl[1] = !ppu.wl[0];
    ppu.wl[3] = !ppu.wl[2];
    if (!ppu.wl[3]) ppu.wr_sr = 0;
    ppu.db_par = ppu.wl[1] && !ppu.wl[3];
    ppu.wr = !ppu.pal_enable && ppu.db_par;
    if (ppu.db_par) vram_store(ppu.write_data);
    sm_rest = sm_at_rest();
}

/* ---- dots ---- */

/* The next dot's position, the events tied to it, and the status flag
 * latches clocked on the first half of every dot. */
HW_ALWAYS_INLINE void advance_dot(void)
{
    if (hw_zapper.port) hw_zapper.dots++;
    if (++ppu.dot > 340) {
        ppu.dot = 0;
        if (ppu.scanline == 240 && hw_extra_timing.line < hw_extra_timing.extra_scanlines) {
            hw_extra_timing.line++;
            hw_extra_timing.active = 1;
        } else {
            hw_extra_timing.line = 0;
            hw_extra_timing.active = 0;
            if (++ppu.scanline > hw_prerender_line()) ppu.scanline = 0;
        }
        if (!hw_extra_timing.active && ppu.scanline == hw_observe_line) hw_observe_hit = hw_frame_done = true;
    }
    if (ppu.scanline >= 241) {
        if (ppu.scanline == 241) {
            if (ppu.dot == 0) ppu.vblank_pending = 1;
            else if (ppu.dot == 1) hw_frame_done = hw_frame_end_hit = true;
        } else if (ppu.scanline == hw_prerender_line() - 1 && ppu.dot == 340) {
            ppu.odd_frame = !ppu.odd_frame;
        } else if (ppu.scanline == hw_prerender_line() && ppu.dot == 1) {
            ppu.vblank = 0;
            ppu.can_s0hit = 1;
            ppu.s0hit = 0;
            ppu.overflow = 0;
            ppu.s0hit_late = 0;
        }
    }

    /* VBlank flag: set through VSET's latches, cleared by a $2002 read. */
    ppu.vset_latch1 = !ppu.vset;
    if (ppu.vset && !ppu.vset_latch2) ppu.vblank = 1;
    if (ppu.read2002) {
        ppu.read2002 = 0;
        ppu.vblank = 0;
    }
    ppu.overflow_late = ppu.overflow;
}

/* The status latches clocked on the second half of every dot. */
HW_ALWAYS_INLINE void half_dot_status(void)
{
    ppu.vset = ppu.vblank_pending;
    ppu.vblank_pending = 0;
    ppu.vset_latch2 = !ppu.vset_latch1;

    ppu.s0hit_late = ppu.s0hit;
    if (ppu.s0hit_pending2) {
        ppu.s0hit_pending2 = 0;
        ppu.s0hit = 1;
    }
    if (ppu.s0hit_pending1) {
        ppu.s0hit_pending1 = 0;
        ppu.s0hit_pending2 = 1;
    }
}

/* ---- blank dots ----
 *
 * Most dots in a program that measures the hardware have rendering off with
 * nothing in progress. Those take this path, which is the general dot with
 * every branch that condition rules out removed. The classification is
 * cached; anything that can change its inputs (any register access, or a
 * general dot or half dot, which apply pending writes and step the $2007
 * state machine) clears it. The general path runs otherwise, and
 * native/--interp-only comparisons cover both. */

enum { DOT_UNKNOWN, DOT_BLANK, DOT_GENERAL, DOT_RENDER };
static uint8_t dot_kind;

static bool is_blank(void)
{
    return !pal_oam_refresh() && !(ppu.show_bg | ppu.show_spr | ppu.eval_bg | ppu.eval_spr | ppu.instant_bg | ppu.instant_spr |
             ppu.skipped_dot | ppu.w2001_delay | ppu.w2001_oam_delay | ppu.w2001_emph_delay | ppu.w2005_delay |
             ppu.w2006_delay | ppu.oamc_disabled | ppu.oamc_disabled_now | ppu.palc_disabled | ppu.palc_v_left |
             ppu.commit | ppu.rd_sr | ppu.wr_sr | ppu.rl[0] | ppu.rl[2] | ppu.rl[4] | ppu.wl[0] | ppu.wl[2] |
             ppu.wl[4] | ppu.pd_rb | ppu.rd_ale | ppu.wr_ale | ppu.db_par | ppu.tstep_latch) &&
           ppu.rl[1] && ppu.rl[3] && ppu.wl[1] && ppu.wl[3];
}

static void blank_dot(void)
{
    advance_dot();
    int sl = ppu.scanline, dot = ppu.dot;
    bool line = sl < 240 || sl == hw_prerender_line();

    /* data_sm_dot() at rest, blanked */
    ppu.blnk_latch = 1;
    ppu.pal_enable = (ppu.vbus & 0x3F00) == 0x3F00;
    ppu.rd = 0;
    hw_cart_ppu_rd(false);
    ppu.ale = 0;

    ppu.oam_latch = ppu.oam_buffer;
    ppu.copy_v = 0;
    if (ppu.oam2_reset > 0) ppu.oam2_reset--;
    if (line && dot >= 257) {
        if (dot <= 320) ppu.cur_has_s0 = ppu.next_has_s0;
        ppu.oam_buffer_in = ppu.oam[ppu.oam_addr];
        if (dot == 339) memset(ppu.spr_x, 0, sizeof(ppu.spr_x));
    }
    ppu.render_count = 0;
    ppu.vbus = ppu.v;

#ifdef NESRECOMP_CYCLE_HDPACK
    cyc_hdpack_clock();
#endif
    if(cyc_background_enabled){cyc_background_pipe[3]=cyc_background_pipe[2];cyc_background_pipe[2]=cyc_background_pipe[1];cyc_background_pipe[1]=cyc_background_pipe[0];}
    ppu.color[3] = ppu.color[2];
    ppu.color[2] = ppu.color[1];
    ppu.color[1] = ppu.color[0];
    if (line) {
        if (dot >= 1 && dot <= 256) {
            if (sl < 240) {
                if (dot == 1) capture_line(sl);
                hw_frame_bg[sl * 256 + dot - 1] = 0;
                uint8_t addr = 0;
                if ((ppu.v & 0x3F1F) >= 0x3F00) {
                    addr = (uint8_t)(ppu.v & 0x1F);
                    if ((addr & 3) == 0) addr &= 0x0F;
                }
                ppu.color[0] = ppu.palette[addr] & 0x3F;
                if(cyc_background_enabled)cyc_background_pipe[0]=ppu.color[0];
#ifdef NESRECOMP_CYCLE_HDPACK
                cyc_hdpack_blank(ppu.color[0]);
#endif
            }
            uint64_t counters;
            memcpy(&counters, ppu.spr_x, sizeof(counters));
            if (counters)
                for (int i = 0; i < 8; i++) ppu.spr_x[i] -= ppu.spr_x[i] > 0;
        }
        if (sl < 240 && dot > 3 && dot <= 259) {
            output_pixel();
        }
    }
    io_bus_decay();
}

static void blank_half_dot(void)
{
    if (ppu.oam2_reset > 0 && --ppu.oam2_reset == 0) {
        ppu.oam2_addr = 0;
        ppu.oam2_full = 0;
    }
    half_dot_status();
    /* data_sm_half() at rest */
    ppu.ale = 0;
    ppu.wr = 0;
}

/* ---- general dots ---- */

static void general_dot(void)
{
    /* $2005 lands. */
    if (ppu.w2005_delay && --ppu.w2005_delay == 0) {
        uint8_t val = ppu.w2005_value;
        if (!ppu.addr_latch) {
            ppu.fine_x = val & 7;
            ppu.t = (uint16_t)((ppu.t & 0x7FE0) | (val >> 3));
        } else {
            ppu.t = (uint16_t)((ppu.t & 0x0C1F) | ((val & 0xF8) << 2) | ((val & 7) << 12));
        }
        ppu.addr_latch = !ppu.addr_latch;
    }

    /* Scroll copies and the vertical increment, at the end of the dot. */
    if (render_line() && rendering() && ppu.render_count >= 1) {
        if (ppu.dot == 256) increment_y();
        else if (ppu.dot == 257) ppu.v = (uint16_t)((ppu.v & 0x7BE0) | (ppu.t & 0x041F));
        if (ppu.dot >= 280 && ppu.dot <= 304 && ppu.scanline == hw_prerender_line())
            ppu.v = (uint16_t)((ppu.v & 0x041F) | (ppu.t & 0x7BE0));
    }

    advance_dot();
    int sl = ppu.scanline;

    if (!hw_pal() && ppu.odd_frame && rendering() && sl == 0) {
        if (ppu.dot == 0) {
            /* Odd frames skip dot 0 of scanline 0. */
            ppu.dot = 1;
            ppu.skipped_dot = 1;
        } else if (ppu.dot == 2) {
            ppu.skipped_dot = 0;
        }
    }
    int dot = ppu.dot;
    if (dot == 1 && sl < 240) capture_line(sl);

    /* At alignment 1 sprite evaluation sees $2001 one dot later. */
    if (hw.align != 1) {
        ppu.eval_bg = ppu.show_bg;
        ppu.eval_spr = ppu.show_spr;
    }

    data_sm_dot((!ppu.show_bg && !ppu.show_spr) || (sl >= 240 && sl < hw_prerender_line()));
    ppu.oam_latch = ppu.oam_buffer;

    /* $2006 lands. */
    ppu.copy_v = 0;
    if (ppu.w2006_delay && --ppu.w2006_delay == 0) {
        uint16_t old = ppu.v;
        ppu.copy_v = 1;
        ppu.v = ppu.t;
        ppu.vbus = ppu.v;
        if ((old & 0x3FFF) >= 0x3F00 && (ppu.vbus & 0x3FFF) < 0x3F00 && sl < 240 && dot <= 256 && (old & 0xF) != 0)
            ppu.palc_v_left = 1;
    }

    if (ppu.oam2_reset > 0) ppu.oam2_reset--;

    if (render_line()) {
        sprite_evaluation();
        if (eval_rendering() && (dot == 63 || dot == 255 || dot == 339)) ppu.oam2_reset = 3;
    } else if (pal_oam_refresh()) {
        /* 2C07 refresh clocks the OAM evaluator, not the pattern fetch bus.
         * VBlank lines above 255 cannot select an onscreen sprite by wrapping
         * their Y coordinate. Doing so leaves ghost sprites on line zero. */
        if (dot <= 64) eval_clear(false);
        else if (dot <= 256) eval_objects(false);
        else if (dot <= 320) ppu.oam_addr = 0;
    }

    if (hw.align == 1) {
        ppu.eval_bg = ppu.show_bg;
        ppu.eval_spr = ppu.show_spr;
    }

    if (!rendering()) {
        ppu.render_count = 0;
        ppu.vbus = ppu.v;
    } else if (ppu.render_count < 5) {
        ppu.render_count++;
    }

    /* $2001 lands, in three parts. */
    if (ppu.w2001_delay && --ppu.w2001_delay == 0) {
        uint8_t val = ppu.w2001_value;
        ppu.show_bg8 = (val & 0x02) != 0;
        ppu.show_spr8 = (val & 0x04) != 0;
        ppu.show_bg = ppu.instant_bg = (val & 0x08) != 0;
        ppu.show_spr = ppu.instant_spr = (val & 0x10) != 0;
    }
    if (ppu.w2001_oam_delay && --ppu.w2001_oam_delay == 0) {
        if (ppu.w2001_was_rendering && !(ppu.w2001_value & 0x18) && render_line() && !ppu.oamc_pending)
            ppu.oamc_disabled = 1;
    }
    if (ppu.w2001_emph_delay && --ppu.w2001_emph_delay == 0) {
        ppu.greyscale = ppu.w2001_value & 1;
        ppu.emphasis = ppu.w2001_value >> 5;
    }

#ifdef NESRECOMP_CYCLE_HDPACK
    cyc_hdpack_clock();
#endif
    if(cyc_background_enabled){cyc_background_pipe[3]=cyc_background_pipe[2];cyc_background_pipe[2]=cyc_background_pipe[1];cyc_background_pipe[1]=cyc_background_pipe[0];}
    ppu.color[3] = ppu.color[2];
    ppu.color[2] = ppu.color[1];
    ppu.color[1] = ppu.color[0];
    if (render_line() || (sl == 240 && dot == 0)) {
        if ((dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 336)) {
            if (eval_rendering()) bg_fetch();
        } else if (dot >= 337 || dot == 0) {
            if (eval_rendering()) bg_fetch_tail();
        }
        if (dot >= 1 && dot <= 256) {
            if (sl < 240) compute_pixel();
            shift_sprites();
        }
        if (sl < 240) output_pixel();
    }

    data_sm_dot_end();
    io_bus_decay();
}

static void general_half_dot(void)
{
    if (render_line() && rendering() && ((ppu.dot >= 1 && ppu.dot <= 257) || (ppu.dot >= 321 && ppu.dot <= 336))) {
#if NESRECOMP_PPU_HLE
        ppu.bg_pixels = (ppu.bg_pixels << 2) | 2u;
        ppu.attr_pixels = (ppu.attr_pixels << 2) | ppu.attr_latch;
#else
        ppu.bg_lo = (uint16_t)(ppu.bg_lo << 1);
        ppu.bg_hi = (uint16_t)(ppu.bg_hi << 1 | 1);
        ppu.attr_lo = (uint16_t)(ppu.attr_lo << 1 | (ppu.attr_latch & 1));
        ppu.attr_hi = (uint16_t)(ppu.attr_hi << 1 | ((ppu.attr_latch >> 1) & 1));
#endif
#ifdef NESRECOMP_CYCLE_HDPACK
        cyc_hdpack_bg_shift();
#endif
    }
    if (ppu.oam2_reset > 0 && --ppu.oam2_reset == 0) {
        ppu.oam2_addr = 0;
        ppu.oam2_full = 0;
    }
    if (ppu.commit) bg_commit();
    if (eval_rendering() || rendering()) ppu.oam_buffer = ppu.oam_buffer_in;
    half_dot_status();
    data_sm_half();
}

#if NESRECOMP_PPU_HLE
/* A native rendering span owns the stable interior of a visible scanline.
 * Register access invalidates dot_kind, so no delayed CPU write/read or data
 * port transaction is hidden by this classification. Edge dots retain the
 * existing adapter (scroll copies, odd skip, VBlank and sprite loading).
 * This is selected once as latches settle, not guarded afresh for each pixel. */
static bool render_span_ready(void)
{
    return ppu.scanline < 240 && ppu.dot >= 8 && ppu.dot < 253 &&
           rendering() && sm_rest && ppu.render_count == 5 &&
           ppu.eval_bg == ppu.show_bg && ppu.eval_spr == ppu.show_spr &&
           ppu.instant_bg == ppu.show_bg && ppu.instant_spr == ppu.show_spr &&
           !(ppu.skipped_dot | ppu.w2001_delay | ppu.w2001_oam_delay |
             ppu.w2001_emph_delay | ppu.w2005_delay | ppu.w2006_delay |
             ppu.oamc_disabled | ppu.oamc_disabled_now | ppu.oamc_pending |
             ppu.palc_disabled | ppu.palc_v_left | ppu.read2002 |
             ppu.vset | ppu.vblank_pending) && ppu.vset_latch1;
}

static void render_span_dot(void)
{
    ++ppu.dot;
    if (hw_zapper.port) ++hw_zapper.dots;
    ppu.overflow_late = ppu.overflow;
    ppu.blnk_latch = 0;
    ppu.pal_enable = 0;
    ppu.rd = ((ppu.dot - 1) & 1) != 0;
    hw_cart_ppu_rd(ppu.rd != 0);
    ppu.ale = !ppu.rd;
    ppu.oam_latch = ppu.oam_buffer;
    ppu.copy_v = 0;
    if (ppu.oam2_reset > 0) --ppu.oam2_reset;
    sprite_evaluation();
    if (ppu.dot == 63) ppu.oam2_reset = 3;
#ifdef NESRECOMP_CYCLE_HDPACK
    cyc_hdpack_clock();
#endif
    if (cyc_background_enabled) {
        cyc_background_pipe[3] = cyc_background_pipe[2];
        cyc_background_pipe[2] = cyc_background_pipe[1];
        cyc_background_pipe[1] = cyc_background_pipe[0];
    }
    ppu.color[3] = ppu.color[2];
    ppu.color[2] = ppu.color[1];
    ppu.color[1] = ppu.color[0];
    bg_fetch();
    compute_pixel();
    shift_sprites();
    output_pixel();
    io_bus_decay();
}

static void render_span_half_dot(void)
{
    ppu.bg_pixels = (ppu.bg_pixels << 2) | 2u;
    ppu.attr_pixels = (ppu.attr_pixels << 2) | ppu.attr_latch;
#ifdef NESRECOMP_CYCLE_HDPACK
    cyc_hdpack_bg_shift();
#endif
    if (ppu.oam2_reset > 0 && --ppu.oam2_reset == 0) {
        ppu.oam2_addr = 0;
        ppu.oam2_full = 0;
    }
    if (ppu.commit) bg_commit();
    ppu.oam_buffer = ppu.oam_buffer_in;
    /* VBlank has no edge inside this span, but sprite zero retains its two
     * half-dot publication stages for CPU status consumers. */
    ppu.vset_latch2 = 0;
    ppu.s0hit_late = ppu.s0hit;
    if (ppu.s0hit_pending2) {
        ppu.s0hit_pending2 = 0;
        ppu.s0hit = 1;
    }
    if (ppu.s0hit_pending1) {
        ppu.s0hit_pending1 = 0;
        ppu.s0hit_pending2 = 1;
    }
    ppu.ale = 0;
    ppu.wr = 0;
}
#endif
/* A general dot can only lead to a blank one once rendering is off: while it
 * is on, the classification stays until a register access. */
/* A12-A13 are direct pins on the cartridge connector, so a mapper that
 * watches the PPU's address (MMC3's IRQ counter) sees a new address as soon as
 * the PPU drives one. That is the first half of a dot: a fetch puts its
 * address out with ALE, and the second half only replaces the low byte the
 * octal latch holds with the data read, which A12 is not part of. (Measured:
 * over 3,000 frames of SMB3, every A12 edge the MMC3 counted was seen in the
 * first half.) A cartridge that does not watch the bus costs one predictable
 * branch (hw_cart_ppu_addr). */
void ppu_dot(void)
{
    /* The refresh begins independently of $2001 and must invalidate a
     * blank-dot classification cached earlier in VBlank. */
    if (hw_pal() && ppu.scanline >= 264) dot_kind = DOT_UNKNOWN;
    if (dot_kind == DOT_UNKNOWN) dot_kind = is_blank() ? DOT_BLANK : DOT_GENERAL;
    if (dot_kind == DOT_BLANK) {
        blank_dot();
#if NESRECOMP_PPU_HLE
    } else if (dot_kind == DOT_RENDER && ppu.dot < 253) {
        render_span_dot();
#endif
    } else {
        general_dot();
        if (!rendering()) dot_kind = DOT_UNKNOWN;
#if NESRECOMP_PPU_HLE
        else dot_kind = render_span_ready() ? DOT_RENDER : DOT_GENERAL;
#endif
    }
    hw_cart_ppu_addr(ppu.vbus);
}

void ppu_half_dot(void)
{
    if (dot_kind == DOT_UNKNOWN) dot_kind = is_blank() ? DOT_BLANK : DOT_GENERAL;
    if (dot_kind == DOT_BLANK) {
        blank_half_dot();
#if NESRECOMP_PPU_HLE
    } else if (dot_kind == DOT_RENDER) {
        render_span_half_dot();
#endif
    } else {
        general_half_dot();
        if (!rendering()) dot_kind = DOT_UNKNOWN;
#if NESRECOMP_PPU_HLE
        else if (render_span_ready()) dot_kind = DOT_RENDER;
#endif
    }
}

/* ---- registers ---- */

static uint8_t read_oam(void)
{
    return (rendering() && ppu.scanline < 240) || pal_oam_refresh() ? ppu.oam_latch : ppu.oam[ppu.oam_addr];
}

static uint8_t read_register(uint16_t addr)
{
    uint8_t value;
    switch (addr & 7) {
    case 2:
        /* VBlank is sampled as the read starts, the sprite flags and open
         * bus as it ends. */
        value = ppu.vblank ? 0x80 : 0;
        ppu.read2002 = 1;
        hw_clock_run_ticks(7);
        value |= (uint8_t)((ppu.s0hit_late ? 0x40 : 0) | (ppu.overflow_late ? 0x20 : 0) | (ppu.io_bus & 0x1F));
        ppu.addr_latch = 0;
        ppu.io_bus = value;
        io_bus_refresh(0xE0);
        return value;
    case 4:
        hw_clock_run_ticks(7);
        value = read_oam();
        ppu.io_bus = value;
        io_bus_refresh(0xFF);
        return value;
    case 7:
        hw_cart_ppu_data_read();
        if ((ppu.vbus & 0x3FFF) >= 0x3F00) {
            uint16_t pal = ppu.v & 0x1F;
            if ((pal & 3) == 0) pal &= 0x0F;
            value = (uint8_t)((ppu.palette[pal] & (ppu.greyscale ? 0x30 : 0x3F)) | (ppu.io_bus & 0xC0));
        } else {
            value = ppu.read_buffer;
        }
        ppu.io_bus = value;
        io_bus_refresh(0xFF);
        hw_clock_run_ticks(7);
        ppu.rd_sr = 1;
        sm_rest = false;
        return value;
    default:
        return ppu.io_bus;
    }
}

static void write_register(uint16_t addr, uint8_t value)
{
    ppu.io_bus = value;
    io_bus_refresh(0xFF);
    switch (addr & 7) {
    case 0:
        /* The nametable bits of t take the bus's previous value first: the
         * register latches before the CPU drives the new byte. */
        ppu.t = (uint16_t)((ppu.t & 0x73FF) | ((hw.data_bus & 3) << 10));
        hw_clock_run_ticks(2);
        ppu.nmi_enable = (value & 0x80) != 0;
        ppu.inc32 = (value & 0x04) != 0;
        ppu.sprite16 = (value & 0x20) != 0;
        ppu.sprite_table = (value & 0x08) != 0;
        ppu.bg_table = (value & 0x10) != 0;
        ppu.t = (uint16_t)((ppu.t & 0x73FF) | ((value & 3) << 10));
        break;
    case 1: {
        static const uint8_t mask_delay[4] = {2, 2, 3, 2};
        static const uint8_t oam_delay[4] = {2, 3, 3, 2};
        bool was = rendering(), now = (value & 0x18) != 0;
        ppu.w2001_delay = hw_pal() ? 2 : mask_delay[hw.align];
        ppu.w2001_oam_delay = hw_pal() ? 2 : oam_delay[hw.align];
        ppu.w2001_was_rendering = was;
        ppu.instant_bg = ppu.show_bg;
        ppu.instant_spr = ppu.show_spr;
        if (was && !now) {
            if (ppu.scanline < 241 || ppu.scanline == hw_prerender_line()) {
                ppu.oamc_disabled_now = 1;
                if ((ppu.dot & 7) < 2 && ppu.dot <= 250 && (ppu.v & 0x3FFF) >= 0x3C00) ppu.palc_disabled = 1;
            }
        } else if (!was && now) {
            if ((ppu.scanline < 241 || ppu.scanline == hw_prerender_line()) && ppu.oamc_pending && (hw.align == 1 || hw.align == 2))
                ppu.oamc_reenabled = 1;
        }
        /* Greyscale and blue emphasis follow the bus's previous value
         * immediately at alignments 0 and 3; red and green take the new
         * value immediately; all settle two dots later. */
        if (hw.align == 0 || hw.align == 3) {
            ppu.greyscale = hw.data_bus & 1;
            ppu.emphasis = (uint8_t)((ppu.emphasis & 3) | ((hw.data_bus & 0x80) ? 4 : 0));
        }
        ppu.w2001_emph_delay = 2;
        ppu.emphasis = (uint8_t)((ppu.emphasis & 4) | ((value >> 5) & 3));
        ppu.w2001_value = value;
        break;
    }
    case 3:
        ppu.oam_addr = value;
        break;
    case 4:
        if (!pal_oam_refresh() && (!rendering() || (ppu.scanline >= 240 && ppu.scanline < hw_prerender_line()))) {
            if ((ppu.oam_addr & 3) == 2) value &= 0xE3;
            ppu.oam[ppu.oam_addr++] = value;
        } else {
            /* A write during rendering only bumps the address. */
            ppu.oam_addr = (uint8_t)((ppu.oam_addr + 4) & 0xFC);
        }
        break;
    case 5:
        ppu.w2005_delay = !hw_pal() && hw.align == 2 ? 2 : 1;
        ppu.w2005_value = value;
        /* Until it lands, the scroll takes the bus's previous value. */
        if (!ppu.addr_latch) {
            ppu.fine_x = hw.data_bus & 7;
            ppu.t = (uint16_t)((ppu.t & 0x7FE0) | (hw.data_bus >> 3));
        } else {
            ppu.t = (uint16_t)((ppu.t & 0x0C1F) | ((hw.data_bus & 0xF8) << 2) | ((hw.data_bus & 7) << 12));
        }
        break;
    case 6:
        if (!ppu.addr_latch) {
            ppu.t = (uint16_t)((ppu.t & 0x00FF) | ((value & 0x3F) << 8));
        } else {
            ppu.t = (uint16_t)((ppu.t & 0x7F00) | value);
            if (content_write_hook && !hw_isolated)
                content_write_hook(6, ppu.t & 0x3FFF, value, ppu.inc32 ? 32 : 1);
            ppu.w2006_value = ppu.t;
            ppu.w2006_old_v = ppu.v;
            ppu.w2006_delay = !hw_pal() && hw.align == 2 ? 5 : 4;
        }
        ppu.addr_latch = !ppu.addr_latch;
        break;
    case 7:
        if (content_write_hook && !hw_isolated)
            content_write_hook(7, ppu.v & 0x3FFF, value, ppu.inc32 ? 32 : 1);
        ppu.write_data = value;
        hw_clock_run_ticks(7);
        ppu.wr_sr = 1;
        sm_rest = false;
        break;
    default: /* $2002 */
        break;
    }
}

/* Register accesses can change what the blank-dot test reads, including
 * after the ticks they run themselves. */
uint8_t ppu_read(uint16_t addr)
{
    dot_kind = DOT_UNKNOWN;
    uint8_t value = read_register(addr);
    dot_kind = DOT_UNKNOWN;
    return value;
}

void ppu_write(uint16_t addr, uint8_t value)
{
    dot_kind = DOT_UNKNOWN;
    write_register(addr, value);
    dot_kind = DOT_UNKNOWN;
}

/* ---- power-on ---- */

void ppu_state_reloaded(void)
{
    sm_rest = false;
    dot_kind = DOT_UNKNOWN;
}

void ppu_power_on(void)
{
    memset(&ppu, 0, sizeof(ppu));
    sm_rest = false;
    dot_kind = DOT_UNKNOWN;
    ppu.io_decay_next = UINT64_MAX;
    ppu.odd_frame = 1;
    memset(ppu.oam2, 0xFF, sizeof(ppu.oam2));
    /* CIRAM at power-on holds a pattern of $F0 and $0F runs, and palette RAM
     * the values of the console AccuracyCoin's power-on pages were drawn
     * from (neither is defined by the hardware). */
    for (int i = 0; i < 0x800; i++) {
        bool bit1_clear = (i & 2) == 0, upper = (i & 0x1F) >= 0x10;
        ppu.ciram[i] = bit1_clear != upper ? 0xF0 : 0x0F;
    }
    static const uint8_t power_on_palette[32] = {
        0x00, 0x00, 0x28, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x01, 0x20, 0x00, 0x08, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00,
    };
    memcpy(ppu.palette, power_on_palette, sizeof(ppu.palette));
    memset(hw_frame_index, 0, sizeof(hw_frame_index));
    memset(hw_frame_lines, 0, sizeof(hw_frame_lines));
    memset(hw_frame_bg, 0, sizeof(hw_frame_bg));
}

/* ---- state hash ---- */

#define PPU_FIELD(f) {#f, offsetof(HwPpu, f), sizeof(((HwPpu *)0)->f)}
static const struct {
    const char *name;
    size_t      offset, size;
} ppu_fields[] = {
    PPU_FIELD(scanline), PPU_FIELD(dot), PPU_FIELD(odd_frame), PPU_FIELD(skipped_dot), PPU_FIELD(nmi_enable),
    PPU_FIELD(inc32), PPU_FIELD(sprite16), PPU_FIELD(sprite_table), PPU_FIELD(bg_table), PPU_FIELD(greyscale),
    PPU_FIELD(emphasis), PPU_FIELD(show_bg8), PPU_FIELD(show_spr8), PPU_FIELD(show_bg), PPU_FIELD(show_spr),
    PPU_FIELD(eval_bg), PPU_FIELD(eval_spr), PPU_FIELD(instant_bg), PPU_FIELD(instant_spr),
    PPU_FIELD(render_count), PPU_FIELD(w2001_value), PPU_FIELD(w2001_delay), PPU_FIELD(w2001_emph_delay),
    PPU_FIELD(w2001_oam_delay), PPU_FIELD(w2001_was_rendering), PPU_FIELD(w2005_value), PPU_FIELD(w2005_delay),
    PPU_FIELD(w2006_delay), PPU_FIELD(copy_v), PPU_FIELD(w2006_value), PPU_FIELD(w2006_old_v), PPU_FIELD(v),
    PPU_FIELD(t), PPU_FIELD(fine_x), PPU_FIELD(addr_latch), PPU_FIELD(vblank), PPU_FIELD(s0hit),
    PPU_FIELD(s0hit_late), PPU_FIELD(s0hit_pending1), PPU_FIELD(s0hit_pending2), PPU_FIELD(can_s0hit),
    PPU_FIELD(overflow), PPU_FIELD(overflow_late), PPU_FIELD(vset), PPU_FIELD(vset_latch1), PPU_FIELD(vset_latch2),
    PPU_FIELD(vblank_pending), PPU_FIELD(read2002), PPU_FIELD(io_bus), PPU_FIELD(io_decay_clock),
    PPU_FIELD(io_decay_deadline), PPU_FIELD(read_buffer), PPU_FIELD(oam_addr), PPU_FIELD(vbus),
    PPU_FIELD(octal_latch), PPU_FIELD(ale), PPU_FIELD(rd), PPU_FIELD(wr), PPU_FIELD(rd_sr), PPU_FIELD(wr_sr),
    PPU_FIELD(rl), PPU_FIELD(wl), PPU_FIELD(pd_rb), PPU_FIELD(rd_ale), PPU_FIELD(wr_ale), PPU_FIELD(db_par),
    PPU_FIELD(tstep_latch), PPU_FIELD(blnk_latch), PPU_FIELD(pal_enable), PPU_FIELD(write_data),
    PPU_FIELD(par_chr), PPU_FIELD(fetch_data), PPU_FIELD(commit), PPU_FIELD(lo_plane), PPU_FIELD(hi_plane),
    PPU_FIELD(attribute), PPU_FIELD(attr_latch), PPU_FIELD(bg_lo), PPU_FIELD(bg_hi), PPU_FIELD(attr_lo),
    PPU_FIELD(attr_hi), PPU_FIELD(oam2), PPU_FIELD(oam2_addr), PPU_FIELD(oam2_full), PPU_FIELD(oam2_reset),
    PPU_FIELD(eval_tick), PPU_FIELD(eval_wrapped), PPU_FIELD(eval_nine), PPU_FIELD(eval_odd_corrupt),
    PPU_FIELD(sprite_row), PPU_FIELD(oam_buffer_in), PPU_FIELD(oam_buffer), PPU_FIELD(oam_latch),
    PPU_FIELD(spr_lo), PPU_FIELD(spr_hi), PPU_FIELD(spr_attr), PPU_FIELD(spr_x), PPU_FIELD(load_tile),
    PPU_FIELD(load_attr), PPU_FIELD(next_has_s0), PPU_FIELD(cur_has_s0), PPU_FIELD(oamc_disabled),
    PPU_FIELD(oamc_disabled_now), PPU_FIELD(oamc_pending), PPU_FIELD(oamc_index), PPU_FIELD(oamc_reenabled),
    PPU_FIELD(palc_disabled), PPU_FIELD(palc_v_left), PPU_FIELD(color),
};

uint64_t ppu_state_hash(uint64_t h)
{
#if NESRECOMP_PPU_HLE
    /* Test-only observation: normalize private packed rows for the established
     * differential trace schema. No second implementation executes. */
    HwPpu observed = ppu;
    observed.bg_lo = ppu_hle_plane(ppu.bg_pixels, 0);
    observed.bg_hi = ppu_hle_plane(ppu.bg_pixels, 1);
    observed.attr_lo = ppu_hle_plane(ppu.attr_pixels, 0);
    observed.attr_hi = ppu_hle_plane(ppu.attr_pixels, 1);
    const HwPpu *state = &observed;
#else
    const HwPpu *state = &ppu;
#endif
    uint64_t acc = 0;
    for (size_t k = 0; k < sizeof(ppu_fields) / sizeof(ppu_fields[0]); k++) {
        const uint8_t *b = (const uint8_t *)state + ppu_fields[k].offset;
        for (size_t i = 0; i < ppu_fields[k].size; i++) acc = acc * 131 + b[i];
    }
    return h ^ (acc + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
}

void ppu_state_dump(void *file)
{
#if NESRECOMP_PPU_HLE
    HwPpu observed = ppu;
    observed.bg_lo = ppu_hle_plane(ppu.bg_pixels, 0);
    observed.bg_hi = ppu_hle_plane(ppu.bg_pixels, 1);
    observed.attr_lo = ppu_hle_plane(ppu.attr_pixels, 0);
    observed.attr_hi = ppu_hle_plane(ppu.attr_pixels, 1);
    const HwPpu *state = &observed;
#else
    const HwPpu *state = &ppu;
#endif
    FILE *f = (FILE *)file;
    for (size_t k = 0; k < sizeof(ppu_fields) / sizeof(ppu_fields[0]); k++) {
        const uint8_t *b = (const uint8_t *)state + ppu_fields[k].offset;
        fprintf(f, "ppu.%s", ppu_fields[k].name);
        for (size_t i = 0; i < ppu_fields[k].size; i++) fprintf(f, "%s%02X", i % 32 ? "" : " ", b[i]);
        fputc('\n', f);
    }
}
