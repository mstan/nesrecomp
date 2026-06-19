/*
 * runtime.c — NES memory map, PPU register stubs, hardware I/O
 */
#include "nes_runtime.h"
#include "debug_server.h"
#include "mapper.h"
#include "logger.h"
#include "apu.h"
#include "game_extras.h"
#include "override_chr.h"
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
int     g_spr0_predict_disable = 1; /* default 1 (predictor OFF). The hardware-correct
                                     * cycle predictor introduced regressions in normal
                                     * gameplay polls of $2002 — bit 6 staying sticky
                                     * until VBlank breaks games that expect it to
                                     * oscillate via consume-on-read. Predictor code is
                                     * kept; set this to 0 in extras.c to enable per-game
                                     * once the regression class is solved (likely via
                                     * gating prediction on shot-active state). */
int     g_predicted_spr0_scanline = 240;  /* set per-frame by main_runner.c via predictor */
int     g_spr0_reads_ctr_legacy = 0;  /* used only when predict_disable=1; non-static so savestate can persist */
/* Scanline at which the game wrote the FIRST playfield scroll ($2005) AFTER
 * the sprite-0 hit captured the HUD scroll. This is the true HUD/playfield
 * boundary (the hit only synchronizes; the game writes the new scroll on a
 * timed delay so it lands at the HUD bottom). -1 = no post-hit scroll write
 * this frame. Diagnostics for the HUD-tear-during-transition bug. */
int     g_spr0_split_write_scanline = -1;

/* CPU-cycle-to-scanline conversion. PPU runs at 3x CPU; 341 dots per scanline.
 * s_ops_count is the per-frame CPU cycle accumulator from maybe_trigger_vblank. */
static inline int scanline_from_cycles(uint32_t cpu_cycles) {
    return (int)((cpu_cycles * 3u) / 341u);
}
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

/* ---- Zapper (light gun) state ---- */
int     g_zapper_enabled = 0;       /* 1 if Zapper connected on port 2 */
int     g_zapper_x = 0;             /* aim X (0-255) */
int     g_zapper_y = 0;             /* aim Y (0-239) */
int     g_zapper_trigger = 0;       /* 1 if trigger pulled */
static const uint32_t *s_zapper_framebuf = NULL; /* last rendered frame for light detection */
static zapper_render_fn s_zapper_render = NULL;  /* on-demand render callback */
static uint8_t s_zapper_last_ppumask = 0;        /* PPUMASK at last Zapper render */

void runtime_set_zapper_framebuf(const uint32_t *fb) {
    s_zapper_framebuf = fb;
    s_zapper_last_ppumask = g_ppumask;  /* framebuffer now matches this PPUMASK */
}
void runtime_set_zapper_render_callback(zapper_render_fn fn) { s_zapper_render = fn; }

/* Check PPU internal OAM for any visible sprite overlapping the Zapper aim
 * point.  Uses g_ppu_oam (set by OAM DMA / $4014) rather than shadow RAM
 * ($0200), because the PPU renders from internal OAM — shadow may have been
 * modified since the last DMA.  On real NES the PPU draws sprites scanline-
 * by-scanline while the game polls $4017 mid-frame. */
static int zapper_oam_hit(void) {
    int x = g_zapper_x, y = g_zapper_y;
    int sprite_h = (g_ppuctrl & 0x20) ? 16 : 8;
    for (int i = 0; i < 64; i++) {
        uint8_t sy   = g_ppu_oam[i * 4 + 0]; /* sprite Y (top - 1) */
        uint8_t sx   = g_ppu_oam[i * 4 + 3]; /* sprite X */
        if (sy >= 0xEF) continue; /* offscreen / hidden */
        int top = sy + 1; /* OAM Y is one less than display Y */
        if (x >= sx && x < sx + 8 && y >= top && y < top + sprite_h) {
            return 1;
        }
    }
    return 0;
}

/* Check if the pixel at Zapper aim (x,y) is "bright" — simulates the
 * photodiode in the NES Zapper light gun.
 *
 * Key: detection sequences change PPUMASK mid-frame.  Duck Hunt's flow:
 *   Phase 1: PPUMASK rendering OFF (screen blank) → anti-cheat check.
 *            If Zapper sees light here → shot rejected (aimed at lamp).
 *   Phase 2: PPUMASK sprites ON, detection sprites in PPU OAM →
 *            If Zapper sees light → aimed at duck → HIT.
 *
 * The framebuffer from the last ppu_render_frame may not match the current
 * PPUMASK.  When PPUMASK has changed, we do an on-demand render so the
 * framebuffer reflects what the CRT would actually display. */
static int zapper_light_detected(void) {
    /* During early init (before enough frames render), report light detected.
     * This simulates the Zapper pointing at a lit TV screen at power-on,
     * which is how real NES hardware detects the Zapper on combo carts. */
    if (!s_zapper_framebuf || g_frame_count < 2) return 1;

    /* If PPU rendering is completely disabled (both sprites and BG off),
     * the screen outputs the universal background color — typically dark.
     * The Zapper sees no light.  This is critical for the anti-cheat phase:
     * the game blanks the screen, and if the Zapper still sees light the
     * shot is rejected. */
    if (!(g_ppumask & 0x18)) return 0;

    /* If PPUMASK changed since the framebuffer was last rendered, do an
     * on-demand render so it reflects the current PPU state (e.g. the
     * detection flash sprites that are now visible). */
    if (s_zapper_render && (g_ppumask & 0x18) != (s_zapper_last_ppumask & 0x18)) {
        s_zapper_render();
        /* s_zapper_last_ppumask updated by runtime_set_zapper_framebuf */
    }

    extern int g_render_width;
    int x = g_zapper_x, y = g_zapper_y;
    if (x < 0 || x >= 256 || y < 0 || y >= 240) return 0;
    /* Sample a small area around the aim point for robustness */
    int total_lum = 0, count = 0;
    for (int dy = -4; dy <= 4; dy += 4) {
        for (int dx = -4; dx <= 4; dx += 4) {
            int px = x + dx, py = y + dy;
            if (px < 0 || px >= 256 || py < 0 || py >= 240) continue;
            uint32_t pixel = s_zapper_framebuf[py * g_render_width + px];
            int r = (pixel >> 16) & 0xFF;
            int g_val = (pixel >> 8) & 0xFF;
            int b = pixel & 0xFF;
            total_lum += (r * 3 + g_val * 6 + b) / 10;
            count++;
        }
    }
    int result = (count > 0 && total_lum / count > 160) ? 1 : 0;
    /* OAM bounding-box fallback removed: it caused false-positive hits
     * because it didn't check actual tile-pixel brightness.  The on-demand
     * s_zapper_render() above already keeps the framebuffer current. */
    return result;
}

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

/* Dispatch-miss policy + counters. Runtime knobs read once at init from
 * the environment, with programmatic override available via
 * nes_set_dispatch_miss_policy. */
DispatchMissPolicy g_dispatch_miss_policy = DISPATCH_MISS_LOG_RETURN;
uint64_t           g_dispatch_miss_count = 0;
uint64_t           g_inline_dispatch_miss_count = 0;

BrkPolicy g_brk_policy = BRK_DIAG;
uint64_t  g_brk_count  = 0;

void nes_set_dispatch_miss_policy(DispatchMissPolicy policy) {
    g_dispatch_miss_policy = policy;
}

void nes_set_brk_policy(BrkPolicy policy) {
    g_brk_policy = policy;
}

static void load_dispatch_miss_policy_from_env(void) {
    const char *v = getenv("NESRECOMP_DISPATCH_MISS");
    if (!v || !*v) return;
    if (strcmp(v, "fatal") == 0)        g_dispatch_miss_policy = DISPATCH_MISS_FATAL;
    else if (strcmp(v, "trap") == 0)    g_dispatch_miss_policy = DISPATCH_MISS_TRAP;
    else if (strcmp(v, "log-return") == 0) g_dispatch_miss_policy = DISPATCH_MISS_LOG_RETURN;
    else fprintf(stderr,
                 "[runtime] NESRECOMP_DISPATCH_MISS='%s' not recognized "
                 "(expected log-return|fatal|trap); using log-return\n", v);
}

static void load_brk_policy_from_env(void) {
    const char *v = getenv("NESRECOMP_BRK");
    if (!v || !*v) return;
    if (strcmp(v, "diag")  == 0)  g_brk_policy = BRK_DIAG;
    else if (strcmp(v, "fatal") == 0) g_brk_policy = BRK_FATAL;
    else if (strcmp(v, "trap")  == 0) g_brk_policy = BRK_TRAP;
    else fprintf(stderr,
                 "[runtime] NESRECOMP_BRK='%s' not recognized "
                 "(expected diag|fatal|trap); using diag\n", v);
}

/* Hook called by codegen at every reachable BRK ($00) site. Records the
 * occurrence and applies the configured policy. The generated code emits
 * `return;` immediately after the call so the enclosing function exits
 * at BRK rather than silently flowing past it.
 *
 * Real BRK semantics (push PC+2, push P|B, dispatch via IRQ vector) are
 * not implemented; instead BRK_FATAL or BRK_TRAP gives the developer a
 * loud failure they can debug. */
void nes_brk_executed(uint16_t pc) {
    static uint32_t last = 0xFFFFFFFF;
    uint32_t key = ((uint32_t)g_current_bank << 16) | pc;
    bool first_for_key = (key != last);

    g_brk_count++;
    if (first_for_key) {
        printf("[BRK] executed at $%04X bank=%d (frame %llu) — "
               "BRK is silently skipped under DIAG; consider real "
               "implementation if this site matters.\n",
               pc, g_current_bank, (unsigned long long)g_frame_count);
        fflush(stdout);
        last = key;
    }

    switch (g_brk_policy) {
        case BRK_DIAG:
            return;
        case BRK_FATAL:
            fprintf(stderr,
                    "[runtime] FATAL: BRK at $%04X (bank=%d, frame=%llu). "
                    "Policy=fatal.\n",
                    pc, g_current_bank, (unsigned long long)g_frame_count);
            fflush(stderr);
            fflush(stdout);
            exit(1);
        case BRK_TRAP: {
            char buf[64];
            snprintf(buf, sizeof(buf), "BRK at $%04X (bank=%d)",
                     pc, g_current_bank);
            debug_server_request_pause(buf);
            return;
        }
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
    load_dispatch_miss_policy_from_env();
    load_brk_policy_from_env();
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
/* Debug cadence counters. Accumulate across a wall-clock frame; the verify
 * layer pops them after native NMI completes. */
static uint32_t s_dbg_nmi_fires = 0;
static uint32_t s_dbg_cycles_ticked = 0;
static uint32_t s_dbg_instrs_ticked = 0;
static uint32_t s_dbg_forced_cap_hits = 0;
static uint32_t s_dbg_max_depth_skips = 0;
static uint32_t s_dbg_pending_no_ppu = 0;
uint32_t runtime_pop_nmi_fires(void) {
    uint32_t v = s_dbg_nmi_fires; s_dbg_nmi_fires = 0; return v;
}
uint32_t runtime_pop_cycle_budget_used(void) {
    uint32_t v = s_dbg_cycles_ticked; s_dbg_cycles_ticked = 0; return v;
}
uint32_t runtime_pop_instrs_ticked(void) {
    uint32_t v = s_dbg_instrs_ticked; s_dbg_instrs_ticked = 0; return v;
}
uint32_t runtime_pop_forced_caps(void) {
    uint32_t a = s_dbg_forced_cap_hits;
    uint32_t b = s_dbg_max_depth_skips;
    uint32_t c = s_dbg_pending_no_ppu;
    s_dbg_forced_cap_hits = 0;
    s_dbg_max_depth_skips = 0;
    s_dbg_pending_no_ppu  = 0;
    /* Encode: low16=forced_caps, mid8=max_depth_skips, high8=no_ppu */
    if (a > 0xFFFF) a = 0xFFFF;
    if (b > 0xFF)   b = 0xFF;
    if (c > 0xFF)   c = 0xFF;
    return (c << 24) | (b << 16) | a;
}

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

    /* Count cycles — always, even during NMI handler execution. */
    {
        uint32_t _c = (cycles > 0) ? (uint32_t)cycles : 1;
        s_ops_count       += _c;
        s_dbg_cycles_ticked += _c;
        s_dbg_instrs_ticked++;
    }
    if (s_ops_count < OPS_PER_FRAME) return;
    if (s_vblank_depth >= MAX_VBLANK_DEPTH) { s_dbg_max_depth_skips++; return; }

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
        /* Immediate fire — safe at top level. */
        s_ops_count = 0;
        s_vblank_depth++;
        g_ppustatus = (g_ppustatus & ~0x40) | 0x80;
        /* Zapper games only: save the legacy pulse counter across the NMI
         * call so the outer detection loop's accumulator survives PPU
         * writes inside the handler.  Non-zapper games clear unconditionally
         * to keep scroll/sprite-0 splits aligned to master behaviour. */
        int saved_ctr0 = g_spr0_reads_ctr_legacy;
        int saved_act0 = g_spr0_split_active;
        g_spr0_split_active = 0;
        g_spr0_reads_ctr_legacy = 0;
        g_spr0_split_write_scanline = -1;
        g_ppuscroll_x = 0;
        g_ppuscroll_y = 0;
        g_ppuscroll_x_hud = 0;
        g_ppuscroll_y_hud = 0;
        g_ppuctrl_hud     = g_ppuctrl & 0x38;
        /* Fire the frame-boundary callback only when NMI is enabled.
         * (Cadence/oracle sync for NMI-disabled init remains a separate
         * concern — see notes on 52f0ea5.) */
        if (g_ppuctrl & 0x80) {
            s_dbg_nmi_fires++;
            nes_vblank_callback();
        }
        if (g_zapper_enabled) {
            g_spr0_reads_ctr_legacy = saved_ctr0;
            g_spr0_split_active     = saved_act0;
        }
        s_vblank_depth--;
    } else {
        /* Deferred fire — wait for backward branch (loop boundary) */
        s_vblank_pending = 1;
        /* Safety cap: if a backward-branch checkpoint never arrives and
         * the cycle budget has blown past 1.5× the frame budget, force
         * the pending NMI to fire here. Without this cap, a sufficiently
         * long linear code path (e.g. a multi-bank JSR chain through
         * inline-dispatch trampolines on GxROM) can accumulate 4–5
         * budgets' worth of real-world game-state progression inside a
         * single perceived frame, causing state machines to advance
         * several logical steps per wall-clock frame.
         *
         * The original deferral's motivation — avoiding mid-operation
         * NMI that corrupts bank-dependent reads — is preserved for
         * typical workloads (DH-style code with frequent back-edges,
         * never exceeds 1× budget). This cap only fires on the
         * pathological unbounded case, where the alternative is worse:
         * today's behavior has no upper bound at all. */
        if (s_ops_count > (OPS_PER_FRAME + OPS_PER_FRAME / 2)) {
            s_dbg_forced_cap_hits++;
            if (!(g_ppuctrl & 0x80)) s_dbg_pending_no_ppu++;
            maybe_fire_pending_vblank();
        }
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

    /* Standard VBlank start: set VBlank flag, clear sprite-0-hit flag. */
    g_ppustatus = (g_ppustatus & ~0x40) | 0x80;

    /* Zapper games only: preserve the legacy pulse counter across nested
     * NMIs.  Detection loops accumulate consecutive $2002 reads to trigger
     * the sprite-0 hit; nested NMI handlers do PPU writes that would
     * otherwise reset it before the detection loop reaches threshold. */
    int saved_spr0_ctr    = g_spr0_reads_ctr_legacy;
    int saved_spr0_active = g_spr0_split_active;

    g_spr0_split_active = 0;
    g_spr0_reads_ctr_legacy = 0;
    g_ppuscroll_x = 0;
    g_ppuscroll_y = 0;
    g_ppuscroll_x_hud = 0;
    g_ppuscroll_y_hud = 0;
    g_ppuctrl_hud     = g_ppuctrl & 0x38;

    if (g_ppuctrl & 0x80) {
        s_dbg_nmi_fires++;
        nes_vblank_callback();
    } else if (s_vblank_depth > 1) {
        g_ram[0x1A] = 1;
    }

    if (g_zapper_enabled) {
        g_spr0_reads_ctr_legacy = saved_spr0_ctr;
        g_spr0_split_active     = saved_spr0_active;
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

static int s_saved_vblank_depth = 0;

void runtime_begin_post_nmi(void) {
    /* Post-NMI code (e.g. RTI hijack trampoline) runs outside the NMI on
     * real hardware, but must NOT trigger new VBlanks — NMI is edge-triggered
     * and only fires once per VBlank transition.  Push depth to MAX so
     * maybe_trigger_vblank() returns immediately without setting pending. */
    s_vblank_pending = 0;
    s_saved_vblank_depth = s_vblank_depth;
    s_vblank_depth = MAX_VBLANK_DEPTH;
}

void runtime_end_post_nmi(void) {
    s_vblank_depth = s_saved_vblank_depth;
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
            if (g_zapper_enabled) {
                /* Zapper on port 2:
                 *   bit 3: light sensor (0=detected, 1=not detected)
                 *   bit 4: trigger (1=pulled, 0=not pulled)
                 * Verified against Nestopia (NstInpZapper.cpp line 174). */
                /* Reset spr0 gate on trigger edge for a fresh detection
                 * cycle.  The split-screen spr0 wait sets g_spr0_split_active=1
                 * earlier in the frame.  If this persists into the Zapper
                 * detection Phase 1 loop, the light-detection gate is already
                 * open and the game sees light before spr0 fires in the
                 * detection context — triggering the anti-cheat "aimed at
                 * lamp" rejection.  Clearing spr0_active on trigger press
                 * ensures Phase 1 starts fresh: the counter must accumulate
                 * again before the gate opens. */
                {
                    static uint8_t s_zapper_prev_trigger = 0;
                    if (g_zapper_trigger && !s_zapper_prev_trigger) {
                        g_spr0_split_active = 0;
                        g_spr0_reads_ctr_legacy = 0;
                    }
                    s_zapper_prev_trigger = g_zapper_trigger;
                }
                uint8_t val = 0x40;
                if (!zapper_light_detected()) val |= 0x08;
                /* NES Zapper: $4017 bit 4 is active-low while trigger is
                 * held — set bit 4 only when trigger is RELEASED.
                 * 50cd331 inverted this; empirically Duck Hunt works on
                 * master (no flip), confirming master polarity is correct. */
                if (!g_zapper_trigger) val |= 0x10;
                return val;
            }
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

/* Side-effect-free PRG/SRAM read used by the APU DMC unit to DMA-fetch DPCM
 * sample bytes. Mirrors nes_read's $6000-$FFFF banking but skips bus_tick()
 * and the $2000-$401F I/O regions, which the DMC never reads from. */
uint8_t apu_dmc_read(uint16_t addr) {
    if (addr >= 0xC000) {
        const uint8_t *bank = mapper_get_fixed_bank();
        return bank ? bank[addr - 0xC000] : 0xFF;
    }
    if (addr >= 0x8000) {
        const uint8_t *bank = mapper_get_switchable_bank();
        return bank ? bank[addr - 0x8000] : 0xFF;
    }
    if (addr >= 0x6000) return g_sram[addr - 0x6000];
    return 0xFF;
}

/* Widescreen sidecar: a shadow-OAM X byte was written while a draw-object
 * context may be active.  Re-derive the unwrapped 16-bit screen X from the
 * context; without a valid context (or with an implausible layout offset)
 * record the plain byte = vanilla placement.  See nes_runtime.h. */
static void ws_sidecar_track(uint16_t a, uint8_t val) {
    int slot = (a - 0x200) >> 2;
    int16_t wide = val;
    if (g_ws_obj_ctx_valid) {
        /* Layout offsets relative to the object's rel X.  SMB-class layouts
         * span at most ~5 tiles; reject anything outside a generous window
         * so unrelated writes (HUD/static sprites under a stale context)
         * fall back to vanilla placement. */
        int delta = (int8_t)(uint8_t)(val - g_ws_obj_rel8);
        if (delta >= -24 && delta <= 56) {
            int w = (int)g_ws_obj_true_rel + delta;
            if (w >= -256 && w < 512) wide = (int16_t)w;
        }
    }
    g_ws_shadow_x16[slot] = wide;
}

void nes_write(uint16_t addr, uint8_t val) {
    bus_tick();

    if (addr <= 0x1FFF) {
        uint16_t a = addr & 0x07FF;
        if (g_ws_oam_sidecar && (a & 0x0700) == 0x0200 && (a & 3) == 3)
            ws_sidecar_track(a, val);
        g_ram[a] = val; return;
    }
    if (addr >= 0x2000 && addr <= 0x3FFF) { ppu_write_reg(0x2000 + (addr & 7), val); return; }
    if (addr == 0x4014) {
        uint16_t src = (uint16_t)val << 8;
        for (int i = 0; i < 256; i++) g_ppu_oam[i] = nes_read(src + i);
        if (g_ws_oam_sidecar) {
            /* Keep the render-side sidecar paired with the OAM snapshot.
             * DMA from the tracked shadow page adopts the shadow sidecar;
             * any other source falls back to the plain X bytes. */
            if (val < 0x20 && (src & 0x0700) == 0x0200) {
                memcpy(g_oam_x16, g_ws_shadow_x16, sizeof(g_oam_x16));
            } else {
                for (int i = 0; i < 64; i++)
                    g_oam_x16[i] = g_ppu_oam[i * 4 + 3];
            }
        }
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

uint16_t nes_read16_jmpbug(uint16_t addr) {
    /* NMOS 6502 JMP (abs) page-wrap erratum: high-byte fetch stays inside
     * the same 256-byte page as the low-byte fetch. addr=$12FF reads
     * lo from $12FF and hi from $1200 (NOT $1300). */
    uint8_t lo = nes_read(addr);
    uint8_t hi = nes_read((addr & 0xFF00) | (uint16_t)((addr + 1) & 0x00FF));
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

void ppu_write_reg(uint16_t reg, uint8_t val) {
    ppu_trace_write(reg, val);
    /* Any PPU write between $2002 reads means the read was a latch reset
     * (e.g., LDA $2002; STA $2006), not a sprite-0 spin-wait poll.
     * Reset the legacy pulse counter so only consecutive $2002 reads
     * trigger the fallback hit (only relevant when predict_disable=1).
     * Zapper games suppress the reset during nested NMIs because the
     * outer detection loop's accumulator must survive PPU register
     * setup inside the handler. */
    if (!(g_zapper_enabled && s_vblank_depth > 1))
        g_spr0_reads_ctr_legacy = 0;
    switch (reg) {
        case 0x2000:
            g_ppuctrl = val;
            /* $2000 bits 0-1 → t bits 10-11 (nametable select) */
            s_ppu_t = (s_ppu_t & 0xF3FF) | ((uint16_t)(val & 3) << 10);
            break;
        case 0x2001: g_ppumask = val; break;
        case 0x2003: g_oamaddr = val; break;
        case 0x2004:
            /* Direct OAM writes carry no draw context: plain X in the sidecar. */
            if (g_ws_oam_sidecar && (g_oamaddr & 3) == 3)
                g_oam_x16[g_oamaddr >> 2] = val;
            g_ppu_oam[g_oamaddr++] = val;
            break;
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
            /* If the HUD scroll was already captured at the sprite-0 hit this
             * frame, this $2005 write is the post-hit playfield scroll — record
             * the scanline it lands on (the true HUD/playfield split). */
            if (g_spr0_split_active && g_spr0_split_write_scanline < 0)
                g_spr0_split_write_scanline = scanline_from_cycles(s_ops_count);
            g_ppuaddr_latch ^= 1;
            break;
        }
        case 0x2006:
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
                if (chr_override_active())
                    chr_override_on_ppuaddr(g_ppuaddr);
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
                if (chr_override_active())
                    chr_override_on_chr_write(a, val);
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
            /* Sprite-0-hit (bit 6) cycle prediction.
             *
             * Real hardware: bit 6 is set by the PPU during active rendering when
             * sprite 0's opaque pixel overlaps an opaque BG pixel, and cleared
             * only at the pre-render scanline. Reads of $2002 do NOT clear it.
             *
             * Our model: at frame start (nes_vblank_callback), the predictor
             * scans sprite 0 vs BG to determine the scanline at which the hit
             * would fire. Here we convert the CPU's per-frame cycle counter
             * (s_ops_count) to the current scanline and compare. Bit 6 latches
             * monotonically until VBlank clears it (matches real hardware).
             *
             * The legacy 3-read pulse counter is preserved behind
             * g_spr0_predict_disable for emergency opt-out only. */
            if (!g_spr0_predict_disable && g_predicted_spr0_scanline < 240) {
                /* Cycle-accurate path: predictor says sprite 0 will hit at a
                 * known scanline this frame. Fire bit 6 when CPU cycle
                 * position crosses that scanline; bit 6 latches sticky until
                 * VBlank (real-hardware behavior). */
                int now = scanline_from_cycles(s_ops_count);
                if (now >= g_predicted_spr0_scanline) {
                    if (!(g_ppustatus & 0x40)) {
                        g_ppuscroll_x_hud = g_ppuscroll_x;
                        g_ppuscroll_y_hud = g_ppuscroll_y;
                        g_ppuctrl_hud     = g_ppuctrl & 0x38;
                    }
                    g_ppustatus |= 0x40;
                    g_spr0_split_active = 1;
                }
            } else {
                /* Pulse-with-consume fallback: predictor said no hit possible
                 * (rendering disabled, sprite 0 off-screen, or no overlap),
                 * OR predict mode is disabled entirely. Bit 6 oscillates so
                 * games polling $2002 for VBlank detection or split-screen
                 * spin-waits don't see a permanent stuck-on flag. This is
                 * the legacy heuristic — wrong vs hardware but lets games
                 * make forward progress when our model can't help. */
                if (g_ppustatus & 0x40) {
                    g_ppustatus &= ~0x40;
                    g_spr0_reads_ctr_legacy = 0;
                    g_spr0_split_active = 1;
                } else if (++g_spr0_reads_ctr_legacy >= 3) {
                    g_ppuscroll_x_hud = g_ppuscroll_x;
                    g_ppuscroll_y_hud = g_ppuscroll_y;
                    g_ppuctrl_hud     = g_ppuctrl & 0x38;
                    g_ppustatus |= 0x40;
                    g_spr0_reads_ctr_legacy = 0;
                    /* Activate the HUD split here, not only in the consume
                     * branch above.  A sprite-0 split spin-wait that exits on
                     * the SET edge (e.g. Faxanadu's NMI routine $C9D6:
                     *   STA $2000/$2005 (HUD scroll) ; BIT $2002/BVC (wait hit)
                     *   ; STA $2000/$2005 (game scroll))
                     * reads $2002 exactly until bit 6 pulses on, then never
                     * re-reads — so it captures the HUD scroll here but never
                     * reaches the consume branch, leaving g_spr0_split_active=0
                     * and the renderer skipping the split (HUD region falls
                     * through to the game base nametable).  Gate on a genuine
                     * predicted sprite-0 hit at the CURRENT (HUD-region) PPU
                     * state so this never false-triggers on games that merely
                     * busy-poll $2002 for VBlank (sprite 0 off-screen / no BG
                     * overlap -> predictor returns 240). */
                    if (ppu_predict_spr0_hit_scanline() < 240)
                        g_spr0_split_active = 1;
                }
            }
            uint8_t s = g_ppustatus;
            g_ppustatus &= ~0x80;  /* clear VBlank flag on read (standard NES) */
            g_ppuaddr_latch = 0;   /* shared w toggle — clears for both $2005 and $2006 */
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

MissRecord g_miss_ring[MAX_MISS_RING];
int        g_miss_ring_head  = 0;
int        g_miss_ring_count = 0;

#ifdef RECOMP_STACK_TRACKING
extern const char *g_recomp_stack[];
extern int         g_recomp_stack_top;
extern const char *g_last_recomp_func;
#endif

/* Apply the configured DispatchMissPolicy after a miss has been recorded.
 *   kind        "dispatch" or "inline_dispatch" — appears in the diagnostic.
 *   addr        the missed target (or dispatch PC for inline misses).
 *   bank        active bank at miss time.
 *   class_name  miss-target classification ("CODE", "ZERO_FILLED", etc.).
 *   ctx         caller PC (dispatch) or A register (inline). For diagnostic.
 * No-op for LOG_RETURN. exit(1) for FATAL. Pause flag for TRAP. */
static void apply_dispatch_miss_policy(const char *kind, uint16_t addr,
                                       int bank, const char *class_name,
                                       uint16_t ctx) {
    switch (g_dispatch_miss_policy) {
        case DISPATCH_MISS_LOG_RETURN:
            return;
        case DISPATCH_MISS_FATAL:
            fprintf(stderr,
                    "[runtime] FATAL: %s miss at $%04X (bank=%d, class=%s, "
                    "ctx=$%04X, frame=%llu). Policy=fatal.\n",
                    kind, addr, bank, class_name, ctx,
                    (unsigned long long)g_frame_count);
            fflush(stderr);
            fflush(stdout);
            exit(1);
        case DISPATCH_MISS_TRAP: {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "%s miss at $%04X (bank=%d, class=%s, ctx=$%04X)",
                     kind, addr, bank, class_name, ctx);
            debug_server_request_pause(buf);
            return;
        }
    }
}

/* Classify the bytes at a miss target. See MissTargetClass in nes_runtime.h.
 * Pure helper — safe to call from anywhere. */
static uint8_t classify_miss_target(const uint8_t bytes[8]) {
    int all_zero = 1;
    for (int i = 0; i < 8; i++) if (bytes[i] != 0) { all_zero = 0; break; }
    if (all_zero)       return MISS_TARGET_ZERO;
    if (bytes[0] == 0x60) return MISS_TARGET_RTS_STUB;
    return MISS_TARGET_CODE;
}

void nes_log_dispatch_miss(uint16_t addr) {
    /* Let the game handle unmapped addresses (e.g. SRAM code remapping) */
    if (game_dispatch_override(addr)) return;
    static uint32_t last = 0xFFFFFFFF;
    uint32_t key = ((uint32_t)g_current_bank << 16) | addr;
    bool first_for_key = (key != last);

    /* Capture target bytes + classification up front so we can print it
     * inline with the first-sighting log line. */
    uint8_t tbytes[8];
    for (int i = 0; i < 8; i++)
        tbytes[i] = mapper_peek_prg((uint16_t)(addr + i));
    uint8_t tclass = classify_miss_target(tbytes);
    const char *class_name =
        (tclass == MISS_TARGET_ZERO)     ? "ZERO_FILLED" :
        (tclass == MISS_TARGET_RTS_STUB) ? "RTS_STUB"    : "CODE";

    /* Decode the hardware-pushed return on the 6502 stack.
     * After JSR, the stack holds (PC+2) as (hi, lo) with PC-1 semantics —
     * i.e. top-of-stack low byte at SP+1, high at SP+2. Reading them here
     * gives the address of the byte AFTER the JSR that reached the
     * dispatcher (the JSR itself is at call_site_pc - 3). */
    uint8_t s_lo_idx = (uint8_t)(g_cpu.S + 1);
    uint8_t s_hi_idx = (uint8_t)(g_cpu.S + 2);
    uint16_t call_site_pc = (uint16_t)g_ram[0x100 + s_lo_idx] |
                            ((uint16_t)g_ram[0x100 + s_hi_idx] << 8);

    if (first_for_key) {
        printf("[Dispatch] MISS: no func for $%04X bank=%d target=%s "
               "A=%02X X=%02X Y=%02X call_site=$%04X\n",
               addr, g_current_bank, class_name,
               g_cpu.A, g_cpu.X, g_cpu.Y, call_site_pc);
        last = key;
    }
    g_miss_count_any++;
    g_miss_last_addr  = addr;
    g_miss_last_frame = g_frame_count;
    g_miss_last_bank  = g_current_bank;
    /* Capture caller context: top of recomp call stack + 6502 stack snapshot */
    const char *c0 = "(none)";
    const char *c1 = "(none)";
#ifdef RECOMP_STACK_TRACKING
    c0 = (g_recomp_stack_top > 0) ? g_recomp_stack[g_recomp_stack_top - 1] : g_last_recomp_func;
    c1 = (g_recomp_stack_top > 1) ? g_recomp_stack[g_recomp_stack_top - 2] : "(none)";
    strncpy(g_miss_last_caller, c0 ? c0 : "(none)", sizeof(g_miss_last_caller)-1);
    g_miss_last_caller[sizeof(g_miss_last_caller)-1] = '\0';
    strncpy(g_miss_last_stack2, c1 ? c1 : "(none)", sizeof(g_miss_last_stack2)-1);
    g_miss_last_stack2[sizeof(g_miss_last_stack2)-1] = '\0';
#endif
    g_miss_last_sp = g_cpu.S;
    /* Snapshot 16 bytes above SP (the most recent pushes) */
    uint8_t stack_snap[16];
    for (int i = 0; i < 16; i++) {
        uint8_t s = (uint8_t)(g_cpu.S + 1 + i);
        stack_snap[i] = g_ram[0x100 + s];
        g_miss_last_stack_bytes[i] = stack_snap[i];
    }

    /* Push a full record into the ring (newest at head-1, oldest wraps). */
    {
        MissRecord *r = &g_miss_ring[g_miss_ring_head];
        r->addr         = addr;
        r->bank         = g_current_bank;
        r->frame        = g_frame_count;
        r->cpu_a        = g_cpu.A;
        r->cpu_x        = g_cpu.X;
        r->cpu_y        = g_cpu.Y;
        r->cpu_p        = g_cpu.P;
        r->cpu_s        = g_cpu.S;
        r->call_site_pc = call_site_pc;
        for (int i = 0; i < 8;  i++) r->target_bytes[i] = tbytes[i];
        r->target_class = tclass;
        for (int i = 0; i < 16; i++) r->stack_bytes[i]  = stack_snap[i];
        strncpy(r->caller,  c0 ? c0 : "(none)", sizeof(r->caller)-1);
        r->caller[sizeof(r->caller)-1]   = '\0';
        strncpy(r->caller2, c1 ? c1 : "(none)", sizeof(r->caller2)-1);
        r->caller2[sizeof(r->caller2)-1] = '\0';
        g_miss_ring_head = (g_miss_ring_head + 1) % MAX_MISS_RING;
        if (g_miss_ring_count < MAX_MISS_RING) g_miss_ring_count++;
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
            fprintf(mf, "extra_func %d 0x%04X  # target=%s A=%02X X=%02X Y=%02X call_site=$%04X\n",
                    g_current_bank, addr, class_name,
                    g_cpu.A, g_cpu.X, g_cpu.Y, call_site_pc);
            fclose(mf);
        }
        printf("[Dispatch] NEW miss logged: extra_func %d 0x%04X (frame %llu) target=%s\n",
               g_current_bank, addr, (unsigned long long)g_frame_count, class_name);
        fflush(stdout);
    }

    g_dispatch_miss_count++;
    apply_dispatch_miss_policy("dispatch", addr, g_current_bank,
                               class_name, call_site_pc);
}

void nes_log_inline_miss(uint16_t dispatch_pc, uint8_t a_val) {
    static uint32_t last = 0xFFFFFFFF;
    uint32_t key = ((uint32_t)dispatch_pc << 8) | a_val;
    if (key != last) {
        printf("[Dispatch] INLINE MISS @$%04X A=%d (0x%02X)\n", dispatch_pc, (int)a_val, (unsigned)a_val);
        last = key;
    }
    g_miss_count_any++;
    g_inline_dispatch_miss_count++;
    apply_dispatch_miss_policy("inline_dispatch", dispatch_pc, g_current_bank,
                               "INLINE", a_val);
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
