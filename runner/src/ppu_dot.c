/*
 * ppu_dot.c — cycle-driven, per-scanline PPU renderer (Phase 3, EXPERIMENTAL).
 *
 * See ppu_dot.h and ACCURACY_PHASE_PLAN.md §Phase 3 for the model. Engaged only
 * when NESRECOMP_DOT_PPU is set in the environment AND the framebuffer is the
 * vanilla 256px width (widescreen falls back to the per-frame renderer). When
 * disabled, every entry point returns immediately and the build behaves exactly
 * as before.
 *
 * Increment 1: per-scanline granularity. The dot clock is slaved to the frame
 * driver's per-frame cycle accumulator (s_ops_count) with cycle 0 == VBlank
 * start (scanline 241); visible scanline 0 begins VBLANK_PRE scanlines later.
 * The clock free-runs through the NMI handler and the main loop, painting each
 * visible scanline from LIVE PPU state, so mid-frame writes (scroll splits,
 * MMC3 CHR/bank swaps) land on the scanlines they actually affect. Sub-scanline
 * (per-dot) precision for the remaining sprite-0 / A12 edge cases is a later
 * increment.
 */
#include "nes_runtime.h"
#include "ppu_dot.h"
#include "mapper.h"
#include <stdlib.h>
#include <string.h>

int g_dot_ppu_on = 0;

/* NES system palette (ARGB8888) and the render-IRQ suppression toggle both live
 * in ppu_renderer.c. The PPU v register (g_ppuaddr) lives in runtime.c. */
extern const uint32_t g_nes_palette[64];
extern int            g_disable_render_irq;
extern uint16_t       g_ppuaddr;

/* PPU geometry. 341 dots/scanline, 3 dots/CPU cycle. Cycle 0 of the frame
 * budget is VBlank start (scanline 241). The visible region begins after 20
 * VBlank lines (241..260) + the pre-render line (261) = 21 scanlines. */
#define DOTS_PER_LINE   341
#define VISIBLE_LINES   240
#define VBLANK_PRE      21
#define DOT_W           256          /* dot path only runs at the vanilla width */

static uint32_t *s_fb        = NULL;            /* presentation framebuffer */
static uint32_t  s_back[DOT_W * VISIBLE_LINES]; /* render target (double buffer) */
static int       s_next_visible    = 0;         /* next visible scanline (0..240) */
static int       s_prerender_done  = 0;         /* pre-render A12 clock done this frame */
static int       s_busy            = 0;         /* reentry guard (IRQ handler re-enters) */

/* Incremental vertical-scroll state, ported from ppu_render_frame's default
 * (non-canonical) path: abs_nt_y models the PPU v-register's per-scanline Y
 * auto-increment so a mid-frame vertical scroll split does not double-count. */
static int s_abs_nt_y   = -1;
static int s_last_sy_y  = -1;
static int s_last_nt_yb = -1;

/* ---- palette resolve (mirror of ppu_renderer.c bg_color) ---- */
static inline uint32_t dot_bg_color(int pal_base, int color_idx) {
    if (color_idx == 0)
        return g_nes_palette[g_ppu_pal[0] & 0x3F];
    return g_nes_palette[g_ppu_pal[(pal_base * 4 + color_idx) & 0x1F] & 0x3F];
}

/* Per-scanline background opacity, consumed by the sprite pass of the SAME
 * scanline for sprite-0 hit and behind-BG priority. 1 = opaque BG pixel. */
static uint8_t s_bg_opaque[DOT_W];

/* Fire the MMC3 scanline IRQ via the NMI push convention (PCH/PCL placeholders
 * + P), matching service_mmc3_scanline_irq / maybe_deliver_irq. The handler may
 * swap CHR banks / scroll mid-frame; if it wrote $2006 (changed v) we resync the
 * render scroll from v. Guarded by s_busy against re-entry. */
static void dot_fire_mmc3_irq(void) {
    uint8_t p_irq = (uint8_t)((g_cpu.N<<7)|(g_cpu.V<<6)|(1<<5)|
                               (g_cpu.D<<3)|(g_cpu.I<<2)|(g_cpu.Z<<1)|g_cpu.C);
    g_ram[0x100 + g_cpu.S] = 0x00;  g_cpu.S--;   /* PCH placeholder */
    g_ram[0x100 + g_cpu.S] = 0x00;  g_cpu.S--;   /* PCL placeholder */
    g_ram[0x100 + g_cpu.S] = p_irq; g_cpu.S--;   /* P */
    uint16_t v_before = g_ppuaddr;
    func_IRQ();
    if (g_ppuaddr != v_before)
        runtime_sync_scroll_from_v();
}

/* Clock the MMC3 A12 edge for one scanline and fire the IRQ if it triggered. */
static void dot_clock_mmc3(void) {
    if (mapper_clock_scanline() && !g_disable_render_irq)
        dot_fire_mmc3_irq();
}

/* Paint one visible scanline (0..239) into the back buffer from the current
 * live PPU state. Fills s_bg_opaque for the sprite pass. */
static void dot_render_scanline(int sy) {
    uint32_t bg = g_nes_palette[g_ppu_pal[0] & 0x3F];
    uint32_t *row = s_back + (size_t)sy * DOT_W;

    /* Background ($2001 bit3). Disabled => universal color, no opaque pixels. */
    if (!(g_ppumask & 0x08)) {
        for (int x = 0; x < DOT_W; x++) { row[x] = bg; s_bg_opaque[x] = 0; }
    } else {
        int chr_base  = (g_ppuctrl & 0x10) ? 0x1000 : 0x0000;
        int origin_x  = g_ppuscroll_x + ((g_ppuctrl & 0x01) ? 256 : 0);
        int cur_nt_yb = (g_ppuctrl & 0x02) ? 1 : 0;
        int mirroring = mapper_get_mirroring();

        /* Vertical: reset abs_nt_y on the first painted scanline of the frame
         * or whenever the Y scroll source changed mid-frame (split); otherwise
         * it was auto-incremented after the previous scanline. */
        if (s_abs_nt_y < 0 || g_ppuscroll_y != s_last_sy_y || cur_nt_yb != s_last_nt_yb)
            s_abs_nt_y = g_ppuscroll_y + (cur_nt_yb ? 240 : 0);

        int nt_y = s_abs_nt_y;
        if (nt_y >= 480) nt_y -= 480;
        int tile_y   = nt_y / 8;
        int tile_row = nt_y % 8;
        int nt_row   = (tile_y >= 30) ? 1 : 0;
        int local_ty = tile_y % 30;

        for (int sx = 0; sx < DOT_W; sx++) {
            int nt_x     = (origin_x + sx) & 0x1FF;
            int tile_x   = nt_x / 8;
            int px_col   = nt_x % 8;
            int nt_col   = (tile_x >= 32) ? 1 : 0;
            int local_tx = tile_x % 32;

            int virt_nt = nt_row * 2 + nt_col;
            int phys_nt;
            switch (mirroring) {
                case 0:  phys_nt = 0;            break;
                case 1:  phys_nt = 1;            break;
                case 2:  phys_nt = virt_nt & 1;  break;
                case 3:  phys_nt = virt_nt >> 1; break;
                default: phys_nt = virt_nt & 1;  break;
            }
            int nt_off = phys_nt * 0x400;

            uint8_t tile_id = g_ppu_nt[(nt_off + local_ty * 32 + local_tx) & 0x0FFF];
            uint8_t attr    = g_ppu_nt[(nt_off + 0x3C0 + (local_ty / 4) * 8 + (local_tx / 4)) & 0x0FFF];
            int sub_x = (local_tx / 2) & 1, sub_y = (local_ty / 2) & 1;
            int pal_base = (attr >> ((sub_y * 2 + sub_x) * 2)) & 0x03;

            int bit = 7 - px_col;
            int chr_off = chr_base + tile_id * 16 + tile_row;
            int ci = ((g_chr_ram[chr_off] >> bit) & 1) |
                     (((g_chr_ram[chr_off + 8] >> bit) & 1) << 1);

            /* PPUMASK bit1: clip leftmost 8 BG pixels (treated as transparent). */
            if (sx < 8 && !(g_ppumask & 0x02)) {
                row[sx] = bg;
                s_bg_opaque[sx] = 0;
            } else {
                row[sx] = dot_bg_color(pal_base, ci);
                s_bg_opaque[sx] = (ci != 0);
            }
        }

        s_abs_nt_y++;
        s_last_sy_y  = g_ppuscroll_y;
        s_last_nt_yb = cur_nt_yb;
    }

    /* Sprites ($2001 bit4). Back-to-front so sprite 0 lands on top; sprite-0
     * hit + overflow are evaluated against this scanline's BG opacity. */
    if (!(g_ppumask & 0x10)) return;

    int spr_tall     = (g_ppuctrl & 0x20) != 0;
    int spr_height   = spr_tall ? 16 : 8;
    int spr_chr_base = (g_ppuctrl & 0x08) ? 0x1000 : 0x0000;

    /* Sprite overflow (bit5): set when more than 8 sprites cover this line. */
    int on_line = 0;
    for (int s = 0; s < 64; s++) {
        int sy0 = g_ppu_oam[s * 4 + 0];
        if (sy0 >= 0xEF) continue;
        int top = sy0 + 1;
        if (sy >= top && sy < top + spr_height) { if (++on_line > 8) { g_ppustatus |= 0x20; break; } }
    }

    for (int s = 63; s >= 0; s--) {
        uint8_t spr_y = g_ppu_oam[s * 4 + 0];
        if (spr_y >= 0xEF) continue;
        int top = (int)spr_y + 1;            /* OAM Y is one less than display Y */
        if (sy < top || sy >= top + spr_height) continue;

        uint8_t spr_tile = g_ppu_oam[s * 4 + 1];
        uint8_t spr_attr = g_ppu_oam[s * 4 + 2];
        int     spr_x    = g_ppu_oam[s * 4 + 3];
        int flip_h   = (spr_attr >> 6) & 1;
        int flip_v   = (spr_attr >> 7) & 1;
        int priority = (spr_attr >> 5) & 1;  /* 1 = behind BG */
        int spr_pal  = (spr_attr & 0x03) + 4;

        int row_in   = sy - top;             /* 0..spr_height-1 */
        int draw_row = flip_v ? (spr_height - 1 - row_in) : row_in;

        int tile_base, tile_chr_base;
        if (spr_tall) {
            tile_chr_base = (spr_tile & 1) ? 0x1000 : 0x0000;
            tile_base = spr_tile & 0xFE;
        } else {
            tile_chr_base = spr_chr_base;
            tile_base = spr_tile;
        }
        int tile_row = draw_row, tile_num = tile_base;
        if (spr_tall && tile_row >= 8) { tile_num = tile_base + 1; tile_row -= 8; }

        int chr_off = tile_chr_base + tile_num * 16 + tile_row;
        uint8_t lo = g_chr_ram[chr_off];
        uint8_t hi = g_chr_ram[chr_off + 8];

        for (int bit = 7; bit >= 0; bit--) {
            int chr_bit = flip_h ? (7 - bit) : bit;
            int px = spr_x + (7 - bit);
            if (px < 0 || px >= DOT_W) continue;
            int ci = ((lo >> chr_bit) & 1) | (((hi >> chr_bit) & 1) << 1);
            if (ci == 0) continue;                       /* transparent */
            if (px < 8 && !(g_ppumask & 0x04)) continue; /* leftmost-8 sprite clip */

            /* Sprite-0 hit: opaque sprite-0 pixel over opaque BG, x != 255,
             * both BG+sprites enabled, BG not clipped at this x. */
            if (s == 0 && px < 255 && (g_ppumask & 0x18) == 0x18) {
                int bg_clipped = (px < 8 && !(g_ppumask & 0x02));
                if (!bg_clipped && s_bg_opaque[px])
                    g_ppustatus |= 0x40;
            }
            /* Behind-BG priority: only draw over transparent BG. */
            if (priority && s_bg_opaque[px]) continue;

            uint8_t nc = g_ppu_pal[(spr_pal * 4 + ci) & 0x1F] & 0x3F;
            row[px] = g_nes_palette[nc];
        }
    }
}

/* Paint one visible scanline and clock its MMC3 A12 edge (the edge precedes the
 * line's fetches on hardware, so the IRQ handler's writes apply to later lines). */
static void dot_step_scanline(void) {
    dot_clock_mmc3();
    dot_render_scanline(s_next_visible);
    s_next_visible++;
}

/* ---- public API ---- */

void ppu_dot_init(uint32_t *framebuf) {
    s_fb = framebuf;
    /* The dot-accurate per-scanline PPU is the DEFAULT renderer for all games —
     * it is the single, hardware-faithful path (true per-scanline sprite-0,
     * live scroll, dot-clock MMC3 IRQ), replacing the per-frame compositor and
     * its sprite-0 split heuristics. NESRECOMP_DOT_PPU=0 forces the legacy
     * per-frame renderer (kept for A/B and as the width!=256 widescreen fallback
     * until the dot path gains widescreen support; see ppu_dot_advance). */
    const char *e = getenv("NESRECOMP_DOT_PPU");
    g_dot_ppu_on = (e && *e) ? (*e != '0') : 1;
    printf("[ppu_dot] dot-accurate per-scanline PPU: %s\n",
           g_dot_ppu_on ? "ON (default)" : "off (per-frame renderer)");
}

void ppu_dot_advance(uint32_t ops) {
    if (!g_dot_ppu_on || s_busy) return;
    if (g_render_width != DOT_W) return;          /* widescreen -> per-frame path */
    if (s_next_visible >= VISIBLE_LINES) return;

    /* Scanline offset from VBlank start (cycle 0). Visible scanline index = the
     * offset minus the VBlank + pre-render lines. */
    int sl  = (int)((ops * 3u) / DOTS_PER_LINE);
    int vis = sl - VBLANK_PRE;
    if (vis < 0) return;                          /* still in VBlank / pre-render */

    s_busy = 1;
    if (!s_prerender_done) {
        /* Pre-render line: clear sprite-0 hit + overflow (hardware clears these
         * at pre-render dot 1, NOT at VBlank start) and clock the MMC3 A12 edge.
         * Clearing here lets a game's NMI wait out the previous frame's hit. */
        g_ppustatus &= ~0x60;
        dot_clock_mmc3();
        s_prerender_done = 1;
    }
    int upto = vis + 1;
    if (upto > VISIBLE_LINES) upto = VISIBLE_LINES;
    while (s_next_visible < upto) dot_step_scanline();
    s_busy = 0;
}

void ppu_dot_frame_boundary(void) {
    if (!g_dot_ppu_on || g_render_width != DOT_W) return;

    if (!s_busy) {
        s_busy = 1;
        /* Finish any scanlines the budget did not reach, using state that still
         * reflects the just-completed frame (the NMI below has not run yet).
         * Common case: already at 240 and this paints nothing. */
        if (!s_prerender_done) { dot_clock_mmc3(); s_prerender_done = 1; }
        while (s_next_visible < VISIBLE_LINES) dot_step_scanline();
        s_busy = 0;
        /* Publish the completed frame for presentation. */
        memcpy(s_fb, s_back, sizeof(s_back));
    }

    /* Arm the next frame's visible region. */
    s_next_visible   = 0;
    s_prerender_done = 0;
    s_abs_nt_y       = -1;
    s_last_sy_y      = -1;
    s_last_nt_yb     = -1;
}
