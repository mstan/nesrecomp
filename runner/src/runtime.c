/*
 * runtime.c — NES memory map, PPU register stubs, hardware I/O
 */
#include "nes_runtime.h"
#include "mapper.h"
#include "logger.h"
#include "apu.h"
#include "game_extras.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

CPU6502State g_cpu;
uint8_t      g_ram[0x0800];
uint8_t      g_sram[0x2000]; /* $6000-$7FFF battery-backed SRAM (8KB) */
uint8_t      g_chr_ram[0x2000];
int          g_chr_is_rom = 0;
uint8_t      g_ppu_oam[0x100];
uint8_t      g_ppu_pal[0x20];
uint8_t      g_ppu_nt[0x1000]; /* 4KB nametable RAM: $2000-$2FFF */

uint8_t g_ppuctrl     = 0;
uint8_t g_ppumask     = 0;
uint8_t g_ppustatus   = 0;
uint8_t g_oamaddr     = 0;
uint8_t g_ppuscroll_x = 0;
uint8_t g_ppuscroll_y = 0;
uint16_t g_ppuaddr    = 0;

uint8_t g_ppuscroll_x_hud = 0;
uint8_t g_ppuscroll_y_hud = 0;
uint8_t g_ppuctrl_hud     = 0;
int     g_spr0_split_active = 0;
int     g_spr0_reads_ctr    = 0;  /* sprite-0 hit simulation read counter */
static int     g_ppuaddr_latch = 0;
static int     g_scroll_latch  = 0;
static uint8_t g_ppudata_buf   = 0; /* PPUDATA read buffer (NES read-delay) */

uint64_t g_frame_count = 0;

/* ---- Controller state ---- */
uint8_t g_controller1_buttons = 0;
uint8_t g_controller2_buttons = 0;
static uint8_t s_ctrl1_shift   = 0;
static uint8_t s_ctrl2_shift   = 0;
static bool    s_ctrl1_strobe  = false;

static FILE *s_ppu_trace = NULL;

static void ppu_trace_init(void) {
    s_ppu_trace = fopen("C:/temp/ppu_trace.csv", "w");
    if (s_ppu_trace) {
        fprintf(s_ppu_trace, "DIR,ADDR,VALUE,PC,FRAME\n");
        fflush(s_ppu_trace);
    }
}

static void ppu_trace_write(uint16_t reg, uint8_t val) {
    static uint32_t s_trace_count = 0;
    if (s_ppu_trace && s_trace_count < 50000) {
        fprintf(s_ppu_trace, "W,$%04X,$%02X,PC=?,F=%llu\n",
                reg, val, (unsigned long long)g_frame_count);
        fflush(s_ppu_trace);
        s_trace_count++;
    }
}

void runtime_init(void) {
    memset(&g_cpu, 0, sizeof(g_cpu));
    memset(g_ram,     0, sizeof(g_ram));
    memset(g_sram, 0xFF, sizeof(g_sram)); /* fresh battery SRAM = all 0xFF */
    if (!g_chr_is_rom) memset(g_chr_ram, 0, sizeof(g_chr_ram));
    memset(g_ppu_oam, 0, sizeof(g_ppu_oam));
    memset(g_ppu_pal, 0, sizeof(g_ppu_pal));
    memset(g_ppu_nt,  0, sizeof(g_ppu_nt));
    g_cpu.S = 0xFD;
    g_cpu.I = 1;
    apu_init();
    ppu_trace_init();
}

/* Deterministic VBlank simulation: fires NMI every N bus operations.
 * A real NES has ~29780 CPU cycles per frame. Each nes_read/nes_write
 * represents at least one bus cycle. Counting operations instead of
 * wall-clock time makes the demo playback perfectly deterministic.
 *
 * The threshold is tuned so games run at approximately correct speed
 * when combined with the wall-clock frame pacing in nes_vblank_callback. */
static int  s_vblank_depth = 0;   /* NMI nesting depth (0 = not in NMI) */
#define MAX_VBLANK_DEPTH 3        /* allow limited re-entrancy for games that
                                   * spin-wait inside the NMI handler */
static uint32_t s_ops_count = 0;

/* Cycle budget per frame.  Real NES: 29780.666 CPU cycles between NMIs
 * (341*262/3 for NTSC).  Generated code passes each instruction's cycle
 * count to maybe_trigger_vblank(). */
#define OPS_PER_FRAME 29781

/* bus_tick: called by nes_read/nes_write to count bus operations.
 * Kept for backward compatibility but no longer critical for NMI timing
 * since maybe_trigger_vblank now receives per-instruction cycle counts. */
static inline void bus_tick(void) {
    /* no-op: cycle counting moved to per-instruction maybe_trigger_vblank */
}

void maybe_trigger_vblank(int cycles) {
    /* Always count cycles, even during NMI handler execution.
     * On real NES, the NMI handler consumes CPU cycles that reduce
     * the frame budget.  Without this, the main loop gets a "free"
     * full budget every frame, causing it to run ahead of real NES. */
    s_ops_count += (cycles > 0) ? (uint32_t)cycles : 1;
    if (s_vblank_depth >= MAX_VBLANK_DEPTH) return;
    if (s_ops_count < OPS_PER_FRAME) return;
    s_ops_count = 0;
    s_vblank_depth++;
    /* Set VBlank (bit7), clear sprite-0 hit (bit6) — standard NES VBlank start */
    g_ppustatus = (g_ppustatus & ~0x40) | 0x80;
    g_spr0_split_active = 0;  /* reset per-frame split state */
    g_spr0_reads_ctr    = 0;  /* reset sprite-0 hit counter */
    /* Reset scroll to (0,0) at VBlank start so the HUD region renders with
     * correct scroll.  The NMI handler writes PPUCTRL ($2000) early for the
     * HUD nametable but does NOT write PPUSCROLL ($2005) until after the
     * sprite-0 wait.  Without this reset the HUD captures stale gameplay
     * scroll values from the previous frame, causing flicker. */
    g_ppuscroll_x = 0;
    g_ppuscroll_y = 0;
    g_scroll_latch = 0;
    /* Pre-capture HUD scroll as (0,0) with nametable 0.  On the real NES the
     * sprite-0 hit always fires (it's hardware).  Our counter-based sim can
     * miss because $2002 reads during PPU data transfers contaminate the
     * counter.  By pre-setting valid HUD values here, the renderer can fall
     * back to these even when the counter doesn't trigger. */
    g_ppuscroll_x_hud = 0;
    g_ppuscroll_y_hud = 0;
    g_ppuctrl_hud     = g_ppuctrl & 0x38;  /* keep bits 3-5 (spr size, spr/bg pattern);
                                            * clear bits 0-1 (force NT0 for HUD),
                                            * clear bit 2 (VRAM inc — irrelevant),
                                            * clear bit 7 (NMI enable — irrelevant) */
    /* Only fire NMI if $2000 bit7 (NMI enable) is set — gates init spin-waits correctly.
     * Save and restore the PPU address/scroll latch state around the NMI call.
     * On real hardware, NMI only fires between frames — never mid-instruction-stream.
     * The NMI handler reads $2002 which resets g_ppuaddr_latch.  If game code was
     * between two $2006 writes (latch=1), the NMI's $2002 read would reset it,
     * corrupting the VRAM address for all subsequent writes.  Similarly for scroll. */
    int saved_ppuaddr_latch = g_ppuaddr_latch;
    int saved_scroll_latch  = g_scroll_latch;
    uint16_t saved_ppuaddr  = g_ppuaddr;
    if (g_ppuctrl & 0x80) {
        /* On real 6502, the CPU pushes PCH, PCL, P to the stack before
         * entering the NMI handler.  RTI at the end pops these 3 bytes.
         * We push them here so RTI has correct data.  Additionally, we
         * save and restore S around the call to guarantee the stack is
         * balanced even if the handler has internal imbalances. */
        uint8_t p = (g_cpu.N<<7)|(g_cpu.V<<6)|0x20|(g_cpu.D<<3)|
                    (g_cpu.I<<2)|(g_cpu.Z<<1)|g_cpu.C;
        g_ram[0x100 + g_cpu.S] = 0x00; g_cpu.S--;  /* PCH (dummy) */
        g_ram[0x100 + g_cpu.S] = 0x00; g_cpu.S--;  /* PCL (dummy) */
        g_ram[0x100 + g_cpu.S] = p;    g_cpu.S--;  /* P */
        nes_vblank_callback();
    }
    g_ppuaddr_latch = saved_ppuaddr_latch;
    g_scroll_latch  = saved_scroll_latch;
    g_ppuaddr       = saved_ppuaddr;
    s_vblank_depth--;
}

void runtime_set_vblank_firing(int active) {
    if (active)
        s_vblank_depth++;
    else if (s_vblank_depth > 0)
        s_vblank_depth--;
}

uint8_t nes_read(uint16_t addr) {
    bus_tick();
    if (addr <= 0x1FFF) return g_ram[addr & 0x07FF];
    if (addr >= 0x2000 && addr <= 0x3FFF) return ppu_read_reg(0x2000 + (addr & 7));
    if (addr >= 0x4000 && addr <= 0x401F) {
        if (addr == 0x4015) return apu_read_status();
        if (addr == 0x4016) {
            if (s_ctrl1_strobe) return 0x40 | (g_controller1_buttons >> 7);
            uint8_t bit = (s_ctrl1_shift & 0x80) ? 1 : 0;
            s_ctrl1_shift <<= 1;
            return 0x40 | bit;
        }
        if (addr == 0x4017) {
            if (s_ctrl1_strobe) return 0x40 | (g_controller2_buttons >> 7);
            uint8_t bit = (s_ctrl2_shift & 0x80) ? 1 : 0;
            s_ctrl2_shift <<= 1;
            return 0x40 | bit;
        }
        return 0;
    }
    if (addr >= 0x6000 && addr <= 0x7FFF) return g_sram[addr - 0x6000];
    if (addr >= 0x8000 && addr <= 0xBFFF) {
        const uint8_t *bank = mapper_get_switchable_bank();
        return bank ? bank[addr - 0x8000] : 0xFF;
    }
    if (addr >= 0xC000) {
        const uint8_t *bank = mapper_get_fixed_bank();
        return bank ? bank[addr - 0xC000] : 0xFF;
    }
    return 0xFF;
}

/* Write breakpoint state */
uint16_t g_write_bp_addr = 0xFFFF;
uint8_t  g_write_bp_match_val = 0xFF;
write_bp_callback_t g_write_bp_callback = NULL;

void nes_write(uint16_t addr, uint8_t val) {
    bus_tick();

    if (addr <= 0x1FFF) {
        uint16_t a = addr & 0x07FF;
        if (a == g_write_bp_addr && g_write_bp_callback &&
            (g_write_bp_match_val == 0xFF || val == g_write_bp_match_val)) {
            g_write_bp_callback(a, g_ram[a], val);
        }
        /* Debug: trace GoBankInit dispatch pointer */
        if (a == 0x0B && val >= 0xC0 && g_frame_count >= 120 && g_frame_count <= 130) {
            printf("[JMP_IND] $000B=%02X (lo=$0A=%02X) → target=$%02X%02X frame=%llu A=%02X Y=%02X\n",
                   val, g_ram[0x0A], val, g_ram[0x0A], (unsigned long long)g_frame_count,
                   g_cpu.A, g_cpu.Y);
        }
        /* Debug: trace game state variables */
        if ((a == 0x1D || a == 0x1E || a == 0x24) && val != g_ram[a]) {
            static int s_state_log = 0;
            if (s_state_log < 60) {
                printf("[STATE] $%04X: %02X -> %02X (frame=%llu) A=%02X Y=%02X S=%02X depth=%d\n",
                       a, g_ram[a], val, (unsigned long long)g_frame_count,
                       g_cpu.A, g_cpu.Y, g_cpu.S, s_vblank_depth);
            }
            s_state_log++;
        }
        g_ram[a] = val; return;
    }
    if (addr >= 0x2000 && addr <= 0x3FFF) { ppu_write_reg(0x2000 + (addr & 7), val); return; }
    if (addr == 0x4014) {
        uint16_t src = (uint16_t)val << 8;
        for (int i = 0; i < 256; i++) g_ppu_oam[i] = nes_read(src + i);
        return;
    }
    if (addr == 0x4016) {
        if (val & 1) {
            s_ctrl1_strobe = true;
        } else if (s_ctrl1_strobe) {
            s_ctrl1_strobe = false;
            s_ctrl1_shift  = g_controller1_buttons; /* latch on falling edge */
            s_ctrl2_shift  = g_controller2_buttons;
        }
        return;
    }
    if (addr >= 0x4000 && addr <= 0x401F) { apu_write(addr, val); return; }
    if (addr >= 0x6000 && addr <= 0x7FFF) { g_sram[addr - 0x6000] = val; return; }
    if (addr >= 0x8000) { mapper_write(addr, val); return; }
}

uint16_t nes_read16(uint16_t addr) {
    return (uint16_t)nes_read(addr) | ((uint16_t)nes_read(addr + 1) << 8);
}

uint16_t nes_read16zp(uint8_t zp) {
    return (uint16_t)g_ram[zp] | ((uint16_t)g_ram[(uint8_t)(zp + 1)] << 8);
}

void ppu_write_reg(uint16_t reg, uint8_t val) {
    ppu_trace_write(reg, val);
    /* Any PPU write between $2002 reads means the read was a latch reset
     * (e.g., LDA $2002; STA $2006), not a sprite-0 spin-wait poll.
     * Reset the counter so only consecutive $2002 reads trigger the hit. */
    g_spr0_reads_ctr = 0;
    switch (reg) {
        case 0x2000: g_ppuctrl = val; break;
        case 0x2001: g_ppumask = val; break;
        case 0x2003: g_oamaddr = val; break;
        case 0x2004: g_ppu_oam[g_oamaddr++] = val; break;
        case 0x2005:
            if (!g_scroll_latch) g_ppuscroll_x = val;
            else                 g_ppuscroll_y = val;
            g_scroll_latch ^= 1;
            break;
        case 0x2006:
            if (!g_ppuaddr_latch) g_ppuaddr = (uint16_t)val << 8;
            else                   g_ppuaddr |= val;
            g_ppuaddr_latch ^= 1;
            g_ppudata_buf = 0; /* writing $2006 resets the read buffer */
            break;
        case 0x2007: {
            uint16_t a = g_ppuaddr & 0x3FFF;
            if (a >= 0x3F00) {
                /* NES palette mirror: $3F10/$3F14/$3F18/$3F1C share storage
                 * with $3F00/$3F04/$3F08/$3F0C (transparent color slots). */
                uint8_t idx = a & 0x1F;
                if (idx == 0x10 || idx == 0x14 || idx == 0x18 || idx == 0x1C)
                    idx &= 0x0F;
                g_ppu_pal[idx] = val;
            } else if (a >= 0x2000) {
                /* Apply mirroring to nametable writes so they land in the
                 * same physical NT the renderer reads from. */
                int vnt = ((a - 0x2000) / 0x400) & 3;
                int pnt;
                switch (mapper_get_mirroring()) {
                    case 0:  pnt = 0;          break; /* one-screen lower */
                    case 1:  pnt = 1;          break; /* one-screen upper */
                    case 2:  pnt = vnt & 1;    break; /* vertical */
                    case 3:  pnt = vnt >> 1;   break; /* horizontal */
                    default: pnt = vnt & 1;    break;
                }
                g_ppu_nt[pnt * 0x400 + (a & 0x3FF)] = val;
            }
            else if (!g_chr_is_rom) {
                g_chr_ram[a] = val; /* CHR RAM only — CHR ROM is read-only */
            }
            g_ppuaddr += (g_ppuctrl & 0x04) ? 32 : 1;
            break;
        }
    }
}

uint8_t ppu_read_reg(uint16_t reg) {
    switch (reg) {
        case 0x2002: {
            uint8_t s = g_ppustatus;
            g_ppustatus &= ~0x80;  /* clear VBlank flag (standard NES) */
            g_scroll_latch  = 0;
            g_ppuaddr_latch = 0;
            /* Simulate sprite-0 hit (bit6) for split-screen spin-waits.
             * On real NES, bit6 is set by hardware during active rendering and
             * cleared only at the pre-render scanline — never by reading $2002.
             * Our simulation has no scanline timing, so we model it as "pulse on
             * read": bit6 is set after 3 consecutive reads that see bit6=0, and
             * cleared (consumed) when bit6=1 is read back.  This ensures:
             *   - "wait for bit6=0" spin-wait exits in at most 2 reads
             *   - "wait for bit6=1" spin-wait exits after the 3-read trigger
             *   - Random $2002 reads during PPU uploads don't cause a spurious
             *     permanent bit6=1 that would lock the first spin-wait forever
             * Counter is reset at VBlank start via g_spr0_split_active reset
             * path in maybe_trigger_vblank. */
            {
                if (s & 0x40) {
                    /* Bit6 was set; consume it (clear) and reset counter */
                    g_ppustatus &= ~0x40;
                    g_spr0_reads_ctr = 0;
                } else {
                    if (++g_spr0_reads_ctr >= 3) {
                        /* Capture scroll/ppuctrl state as HUD (pre-split) values.
                         * Mask ppuctrl to only rendering-relevant bits: 0-1 (NT),
                         * 3 (spr pattern), 4 (BG pattern), 5 (spr size).
                         * Bit 2 (VRAM inc) and bit 7 (NMI enable) don't affect
                         * rendering and may have transient values from PPU uploads. */
                        g_ppuscroll_x_hud   = g_ppuscroll_x;
                        g_ppuscroll_y_hud   = g_ppuscroll_y;
                        g_ppuctrl_hud       = g_ppuctrl & 0x38;
                        g_spr0_split_active = 1;
                        g_ppustatus |= 0x40;
                        g_spr0_reads_ctr = 0;
                    }
                }
            }
            return s;
        }
        case 0x2004: return g_ppu_oam[g_oamaddr];
        case 0x2007: {
            /* NES PPU $2007 read: buffered for CHR/NT, immediate for palette.
             * The real NES returns the OLD buffer contents for non-palette reads,
             * then updates the buffer with the byte at the current address.
             * Palette reads ($3F00+) are immediate (no buffer delay). */
            uint16_t a = g_ppuaddr & 0x3FFF;
            g_ppuaddr += (g_ppuctrl & 0x04) ? 32 : 1;
            if (a >= 0x3F00) {
                /* Palette: immediate read, but also update buffer with NT mirror */
                {
                    int vnt_p = ((a - 0x2000) / 0x400) & 3;
                    int pnt_p;
                    switch (mapper_get_mirroring()) {
                        case 0:  pnt_p = 0;            break;
                        case 1:  pnt_p = 1;            break;
                        case 2:  pnt_p = vnt_p & 1;    break;
                        case 3:  pnt_p = vnt_p >> 1;   break;
                        default: pnt_p = vnt_p & 1;    break;
                    }
                    g_ppudata_buf = g_ppu_nt[pnt_p * 0x400 + (a & 0x3FF)];
                }
                uint8_t idx = a & 0x1F;
                if (idx == 0x10 || idx == 0x14 || idx == 0x18 || idx == 0x1C)
                    idx &= 0x0F;
                return g_ppu_pal[idx];
            }
            uint8_t ret = g_ppudata_buf;
            if (a >= 0x2000) {
                /* Apply mirroring to NT reads to match write path */
                int vnt = ((a - 0x2000) / 0x400) & 3;
                int pnt;
                switch (mapper_get_mirroring()) {
                    case 0:  pnt = 0;          break;
                    case 1:  pnt = 1;          break;
                    case 2:  pnt = vnt & 1;    break;
                    case 3:  pnt = vnt >> 1;   break;
                    default: pnt = vnt & 1;    break;
                }
                g_ppudata_buf = g_ppu_nt[pnt * 0x400 + (a & 0x3FF)];
            } else {
                g_ppudata_buf = g_chr_ram[a];
            }
            return ret;
        }
    }
    return 0;
}

void runtime_get_latch_state(uint8_t *ppuaddr_latch, uint8_t *scroll_latch) {
    *ppuaddr_latch = (uint8_t)g_ppuaddr_latch;
    *scroll_latch  = (uint8_t)g_scroll_latch;
}

void runtime_set_latch_state(uint8_t ppuaddr_latch, uint8_t scroll_latch) {
    g_ppuaddr_latch = (int)ppuaddr_latch;
    g_scroll_latch  = (int)scroll_latch;
}

uint32_t g_miss_count_any   = 0;
uint16_t g_miss_last_addr   = 0;
uint64_t g_miss_last_frame  = 0;

#define MAX_MISS_UNIQUE 12
uint16_t g_miss_unique_addrs[MAX_MISS_UNIQUE];
int      g_miss_unique_count = 0;

void nes_log_dispatch_miss(uint16_t addr) {
    /* Let the game handle unmapped addresses (e.g. SRAM code remapping) */
    if (game_dispatch_override(addr)) return;
    static uint32_t last = 0xFFFFFFFF;
    uint32_t key = ((uint32_t)g_current_bank << 16) | addr;
    if (key != last) {
        printf("[Dispatch] MISS: no func for $%04X bank=%d\n", addr, g_current_bank);
        last = key;
    }
    g_miss_count_any++;
    g_miss_last_addr  = addr;
    g_miss_last_frame = g_frame_count;
    /* Add to unique list if not already present; log new misses to file */
    int found = 0;
    for (int i = 0; i < g_miss_unique_count; i++)
        if (g_miss_unique_addrs[i] == addr) { found = 1; break; }
    if (!found && g_miss_unique_count < MAX_MISS_UNIQUE) {
        g_miss_unique_addrs[g_miss_unique_count++] = addr;
        /* Append to dispatch_misses.log in game.cfg-compatible format */
        FILE *mf = fopen("dispatch_misses.log", "a");
        if (mf) {
            fprintf(mf, "extra_func %d 0x%04X\n", g_current_bank, addr);
            fclose(mf);
        }
        fprintf(stderr, "[Dispatch] NEW miss logged: extra_func %d 0x%04X (frame %llu)\n",
                g_current_bank, addr, (unsigned long long)g_frame_count);
    }
}

void nes_log_inline_miss(uint16_t dispatch_pc, uint8_t a_val) {
    static uint32_t last = 0xFFFFFFFF;
    uint32_t key = ((uint32_t)dispatch_pc << 8) | a_val;
    if (key != last) {
        printf("[Dispatch] INLINE MISS @$%04X A=%d (0x%02X)\n", dispatch_pc, (int)a_val, (unsigned)a_val);
        last = key;
    }
    g_miss_count_any++;
}
