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
#include <stdio.h>
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
#define DOT_MAXW        512          /* max framebuffer width (matches s_framebuf) */

static uint32_t *s_fb        = NULL;             /* presentation framebuffer */
static uint32_t  s_back[DOT_MAXW * VISIBLE_LINES]; /* render target (double buffer) */
static int       s_next_visible    = 0;         /* next visible scanline (0..240) */
static int       s_prerender_done  = 0;         /* pre-render A12 clock done this frame */
static int       s_busy            = 0;         /* reentry guard (IRQ handler re-enters) */

/* Vertical-scroll state, ported from ppu_render_frame's "Option A hybrid":
 *  - scroll_y < 240 (normal): the linear abs_nt_y model (PPU v-register Y
 *    auto-increment), so a mid-frame vertical split doesn't double-count.
 *  - scroll_y >= 240 (the "negative-Y" trick, e.g. Yoshi's title at $F8): the
 *    canonical coarse_y/fine_y/nt_row state machine honoring the NESdev wrap
 *    rule (coarse_y==29 toggles the vertical NT bit; ==31 wraps within the same
 *    NT). Re-derived on the first painted scanline or any Y-scroll-source change.*/
static int s_v_init    = 0;     /* 0 until the first painted scanline this frame */
static int s_use_canon = 0;     /* 1 when the canonical (scroll_y>=240) path is active */
static int s_v_coarse_y = 0, s_v_fine_y = 0, s_v_nt_row = 0;  /* canonical state */
static int s_abs_nt_y   = -1;   /* linear-path absolute nametable Y */
static int s_last_sy_y  = -1;
static int s_last_nt_yb = -1;

/* ---- palette resolve (mirror of ppu_renderer.c bg_color) ---- */
static inline uint32_t dot_bg_color(int pal_base, int color_idx) {
    if (color_idx == 0)
        return g_nes_palette[g_ppu_pal[0] & 0x3F];
    return g_nes_palette[g_ppu_pal[(pal_base * 4 + color_idx) & 0x1F] & 0x3F];
}

/* Per-scanline background opacity (indexed by framebuffer x), consumed by the
 * sprite pass of the SAME scanline for sprite-0 hit and behind-BG priority.
 * 1 = opaque BG pixel. Pillarbox margins stay 0 (transparent). */
static uint8_t s_bg_opaque[DOT_MAXW];

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
 * live PPU state. Fills s_bg_opaque (indexed by framebuffer x) for the sprite
 * pass. Handles widescreen: the vanilla screen occupies framebuffer columns
 * [g_widescreen_left, g_widescreen_left+256); the effective margins
 * (g_ws_eff_left/right, clamped) extend the view, and anything outside the
 * active span is pillarboxed black. Mirrors ppu_render_frame's geometry. */
static void dot_render_scanline(int sy) {
    const int W = g_render_width;
    uint32_t bg = g_nes_palette[g_ppu_pal[0] & 0x3F];
    uint32_t *row = s_back + (size_t)sy * W;

    /* Effective widescreen margins this scanline (-1 = follow configured). */
    int ws_l = g_widescreen_left, ws_r = g_widescreen_right;
    int ws_eff_l = g_ws_eff_left, ws_eff_r = g_ws_eff_right;
    if (ws_eff_l < 0 || ws_eff_l > ws_l) ws_eff_l = ws_l;
    if (ws_eff_r < 0 || ws_eff_r > ws_r) ws_eff_r = ws_r;
    int wide = (ws_l || ws_r);

    /* BG opacity for the whole row defaults transparent (pillarbox margins). */
    for (int x = 0; x < W; x++) s_bg_opaque[x] = 0;

    /* Pillarbox: black outside the active span, universal bg inside. (Vanilla
     * builds skip this — the BG loop writes every pixel of the 256px row.) */
    if (wide) {
        for (int x = 0; x < W; x++) row[x] = 0xFF000000u;
        int span_x0 = ws_l - ws_eff_l;
        int span_w  = 256 + ws_eff_l + ws_eff_r;
        for (int i = 0; i < span_w; i++) row[span_x0 + i] = bg;
    }

    /* Background ($2001 bit3). */
    if (!(g_ppumask & 0x08)) {
        if (!wide) for (int x = 0; x < 256; x++) row[x] = bg;
    } else {
        int chr_base  = (g_ppuctrl & 0x10) ? 0x1000 : 0x0000;
        int origin_x  = g_ppuscroll_x + ((g_ppuctrl & 0x01) ? 256 : 0);
        int cur_nt_yb = (g_ppuctrl & 0x02) ? 1 : 0;
        int mirroring = mapper_get_mirroring();

        /* Vertical scroll: re-derive on the first painted scanline of the frame
         * or any Y-scroll-source change (mid-frame split). scroll_y >= 240 selects
         * the canonical negative-Y state machine (e.g. Yoshi's title at $F8); else
         * the linear abs_nt_y model. */
        if (!s_v_init || g_ppuscroll_y != s_last_sy_y || cur_nt_yb != s_last_nt_yb) {
            if (g_ppuscroll_y >= 240) {
                s_use_canon  = 1;
                s_v_coarse_y = (g_ppuscroll_y >> 3) & 0x1F;
                s_v_fine_y   = g_ppuscroll_y & 0x07;
                s_v_nt_row   = cur_nt_yb;
            } else {
                s_use_canon = 0;
                s_abs_nt_y  = g_ppuscroll_y + (cur_nt_yb ? 240 : 0);
            }
            s_v_init = 1;
        }

        int nt_row, local_ty, tile_row;
        if (s_use_canon) {
            nt_row = s_v_nt_row; local_ty = s_v_coarse_y; tile_row = s_v_fine_y;
        } else {
            int nt_y = s_abs_nt_y;
            if (nt_y >= 480) nt_y -= 480;
            int tile_y = nt_y / 8;
            tile_row   = nt_y % 8;
            nt_row     = (tile_y >= 30) ? 1 : 0;
            local_ty   = tile_y % 30;
        }

        for (int sx = -ws_eff_l; sx < 256 + ws_eff_r; sx++) {
            int fb_x     = sx + ws_l;             /* column in the (wide) framebuffer */
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

            /* PPUMASK bit1: clip leftmost 8 BG pixels (vanilla columns 0..7). */
            if (sx >= 0 && sx < 8 && !(g_ppumask & 0x02)) {
                row[fb_x] = bg;
                s_bg_opaque[fb_x] = 0;
            } else {
                row[fb_x] = dot_bg_color(pal_base, ci);
                s_bg_opaque[fb_x] = (ci != 0);
            }
        }

        /* Advance vertical state for the next scanline. */
        if (s_use_canon) {
            if (++s_v_fine_y == 8) {
                s_v_fine_y = 0;
                if (s_v_coarse_y == 29)      { s_v_coarse_y = 0; s_v_nt_row ^= 1; } /* NT toggle */
                else if (s_v_coarse_y == 31) { s_v_coarse_y = 0; }                  /* same-NT wrap */
                else                         { s_v_coarse_y = (s_v_coarse_y + 1) & 0x1F; }
            }
        } else {
            s_abs_nt_y++;
        }
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
        /* Widescreen: render from the unwrapped 16-bit X sidecar (it equals the
         * OAM byte on the vanilla screen); vanilla uses the 8-bit OAM X. */
        int     spr_x    = g_ws_oam_sidecar ? (int)g_oam_x16[s]
                                            : (int)g_ppu_oam[s * 4 + 3];
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
            /* Clip to the effective viewport (vanilla [0,256) when margins 0). */
            if (px < -ws_eff_l || px >= 256 + ws_eff_r) continue;
            int ci = ((lo >> chr_bit) & 1) | (((hi >> chr_bit) & 1) << 1);
            if (ci == 0) continue;                       /* transparent */
            if (px < 8 && !(g_ppumask & 0x04)) continue; /* leftmost-8 sprite clip */

            int fb_x = px + ws_l;
            /* Sprite-0 hit: opaque sprite-0 pixel over opaque BG, x != 255,
             * both BG+sprites enabled, BG not clipped at this x. */
            if (s == 0 && px >= 0 && px < 255 && (g_ppumask & 0x18) == 0x18) {
                int bg_clipped = (px < 8 && !(g_ppumask & 0x02));
                if (!bg_clipped && s_bg_opaque[fb_x])
                    g_ppustatus |= 0x40;
            }
            /* Behind-BG priority: only draw over transparent BG. */
            if (priority && s_bg_opaque[fb_x]) continue;

            uint8_t nc = g_ppu_pal[(spr_pal * 4 + ci) & 0x1F] & 0x3F;
            row[fb_x] = g_nes_palette[nc];
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
    /* The dot-accurate per-scanline PPU is the SOLE renderer — the per-frame
     * compositor and its sprite-0 split heuristics have been removed. This is
     * the single, hardware-faithful path: true per-scanline sprite-0, live
     * scroll, dot-clock MMC3 IRQ, widescreen. */
    g_dot_ppu_on = 1;
    printf("[ppu_dot] dot-accurate per-scanline PPU (sole renderer)\n");
}

void ppu_dot_advance(uint32_t ops) {
    if (!g_dot_ppu_on || s_busy) return;
    if (g_render_width > DOT_MAXW) return;         /* beyond buffer (shouldn't happen) */
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
    if (!g_dot_ppu_on || g_render_width > DOT_MAXW) return;

    if (!s_busy) {
        s_busy = 1;
        /* Finish any scanlines the budget did not reach, using state that still
         * reflects the just-completed frame (the NMI below has not run yet).
         * Common case: already at 240 and this paints nothing. */
        if (!s_prerender_done) { dot_clock_mmc3(); s_prerender_done = 1; }
        while (s_next_visible < VISIBLE_LINES) dot_step_scanline();
        s_busy = 0;
        /* Publish the completed frame for presentation (only the active width). */
        memcpy(s_fb, s_back, (size_t)g_render_width * VISIBLE_LINES * sizeof(uint32_t));
    }

    /* Arm the next frame's visible region. */
    s_next_visible   = 0;
    s_prerender_done = 0;
    s_v_init         = 0;
    s_abs_nt_y       = -1;
    s_last_sy_y      = -1;
    s_last_nt_yb     = -1;
}
