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
#include "mmc5_state.h"
#include "../../common/nes_cart.h"
#include "../../common/nes_eeprom.h"
#include "../../common/nes_barcode.h"

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

/* Namco 163/175/340 registers (hw_namco.inc); the 163's RAM is exram[0-127]. */
typedef struct {
    uint8_t chr[8], nt[4], prg[3], protect, ram_enable;
    uint8_t sound_addr, sound_inc, sound_step, channel;
    uint16_t irq;              /* bit 15: enable; bits 0-14: counter */
    int16_t output;            /* the channel currently held on the DAC */
} HwNamco;

/* Tengen RAMBO-1 registers beyond the MMC3's (hw_tengen.inc). */
typedef struct {
    uint8_t reg[16], cycle_mode, prescaler, delay;
    uint8_t chr_a17[8];        /* CHR A17 per physical pattern page (mapper 158 CIRAM A10) */
} HwRambo;

/* Jaleco SS88006 registers (hw_jaleco.inc). */
typedef struct {
    uint8_t prg[3], chr[8], ram_ctrl, reload[4], ctrl, mirror;
    uint16_t counter;
} HwJaleco;

/* Sunsoft FME-7 registers, and the 5B's sound generator (hw_sunsoft.inc). */
typedef struct {
    uint8_t command, chr[8], prg[4], mirror, irq_ctrl;
    uint16_t counter;
} HwFme7;

typedef struct {
    uint8_t reg[16], address;
    uint8_t prescale, noise_half, noise_count, tone_out;
    uint8_t env_level, env_up, env_hold;
    uint16_t tone_count[3], env_count;
    uint32_t lfsr;
} Hw5B;

/* The FDS RAM Adapter's registers and the drive's head/transfer state
 * (hw_fds.c). The disk sides themselves live in hw_fds.c: they are media,
 * kept across power cycles. */
typedef struct {
    uint16_t irq_reload, irq_counter;
    uint8_t  irq_enabled, irq_repeat;          /* $4022 */
    uint8_t  disk_regs, sound_regs;            /* $4023 */
    uint8_t  write_data;                       /* $4024 */
    uint8_t  ctrl;                             /* $4025 as written */
    uint8_t  motor_on, reset_transfer, read_mode, crc_control, crc_enable, transfer_irq;
    uint8_t  ext_out;                          /* $4026 */
    uint8_t  timer_irq, disk_irq;              /* /IRQ sources */
    uint8_t  transfer, read_data, bad_crc;     /* $4030/$4031 */
    uint8_t  end_of_head, gap_ended, scanning, prev_crc_control, at_end;
    uint16_t crc;
    uint32_t delay, position;
    /* The sound unit (hw_fds.c "Sound"): Mesen's FdsAudio, its volume and
     * modulator channels (BaseFdsChannel, ModChannel), field for field. */
    uint8_t  wave[64], wave_write;             /* $4040-$407F, $4089.7 */
    uint8_t  sound_reg[0x0B];                  /* $4080-$408A as written */
    uint8_t  vol_speed, vol_gain, vol_env_off, vol_increase;   /* $4080 */
    uint8_t  mod_speed, mod_gain, mod_env_off, mod_increase;   /* $4084 */
    uint16_t vol_freq, mod_freq;               /* $4082/$4083, $4086/$4087: 12 bits */
    uint32_t vol_timer, mod_timer;             /* CPU cycles to the next envelope tick */
    uint8_t  master_speed;                     /* $408A, both envelopes */
    uint8_t  env_disabled, wave_halt;          /* $4083 bits 6, 7 */
    uint8_t  master_vol;                       /* $4089 bits 0-1 */
    int8_t   mod_counter;                      /* $4085: 7-bit signed */
    uint8_t  mod_disabled;                     /* $4087.7 */
    uint8_t  mod_pos;                          /* 0-63 */
    uint8_t  mod_table[64];                    /* 3-bit steps, each written twice by $4088 */
    uint16_t mod_overflow, wave_overflow;      /* 16-bit phase accumulators */
    int32_t  mod_output;                       /* the pitch adjustment */
    uint8_t  wave_pos;                         /* 0-63 */
    uint8_t  out_level;                        /* 0-63: the channel's output */
} HwFds;

typedef struct {
    uint8_t *prg;
    uint32_t prg_len;
    uint32_t prg_slots;         /* prg_len / 0x1000, rounded up to a power of 2 */
    uint8_t *chr;               /* CHR ROM, or CHR RAM */
    uint32_t chr_len;
    uint32_t chr_pages;         /* chr_len / 0x400, rounded up to a power of 2 */
    uint8_t  chr_ram;           /* all CHR is RAM */
    /* CHR RAM inside chr: all of it on CHR-RAM boards, or the chip after the
     * padded CHR ROM on boards with both (TQROM). A program can read it back,
     * so it is compared across implementations in cyc_mem_hash. */
    uint32_t chr_ram_base, chr_ram_len;
    uint8_t  chr_write[8];      /* the 1KB page is writable RAM */
    uint8_t  chr_ciram;         /* bit per 1KB page backed by CIRAM (Namco 163); chr_off
                                   then holds the CIRAM page offset, 0 or 0x400 */

    /* Where each window reads from, as a byte offset into prg/chr. Mappers
     * set these only through hw_cart_map_prg8()/hw_cart_map_chr1(). */
    uint32_t prg_off[8];        /* $8000, $9000, ... $F000 */
    uint32_t chr_off[8];        /* $0000, $0400, ... $1C00 */

    NesCartInfo info;
    NesEeprom eeprom[2];
    NesBarcode barcode;
    uint8_t exram[1024]; /* MMC5 ExRAM or Namco 163 internal RAM, battery-powered separately. */
    uint32_t wram_len, wram_bank;
    uint16_t mapper;            /* iNES mapper number */
    uint8_t  mirroring;         /* HwMirroring, as the cartridge drives CIRAM A10 */
    uint8_t  watch_ppu_addr;    /* the mapper needs every PPU address (MMC3) */
    uint8_t  watch_cpu;
    uint8_t  clock_late;        /* the board is clocked before the CPU's access (FDS) */

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
        uint8_t  data_reads;        /* $2007 reads since power-on (mapper 185 submapper 0) */
        uint8_t  pattern_pending;
        uint16_t pattern_addr;
        uint16_t vrc_chr[8], irq_latch16, irq_counter16;
        int16_t irq_prescaler;
        uint8_t irq_mode;
        Mmc5State mmc5;
        HwFds fds;
        HwVrc6Audio vrc6_audio;
        HwNamco namco;
        HwJaleco jaleco;
        HwRambo rambo;
        HwFme7 fme7;
        Hw5B s5b;
        uint8_t vrc7_reg[64], vrc7_address;
        uint64_t vrc7_phase;
        int16_t vrc7_output;
    } m;
} HwCart;

extern HwMachine hw;
extern HwCart    hw_cart;

/* PRG ROM as the CPU sees it at addr ($8000-$FFFF). */
HW_ALWAYS_INLINE uint8_t hw_cart_prg_read(uint16_t addr)
{
    uint32_t offset=hw_cart.prg_off[(addr >> 12)&7] | (addr&4095);
    if (offset&MMC5_PRG_OPEN) return hw.data_bus;
    if (offset&MMC5_PRG_RAM) return hw_cart.wram[offset&0x1ffff];
    return hw_cart.prg[offset];
}

/* CHR as the PPU sees it at a ($0000-$1FFF). */
HW_ALWAYS_INLINE uint32_t hw_cart_chr_index(uint16_t a)
{
    if (hw_cart.mapper==5) return hw_cart_mmc5_chr_index(a);
    uint32_t index = hw_cart.chr_off[(a >> 10) & 7] | (a & 0x3FF);
    return hw_cart.chr_ram && hw_cart.chr_len ? index % hw_cart.chr_len : index;
}

/* CIRAM A10 as the cartridge drives it, as a CIRAM index bit. */
HW_ALWAYS_INLINE uint16_t hw_cart_ciram_a10(uint16_t vbus)
{
    if (hw_cart.mapper == 24 || hw_cart.mapper == 26 || hw_cart.mapper == 118 || hw_cart.mapper == 158 || hw_cart.mapper == 95 || hw_cart.mapper == 19 || hw_cart.mapper == 207)
        return hw_cart_nt_a10(vbus);
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

/* The code watch: RAM bytes that some compiled RAM view folds to a constant
 * (cyc_ramview.c fills it). Indexed by physical RAM byte: CPU RAM 0-$7FF,
 * then the FDS PRG RAM ($6000-$DFFF) from HW_CODE_PRG_RAM. Every store that
 * reaches RAM (CPU writes; the DMAs only read) goes through hw_bus_write or
 * fds_cpu_write, which call hw_code_write before a watched byte changes
 * value. It observes and never alters what the machine does. */
#define HW_CODE_PRG_RAM 0x800u
#define HW_CODE_BYTES   (HW_CODE_PRG_RAM + 0x8000u)
extern uint8_t hw_code_watch[HW_CODE_BYTES];
extern void (*hw_code_write)(unsigned phys, uint8_t value);
HW_ALWAYS_INLINE void hw_code_store(uint8_t *cell, unsigned phys, uint8_t value)
{
    if (hw_code_watch[phys] && *cell != value) hw_code_write(phys, value);
    *cell = value;
}

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
/* The FDS boot skip's view of PPU memory and the snapshot hook (hw_ppu.c). */
void    ppu_hle_store(uint16_t addr, uint8_t value);
uint8_t ppu_hle_peek(uint16_t addr);
void    ppu_restored(void);
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
/* The console output stage (cyc_set_console): 0 = by board, 1 = NES,
 * 2 = Famicom (CycConsole). apu_console() is the model in effect. */
void    apu_set_console(int console);
int     apu_console(void);
/* One output sample's level straight into the output stage, as the mixer's
 * per-sample average would be (tests: the stage's impulse response). */
void    apu_debug_emit(double level);
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

/* The APU as a machine snapshot holds it (cyc_fds_skip.c). */
size_t  apu_snapshot_size(void);
void    apu_snapshot_save(void *out);
void    apu_snapshot_load(const void *in);

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
