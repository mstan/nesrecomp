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
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

CPU6502State g_cpu;
uint8_t      g_ram[0x0800];
int          g_bail_active;  /* set by stack_bail_func, checked at JSR call sites */
uint16_t     g_trampoline_target;  /* tail-call trampoline: set by cross-function branches */
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

/* ---- Zapper debug trace ---- */
static int   s_zapper_trace_count = 0;
static int   s_zapper_prev_trigger = 0;
#define ZAPPER_TRACE_MAX 300
static void zapper_trace(const char *fmt, ...) {
    if (s_zapper_trace_count >= ZAPPER_TRACE_MAX) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
    s_zapper_trace_count++;
}
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

    /* Gate on sprite-0 hit.  On real NES, the CRT electron beam scans
     * top-to-bottom.  The Zapper can only see light once the beam has
     * passed the target area.  Games use sprite-0 hit as a scanline
     * marker: the detection loop polls $2002 (spr0) and $4017 (light)
     * simultaneously.  Before spr0 fires, the beam hasn't reached the
     * detection zone, so the Zapper sees no light.
     *
     * Without this gate, zapper_light_detected() checks the full
     * framebuffer and may report light BEFORE spr0 fires, triggering
     * the anti-cheat path (light before spr0 = pointing at a lamp). */
    if (!g_spr0_split_active) return 0;

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
    int result = (count > 0 && total_lum / count > 30) ? 1 : 0;
    /* Sprite fallback: check PPU internal OAM for sprites at the aim point.
     * Only when sprites are enabled in PPUMASK — during phases where sprites
     * are disabled, the Zapper should not see them even if they exist in OAM. */
    if (!result && (g_ppumask & 0x10))
        result = zapper_oam_hit();
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
    /* S-register change tracking (per-instruction) */
    debug_server_check_s();

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
        /* Immediate fire — safe at top level.
         * Reset ops count AFTER the NMI handler returns, not before.
         * On real NES, the NMI handler executes during VBlank; the main
         * loop resumes with a full frame budget (29781 cycles).  If we
         * reset before, NMI cycles count against the main loop budget,
         * causing premature next-VBlank that can corrupt ZP pointers
         * used by bank-switch dispatch (GxROM GoBankInit pattern). */
        s_vblank_depth++;
        g_ppustatus = (g_ppustatus & ~0x40) | 0x80;
        /* Save legacy pulse counter and active flag — NMI handler PPU writes
         * would otherwise reset them. The new predictor doesn't depend on a
         * counter, but g_spr0_split_active is still the light-detection gate. */
        int saved_ctr0 = g_spr0_reads_ctr_legacy;
        int saved_act0 = g_spr0_split_active;
        g_spr0_split_active = 0;
        g_spr0_reads_ctr_legacy = 0;
        g_ppuscroll_x = 0;
        g_ppuscroll_y = 0;
        g_ppuscroll_x_hud = 0;
        g_ppuscroll_y_hud = 0;
        g_ppuctrl_hud     = g_ppuctrl & 0x38;
        /* Always invoke the frame-boundary callback, even when NMI is
         * disabled ($2000 bit7=0).  The callback itself gates game_run_nmi
         * on NMI-enable (main_runner.c); but it also drives wall-clock
         * frame cadence: g_frame_count tick, SDL event poll, and in
         * verify mode the oracle's retro_run.  Gating here caused a silent
         * budget-consumption bug: when NMI was disabled (e.g. during a
         * PPU-off mode transition), s_ops_count kept resetting without
         * the callback running, so main-loop code advanced multiple
         * frame-budgets' worth of game state between two visible frames.
         * In --verify mode this manifested as native's mode machine
         * racing 3 frames ahead of the oracle at mode-transition points. */
        s_dbg_nmi_fires++;
        nes_vblank_callback();
        /* Restore spr0 state for outer code's detection loop */
        g_spr0_reads_ctr_legacy = saved_ctr0;
        g_spr0_split_active     = saved_act0;
        s_vblank_depth--;
        s_ops_count = 0;
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

    /* Save sprite-0 state across nested VBlanks. The cycle predictor's
     * frame-start sample (g_predicted_spr0_scanline) is set in
     * nes_vblank_callback below; preserving the outer split_active flag
     * ensures the outer detection loop's light-gate stays consistent
     * across the nested fire. The legacy pulse counter is preserved too
     * for the opt-out path (g_spr0_predict_disable=1). */
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

    /* Restore sprite-0 state for the outer loop's detection */
    g_spr0_reads_ctr_legacy = saved_spr0_ctr;
    g_spr0_split_active     = saved_spr0_active;

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

/* DEBUG: $14 lifecycle trace — active only when START is held */
int s_14_trace_active = 0;
int s_14_trace_read_count = 0;
uint8_t s_14_trace_bits[16];

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
            if (s_14_trace_active && s_14_trace_read_count < 16) {
                s_14_trace_bits[s_14_trace_read_count++] = bit;
            }
            return 0x40 | bit;
        }
        if (addr == 0x4017) {
            if (g_zapper_enabled) {
                /* Zapper on port 2:
                 *   bit 3: light sensor (0=detected, 1=not detected)
                 *   bit 4: trigger (1=pulled, 0=not pulled)
                 * Verified against Nestopia (NstInpZapper.cpp line 174). */
                /* Trace trigger edge */
                if (g_zapper_trigger && !s_zapper_prev_trigger) {
                    s_zapper_trace_count = 0; /* reset on new trigger press */
                    /* Reset spr0 gate for the new detection cycle.
                     * The split-screen spr0 wait sets g_spr0_split_active=1
                     * earlier in the frame.  If this persists into the Zapper
                     * detection Phase 1 loop, the light-detection gate is
                     * already open and the game sees light before spr0 fires
                     * in the detection context — triggering the anti-cheat
                     * "aimed at lamp" rejection.  Clearing spr0_active on
                     * trigger press ensures Phase 1 starts fresh: the counter
                     * must accumulate again before the gate opens. */
                    g_spr0_split_active = 0;
                    g_spr0_reads_ctr_legacy = 0;
                    zapper_trace("=== TRIGGER PRESSED frame=%llu aim=(%d,%d) ===\n",
                        g_frame_count, g_zapper_x, g_zapper_y);
                    zapper_trace("  RAM: $C5=%02X $24=%02X $26=%02X $4F=%02X $CD=%02X $84=%02X $0B=%02X\n",
                        g_ram[0xC5], g_ram[0x24], g_ram[0x26], g_ram[0x4F],
                        g_ram[0xCD], g_ram[0x84], g_ram[0x0B]);
                    zapper_trace("  RAM: $E3=%02X $E4=%02X $D2=%02X $D3=%02X $D4=%02X\n",
                        g_ram[0xE3], g_ram[0xE4], g_ram[0xD2], g_ram[0xD3], g_ram[0xD4]);
                    zapper_trace("  spr0_active=%d pred_sl=%d ppumask=%02X ppustatus=%02X\n",
                        g_spr0_split_active, g_predicted_spr0_scanline, g_ppumask, g_ppustatus);
                }
                if (!g_zapper_trigger && s_zapper_prev_trigger) {
                    zapper_trace("=== TRIGGER RELEASED frame=%llu ===\n", g_frame_count);
                    zapper_trace("  RAM: $C5=%02X $CD=%02X $E3=%02X $E4=%02X\n",
                        g_ram[0xC5], g_ram[0xCD], g_ram[0xE3], g_ram[0xE4]);
                }
                s_zapper_prev_trigger = g_zapper_trigger;
                uint8_t val = 0x40;
                int light = zapper_light_detected();
                if (!light) val |= 0x08;
                if (g_zapper_trigger) val |= 0x10;
                if (g_ram[0xC5])
                    zapper_trace("  $4017: val=%02X light=%d spr0_act=%d ppumask=%02X frame=%llu\n",
                        val, light, g_spr0_split_active, g_ppumask,
                        (unsigned long long)g_frame_count);
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
        /* DEBUG: trace all writes to $14/$15 when START trace active */
        if (s_14_trace_active && (a == 0x14 || a == 0x15)) {
            printf("[T14] W $%02X: %02X -> %02X f=%llu S=%02X depth=%d\n",
                a, g_ram[a], val, (unsigned long long)g_frame_count,
                g_cpu.S, s_vblank_depth);
            fflush(stdout);
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
        /* Debug: trace spawn-chain variables $25 $26 $E7 $5E $DB (skip $2C noise) */
        if ((a == 0x25 || a == 0x26 || a == 0xE7 || a == 0x5E || a == 0xDB) && val != g_ram[a]) {
            static int s_spawn_log = 0;
            if (s_spawn_log < 400) {
                printf("[SPAWN] $%04X: %02X -> %02X (frame=%llu) $24=%02X $25=%02X $26=%02X $E7=%02X $DB=%02X bank=%d $5F0=%02X $5BB=%02X\n",
                       a, g_ram[a], val, (unsigned long long)g_frame_count,
                       g_ram[0x24], g_ram[0x25], g_ram[0x26], g_ram[0xE7],
                       g_ram[0xDB], g_current_bank,
                       g_ram[0x5F0], g_ram[0x5BB]);
            }
            s_spawn_log++;
        }
        /* Trace entity slot 0 writes ($600-$60B) */
        if (a >= 0x600 && a <= 0x60B && val != g_ram[a]) {
            static int s_e0_log = 0;
            if (s_e0_log < 100) {
                zapper_trace("  [ENT0] $%03X: %02X->%02X frame=%llu (offset=%d)\n",
                    a, g_ram[a], val, (unsigned long long)g_frame_count, a - 0x600);
                s_e0_log++;
            }
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
            if (s_14_trace_active) {
                printf("[T14] LATCH f=%llu ctrl1=%02X shift=%02X\n",
                    (unsigned long long)g_frame_count, g_controller1_buttons, s_ctrl1_shift);
                fflush(stdout);
                s_14_trace_read_count = 0;
            }
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
     * Reset the legacy pulse counter so only consecutive $2002 reads
     * trigger the fallback hit (only relevant when predict_disable=1).
     * But DON'T reset during nested NMIs — the outer detection loop's
     * counter must survive the NMI handler's PPU register setup. */
    if (s_vblank_depth <= 1)
        g_spr0_reads_ctr_legacy = 0;
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
                }
            }
            uint8_t s = g_ppustatus;
            g_ppustatus &= ~0x80;  /* clear VBlank flag on read (standard NES) */
            g_ppuaddr_latch = 0;   /* shared w toggle — clears for both $2005 and $2006 */
            if (g_ram[0xC5])
                zapper_trace("  $2002: ret=%02X spr0_act=%d pred_sl=%d cyc=%u ppumask=%02X frame=%llu\n",
                    s, g_spr0_split_active, g_predicted_spr0_scanline,
                    (unsigned)s_ops_count, g_ppumask,
                    (unsigned long long)g_frame_count);
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
