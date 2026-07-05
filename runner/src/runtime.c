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
#include "ppu_dot.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ---- APU register-write trace ring (always-on, env-gated) -------------------
 * NESRECOMP_APU_TRACE=<path>  ->  capture every $4000-$401F APU register write as
 * (cpu_cycle, addr, val) into a bounded ring, dumped to CSV at exit. Used to diff
 * the recomp's APU-input stream bit-exactly against the Mesen2 oracle's. Pays
 * nothing (one branch) unless the env var is set. Cycle stamp is the true
 * monotonic counter g_nes_cycles (real instruction + DMC + OAM DMA cycles), so
 * inter-write deltas line up with Mesen's cpu.cycleCount without inheriting the
 * old fixed-frame-length (29781) estimate error. */
#define APU_TRACE_CAP (1u << 20)   /* 1,048,576 entries */
typedef struct { uint64_t cyc; uint16_t addr; uint8_t val; } ApuTraceEnt;
static ApuTraceEnt *s_apu_trace = NULL;
static uint64_t     s_apu_trace_n = 0;     /* total pushed (monotonic) */
static const char  *s_apu_trace_path = NULL;
static int          s_apu_trace_state = -1; /* -1 unqueried, 0 off, 1 on */

static void apu_trace_dump(void) {
    if (s_apu_trace_state != 1 || !s_apu_trace || !s_apu_trace_path) return;
    FILE *f = fopen(s_apu_trace_path, "w");
    if (!f) return;
    fprintf(f, "cycle,addr,val\n");
    uint64_t total = s_apu_trace_n;
    uint64_t first = (total > APU_TRACE_CAP) ? (total - APU_TRACE_CAP) : 0;
    for (uint64_t k = first; k < total; ++k) {
        ApuTraceEnt *e = &s_apu_trace[k % APU_TRACE_CAP];
        fprintf(f, "%llu,%u,%u\n", (unsigned long long)e->cyc, (unsigned)e->addr, (unsigned)e->val);
    }
    fclose(f);
}

static void apu_trace_push(uint64_t cyc, uint16_t addr, uint8_t val) {
    if (s_apu_trace_state < 0) {
        const char *e = getenv("NESRECOMP_APU_TRACE");
        if (e && *e) {
            s_apu_trace = (ApuTraceEnt *)malloc(sizeof(ApuTraceEnt) * APU_TRACE_CAP);
            if (s_apu_trace) { s_apu_trace_path = e; s_apu_trace_state = 1; atexit(apu_trace_dump); }
            else             { s_apu_trace_state = 0; }
        } else { s_apu_trace_state = 0; }
    }
    if (s_apu_trace_state != 1) return;
    ApuTraceEnt *slot = &s_apu_trace[s_apu_trace_n % APU_TRACE_CAP];
    slot->cyc = cyc; slot->addr = addr; slot->val = val;
    s_apu_trace_n++;
}

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
static const uint32_t *s_zapper_framebuf = NULL; /* PRESENT buffer (1-frame stale) */
static const uint32_t *s_zapper_snap     = NULL; /* LIVE snapshot of current PPU state */
static zapper_render_fn s_zapper_render = NULL;  /* on-demand live-snapshot callback */

void runtime_set_zapper_framebuf(const uint32_t *fb) { s_zapper_framebuf = fb; }
void runtime_set_zapper_render_callback(zapper_render_fn fn) { s_zapper_render = fn; }
void runtime_set_zapper_snapshot(const uint32_t *fb) { s_zapper_snap = fb; }

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

/* Always-on (env-gated) tap for every $4017 light-sensor probe. Set
 * NESRECOMP_ZAPPER_TRACE=<path> to append a CSV row per call:
 *   frame,x,y,ppumask,render_width,stale_lum,live_lum,result
 * stale_lum = luminance from the 1-frame-delayed present buffer (the old,
 * broken source); live_lum = luminance from the current-state snapshot (the
 * value the decision now uses). A divergence proves the staleness defect.
 * This is a continuous tap, not an arm-then-capture probe. */
static FILE *s_zapper_trace = NULL;
static int   s_zapper_trace_init = 0;
static void zapper_trace_record(int x, int y, uint8_t ppumask, int render_width,
                                int stale_lum, int live_lum, int result) {
    if (!s_zapper_trace_init) {
        s_zapper_trace_init = 1;
        const char *p = getenv("NESRECOMP_ZAPPER_TRACE");
        if (p && *p) {
            s_zapper_trace = fopen(p, "w");
            if (s_zapper_trace)
                fprintf(s_zapper_trace,
                        "frame,x,y,ppumask,render_width,stale_lum,live_lum,result,trigger,val4017\n");
        }
    }
    if (!s_zapper_trace) return;
    /* Reconstruct the $4017 byte the game sees: bit3=0 light detected,
     * bit4=0 trigger pulled (both active-low), bit6 always set. */
    uint8_t val = 0x40;
    if (!result) val |= 0x08;
    if (!g_zapper_trigger) val |= 0x10;
    fprintf(s_zapper_trace, "%llu,%d,%d,%02X,%d,%d,%d,%d,%d,%02X\n",
            (unsigned long long)g_frame_count, x, y, ppumask,
            render_width, stale_lum, live_lum, result, g_zapper_trigger, val);
    fflush(s_zapper_trace);
}

/* Average luminance of the 3x3 neighbourhood around (x,y) in `fb`, or -1 if
 * the buffer is NULL / aim is off-screen. fb is g_render_width wide. */
static int zapper_sample_lum(const uint32_t *fb, int x, int y, int width) {
    if (!fb || x < 0 || x >= 256 || y < 0 || y >= 240) return -1;
    int total = 0, count = 0;
    for (int dy = -4; dy <= 4; dy += 4) {
        for (int dx = -4; dx <= 4; dx += 4) {
            int px = x + dx, py = y + dy;
            if (px < 0 || px >= 256 || py < 0 || py >= 240) continue;
            uint32_t pixel = fb[py * width + px];
            int r = (pixel >> 16) & 0xFF, g = (pixel >> 8) & 0xFF, b = pixel & 0xFF;
            total += (r * 3 + g * 6 + b) / 10;
            count++;
        }
    }
    return (count > 0) ? total / count : -1;
}

/* Simulate the NES Zapper photodiode: is the pixel currently displayed at the
 * aim point "bright"?  The dot-PPU publishes its framebuffer with a 1-frame
 * pipeline delay, so the present buffer is always one frame behind the moment
 * the game reads $4017.  We therefore render a LIVE snapshot of current PPU
 * state (current OAM + nametables + scroll + palette + PPUMASK) and sample
 * that — exactly what the CRT beam would show.  This restores the no-pipeline-
 * delay behaviour the per-frame renderer had (known-good for Duck Hunt and
 * Gumshoe) and is ungated: detection that does NOT toggle PPUMASK (Gumshoe)
 * is sampled just the same as detection that does (Duck Hunt). */
static int zapper_light_detected(void) {
    extern int g_render_width;
    int x = g_zapper_x, y = g_zapper_y;

    /* During early init (before enough frames render), report light detected.
     * This simulates the Zapper pointing at a lit TV screen at power-on,
     * which is how real NES hardware detects the Zapper on combo carts. */
    if (!s_zapper_framebuf || g_frame_count < 2) {
        zapper_trace_record(x, y, g_ppumask, g_render_width, -1, -1, 1);
        return 1;
    }

    /* If PPU rendering is completely disabled (both sprites and BG off),
     * the screen outputs the universal background color — typically dark.
     * The Zapper sees no light.  This is critical for the anti-cheat phase:
     * the game blanks the screen, and if the Zapper still sees light the
     * shot is rejected. */
    if (!(g_ppumask & 0x18)) {
        zapper_trace_record(x, y, g_ppumask, g_render_width, -2, -2, 0);
        return 0;
    }

    /* Render a live snapshot of the CURRENT PPU state.  Ungated — the previous
     * PPUMASK-change gate left non-toggling detection (Gumshoe) reading the
     * stale present buffer, which is why its shots never registered. */
    if (s_zapper_render) s_zapper_render();   /* fills s_zapper_snap */

    const uint32_t *probe = s_zapper_snap ? s_zapper_snap : s_zapper_framebuf;
    int live_lum  = zapper_sample_lum(probe, x, y, g_render_width);
    int stale_lum = zapper_sample_lum(s_zapper_framebuf, x, y, g_render_width);
    int result = (live_lum > 160) ? 1 : 0;
    /* OAM bounding-box fallback removed: it caused false-positive hits
     * because it didn't check actual tile-pixel brightness.  The live
     * snapshot above samples the real displayed pixels. */
    zapper_trace_record(x, y, g_ppumask, g_render_width, stale_lum, live_lum, result);
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

/* Monotonic guest CPU-cycle counter (see nes_runtime.h). Advanced by the same
 * _c as s_ops_count, but never reset — the co-sim alignment ruler. */
uint64_t g_nes_cycles = 0;
/* g_nes_cycles sampled at the frame-boundary FIRE (before the NMI handler runs).
 * The co-sim must measure frame length here, not at the post-handler tap: the
 * handler's length varies frame-to-frame, so sampling after it injects that
 * variance as spurious per-frame cycle "jitter". Fire-to-fire delta is the true,
 * essentially-constant frame length. */
uint64_t g_frame_boundary_cyc = 0;

/* Co-sim video-frame index — DERIVED FROM ELAPSED CYCLES, not counted per NMI.
 * Set at each trace emit (nes_cosim_emit_boundary) to round(g_frame_boundary_cyc /
 * 29780.5) = the true NTSC video-frame number. This is what keeps the trace index
 * equal to the oracle's video-frame count even across NMI-off / post-NMI stretches:
 * a frame counter that ticks per NMI callback (like g_frame_count) STALLS when the
 * boundary is deferred or depth-suppressed, so real video frames elapse un-counted
 * and byte-identical boot/transition state (Zelda $15, Gumshoe timers) looks
 * phase-divergent when the recomp is really matching Mesen frame-for-frame — a pure
 * MEASUREMENT artifact. Deriving the index from the cycle ruler removes it. Only
 * read on the env-gated trace path; g_frame_count (the game-logic clock) is untouched. */
uint64_t g_cosim_vframe = 0;

/* OAM DMA ($4014) cycle-steal accumulator. Writing $4014 halts the CPU 513/514
 * cycles while 256 bytes copy into OAM; those cycles elapse on hardware (PPU
 * and APU keep advancing). Charged in the $4014 handler, drained by
 * runtime_take_oam_dma_stall() into the CPU frame budget + APU clock, exactly
 * mirroring apu_take_dmc_stall(). */
static int s_oam_dma_stall = 0;
int runtime_take_oam_dma_stall(void) { int s = s_oam_dma_stall; s_oam_dma_stall = 0; return s; }

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

/* Rung 2 — dot-accurate frame length (env NESRECOMP_DOT_CLOCK, default OFF).
 * The fixed 29781 threshold runs +0.5 cyc/frame fast vs the NTSC 29780.5 average
 * (measured by the co-sim: recomp 29780.999 vs Mesen 29780.500). The true frame
 * is 89342 PPU dots (=29780.667 cyc), or 89341 on odd frames with rendering on
 * (the skipped pre-render dot). We track the exact dot debt and hand back an
 * INTEGER per-frame CPU-cycle budget carrying the remainder thirds, so the mean
 * frame length is the oracle's 29780.5 instead of 29781 — the frame boundary
 * becomes a dot event, not a fixed cycle count. Gated + default-off until the
 * co-sim certifies no game-logic regression; then it becomes the default. */
#define FRAME_DOTS_EVEN 89342          /* 341 * 262 */
static int      s_dotclock     = -1;   /* -1 unqueried, 0 legacy, 1 dot-accurate */
static int      s_odd_frame    = 0;    /* toggles each frame (odd-frame dot skip) */
static int      s_dot_debt     = 0;    /* leftover dots (0..2) carried across frames */
static uint32_t s_frame_budget = OPS_PER_FRAME;  /* current frame's integer cycle target */

/* Compute the NEXT frame's integer CPU-cycle budget from the exact dot count,
 * carrying the sub-cycle remainder so the running mean is dot-accurate. */
static uint32_t next_frame_budget(void) {
    if (s_dotclock <= 0) return OPS_PER_FRAME;
    int dots = FRAME_DOTS_EVEN - ((s_odd_frame && (g_ppumask & 0x18)) ? 1 : 0);
    s_odd_frame ^= 1;
    s_dot_debt += dots;
    uint32_t cyc = (uint32_t)(s_dot_debt / 3);
    s_dot_debt -= (int)(cyc * 3);
    return cyc;
}

/* bus_tick: called by nes_read/nes_write to count bus operations.
 * Kept for backward compatibility but no longer critical for NMI timing
 * since maybe_trigger_vblank now receives per-instruction cycle counts. */
static inline void bus_tick(void) {
    /* no-op: cycle counting moved to per-instruction maybe_trigger_vblank */
}

static int s_vblank_pending = 0;   /* VBlank waiting to fire at next safe point */

/* ---- Per-frame WRAM delta trace (snesref-style, env-gated, observability only) ----
 * NESRECOMP_WRAM_TRACE=<path>  ->  once per frame, snapshot $0000-$07FF and emit changed
 * bytes as JSONL  {"f":<frame>,"adr":"0x<addr>","old":"0x<v>","val":"0x<v>"} ; frame 0
 * emits the full baseline. Offline first-divergence diff vs the Mesen "nesref" trace.
 * Pure snapshot+compare — does not touch emulation state or timing. */
static FILE   *s_wram_trace_f = NULL;
static int     s_wram_trace_state = -1;   /* -1 unqueried, 0 off, 1 on */
static uint8_t s_wram_prev[0x800];

/* Cross-engine determinism hack (accuracy harness only): force fixed RAM bytes
 * once per frame so RNG/seed state is identical between the recomp and the
 * nesref oracle. Free-running engines desync because Random ($18) is seeded by
 * FrameCounter ($15) and any boot-timing offset cascades into total state
 * divergence. Freezing the RNG seed (e.g. NESRECOMP_FREEZE="0x18=0x00") makes
 * the differential WRAM diff valid: both sides get identical RNG, so any
 * remaining divergence is a real recompiler bug. Env-gated, OFF by default →
 * zero behavior change in normal builds. nesref honors the same NESREF_FREEZE. */
void nes_apply_freeze(void) {
    static int st = -1;
    static uint16_t addrs[16]; static uint8_t vals[16]; static int n = 0;
    if (st < 0) {
        st = 0;
        const char *e = getenv("NESRECOMP_FREEZE");
        if (e && *e) {
            char buf[256]; strncpy(buf, e, sizeof buf - 1); buf[sizeof buf - 1] = 0;
            for (char *t = strtok(buf, ","); t && n < 16; t = strtok(NULL, ",")) {
                unsigned a, v;
                if (sscanf(t, " 0x%x = 0x%x", &a, &v) == 2) { addrs[n]=(uint16_t)a; vals[n]=(uint8_t)v; n++; }
            }
            st = n ? 1 : 0;
        }
    }
    if (st != 1) return;
    for (int i = 0; i < n; i++) if (addrs[i] < 0x800) g_ram[addrs[i]] = vals[i];
}

void nes_wram_trace_frame(void) {
    if (s_wram_trace_state < 0) {
        const char *e = getenv("NESRECOMP_WRAM_TRACE");
        s_wram_trace_f = (e && *e) ? fopen(e, "w") : NULL;
        s_wram_trace_state = s_wram_trace_f ? 1 : 0;
        if (s_wram_trace_state == 1) {
            for (int a = 0; a < 0x800; a++) {
                fprintf(s_wram_trace_f,
                        "{\"f\":%llu,\"adr\":\"0x%04x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
                        (unsigned long long)g_cosim_vframe, a, 0, g_ram[a]);
                s_wram_prev[a] = g_ram[a];
            }
            fflush(s_wram_trace_f);
            return;
        }
    }
    if (s_wram_trace_state != 1) return;
    for (int a = 0; a < 0x800; a++) {
        if (g_ram[a] != s_wram_prev[a]) {
            fprintf(s_wram_trace_f,
                    "{\"f\":%llu,\"adr\":\"0x%04x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
                    (unsigned long long)g_cosim_vframe, a, s_wram_prev[a], g_ram[a]);
            s_wram_prev[a] = g_ram[a];
        }
    }
    fflush(s_wram_trace_f);
}

/* ---- Differential co-sim full-state hash (env-gated, observability only) ----
 * NESRECOMP_COSIM_HASH=<path> -> once per frame at the NMI boundary, emit one
 * JSONL row: {"f":<frame>,"clk":<g_nes_cycles>,"chain":"<hex>","sub":{...}} where
 * `sub` carries a per-subsystem FNV-1a-64 hash of the WHOLE guest-architectural
 * machine (no axis-verdict trimming) and `chain` is a running fingerprint of the
 * entire trajectory (first frame `chain` differs between two runs = the first
 * divergence). This is the recomp side of the frame-granular differential
 * co-sim (see recomp-template/NES/DIFFERENTIAL-COSIM-PROPOSAL.md). Pure read of
 * live state; does not touch emulation or timing. Zero cost unless the env set.
 *
 * Subsystem split follows the doc's "mask by class, never drop a subsystem":
 *   cpu      A/X/Y/S/P + N/V/D/I/Z/C   (PC excluded — currency caveat)
 *   ram      $000-$0FF + $200-$7FF     (stack page split out below)
 *   stack    $100-$1FF                 (recomp never pushes JSR return addrs =>
 *                                        cross-impl expected-diff; REPORTED, not
 *                                        a false "no divergence"; A-vs-A matches)
 *   ppu_mem  nametables + OAM + palette
 *   ppu_regs ctrl/mask/status/oamaddr + loopy v(g_ppuaddr)/t(s_ppu_t)/w-latch/read-buf
 *   apu      full channel + frame-sequencer state (the savestate.c blind spot)
 *   mapper   bank regs + MMC3 IRQ counters (MapperState)
 *   chr      g_chr_ram (meaningful for CHR-RAM carts)
 *   sram     $6000-$7FFF battery RAM
 *   openbus  the CPU data-bus latch
 * EXCLUDED (host-only, per the doc): SDL/audio, DRC ring, fibers, fn pointers. */
static uint8_t s_open_bus;            /* tentative fwd decl; defined (=0) later in this TU */
static FILE *s_cosim_f = NULL;
static int   s_cosim_state = -1;      /* -1 unqueried, 0 off, 1 on */
static uint64_t s_cosim_chain = 1469598103934665603ULL;  /* FNV offset basis */
/* Gate-3 fault injection: NESRECOMP_COSIM_INJECT=<frame>:<region>:<index>:<xor>
 * flips ONE live-state byte at the given frame BEFORE hashing, so the coordinator
 * can prove the tool detects + localizes a divergence (and is not silently blind
 * to a whole subsystem). region: "ram"|"oam". */
static long long s_cosim_inj_frame = -1;
static int        s_cosim_inj_region = 0;  /* 0=none 1=ram 2=oam */
static int        s_cosim_inj_index = 0;
static uint8_t    s_cosim_inj_xor   = 0;

static inline uint64_t cosim_fnv(uint64_t h, const void *data, int len) {
    const uint8_t *p = (const uint8_t *)data;
    for (int k = 0; k < len; k++) { h ^= p[k]; h *= 1099511628211ULL; }
    return h;
}
static inline uint64_t cosim_hash_bytes(const void *data, int len) {
    return cosim_fnv(1469598103934665603ULL, data, len);
}

void nes_cosim_hash_frame(void) {
    if (s_cosim_state < 0) {
        const char *e = getenv("NESRECOMP_COSIM_HASH");
        s_cosim_f = (e && *e) ? fopen(e, "w") : NULL;
        s_cosim_state = s_cosim_f ? 1 : 0;
        const char *inj = getenv("NESRECOMP_COSIM_INJECT");
        if (inj && *inj) {
            char reg[16] = {0};
            long long fr; int idx; unsigned xr;
            /* %i on the index accepts both decimal and 0x-hex robustly. */
            if (sscanf(inj, "%lld:%15[^:]:%i:%x", &fr, reg, &idx, &xr) == 4) {
                s_cosim_inj_frame = fr;
                s_cosim_inj_index = idx;
                s_cosim_inj_xor   = (uint8_t)xr;
                if      (!strcmp(reg, "ram")) s_cosim_inj_region = 1;
                else if (!strcmp(reg, "oam")) s_cosim_inj_region = 2;
            }
        }
    }
    if (s_cosim_state != 1) return;

    /* Gate 3: apply the one-byte fault to LIVE state before we read it, so the
     * hasher must actually read real state to see it (a constant-returning
     * hasher would fail this gate). */
    if (s_cosim_inj_region && (long long)g_cosim_vframe == s_cosim_inj_frame) {
        if (s_cosim_inj_region == 1) g_ram[s_cosim_inj_index & 0x7FF]     ^= s_cosim_inj_xor;
        else                         g_ppu_oam[s_cosim_inj_index & 0xFF]  ^= s_cosim_inj_xor;
    }

    /* CPU (PC excluded — recomp keeps no live PC; currency caveat). */
    uint8_t cpu[11] = { g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S, g_cpu.P,
                        g_cpu.N, g_cpu.V, g_cpu.D, g_cpu.I, g_cpu.Z, g_cpu.C };
    uint64_t h_cpu = cosim_hash_bytes(cpu, sizeof(cpu));

    /* RAM excluding the stack page; stack page hashed separately. */
    uint64_t h_ram = cosim_hash_bytes(g_ram, 0x100);
    h_ram = cosim_fnv(h_ram, g_ram + 0x200, 0x600);
    uint64_t h_stack = cosim_hash_bytes(g_ram + 0x100, 0x100);

    /* PPU memory + registers. */
    uint64_t h_ppumem = cosim_hash_bytes(g_ppu_nt, 0x1000);
    h_ppumem = cosim_fnv(h_ppumem, g_ppu_oam, 0x100);
    h_ppumem = cosim_fnv(h_ppumem, g_ppu_pal, 0x20);
    uint8_t preg[9];
    preg[0]=g_ppuctrl; preg[1]=g_ppumask; preg[2]=g_ppustatus; preg[3]=g_oamaddr;
    preg[4]=(uint8_t)g_ppuaddr; preg[5]=(uint8_t)(g_ppuaddr>>8);
    preg[6]=(uint8_t)s_ppu_t;   preg[7]=(uint8_t)(s_ppu_t>>8);
    preg[8]=(uint8_t)(g_ppuaddr_latch?1:0);   /* w write-toggle */
    uint64_t h_ppureg = cosim_hash_bytes(preg, sizeof(preg));
    h_ppureg = cosim_fnv(h_ppureg, &g_ppudata_buf, 1);  /* PPUDATA read buffer */

    /* APU (full state via the serializer — the save-state blind spot). */
    uint8_t apu_blob[256];
    int apu_n = apu_get_state_blob(apu_blob, sizeof(apu_blob));
    uint64_t h_apu = cosim_hash_bytes(apu_blob, apu_n);

    /* Mapper (bank regs + MMC3 IRQ counters). memset first: MapperState has
     * padding between its uint8_t and int fields, and a raw struct hash would
     * otherwise fold in uninitialized stack bytes → a false, nondeterministic
     * divergence (Gate 1 caught exactly this). Zeroing makes padding canonical. */
    MapperState ms; memset(&ms, 0, sizeof(ms)); mapper_get_state(&ms);
    uint64_t h_map = cosim_hash_bytes(&ms, sizeof(ms));

    uint64_t h_chr = cosim_hash_bytes(g_chr_ram, 0x2000);
    uint64_t h_sram = cosim_hash_bytes(g_sram, 0x2000);
    uint64_t h_ob  = cosim_hash_bytes(&s_open_bus, 1);

    /* Fold every sub-hash into the running chain (order fixed). */
    uint64_t subs[10] = { h_cpu, h_ram, h_stack, h_ppumem, h_ppureg,
                          h_apu, h_map, h_chr, h_sram, h_ob };
    s_cosim_chain = cosim_fnv(s_cosim_chain, subs, sizeof(subs));

    fprintf(s_cosim_f,
        "{\"f\":%llu,\"clk\":%llu,\"bclk\":%llu,\"chain\":\"%016llx\",\"sub\":{"
        "\"cpu\":\"%016llx\",\"ram\":\"%016llx\",\"stack\":\"%016llx\","
        "\"ppu_mem\":\"%016llx\",\"ppu_regs\":\"%016llx\",\"apu\":\"%016llx\","
        "\"mapper\":\"%016llx\",\"chr\":\"%016llx\",\"sram\":\"%016llx\","
        "\"openbus\":\"%016llx\"}}\n",
        (unsigned long long)g_cosim_vframe, (unsigned long long)g_nes_cycles,
        (unsigned long long)g_frame_boundary_cyc,
        (unsigned long long)s_cosim_chain,
        (unsigned long long)h_cpu, (unsigned long long)h_ram, (unsigned long long)h_stack,
        (unsigned long long)h_ppumem, (unsigned long long)h_ppureg, (unsigned long long)h_apu,
        (unsigned long long)h_map, (unsigned long long)h_chr, (unsigned long long)h_sram,
        (unsigned long long)h_ob);
    fflush(s_cosim_f);
}

/* Per-frame PPU-memory delta trace (Axis 4/5a), env NESRECOMP_PPUMEM_TRACE=<path>.
 * Same JSONL shape as the WRAM trace, over a synthetic PPU-memory image so the
 * existing wram_diff.py works unchanged:
 *   [0x000-0x0FF] OAM (g_ppu_oam)   [0x100-0x11F] palette (g_ppu_pal)
 *   [0x200-0x9FF] nametable RAM 2KB (g_ppu_nt physical NTs 0/1)
 * Oracle side = mesen_ppumem.lua (nesSpriteRam/nesPaletteRam/nesNametableRam).
 * Pair with the RNG-seed freeze (NESRECOMP_FREEZE/NESREF_FREEZE) for a valid diff. */
static FILE   *s_ppu_trace_f = NULL;
static int     s_ppu_trace_state = -1;
static uint8_t s_ppu_prev[0xA00];
static void ppumem_image(uint8_t *img) {
    for (int i = 0; i < 0x100; i++) img[0x000 + i] = g_ppu_oam[i];  /* OAM */
    for (int i = 0; i < 0x020; i++) img[0x100 + i] = g_ppu_pal[i];  /* palette */
    for (int i = 0; i < 0x800; i++) img[0x200 + i] = g_ppu_nt[i];   /* nametable (gap 0x120-0x1FF=0) */
}
void nes_ppumem_trace_frame(void) {
    if (s_ppu_trace_state < 0) {
        const char *e = getenv("NESRECOMP_PPUMEM_TRACE");
        s_ppu_trace_f = (e && *e) ? fopen(e, "w") : NULL;
        s_ppu_trace_state = s_ppu_trace_f ? 1 : 0;
        if (s_ppu_trace_state == 1) {
            uint8_t img[0xA00]; memset(img, 0, sizeof img); ppumem_image(img);
            for (int a = 0; a < 0xA00; a++) {
                fprintf(s_ppu_trace_f,
                        "{\"f\":%llu,\"adr\":\"0x%04x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
                        (unsigned long long)g_cosim_vframe, a, 0, img[a]);
                s_ppu_prev[a] = img[a];
            }
            fflush(s_ppu_trace_f);
            return;
        }
    }
    if (s_ppu_trace_state != 1) return;
    uint8_t img[0xA00]; memset(img, 0, sizeof img); ppumem_image(img);
    for (int a = 0; a < 0xA00; a++) {
        if (img[a] != s_ppu_prev[a]) {
            fprintf(s_ppu_trace_f,
                    "{\"f\":%llu,\"adr\":\"0x%04x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\"}\n",
                    (unsigned long long)g_cosim_vframe, a, s_ppu_prev[a], img[a]);
            s_ppu_prev[a] = img[a];
        }
    }
    fflush(s_ppu_trace_f);
}

/* Emit all env-gated co-sim trace rows for the current frame boundary, tagged with
 * the cycle-derived video-frame index (see g_cosim_vframe). Called post-NMI-handler
 * from nes_vblank_callback (NMI on) and directly at the boundary for NMI-off frames.
 * Deriving the index from g_frame_boundary_cyc (set at the boundary FIRE, pre-handler)
 * means NMI-off / suppressed frames don't skew the index: whichever frames do emit
 * carry their true video-frame number, so they align to the oracle across gaps.
 * Each trace function is individually env-gated → no-op in normal builds. */
void nes_cosim_emit_boundary(void) {
    /* round(g_frame_boundary_cyc / 29780.5) = NTSC video-frame number. */
    g_cosim_vframe = (2ULL * g_frame_boundary_cyc + 29780ULL) / 59561ULL;
    nes_wram_trace_frame();
    nes_ppumem_trace_frame();
    nes_cosim_hash_frame();
}

/* ---- General pending-IRQ delivery hook ----------------------------------
 * The recompiler emits maybe_trigger_vblank() at every instruction boundary,
 * so polling the IRQ line here samples it exactly where the real 6502 does —
 * between instructions.  This is the single delivery point for the maskable
 * IRQ sources that are NOT scanline-timed (the MMC3 scanline IRQ keeps its
 * own dot-synced path in ppu_renderer.c — see service_mmc3_scanline_irq).
 *
 * Sources are LEVEL-triggered: the source stays asserted until the handler
 * acknowledges it ($4015 read clears the frame flag; $4015 write / $4010
 * IRQ-disable clears the DMC flag).  We re-poll every instruction, so a
 * still-asserted line re-fires after RTI — matching hardware IRQ behaviour.
 *
 * Gates: I-flag clear (IRQ is maskable, unlike NMI); no re-entry; and only at
 * top level (s_vblank_depth == 0) so an IRQ never preempts the batched NMI
 * frame driver — sub-frame NMI/IRQ interleave is the deferred Axis 2 work. */
static int s_in_irq = 0;
static void maybe_deliver_irq(void) {
    if (g_cpu.I)              return;   /* IRQ masked */
    if (s_in_irq)             return;   /* no re-entry while a handler runs */
    if (s_vblank_depth != 0)  return;   /* defer during the NMI frame driver */
    if (!apu_irq_asserted())  return;   /* no source asserting the line */

    /* Enter IRQ: push P (B clear, bit5 set), then PCL/PCH placeholders in the
     * NMI convention — func_IRQ() is a direct call whose RTI pops these three
     * bytes (the placeholder return address is never dereferenced).  Same push
     * order as the MMC3 path in ppu_renderer.c. */
    uint8_t p_irq = (uint8_t)((g_cpu.N<<7)|(g_cpu.V<<6)|(1<<5)|
                               (g_cpu.D<<3)|(g_cpu.I<<2)|(g_cpu.Z<<1)|g_cpu.C);
    g_ram[0x100+g_cpu.S] = 0x00;  g_cpu.S--;   /* PCH placeholder */
    g_ram[0x100+g_cpu.S] = 0x00;  g_cpu.S--;   /* PCL placeholder */
    g_ram[0x100+g_cpu.S] = p_irq; g_cpu.S--;   /* P (B=0)         */
    g_cpu.I = 1;                                /* IRQ entry sets I */
    s_in_irq = 1;
    func_IRQ();                                 /* handler's RTI pops P/PCL/PCH */
    s_in_irq = 0;
}

void maybe_trigger_vblank(int cycles) {

    /* Rung 2: one-time dot-clock init. Dot-accurate frame length is the DEFAULT
     * (SMB live-validated by the owner + co-sim-certified on SMB/MM3); set
     * NESRECOMP_DOT_CLOCK=0 to force the legacy fixed-29781 threshold. When off,
     * next_frame_budget() returns OPS_PER_FRAME verbatim so the legacy path is
     * byte-unchanged. */
    if (s_dotclock < 0) {
        const char *e = getenv("NESRECOMP_DOT_CLOCK");
        s_dotclock = (e && *e) ? (*e != '0') : 1;   /* default ON */
        s_frame_budget = next_frame_budget();
    }

    /* Drive the APU frame-counter IRQ on the CPU-cycle stream (NMI-independent),
     * then poll for between-instruction maskable-IRQ delivery (DMC + frame). */
    apu_clock_cycles(cycles > 0 ? cycles : 1);
    /* DMC DMA cycle-steal: each DMC sample-byte fetch halts the CPU ~4 cycles
     * while the APU reads from memory. Those cycles still elapse (PPU/APU
     * advance), so they count toward the frame budget as if the CPU executed
     * them — modeling the CPU losing time to DMA (Axis 2). The APU is advanced
     * through the stall too; the ~4-cycle window cannot trigger another fetch
     * (the DMC byte period is >> 4), so this does not recurse. */
    int dmc_stall = apu_take_dmc_stall();
    if (dmc_stall) apu_clock_cycles(dmc_stall);
    /* OAM DMA cycle-steal (see $4014 handler): the APU/PPU advance through the
     * ~513 stolen cycles too, so clock the APU through them like the DMC steal. */
    int oam_stall = runtime_take_oam_dma_stall();
    if (oam_stall) apu_clock_cycles(oam_stall);
    maybe_deliver_irq();

    /* Count cycles — always, even during NMI handler execution. Stolen DMC and
     * OAM DMA cycles are counted here so the frame advances as on hardware. */
    {
        uint32_t _c = ((cycles > 0) ? (uint32_t)cycles : 1)
                    + (uint32_t)dmc_stall + (uint32_t)oam_stall;
        s_ops_count       += _c;
        g_nes_cycles      += _c;   /* monotonic; never reset (co-sim ruler) */
        s_dbg_cycles_ticked += _c;
        s_dbg_instrs_ticked++;
    }
    /* Dot-accurate PPU (opt-in): paint visible scanlines incrementally as the
     * CPU sweeps the frame's cycle budget. No-op unless NESRECOMP_DOT_PPU set. */
    if (g_dot_ppu_on) ppu_dot_advance(s_ops_count);
    if (s_ops_count < s_frame_budget) return;
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
        g_frame_boundary_cyc = g_nes_cycles;   /* true frame-length ruler (pre-handler) */
        /* Immediate fire — safe at top level. Carry the overshoot (the
         * triggering instruction pushed s_ops_count a few cycles past the
         * threshold) into the next frame instead of discarding it — removes an
         * instruction-mix-dependent per-frame drift. Guaranteed >= s_frame_budget
         * here (we passed the check above), so the result is non-negative. */
        s_ops_count -= s_frame_budget;
        s_frame_budget = next_frame_budget();  /* dot-accurate next-frame target */
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
        /* Fire the frame-boundary callback only when NMI is enabled. The callback
         * emits the co-sim trace post-handler. When NMI is disabled the callback is
         * skipped, but this is still a video frame the oracle counts — emit the
         * trace here (cycle-indexed) so NMI-off frames stay represented. */
        if (g_ppuctrl & 0x80) {
            s_dbg_nmi_fires++;
            nes_vblank_callback();
        } else {
            nes_cosim_emit_boundary();
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
        if (s_ops_count > (s_frame_budget + s_frame_budget / 2)) {
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
    g_frame_boundary_cyc = g_nes_cycles;   /* true frame-length ruler (pre-handler) */
    /* Carry the overshoot like the immediate-fire path. This deferred path can
     * fire well past OPS_PER_FRAME (that is why the pending mechanism exists), so
     * a single subtraction leaves any residual budget for the next frame; the
     * carry is approximate here and is superseded by the Rung-2 dot-master clock,
     * which makes the frame boundary a true dot event rather than a threshold. */
    if (s_ops_count >= s_frame_budget) s_ops_count -= s_frame_budget;
    else                               s_ops_count = 0;
    s_frame_budget = next_frame_budget();  /* dot-accurate next-frame target */
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
    } else {
        nes_cosim_emit_boundary();   /* NMI-off (non-nested) deferred frame */
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

/* Open bus: the CPU data bus retains the last value driven onto it (by any read
 * or write). Reads of unmapped / write-only regions return this stale value,
 * NOT a constant 0/0xFF. NESdev "Open bus behavior" / "PPU registers". The
 * nes_read wrapper updates it after every read; nes_write updates it on writes. */
static uint8_t s_open_bus = 0;

/* PPU I/O latch: the PPU's internal data-bus latch, refreshed by any PPU
 * register write and by reads that drive the bus ($2004/$2007). $2002 reads
 * return the PPU status in bits 7-5 and this latch (decay) in bits 4-0 — the
 * only readable source for those bits. NESdev "PPU registers: ports / decay".
 * Modeled as a non-decaying latch (decay timing is sub-frame and unobservable
 * by game logic). */
static uint8_t s_ppu_io_latch = 0;

static uint8_t nes_read_inner(uint16_t addr) {
    bus_tick();
    if (addr <= 0x1FFF) return g_ram[addr & 0x07FF];
    if (addr >= 0x2000 && addr <= 0x3FFF) return ppu_read_reg(0x2000 + (addr & 7));
    if (addr >= 0x4000 && addr <= 0x401F) {
        if (addr == 0x4015) return apu_read_status();
        if (addr == 0x4016) {
            if (s_ctrl1_strobe) return 0x40 | (g_controller1_buttons >> 7);
            uint8_t bit = (s_ctrl1_shift & 0x80) ? 1 : 0;
            /* Shift MSB-first; fill with 1s so reads past the 8 button bits
             * return D0=1, as a standard NES controller does (the data line is
             * driven high after the 8th clock). */
            s_ctrl1_shift = (uint8_t)((s_ctrl1_shift << 1) | 1);
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
                /* NES Zapper $4017 bit 4 = trigger, ACTIVE-HIGH: 1 while the
                 * trigger is pulled, 0 at idle (matches Nestopia line 174 and
                 * the Mesen oracle).  The prior `!g_zapper_trigger` inversion
                 * reported a phantom press every idle frame, which made
                 * Gumshoe's title input-poll ($98D2 reads $4017 & $10, INC $C5
                 * on a press) clear the mode selector $24 and auto-advance the
                 * title screen with no input.  See GUMSHOE_MENU_AUTOADVANCE.md. */
                if (g_zapper_trigger) val |= 0x10;
                return val;
            }
            if (s_ctrl1_strobe) return 0x40 | (g_controller2_buttons >> 7);
            uint8_t bit = (s_ctrl2_shift & 0x80) ? 1 : 0;
            /* Fill with 1s past the 8 button bits (standard controller). */
            s_ctrl2_shift = (uint8_t)((s_ctrl2_shift << 1) | 1);
            return 0x40 | bit;
        }
        return s_open_bus;   /* write-only APU regs ($4000-$4013,$4015w) read open bus */
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
    return s_open_bus;   /* unmapped $4020-$5FFF: open bus */
}

/* Open-bus tracking wrapper: every CPU read drives its result onto the data
 * bus, so unmapped/write-only reads see the most recent value. */
uint8_t nes_read(uint16_t addr) {
    uint8_t v = nes_read_inner(addr);
    s_open_bus = v;
    return v;
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

/* ---- WRAM write-attribution tap (always-on, env-gated) ----------------------
 * NESRECOMP_WRITE_WATCH="0x25,0x26,..."  (comma-separated zeropage/RAM addrs,
 * up to 16) logs every write to a watched RAM address with the writer's emitted
 * function name (g_last_recomp_func, requires RECOMP_STACK_TRACKING) and the
 * live GxROM bank.  Output JSONL to NESRECOMP_WRITE_WATCH_FILE (else stderr).
 * Pays one branch (s_ww_state) per RAM write when disabled. */
static int      s_ww_state = -1;        /* -1 unqueried, 0 off, 1 on */
static uint16_t s_ww_addrs[16];
static int      s_ww_n = 0;
static FILE    *s_ww_file = NULL;
static void wram_write_watch(uint16_t a, uint8_t oldv, uint8_t val) {
    if (s_ww_state < 0) {
        const char *e = getenv("NESRECOMP_WRITE_WATCH");
        if (e && *e) {
            char buf[256]; strncpy(buf, e, sizeof buf - 1); buf[sizeof buf - 1] = 0;
            for (char *t = strtok(buf, ","); t && s_ww_n < 16; t = strtok(NULL, ",")) {
                unsigned x; if (sscanf(t, " 0x%x", &x) == 1 || sscanf(t, " %u", &x) == 1)
                    s_ww_addrs[s_ww_n++] = (uint16_t)(x & 0x07FF);
            }
            const char *p = getenv("NESRECOMP_WRITE_WATCH_FILE");
            s_ww_file = (p && *p) ? fopen(p, "w") : NULL;
        }
        s_ww_state = s_ww_n ? 1 : 0;
    }
    if (s_ww_state != 1) return;
    for (int i = 0; i < s_ww_n; i++) {
        if (s_ww_addrs[i] == a) {
            extern int g_current_bank;
            extern const char *g_last_recomp_func;
            FILE *o = s_ww_file ? s_ww_file : stderr;
            fprintf(o, "{\"f\":%llu,\"adr\":\"0x%04x\",\"old\":\"0x%02x\",\"val\":\"0x%02x\","
                       "\"bank\":%d,\"func\":\"%s\"}\n",
                    (unsigned long long)g_frame_count, a, oldv, val,
                    g_current_bank, g_last_recomp_func ? g_last_recomp_func : "?");
            fflush(o);
            break;
        }
    }
}

void nes_write(uint16_t addr, uint8_t val) {
    bus_tick();
    s_open_bus = val;   /* writes also drive the data bus (open-bus tracking) */

    /* DIAGNOSTIC (env-gated): flag when the 6502 stack pointer descends into the
     * low stack page (where some games keep a VRAM update buffer) — once per
     * frame per function, to catch a stack/buffer collision. NESRECOMP_SLOW_S=1. */
    {
        static int s_slow = -1;
        if (s_slow < 0) s_slow = getenv("NESRECOMP_SLOW_S") ? 1 : 0;
        if (s_slow) {
            /* Track the running minimum S; when a new low is reached, dump the
             * recomp call-stack depth + top frames. A very low S with a SHALLOW
             * recomp stack = a per-call stack leak; a deep stack = real nesting. */
            extern const char *g_recomp_stack[]; extern int g_recomp_stack_top;
            static uint8_t s_minS = 0xFF; static unsigned long long s_mf = ~0ULL;
            if (g_frame_count != s_mf) { s_mf = g_frame_count; s_minS = 0xFF; }
            if (g_cpu.S < s_minS && g_cpu.S < 0x40) {
                s_minS = g_cpu.S;
                fprintf(stderr, "[lowS] F=%llu S=%02X depth=%d top:",
                    (unsigned long long)g_frame_count, g_cpu.S, g_recomp_stack_top);
                for (int i = g_recomp_stack_top - 1, n = 0; i >= 0 && n < 8; i--, n++)
                    fprintf(stderr, " %s", g_recomp_stack[i] ? g_recomp_stack[i] : "?");
                fprintf(stderr, "\n");
            }
        }
    }

    if (addr <= 0x1FFF) {
        uint16_t a = addr & 0x07FF;
        if (g_ws_oam_sidecar && (a & 0x0700) == 0x0200 && (a & 3) == 3)
            ws_sidecar_track(a, val);
        if (s_ww_state != 0) wram_write_watch(a, g_ram[a], val);
        g_ram[a] = val; return;
    }
    if (addr >= 0x2000 && addr <= 0x3FFF) { ppu_write_reg(0x2000 + (addr & 7), val); return; }
    if (addr == 0x4014) {
        uint16_t src = (uint16_t)val << 8;
        for (int i = 0; i < 256; i++) g_ppu_oam[i] = nes_read(src + i);
        /* Cycle-steal: OAM DMA halts the CPU 513 cycles (+1 alignment cycle if
         * the transfer is requested on an odd CPU cycle) = 513/514. Charge it
         * so the frame budget and APU sequencer advance as on hardware; drained
         * next maybe_trigger_vblank. Parity from g_nes_cycles is approximate to
         * one instruction (exact intra-instruction placement is Rung 3), which
         * is far tighter than today's zero-cost model. */
        s_oam_dma_stall += 513 + (int)(g_nes_cycles & 1u);
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
    if (addr >= 0x4000 && addr <= 0x401F) {
        apu_trace_push(g_nes_cycles, addr, val);
        apu_write(addr, val);
        return;
    }
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
    /* Focused VRAM-write trace (env-gated, frame-windowed): logs $2000 (increment
     * mode) and $2006/$2007 writes with the current PPU address + increment stride,
     * for localizing screen-build addressing bugs. NESRECOMP_VRAM_TRACE=lo:hi. */
    {
        static int s_vt = -1; static unsigned long long s_lo=0, s_hi=0;
        if (s_vt < 0) {
            const char *e = getenv("NESRECOMP_VRAM_TRACE");
            if (e && e[0]) { s_vt = 1; s_lo = strtoull(e, NULL, 10);
                const char *c = strchr(e, ':'); s_hi = c ? strtoull(c+1, NULL, 10) : s_lo; }
            else s_vt = 0;
        }
        if (s_vt == 1 && g_frame_count >= s_lo && g_frame_count <= s_hi &&
            (reg == 0x2000 || reg == 0x2006)) {
            extern const char *g_last_recomp_func;
            fprintf(stderr, "[vram] F=%llu %s=%02X addr=%04X inc=%d  fn=%s\n",
                (unsigned long long)g_frame_count,
                reg==0x2000?"CTRL":"ADDR", val,
                g_ppuaddr, (g_ppuctrl & 0x04) ? 32 : 1,
                g_last_recomp_func ? g_last_recomp_func : "?");
            /* One-shot stack-page dump on the first $2006 write of the window,
             * to inspect the VRAM buffer func_F4C0 reads. */
            static unsigned long long s_dumped = 0;
            if (reg == 0x2006 && s_dumped != g_frame_count) {
                s_dumped = g_frame_count;
                fprintf(stderr, "[stack] F=%llu S=%02X page $0100-$01FF:\n",
                    (unsigned long long)g_frame_count, g_cpu.S);
                for (int r = 0; r < 256; r += 16) {
                    fprintf(stderr, "  %02X:", r);
                    for (int c = 0; c < 16; c++) fprintf(stderr, " %02X", g_ram[0x100 + r + c]);
                    fprintf(stderr, "\n");
                }
            }
        }
    }
    s_ppu_io_latch = val;   /* any PPU register write refreshes the I/O latch */
    /* Per-frame renderer (dot-PPU off): any PPU write between $2002 reads means
     * the read was a latch reset (e.g. LDA $2002; STA $2006), not a sprite-0
     * spin-wait poll. Reset the legacy pulse counter so only consecutive $2002
     * reads trigger the fallback hit. Zapper games suppress the reset during
     * nested NMIs (the outer detection loop's accumulator must survive PPU
     * register setup inside the handler). No-op when the dot-PPU owns sprite-0. */
    if (!g_dot_ppu_on && !(g_zapper_enabled && s_vblank_depth > 1))
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
            if (g_dot_ppu_on) {
                /* Dot-accurate PPU owns sprite-0 hit (bit6): the per-scanline
                 * renderer sets it when sprite 0 crosses an opaque BG pixel and
                 * clears it at the pre-render line. Bypass the legacy predictor/
                 * pulse heuristics so they cannot fight the hardware flag.
                 *
                 * Forward-progress safety net: if a sprite-0 spin-wait polls
                 * $2002 for far longer than a frame without the renderer ever
                 * producing a hit (a frame whose per-scanline state shows no
                 * opaque sprite-0/BG overlap our model detects), pulse bit6 so
                 * the game advances — same role as the per-frame fallback. It
                 * cannot fire during normal hits (those set bit6 within one
                 * frame, well under the threshold). */
                /* Threshold > one full frame of tight-spin reads (~3700) so a
                 * normal single-frame wait (renderer sets bit6 at the sprite-0
                 * scanline, resetting this) never reaches it; only a genuine
                 * no-hit frame accumulates past it. */
                static uint32_t s_dot_spr0_spin = 0;
                if (g_ppustatus & 0x40) {
                    s_dot_spr0_spin = 0;
                } else if (++s_dot_spr0_spin >= 6000) {
                    if (getenv("NESRECOMP_DOT_DEBUG")) {
                        fprintf(stderr, "[dot-spr0] stuck F=%llu mask=%02X ctrl=%02X "
                                "scroll=%d,%d OAM0=Y%d T%02X A%02X X%d -> pulse bit6\n",
                                (unsigned long long)g_frame_count, g_ppumask, g_ppuctrl,
                                g_ppuscroll_x, g_ppuscroll_y, g_ppu_oam[0], g_ppu_oam[1],
                                g_ppu_oam[2], g_ppu_oam[3]);
                    }
                    g_ppustatus |= 0x40;
                    s_dot_spr0_spin = 0;
                }
            } else if (!g_spr0_predict_disable && g_predicted_spr0_scanline < 240) {
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
            /* Bits 7-5 = PPU status (vblank/sprite0/overflow); bits 4-0 = the
             * PPU I/O latch (open-bus decay), the only readable source there. */
            uint8_t s = (uint8_t)((g_ppustatus & 0xE0) | (s_ppu_io_latch & 0x1F));
            g_ppustatus &= ~0x80;  /* clear VBlank flag on read (standard NES) */
            g_ppuaddr_latch = 0;   /* shared w toggle — clears for both $2005 and $2006 */
            return s;
        }
        case 0x2004: s_ppu_io_latch = g_ppu_oam[g_oamaddr]; return s_ppu_io_latch;
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
                s_ppu_io_latch = g_ppu_pal[idx];
                return s_ppu_io_latch;
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
            s_ppu_io_latch = ret;
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

/* Most-recently-recorded miss classification/context, stashed by
 * nes_record_dispatch_miss for nes_dispatch_miss_apply_policy. */
static char     s_last_miss_class[16] = "CODE";
static uint16_t s_last_miss_ctx       = 0;

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

/* ---- Dispatch tail trampoline ----
 * Dynamic JMP tails must not grow the C stack: a JMP loop chain dispatched
 * recursively adds a frame per lap and overflows (measured on SMB3 once all
 * mapper-4 absolute transfers went window-dynamic). Instead, a JMP-tail
 * dispatched while already inside a dispatch DEFERS its target and returns;
 * push_all_jsr keeps the 6502 stack canonical, so every generated JSR site's
 * bail check (callee returned without popping its return address → S
 * mismatch) unwinds the C stack to the outermost dispatch frame, which then
 * drives the deferred target from a flat loop. When the driven function
 * finally RTSes, the 6502 stack is exactly what the original callers expect,
 * so the unwound JSR sites that still run resume correctly. */
int g_nes_dispatch_depth = 0;
static int32_t s_tail_pending = -1;
static int     s_tail_caller  = -1;

int nes_dispatch_call(uint16_t addr, int caller_bank) {
    g_nes_dispatch_depth++;
    int r = call_by_address_cb(addr, caller_bank);
    g_nes_dispatch_depth--;
    if (g_nes_dispatch_depth == 0) {
        while (s_tail_pending >= 0) {
            uint16_t a = (uint16_t)s_tail_pending;
            int      c = s_tail_caller;
            s_tail_pending = -1;
            g_nes_dispatch_depth++;
            r = call_by_address_cb(a, c);
            g_nes_dispatch_depth--;
        }
    }
    return r;
}

/* JMP tails dispatch RECURSIVELY by default — that keeps the resume path
 * intact (the tail target's final RTS pops the caller's return address and
 * control unwinds through the live C frames exactly as the 6502 expects; C
 * depth stays bounded by real JSR depth). The one unbounded shape is a JMP
 * CYCLE: a loop lap across functions adds C frames with ZERO net 6502-stack
 * change. Detect that exact shape — same tail target with the same S already
 * active in this dispatch chain — and defer: unwinding a zero-net-stack lap
 * discards nothing (registers/RAM/S survive the unwind untouched), and the
 * outermost dispatch frame re-dispatches the lap flat. */
#define TAIL_ACTIVE_MAX 64
static struct { uint16_t addr; uint8_t s; } s_tail_active[TAIL_ACTIVE_MAX];
static int s_tail_active_n = 0;

int call_by_address_tail(uint16_t addr, int caller_bank) {
    for (int i = 0; i < s_tail_active_n; i++) {
        if (s_tail_active[i].addr == addr && s_tail_active[i].s == g_cpu.S) {
            s_tail_pending = addr;
            s_tail_caller  = caller_bank;
            return 1;   /* cycle lap: defer to the outermost dispatch frame */
        }
    }
    int slot = -1;
    if (s_tail_active_n < TAIL_ACTIVE_MAX) {
        slot = s_tail_active_n++;
        s_tail_active[slot].addr = addr;
        s_tail_active[slot].s    = g_cpu.S;
    }
    int r = nes_dispatch_call(addr, caller_bank);
    if (slot >= 0) s_tail_active_n = slot;
    return r;
}

/* Record a dispatch miss into the ring + dispatch_misses.log + counters, and
 * stash its classification/context for the policy step. Does NOT consult the
 * per-game override and does NOT apply the miss policy — callers compose those
 * (nes_log_dispatch_miss* below; nes_interp_dispatch* in interp.c).
 * Bank-aware form: addr is the gen-layout address (what [[extra_func]] needs),
 * cpu_addr the original 6502 target (live-window byte classification), bank
 * the window-resolved bank. */
void nes_record_dispatch_miss_bank(uint16_t addr, uint16_t cpu_addr, int bank) {
    static uint32_t last = 0xFFFFFFFF;
    uint32_t key = ((uint32_t)bank << 16) | addr;
    bool first_for_key = (key != last);

    /* Capture target bytes + classification up front so we can print it
     * inline with the first-sighting log line. Peek via the original 6502
     * address — the live window holds the actual target bytes; the rebased
     * gen-layout addr may point at a different window's contents. */
    uint8_t tbytes[8];
    for (int i = 0; i < 8; i++)
        tbytes[i] = mapper_peek_prg((uint16_t)(cpu_addr + i));
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

    /* Stash for nes_dispatch_miss_apply_policy (interp fallback decline path). */
    strncpy(s_last_miss_class, class_name, sizeof(s_last_miss_class) - 1);
    s_last_miss_class[sizeof(s_last_miss_class) - 1] = '\0';
    s_last_miss_ctx = call_site_pc;

    if (first_for_key) {
        printf("[Dispatch] MISS: no func for $%04X bank=%d (cpu=$%04X) target=%s "
               "A=%02X X=%02X Y=%02X call_site=$%04X\n",
               addr, bank, cpu_addr, class_name,
               g_cpu.A, g_cpu.X, g_cpu.Y, call_site_pc);
        last = key;
    }
    g_miss_count_any++;
    g_miss_last_addr  = addr;
    g_miss_last_frame = g_frame_count;
    g_miss_last_bank  = bank;
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
        r->bank         = bank;
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
            fprintf(mf, "extra_func %d 0x%04X  # cpu=$%04X target=%s A=%02X X=%02X Y=%02X call_site=$%04X\n",
                    bank, addr, cpu_addr, class_name,
                    g_cpu.A, g_cpu.X, g_cpu.Y, call_site_pc);
            fclose(mf);
        }
        printf("[Dispatch] NEW miss logged: extra_func %d 0x%04X (frame %llu) target=%s\n",
               bank, addr, (unsigned long long)g_frame_count, class_name);
        fflush(stdout);
    }

    g_dispatch_miss_count++;
}

/* Legacy recording entry: bank-unaware (g_current_bank attribution). */
void nes_record_dispatch_miss(uint16_t addr) {
    nes_record_dispatch_miss_bank(addr, addr, g_current_bank);
}

/* Apply the configured miss policy (LOG_RETURN / FATAL / TRAP) using the most
 * recently recorded miss's classification/context. Split from recording so the
 * interpreter fallback can record-and-interpret without applying FATAL/TRAP,
 * yet still apply the policy on the paths where it declines to interpret. */
void nes_dispatch_miss_apply_policy(uint16_t addr) {
    apply_dispatch_miss_policy("dispatch", addr, g_miss_last_bank,
                               s_last_miss_class, s_last_miss_ctx);
}

/* Combined entries: override + record + policy. The single-arg form is
 * retained for committed generated code and non-banked mappers. */
void nes_log_dispatch_miss(uint16_t addr) {
    if (game_dispatch_override(addr)) return;
    nes_record_dispatch_miss(addr);
    nes_dispatch_miss_apply_policy(addr);
}

void nes_log_dispatch_miss_bank(uint16_t addr, uint16_t cpu_addr, int bank) {
    if (game_dispatch_override(addr)) return;
    nes_record_dispatch_miss_bank(addr, cpu_addr, bank);
    nes_dispatch_miss_apply_policy(addr);
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
