/*
 * runtime.c — NES memory map, PPU register stubs, hardware I/O
 */
#include "nes_runtime.h"
#include "debug_server.h"
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
int          g_bail_active;  /* set by stack_bail_func, checked at JSR call sites */
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
/* NOTE: g_scroll_latch removed — real NES shares a single write toggle
 * ("w" register) between $2005 and $2006.  g_ppuaddr_latch is that toggle. */
static uint8_t g_ppudata_buf   = 0; /* PPUDATA read buffer (NES read-delay) */

/* ---- PPU internal t/v registers (Loopy's scrolling model) ----
 * The NES PPU has two 15-bit address registers:
 *   t (temporary) — written by $2000/$2005/$2006, holds pending scroll/address
 *   v (current)   — used for rendering; copied from t at frame start
 * Both $2005 and $2006 write to the SAME t register.  This means $2006 writes
 * for VRAM access also affect scroll, and vice versa.
 *
 * Bit layout of t/v:  yyy NN YYYYY XXXXX
 *   bits  0-4:  coarse X scroll (tile column, 0-31)
 *   bits  5-9:  coarse Y scroll (tile row, 0-29)
 *   bits 10-11: nametable select (from PPUCTRL bits 0-1)
 *   bits 12-14: fine Y scroll (pixel row within tile, 0-7)
 *
 * g_ppuaddr acts as v (used by $2007 reads/writes). */
static uint16_t s_ppu_t      = 0;
static uint8_t  s_ppu_fine_x = 0;
static uint16_t s_ppu_v_at_2006 = 0;  /* v captured at $2006 second write, before $2007 increments */
static int      s_scroll_2005_complete = 0; /* 1 if $2005 pair completed after last $2006 pair */

uint64_t g_frame_count = 0;

/* ---- Controller state ---- */
uint8_t g_controller1_buttons = 0;
uint8_t g_controller2_buttons = 0;
static uint8_t s_ctrl1_shift   = 0;
static uint8_t s_ctrl2_shift   = 0;
static bool    s_ctrl1_strobe  = false;

static FILE *s_ppu_trace = NULL;

/* Scroll write trace — last 64 writes to $2005 */
#define SCROLL_TRACE_SIZE 64
typedef struct { uint64_t frame; uint8_t val; uint8_t which; /* 0=X, 1=Y */ } ScrollTraceEntry;
static ScrollTraceEntry s_scroll_trace[SCROLL_TRACE_SIZE];
static int s_scroll_trace_idx = 0;
static int s_scroll_trace_count = 0;

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
#ifdef RECOMP_STACK_TRACKING
        extern const char *g_recomp_stack[];
        extern int g_recomp_stack_top;
        /* For $2000 writes, include the top of the recomp call stack */
        if (reg == 0x2000 && g_recomp_stack_top > 0) {
            const char *caller = g_recomp_stack[g_recomp_stack_top - 1];
            fprintf(s_ppu_trace, "W,$%04X,$%02X,%s,F=%llu\n",
                    reg, val, caller ? caller : "?",
                    (unsigned long long)g_frame_count);
        } else
#endif
        {
            fprintf(s_ppu_trace, "W,$%04X,$%02X,PC=?,F=%llu\n",
                    reg, val, (unsigned long long)g_frame_count);
        }
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
#define MAX_VBLANK_DEPTH 3        /* Some games (Metroid) spin-wait for NMI inside
                                   * the NMI handler (column loader).  Allow
                                   * re-entrancy so the wait loop can exit. */
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

static int s_vblank_pending = 0;   /* VBlank waiting to fire at next safe point */

void maybe_trigger_vblank(int cycles) {
    /* S-register change tracking (per-instruction) */
    debug_server_check_s();

    /* Count cycles — always, even during NMI handler execution. */
    s_ops_count += (cycles > 0) ? (uint32_t)cycles : 1;
    if (s_ops_count < OPS_PER_FRAME) return;
    if (s_vblank_depth >= MAX_VBLANK_DEPTH) return;

    /* Frame budget exhausted.
     *
     * At depth 0 (main loop, not inside NMI): fire immediately.  The main
     * loop's linear code (RESET init, PPU warmup) needs VBlank to fire even
     * without backward branches.  This is safe because the main loop code
     * that calls wait-for-NMI has already set up the correct bank/state.
     *
     * At depth > 0 (inside NMI handler): DEFER to next backward branch.
     * The NMI handler calls subroutines that switch banks, modify ZP, and
     * do multi-step PPU operations.  Firing a nested NMI mid-operation
     * corrupts bank-dependent data reads ($9560 table) and ZP pointers.
     * Deferring to backward branches ensures the code has reached a loop
     * boundary (like the $1A spin-wait) where state is consistent. */
    if (s_vblank_depth == 0) {
        /* Immediate fire — safe at top level */
        s_ops_count = 0;
        s_vblank_depth++;
        g_ppustatus = (g_ppustatus & ~0x40) | 0x80;
        g_spr0_split_active = 0;
        g_spr0_reads_ctr    = 0;
        g_ppuscroll_x = 0;
        g_ppuscroll_y = 0;
        g_ppuscroll_x_hud = 0;
        g_ppuscroll_y_hud = 0;
        g_ppuctrl_hud     = g_ppuctrl & 0x38;
        if (g_ppuctrl & 0x80) {
            nes_vblank_callback();
        }
        s_vblank_depth--;
    } else {
        /* Deferred fire — wait for backward branch (loop boundary) */
        s_vblank_pending = 1;
    }
}

/* Called from watchdog_check() at backward branch (loop back-edge) points.
 * This is the ONLY place NMI actually fires — at loop boundaries where
 * the game's state is consistent (correct bank mapped, ZP set up, etc.). */
void maybe_fire_pending_vblank(void) {
    if (!s_vblank_pending) return;
    s_vblank_pending = 0;
    s_ops_count = 0;
    s_vblank_depth++;

    /* Standard VBlank start: set flags, reset sprite-0 state */
    g_ppustatus = (g_ppustatus & ~0x40) | 0x80;
    g_spr0_split_active = 0;
    g_spr0_reads_ctr    = 0;
    g_ppuscroll_x = 0;
    g_ppuscroll_y = 0;
    g_ppuscroll_x_hud = 0;
    g_ppuscroll_y_hud = 0;
    g_ppuctrl_hud     = g_ppuctrl & 0x38;

    if (g_ppuctrl & 0x80) {
        nes_vblank_callback();
    } else if (s_vblank_depth > 1) {
        g_ram[0x1A] = 1;
    }
    s_vblank_depth--;
}

void runtime_set_vblank_firing(int active) {
    if (active)
        s_vblank_depth++;
    else if (s_vblank_depth > 0)
        s_vblank_depth--;
}

int runtime_get_vblank_depth(void) {
    return s_vblank_depth;
}

void runtime_reset_vblank_depth(void) {
    s_vblank_depth = 0;
    s_vblank_pending = 0;
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
int      g_write_bp_block = 0;
write_bp_callback_t g_write_bp_callback = NULL;

void nes_write(uint16_t addr, uint8_t val) {
    bus_tick();

    if (addr <= 0x1FFF) {
        uint16_t a = addr & 0x07FF;
        if (a == g_write_bp_addr && g_write_bp_callback &&
            (g_write_bp_match_val == 0xFF || val == g_write_bp_match_val)) {
            g_write_bp_callback(a, g_ram[a], val);
            if (g_write_bp_block) return;  /* callback set block flag → skip write */
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
        /* Follower notification (write-level tracing via TCP) */
        if (val != g_ram[a]) {
            if (debug_server_has_follower(a)) {
                debug_server_notify_write(a, g_ram[a], val);
            }
            /* Hard trace: print $FF changes to stderr for debugging follower issues */
            if (a == 0xFF && g_frame_count < 300) {
                fprintf(stderr, "[FF] %02X->%02X f=%llu has_follow=%d\n",
                        g_ram[a], val, (unsigned long long)g_frame_count,
                        debug_server_has_follower(a));
            }
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
        case 0x2000:
            g_ppuctrl = val;
            /* $2000 bits 0-1 → t bits 10-11 (nametable select) */
            s_ppu_t = (s_ppu_t & 0xF3FF) | ((uint16_t)(val & 3) << 10);
            break;
        case 0x2001: g_ppumask = val; break;
        case 0x2003: g_oamaddr = val; break;
        case 0x2004: g_ppu_oam[g_oamaddr++] = val; break;
        case 0x2005: {
            ScrollTraceEntry *se = &s_scroll_trace[s_scroll_trace_idx];
            se->frame = g_frame_count;
            se->val = val;
            se->which = g_ppuaddr_latch; /* 0=X, 1=Y */
            s_scroll_trace_idx = (s_scroll_trace_idx + 1) % SCROLL_TRACE_SIZE;
            if (s_scroll_trace_count < SCROLL_TRACE_SIZE) s_scroll_trace_count++;

            if (!g_ppuaddr_latch) {
                /* First write (w=0): coarse X → t[0:4], fine X → separate reg */
                s_ppu_t = (s_ppu_t & 0xFFE0) | ((uint16_t)val >> 3);
                s_ppu_fine_x = val & 7;
                g_ppuscroll_x = val;
            } else {
                /* Second write (w=1): fine Y → t[12:14], coarse Y → t[5:9] */
                s_ppu_t = (s_ppu_t & 0x0C1F) |
                          ((uint16_t)(val & 7) << 12) |
                          ((uint16_t)(val >> 3) << 5);
                g_ppuscroll_y = val;
                s_scroll_2005_complete = 1;
            }
            g_ppuaddr_latch ^= 1;
            break;
        }
        case 0x2006:
            /* === $2006 WRITE TRAP: frames 192-196 === */
            {
                static int ppu2006_trap_count = 0;
                if (ppu2006_trap_count < 30 && g_frame_count >= 192 && g_frame_count <= 196) {
                    extern int g_current_bank;
                    fprintf(stderr, "[PPU2006] f=%llu val=$%02X ppuaddr=$%04X latch=%d vblank=%d bank=%d ram1D=$%02X ram1C=$%02X ram1B=$%02X\n",
                            (unsigned long long)g_frame_count, val, g_ppuaddr, g_ppuaddr_latch,
                            s_vblank_depth, g_current_bank,
                            g_ram[0x1D], g_ram[0x1C], g_ram[0x1B]);
                    fflush(stderr);
                    ppu2006_trap_count++;
                }
            }
            /* === END $2006 WRITE TRAP === */
            if (!g_ppuaddr_latch) {
                /* First write (w=0): val[5:0] → t[13:8], clear t bit 14 */
                s_ppu_t = (s_ppu_t & 0x00FF) | ((uint16_t)(val & 0x3F) << 8);
                g_ppuaddr = (uint16_t)val << 8; /* legacy: keep ppuaddr in sync */
            } else {
                /* Second write (w=1): val → t[7:0], then v = t */
                s_ppu_t = (s_ppu_t & 0xFF00) | val;
                g_ppuaddr = s_ppu_t & 0x3FFF; /* v = t (14-bit VRAM address) */
                s_ppu_v_at_2006 = g_ppuaddr;  /* capture v before $2007 increments */
                s_scroll_2005_complete = 0;    /* $2006 pair completed — t now holds VRAM addr */
            }
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
                if (idx == 0 && val == 0xBA && g_ppu_pal[0] != 0xBA) {
                    fprintf(stderr, "\n!!! PAL[0] = $BA at frame %llu, ppuaddr=$%04X, val=$%02X, S=$%02X, ctrl=$%02X !!!\n",
                            (unsigned long long)g_frame_count, g_ppuaddr, val, g_cpu.S, g_ppuctrl);
#ifdef RECOMP_STACK_TRACKING
                    extern const char *g_recomp_stack[];
                    extern int g_recomp_stack_top;
                    for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 12; i--)
                        fprintf(stderr, "  [%d] %s\n", i, g_recomp_stack[i] ? g_recomp_stack[i] : "?");
#endif
                    fflush(stderr);
                }
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
                /* === CHR WRITE TRAP: frames 190-210 === */
                {
                    static int chr_trap_fired = 0;
                    if (!chr_trap_fired && g_frame_count >= 190 && g_frame_count <= 210) {
                        chr_trap_fired = 1;
                        extern int g_current_bank;
                        fprintf(stderr, "\n=== CHR WRITE TRAP ===\n");
                        fprintf(stderr, "frame=%llu ppuaddr=$%04X mapped_a=$%04X val=$%02X\n",
                                (unsigned long long)g_frame_count, g_ppuaddr, a, val);
                        fprintf(stderr, "ctrl=$%02X latch=%d vblank_depth=%d\n",
                                g_ppuctrl, g_ppuaddr_latch, s_vblank_depth);
                        fprintf(stderr, "bank=%d\n", g_current_bank);
                        /* ZP $00-$0F */
                        fprintf(stderr, "ZP $00-$0F:");
                        for (int zi = 0; zi < 16; zi++)
                            fprintf(stderr, " %02X", g_ram[zi]);
                        fprintf(stderr, "\n");
                        /* Key ZP locations */
                        fprintf(stderr, "$1A=%02X $1B=%02X $1C=%02X $1D=%02X $1E=%02X $5A=%02X\n",
                                g_ram[0x1A], g_ram[0x1B], g_ram[0x1C],
                                g_ram[0x1D], g_ram[0x1E], g_ram[0x5A]);
                        /* Pointer at ($00/$01) and first 16 bytes it points to */
                        {
                            uint16_t ptr = g_ram[0x00] | ((uint16_t)g_ram[0x01] << 8);
                            fprintf(stderr, "($00/$01) ptr=$%04X, data:", ptr);
                            for (int di = 0; di < 16; di++) {
                                uint16_t daddr = ptr + di;
                                uint8_t dval = 0;
                                if (daddr < 0x0800)
                                    dval = g_ram[daddr];
                                else if (daddr >= 0x6000 && daddr < 0x8000)
                                    dval = g_sram[daddr - 0x6000];
                                else
                                    dval = 0xFF; /* unmapped for this dump */
                                fprintf(stderr, " %02X", dval);
                            }
                            fprintf(stderr, "\n");
                        }
#ifdef RECOMP_STACK_TRACKING
                        {
                            extern const char *g_recomp_stack[];
                            extern int g_recomp_stack_top;
                            fprintf(stderr, "Recomp stack (top=%d):\n", g_recomp_stack_top);
                            for (int si = g_recomp_stack_top - 1; si >= 0 && si >= g_recomp_stack_top - 20; si--)
                                fprintf(stderr, "  [%d] %s\n", si, g_recomp_stack[si] ? g_recomp_stack[si] : "?");
                        }
#endif
                        fprintf(stderr, "=== END CHR WRITE TRAP ===\n\n");
                        fflush(stderr);
                    }
                }
                /* === END CHR WRITE TRAP === */
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
            g_ppuaddr_latch = 0;  /* shared w toggle — clears for both $2005 and $2006 */
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
    *scroll_latch  = (uint8_t)g_ppuaddr_latch;  /* same toggle on real NES */
}

void runtime_set_latch_state(uint8_t ppuaddr_latch, uint8_t scroll_latch) {
    (void)scroll_latch;  /* same toggle — only ppuaddr_latch matters */
    g_ppuaddr_latch = (int)ppuaddr_latch;
}

void runtime_get_vblank_state(uint32_t *ops_count, int *vblank_depth) {
    *ops_count    = s_ops_count;
    *vblank_depth = s_vblank_depth;
}

void runtime_set_vblank_state(uint32_t ops_count, int vblank_depth) {
    s_ops_count    = ops_count;
    s_vblank_depth = vblank_depth;
}

void runtime_get_controller_shift(uint8_t *shift1, uint8_t *shift2, uint8_t *strobe) {
    *shift1  = s_ctrl1_shift;
    *shift2  = s_ctrl2_shift;
    *strobe  = (uint8_t)s_ctrl1_strobe;
}

void runtime_set_controller_shift(uint8_t shift1, uint8_t shift2, uint8_t strobe) {
    s_ctrl1_shift  = shift1;
    s_ctrl2_shift  = shift2;
    s_ctrl1_strobe = (bool)strobe;
}

uint8_t runtime_get_ppudata_buf(void) { return g_ppudata_buf; }
void    runtime_set_ppudata_buf(uint8_t val) { g_ppudata_buf = val; }
uint16_t runtime_get_ppuaddr(void) { return g_ppuaddr; }
void     runtime_set_ppuaddr(uint16_t addr) { g_ppuaddr = addr; }

uint32_t g_miss_count_any   = 0;
uint16_t g_miss_last_addr   = 0;
uint64_t g_miss_last_frame  = 0;
int      g_miss_last_bank   = 0;
char     g_miss_last_caller[64]  = "(none)";
char     g_miss_last_stack2[64]  = "(none)";
uint8_t  g_miss_last_sp           = 0;
uint8_t  g_miss_last_stack_bytes[16];

#define MAX_MISS_UNIQUE 12
uint16_t g_miss_unique_addrs[MAX_MISS_UNIQUE];
int      g_miss_unique_count = 0;

#ifdef RECOMP_STACK_TRACKING
extern const char *g_recomp_stack[];
extern int         g_recomp_stack_top;
extern const char *g_last_recomp_func;
#endif

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
    g_miss_last_bank  = g_current_bank;
    /* Capture caller context: top of recomp call stack + 6502 stack snapshot */
#ifdef RECOMP_STACK_TRACKING
    const char *c0 = (g_recomp_stack_top > 0) ? g_recomp_stack[g_recomp_stack_top - 1] : g_last_recomp_func;
    const char *c1 = (g_recomp_stack_top > 1) ? g_recomp_stack[g_recomp_stack_top - 2] : "(none)";
    strncpy(g_miss_last_caller, c0 ? c0 : "(none)", sizeof(g_miss_last_caller)-1);
    g_miss_last_caller[sizeof(g_miss_last_caller)-1] = '\0';
    strncpy(g_miss_last_stack2, c1 ? c1 : "(none)", sizeof(g_miss_last_stack2)-1);
    g_miss_last_stack2[sizeof(g_miss_last_stack2)-1] = '\0';
#endif
    g_miss_last_sp = g_cpu.S;
    /* Snapshot 16 bytes above SP (the most recent pushes) */
    for (int i = 0; i < 16; i++) {
        uint8_t s = (uint8_t)(g_cpu.S + 1 + i);
        g_miss_last_stack_bytes[i] = g_ram[0x100 + s];
    }
    /* Add to unique list if not already present; log new misses to file */
    int found = 0;
    for (int i = 0; i < g_miss_unique_count; i++)
        if (g_miss_unique_addrs[i] == addr) { found = 1; break; }
    if (!found && g_miss_unique_count < MAX_MISS_UNIQUE) {
        g_miss_unique_addrs[g_miss_unique_count++] = addr;
        /* Append to dispatch_misses.log next to the executable */
        char miss_path[300];
        snprintf(miss_path, sizeof(miss_path), "%sdispatch_misses.log", g_exe_dir);
        FILE *mf = fopen(miss_path, "a");
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

/* ---- PPU t register: derive scroll from t for rendering ---- */
static uint8_t s_last_sync_sx = 0, s_last_sync_sy = 0;
static uint16_t s_last_sync_t = 0;
static uint64_t s_last_sync_frame = 0;

void runtime_sync_scroll_from_t(void) {
    /* At frame start, derive scroll from t (PPU copies t→v at pre-render).
     * This captures scroll set by $2005/$2006 during the NMI handler.
     * NOTE: Do NOT sync g_ppuctrl bits 0-1 from t here.  Games that write
     * $2000 (PPUCTRL) after $2006 expect bits 0-1 to come from $2000, but
     * $2006 writes may have modified t bits 10-11.  The game's $2000 write
     * already sets both g_ppuctrl AND t bits 10-11, so they stay in sync. */
    g_ppuscroll_x = (uint8_t)(((s_ppu_t & 0x1F) << 3) | (s_ppu_fine_x & 7));
    g_ppuscroll_y = (uint8_t)((((s_ppu_t >> 5) & 0x1F) << 3) | ((s_ppu_t >> 12) & 7));

    s_last_sync_sx = g_ppuscroll_x;
    s_last_sync_sy = g_ppuscroll_y;
    s_last_sync_t = s_ppu_t;
    s_last_sync_frame = g_frame_count;
}

/* Mid-frame scroll sync: derive scroll from v (g_ppuaddr), not t.
 * On real NES, mid-frame $2006 writes immediately update v, and the PPU
 * uses v for rendering.  $2005 writes modify t but NOT v during rendering.
 * So after an IRQ handler sets scroll via $2006+$2005, the rendering scroll
 * comes from v ($2006), while t ($2005) is for the NEXT frame. */
void runtime_sync_scroll_from_v(void) {
    uint16_t v = s_ppu_v_at_2006; /* use v as set by last $2006, not incremented by $2007 */
    g_ppuscroll_x = (uint8_t)(((v & 0x1F) << 3) | (s_ppu_fine_x & 7));
    g_ppuscroll_y = (uint8_t)((((v >> 5) & 0x1F) << 3) | ((v >> 12) & 7));
    g_ppuctrl = (g_ppuctrl & 0xFC) | ((v >> 10) & 3);
}

static uint8_t s_frame_start_sx = 0, s_frame_start_sy = 0;
static uint16_t s_frame_start_t = 0;
static uint64_t s_frame_start_frame = 0;

void runtime_record_frame_start_scroll(void) {
    s_frame_start_sx = g_ppuscroll_x;
    s_frame_start_sy = g_ppuscroll_y;
    s_frame_start_t = s_ppu_t;
    s_frame_start_frame = g_frame_count;
}

void runtime_get_frame_start_scroll(uint8_t *sx, uint8_t *sy, uint16_t *t, uint64_t *frame) {
    if (sx) *sx = s_frame_start_sx;
    if (sy) *sy = s_frame_start_sy;
    if (t) *t = s_frame_start_t;
    if (frame) *frame = s_frame_start_frame;
}

void runtime_get_last_sync(uint8_t *sx, uint8_t *sy, uint16_t *t, uint64_t *frame) {
    if (sx) *sx = s_last_sync_sx;
    if (sy) *sy = s_last_sync_sy;
    if (t) *t = s_last_sync_t;
    if (frame) *frame = s_last_sync_frame;
}

uint16_t runtime_get_ppu_t(void) { return s_ppu_t; }
uint8_t  runtime_get_ppu_fine_x(void) { return s_ppu_fine_x; }
int      runtime_scroll_from_t_valid(void) { return s_scroll_2005_complete; }

/* Save/restore PPU internal state for runahead (game_post_render). */
void runtime_get_ppu_internals(uint16_t *t, uint8_t *fine_x, int *scroll_complete,
                               uint16_t *last_sync_t, uint8_t *last_sync_sx, uint8_t *last_sync_sy,
                               uint16_t *frame_start_t, uint8_t *frame_start_sx, uint8_t *frame_start_sy) {
    *t = s_ppu_t;
    *fine_x = s_ppu_fine_x;
    *scroll_complete = s_scroll_2005_complete;
    *last_sync_t = s_last_sync_t;
    *last_sync_sx = s_last_sync_sx;
    *last_sync_sy = s_last_sync_sy;
    *frame_start_t = s_frame_start_t;
    *frame_start_sx = s_frame_start_sx;
    *frame_start_sy = s_frame_start_sy;
}

void runtime_set_ppu_internals(uint16_t t, uint8_t fine_x, int scroll_complete,
                               uint16_t last_sync_t, uint8_t last_sync_sx, uint8_t last_sync_sy,
                               uint16_t frame_start_t, uint8_t frame_start_sx, uint8_t frame_start_sy) {
    s_ppu_t = t;
    s_ppu_fine_x = fine_x;
    s_scroll_2005_complete = scroll_complete;
    s_last_sync_t = last_sync_t;
    s_last_sync_sx = last_sync_sx;
    s_last_sync_sy = last_sync_sy;
    s_frame_start_t = frame_start_t;
    s_frame_start_sx = frame_start_sx;
    s_frame_start_sy = frame_start_sy;
}

/* IRQ scanline recording for debug */
#define IRQ_SCANLINE_LOG_SIZE 8
static int s_irq_scanlines[IRQ_SCANLINE_LOG_SIZE];
static int s_irq_scanline_count = 0;
static uint64_t s_irq_scanline_frame = 0;

void runtime_record_irq_scanline(int scanline) {
    if (g_frame_count != s_irq_scanline_frame) {
        s_irq_scanline_count = 0;
        s_irq_scanline_frame = g_frame_count;
    }
    if (s_irq_scanline_count < IRQ_SCANLINE_LOG_SIZE)
        s_irq_scanlines[s_irq_scanline_count++] = scanline;
}

void runtime_get_irq_scanlines(int *out, int *count, uint64_t *frame) {
    for (int i = 0; i < s_irq_scanline_count && i < IRQ_SCANLINE_LOG_SIZE; i++)
        out[i] = s_irq_scanlines[i];
    *count = s_irq_scanline_count;
    *frame = s_irq_scanline_frame;
}

/* ---- Scroll write trace accessors ---- */
void runtime_get_scroll_trace(int *out_count, int *out_idx) {
    if (out_count) *out_count = s_scroll_trace_count;
    if (out_idx) *out_idx = s_scroll_trace_idx;
}

typedef struct { uint64_t frame; uint8_t val; uint8_t which; } ScrollTraceEntryExport;

const void *runtime_get_scroll_trace_buf(void) {
    return s_scroll_trace;
}
