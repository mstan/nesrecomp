/*
 * hw_internal.h - NESRecomp's NES hardware: state shared between its parts.
 *
 * The hardware behind hw.h is split by chip:
 *
 *   hw_machine.c   master clock, the CPU's view of each cycle (hw.h), the CPU
 *                  memory map and open bus, NROM cartridge, power-on, host API
 *   hw_apu.c       the rest of the 2A03: frame counter, length counters, DMC,
 *                  DMAs, controller ports, IRQ, and the audio channels
 *   hw_ppu.c       the 2C02
 *   hw_palette.c   color index -> ARGB, from a model of the NTSC signal
 *
 * Hosts must not include this header; they use cyc_core.h.
 *
 * Timing model. The NTSC master clock runs 12 ticks per CPU cycle and 4 per
 * PPU dot. Within a CPU cycle the ticks are numbered 0-11: the CPU puts its
 * access on the bus at tick 0, samples its NMI input at tick 4 and its IRQ
 * input at tick 7. A PPU dot is clocked on the ticks where (alignment + tick)
 * is a multiple of 4 and its second half two ticks later; the alignment (0-3)
 * is fixed at power-on. The APU is clocked once per CPU cycle on tick 0,
 * after the CPU's access and the PPU.
 *
 * Most of what AccuracyCoin measures below the register level (the $2007
 * access state machine, OAM and palette corruption, sprite evaluation
 * address quirks, DMA scheduling, $4015 delays) was characterized on real
 * consoles by the author of AccuracyCoin and TriCNES; where this code follows
 * that research the comments say so. TriCNES itself is used only as the
 * test oracle (tric_core.cpp).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hw_mapper.h"
#include "../../common/nes_cart.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define HW_INLINE static __inline
#define HW_ALWAYS_INLINE static __forceinline
#else
#define HW_INLINE static inline
#define HW_ALWAYS_INLINE static inline __attribute__((always_inline))
#endif

/* ------------------------------------------------------------------------- */
/* Machine: clock, CPU bus, cartridge (hw_machine.c)                          */
/* ------------------------------------------------------------------------- */

typedef struct {
    /* Clock. tick = the next tick of the current CPU cycle whose events have
     * not run (0-12); the CPU acts when it wraps from 12 to 0. */
    uint8_t  tick;
    uint8_t  align;             /* CPU/PPU clock alignment, 0-3 */
    uint64_t cycles;            /* CPU cycles, including DMA cycles */

    /* CPU address and data buses. */
    uint16_t cpu_addr;          /* the address the CPU holds this cycle */
    uint8_t  cpu_reading;       /* R/W high this cycle */
    uint8_t  data_bus;          /* open bus: the last value driven */
    uint8_t  internal_bus;      /* the 2A03's internal bus, seen by $4015 reads */
    uint8_t  data_driven;       /* the current read was driven by some device */

    /* The IRQ input as sampled at tick 7 (NMI goes to the CPU's edge
     * detector at tick 4, cpu_nmi_input). */
    uint8_t  irq_line;

    uint8_t  ram[0x800];
} HwMachine;

/* The cartridge. Address translation is two tables the mapper fills (see
 * hw_mapper.h): PRG in 4KB slots and CHR in 1KB pages, the finest granularity
 * any supported mapper switches. Reads are then one indexed load, and both
 * the recompiler's dispatch and the mapper implementations talk about banks
 * in the same terms. */
typedef struct {
    uint8_t reg[3][3], control, step[3], accumulator;
    uint16_t timer[3];
} HwVrc6Audio;

typedef struct {
    uint8_t *prg;
    uint32_t prg_len;
    uint32_t prg_slots;         /* prg_len / 0x1000, rounded up to a power of 2 */
    uint8_t *chr;               /* CHR ROM, or CHR RAM */
    uint32_t chr_len;
    uint32_t chr_pages;         /* chr_len / 0x400, rounded up to a power of 2 */
    uint8_t  chr_ram;

    /* Where each window reads from, as a byte offset into prg/chr. Mappers
     * set these only through hw_cart_map_prg8()/hw_cart_map_chr1(). */
    uint32_t prg_off[8];        /* $8000, $9000, ... $F000 */
    uint32_t chr_off[8];        /* $0000, $0400, ... $1C00 */

    NesCartInfo info;
    uint32_t wram_len, wram_bank;
    uint16_t mapper;            /* iNES mapper number */
    uint8_t  mirroring;         /* HwMirroring, as the cartridge drives CIRAM A10 */
    uint8_t  watch_ppu_addr;    /* the mapper needs every PPU address (MMC3) */
    uint8_t  watch_cpu;

    /* Work RAM at $6000-$7FFF. Boards without it leave the bus open there. */
    uint8_t  wram[0x20000];
    uint8_t  has_wram, wram_readable, wram_writable;

    /* Per-mapper registers. Only the loaded mapper's members are live; they
     * share one struct so that the state hash and dump cover all of them. */
    struct {
        /* MMC1 ($8000-$FFFF serial port). */
        uint8_t  shift, shift_count, ctrl, chr0, chr1, prg;
        uint64_t last_write_cycle;  /* the serial port ignores back-to-back writes */
        /* MMC3 / MMC6. */
        uint8_t  bank_select, reg[8], mirror_reg, ram_protect;
        uint8_t  irq_latch, irq_counter, irq_reload, irq_enable, irq_out;
        uint8_t  a12;               /* A12 as the mapper last saw it */
        uint64_t a12_low_cycle;     /* hw.cycles when A12 went low (0 = high) */
        /* UxROM / CNROM / AxROM / GxROM latches. */
        uint8_t  latch;
        uint8_t  pattern_pending;
        uint16_t pattern_addr;
        uint16_t vrc_chr[8], irq_latch16, irq_counter16;
        int16_t irq_prescaler;
        uint8_t irq_mode;
        HwVrc6Audio vrc6_audio;
    } m;
} HwCart;

extern HwMachine hw;
extern HwCart    hw_cart;

/* PRG ROM as the CPU sees it at addr ($8000-$FFFF). */
HW_ALWAYS_INLINE uint8_t hw_cart_prg_read(uint16_t addr)
{
    return hw_cart.prg[hw_cart.prg_off[(addr >> 12) & 7] | (addr & 0x0FFF)];
}

/* CHR as the PPU sees it at a ($0000-$1FFF). */
HW_ALWAYS_INLINE uint32_t hw_cart_chr_index(uint16_t a)
{
    uint32_t index = hw_cart.chr_off[(a >> 10) & 7] | (a & 0x3FF);
    return hw_cart.chr_ram && hw_cart.chr_len ? index % hw_cart.chr_len : index;
}

/* CIRAM A10 as the cartridge drives it, as a CIRAM index bit. */
HW_ALWAYS_INLINE uint16_t hw_cart_ciram_a10(uint16_t vbus)
{
    if (hw_cart.mapper == 24 || hw_cart.mapper == 26) return hw_cart_nt_a10(vbus);
    if (hw_cart.info.four_screen) return vbus & 0xc00;
    switch (hw_cart.mirroring) {
    case HW_MIRROR_HORIZONTAL: return (vbus & 0x800) ? 0x400 : 0;
    case HW_MIRROR_VERTICAL:   return (vbus & 0x400) ? 0x400 : 0;
    case HW_MIRROR_SCREEN_B:   return 0x400;
    default:                   return 0;
    }
}

/* Every PPU address, for the mappers that watch the bus. A cartridge that
 * does not is one predictable branch. */
HW_ALWAYS_INLINE void hw_cart_ppu_addr(uint16_t vbus)
{
    if (hw_cart.watch_ppu_addr) hw_cart_ppu_addr_watched(vbus);
}

/* Advance the master clock by n ticks within the current CPU cycle. Used by
 * register accesses that span part of a cycle (PPU reads and some writes). */
void hw_clock_run_ticks(int n);

/* A CPU or DMA bus access through the memory map. */
uint8_t hw_bus_read(uint16_t addr);
void    hw_bus_write(uint16_t addr, uint8_t value);

/* ------------------------------------------------------------------------- */
/* PPU (hw_ppu.c)                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    /* ---- position ---- */
    uint16_t scanline, dot;
    uint8_t  odd_frame;
    uint8_t  skipped_dot;       /* this odd frame skipped dot 0 of scanline 0 */

    /* ---- $2000 ---- */
    uint8_t  nmi_enable, inc32, sprite16, sprite_table, bg_table;

    /* ---- $2001 ---- */
    uint8_t  greyscale, emphasis;  /* emphasis: red 1, green 2, blue 4 */
    uint8_t  show_bg8, show_spr8, show_bg, show_spr;
    uint8_t  eval_bg, eval_spr;       /* the masks as sprite evaluation and the
                                         background fetch see them, a dot late */
    uint8_t  instant_bg, instant_spr; /* the masks as the evaluation of the
                                         dot that sees the write sees them */
    uint8_t  render_count;            /* dots since rendering was enabled, to 5 */

    /* ---- register writes waiting for the PPU clock ---- */
    uint8_t  w2001_value, w2001_delay, w2001_emph_delay, w2001_oam_delay, w2001_was_rendering;
    uint8_t  w2005_value, w2005_delay;
    uint8_t  w2006_delay, copy_v;
    uint16_t w2006_value, w2006_old_v;

    /* ---- scroll ---- */
    uint16_t v, t;
    uint8_t  fine_x, addr_latch;

    /* ---- status ---- */
    uint8_t  vblank, s0hit, s0hit_late, s0hit_pending1, s0hit_pending2, can_s0hit;
    uint8_t  overflow, overflow_late;
    uint8_t  vset, vset_latch1, vset_latch2, vblank_pending, read2002;

    /* ---- CPU-side I/O bus (open bus of the PPU registers) ---- */
    uint8_t  io_bus;
    uint64_t io_decay_clock;          /* dots counted while io_bus != 0 */
    uint64_t io_decay_deadline[8];    /* per bit: the clock value that clears it */
    uint64_t io_decay_next;           /* the earliest deadline still ahead */

    uint8_t  read_buffer, oam_addr;

    /* ---- VRAM address/data bus ---- */
    uint16_t vbus;                    /* AD0-7 multiplexed with the low address byte */
    uint8_t  octal_latch;             /* the latched low address byte (ALE) */
    uint8_t  ale, rd, wr;

    /* ---- $2007 access state machine ---- */
    uint8_t  rd_sr, wr_sr;            /* set by the CPU access, cleared by the PPU */
    uint8_t  rl[5], wl[5];            /* half-dot latch chains */
    uint8_t  pd_rb, rd_ale, wr_ale, db_par, tstep_latch, blnk_latch, pal_enable;
    uint8_t  write_data;

    /* ---- background ---- */
    uint16_t par_chr;                 /* pattern address register */
    uint8_t  fetch_data, commit;      /* commit: COMMIT_* fetched this dot */
    uint8_t  lo_plane, hi_plane, attribute, attr_latch;
    uint16_t bg_lo, bg_hi, attr_lo, attr_hi;

    /* ---- sprites ---- */
    uint8_t  oam2[32];
    uint8_t  oam2_addr, oam2_full, oam2_reset;
    uint8_t  eval_tick, eval_wrapped, eval_nine, eval_odd_corrupt;
    uint16_t sprite_row;              /* scanline - Y, as the in-range check sees it */
    uint8_t  oam_buffer_in, oam_buffer, oam_latch;
    uint8_t  spr_lo[8], spr_hi[8], spr_attr[8], spr_x[8];
    uint8_t  load_tile, load_attr;    /* the object being loaded, dots 257-320 */
    uint8_t  next_has_s0, cur_has_s0;

    /* ---- corruption ---- */
    uint8_t  oamc_disabled, oamc_disabled_now, oamc_pending, oamc_index, oamc_reenabled;
    uint8_t  palc_disabled, palc_v_left;

    /* ---- output ---- */
    uint8_t  color[4];                /* chosen color, and the 3 dots it waits */

    /* ---- memories ---- */
    uint8_t  oam[256];
    uint8_t  palette[32];
    uint8_t  ciram[0x1000] /* upper 2 KiB belongs to four-screen cartridges */;
} HwPpu;

extern HwPpu ppu;
/* 9-bit color indices (color | emphasis << 6) and their ARGB. */
extern uint16_t hw_frame_index[256 * 240];
extern uint32_t hw_frame_argb[256 * 240];

void    ppu_power_on(void);
void    ppu_dot(void);
void    ppu_half_dot(void);
uint8_t ppu_read(uint16_t addr);
void    ppu_write(uint16_t addr, uint8_t value);
/* /NMI as the PPU drives it (active high here). */
HW_INLINE bool ppu_nmi_output(void) { return ppu.nmi_enable && ppu.vblank; }

/* ------------------------------------------------------------------------- */
/* APU, DMAs, controller ports (hw_apu.c)                                    */
/* ------------------------------------------------------------------------- */

void    apu_power_on(void);
/* The APU's once-per-CPU-cycle clock (tick 0). */
void    apu_cycle(void);
/* Tick 7: the IRQ input the CPU will poll, and the frame IRQ re-assertion. */
void    apu_sample_irq(void);
/* Register writes ($4000-$4017). */
void    apu_write(uint16_t addr, uint8_t value);
/* Reads decoded by the CPU's address bus (see hw_bus_read): $4015 returns
 * the internal bus it drives; a controller read returns the port's data bit
 * (D0) and clocks the shift register. */
uint8_t apu_read_status(void);
uint8_t apu_read_controller(int port);
bool    hw_oam_dma_active(void);

/* DMAs, in CPU cycle order. */
bool    dma_wants_cycle(void);
void    dma_cycle(void);
void    dma_end_of_cycle(void);

void    hw_set_controller(int port, uint8_t buttons);

/* Audio (hw_apu.c). */
void    apu_audio_enable(bool on, int sample_rate);
size_t  apu_audio_read(int16_t *out, size_t max);
/* The channels' current output levels: pulse 1, pulse 2, triangle, noise
 * (0-15) and DMC (0-127), and the APU's IRQ output. For co-simulation; the
 * tone generators only run while audio is enabled. */
void    apu_channel_levels(uint8_t out[5]);
bool    apu_irq_output(void);
uint16_t apu_noise_lfsr(void);
/* The DMC timer's free-running phase is not defined at power-on; lets a
 * co-simulation start it where another model does. */
void    apu_debug_set_dmc_timer(uint16_t cycles);
void    apu_debug_set_noise(uint16_t lfsr, uint16_t timer);
/* The DMC's memory reader and output unit, for co-simulation traces. */
typedef struct {
    uint16_t addr, bytes, timer;
    uint8_t  buffer, have_buffer, shifter, bits, out_silent, playing, enable, dma;
} ApuDmcView;
void    apu_debug_dmc(ApuDmcView *v);

/* Hardware state hash and dump, per module. */
uint64_t apu_state_hash(uint64_t h);
void     apu_state_dump(void *file);
uint64_t ppu_state_hash(uint64_t h);
void     ppu_state_dump(void *file);

/* ------------------------------------------------------------------------- */
/* Palette (hw_palette.c)                                                    */
/* ------------------------------------------------------------------------- */

/* ARGB8888 for the 512 color indices. */
extern uint32_t hw_palette_argb[512];
void hw_palette_init(void);

#ifdef __cplusplus
}
#endif
