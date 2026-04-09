/*
 * ppu_renderer.c — NES PPU background + sprite rendering
 *
 * Phase 1: BG nametable → CHR tiles → palette → ARGB8888 framebuffer
 * Phase 2: OAM sprites
 */
#include "nes_runtime.h"
#include "mapper.h"
#include <string.h>
#include <stdio.h>

/* PNG save wrapper — implemented in main_runner.c */
extern void save_png(const char *path, int w, int h, const void *rgb, int stride);

/* Debug toggle — when set, suppress MMC3 IRQ firing during rendering. */
int g_disable_render_irq = 0;

/* NES system palette — 64 colors as ARGB8888 */
const uint32_t g_nes_palette[64] = {
    0xFF545454,0xFF001E74,0xFF081090,0xFF300088,0xFF440064,0xFF5C0030,0xFF540400,0xFF3C1800,
    0xFF202A00,0xFF083A00,0xFF004000,0xFF003C00,0xFF00323C,0xFF000000,0xFF000000,0xFF000000,
    0xFF989698,0xFF084CC4,0xFF3032EC,0xFF5C1EE4,0xFF8814B0,0xFFA01464,0xFF982220,0xFF783C00,
    0xFF545A00,0xFF287200,0xFF087C00,0xFF007628,0xFF006678,0xFF000000,0xFF000000,0xFF000000,
    0xFFECEEEC,0xFF4C9AEC,0xFF787CEC,0xFFB062EC,0xFFE454EC,0xFFEC58B4,0xFFEC6A64,0xFFD48820,
    0xFFA0AA00,0xFF74C400,0xFF4CD020,0xFF38CC6C,0xFF38B4CC,0xFF3C3C3C,0xFF000000,0xFF000000,
    0xFFECEEEC,0xFFA8CCEC,0xFFBCBCEC,0xFFD4B2EC,0xFFECAEEC,0xFFECAED4,0xFFECB4B0,0xFFE4C490,
    0xFFCCD278,0xFFB4DE78,0xFFA8E290,0xFF98E2B4,0xFFA0D6E4,0xFFA0A2A0,0xFF000000,0xFF000000,
};

/* Resolve a NES palette index (0-3 per tile) + attribute palette (0-3) to ARGB.
 * pal_base: which 4-color sub-palette (0-3), from attribute table.
 * color_idx: 0=transparent/BG, 1-3=foreground colors. */
static uint32_t bg_color(int pal_base, int color_idx) {
    if (color_idx == 0) {
        /* Universal background color */
        return g_nes_palette[g_ppu_pal[0] & 0x3F];
    }
    uint8_t nes_color = g_ppu_pal[(pal_base * 4 + color_idx) & 0x1F] & 0x3F;
    return g_nes_palette[nes_color];
}

/* Render one 8x8 tile row into framebuf row.
 * tile_id: nametable tile byte.
 * pal_base: attribute table palette (0-3).
 * chr_base: CHR pattern table base ($0000 or $1000) from PPUCTRL bit 4.
 * tile_row: which row of the tile (0-7).
 * px_x: starting X pixel in framebuf (0-255).
 * px_y: Y pixel row in framebuf. */
static void render_tile_row(uint32_t *framebuf,
                            int tile_id, int pal_base, int chr_base,
                            int tile_row, int px_x, int px_y) {
    if (px_y < 0 || px_y >= 240) return;
    int chr_offset = chr_base + tile_id * 16 + tile_row;
    uint8_t lo = g_chr_ram[chr_offset];
    uint8_t hi = g_chr_ram[chr_offset + 8];
    for (int bit = 7; bit >= 0; bit--) {
        int x = px_x + (7 - bit);
        if (x < 0 || x >= g_render_width) continue;
        int color_idx = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
        framebuf[px_y * g_render_width + x] = bg_color(pal_base, color_idx);
    }
}

/* ---- OAM Debug View ----
 * Renders all 64 OAM slots as an 8x8 grid into a 256x256 ARGB buffer.
 * Each cell is 32x32 (8px tile at 4x scale).
 * Border colors:
 *   dark gray   = slot off-screen (Y >= $EF)
 *   white/cyan/green/magenta = palette 0-3, sprite in front of BG
 *   yellow      = sprite behind BG (priority bit set)
 * Transparent pixels rendered as dark magenta (#200020).
 */
void ppu_render_oam_debug(uint32_t *buf) {
    /* Dark background */
    for (int i = 0; i < 256 * 256; i++) buf[i] = 0xFF101010;

    /* Grid lines */
    for (int gy = 0; gy < 256; gy++)
        for (int gx = 0; gx < 256; gx++)
            if (gx % 32 == 0 || gy % 32 == 0)
                buf[gy * 256 + gx] = 0xFF282828;

    int spr_base = (g_ppuctrl & 0x08) ? 0x1000 : 0x0000;

    /* Border palette: slot on-screen, by palette 0-3 */
    static const uint32_t pal_border[4] = {
        0xFFFFFFFF, /* pal 0: white */
        0xFF00FFFF, /* pal 1: cyan */
        0xFF00FF00, /* pal 2: green */
        0xFFFF00FF, /* pal 3: magenta */
    };

    for (int slot = 0; slot < 64; slot++) {
        uint8_t sy   = g_ppu_oam[slot * 4 + 0];
        uint8_t stile= g_ppu_oam[slot * 4 + 1];
        uint8_t sattr= g_ppu_oam[slot * 4 + 2];
        /* sx unused for rendering but kept for symmetry */

        int col = slot % 8;
        int row = slot / 8;
        int ox  = col * 32;
        int oy  = row * 32;

        int off_screen = (sy >= 0xEF);
        int pal        = (sattr & 0x03);
        int flip_h     = (sattr >> 6) & 1;
        int flip_v     = (sattr >> 7) & 1;
        int behind_bg  = (sattr >> 5) & 1;

        uint32_t border = off_screen ? 0xFF303030
                        : behind_bg  ? 0xFFFFFF00
                        : pal_border[pal];

        /* Draw 1px border around 32x32 cell */
        for (int bx = ox; bx < ox + 32; bx++) {
            buf[oy * 256 + bx]        = border;
            buf[(oy + 31) * 256 + bx] = border;
        }
        for (int by = oy; by < oy + 32; by++) {
            buf[by * 256 + ox]        = border;
            buf[by * 256 + (ox + 31)] = border;
        }

        /* Render tile pixels at 4x scale (1px border inset → 30x30 usable, but we
         * actually use 32x32 and let border overdraw the outermost pixel row) */
        int spr_pal = pal + 4; /* sprite palettes start at sub-palette 4 in g_ppu_pal */
        int chr_off = spr_base + stile * 16;

        for (int tr = 0; tr < 8; tr++) {
            int src_row = flip_v ? (7 - tr) : tr;
            uint8_t lo = g_chr_ram[chr_off + src_row];
            uint8_t hi = g_chr_ram[chr_off + src_row + 8];

            for (int b = 7; b >= 0; b--) {
                int src_bit = flip_h ? (7 - b) : b;
                int ci = ((lo >> src_bit) & 1) | (((hi >> src_bit) & 1) << 1);
                uint32_t color;
                if (ci == 0) {
                    color = off_screen ? 0xFF181818 : 0xFF200020; /* transparent: dark magenta */
                } else {
                    uint8_t nc = g_ppu_pal[(spr_pal * 4 + ci) & 0x1F] & 0x3F;
                    color = g_nes_palette[nc];
                    if (off_screen) {
                        /* Dim off-screen sprites to 25% brightness */
                        uint8_t r = ((color >> 16) & 0xFF) >> 2;
                        uint8_t g = ((color >>  8) & 0xFF) >> 2;
                        uint8_t bv= ( color        & 0xFF) >> 2;
                        color = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | bv;
                    }
                }
                int px0 = ox + (7 - b) * 4;
                int py0 = oy + tr * 4;
                for (int dy = 0; dy < 4; dy++)
                    for (int dx = 0; dx < 4; dx++)
                        buf[(py0 + dy) * 256 + (px0 + dx)] = color;
            }
        }

        /* Redraw border on top so it isn't overwritten by tile pixels */
        for (int bx = ox; bx < ox + 32; bx++) {
            buf[oy * 256 + bx]        = border;
            buf[(oy + 31) * 256 + bx] = border;
        }
        for (int by = oy; by < oy + 32; by++) {
            buf[by * 256 + ox]        = border;
            buf[by * 256 + (ox + 31)] = border;
        }
    }
}

/* Diagnostic counters for the title-screen first-divergence investigation.
 * Disabled by default — re-enable by setting RECOMP_RENDER_DIAG to 1. */
#define RECOMP_RENDER_DIAG 0
#if RECOMP_RENDER_DIAG
uint32_t g_ppu_render_calls   = 0;
uint32_t g_ppu_render_skipped = 0;
uint8_t  g_ppu_render_last_mask = 0;
uint8_t  g_ppu_render_last_ctrl = 0;
uint8_t  g_ppu_render_last_pal0 = 0;
uint8_t  g_ppu_render_last_chr00 = 0;  /* g_chr_ram[0x1000] (BG ptn $00 row 0) */
#endif

void ppu_render_frame(uint32_t *framebuf) {
#if RECOMP_RENDER_DIAG
    g_ppu_render_calls++;
    g_ppu_render_last_mask  = g_ppumask;
    g_ppu_render_last_ctrl  = g_ppuctrl;
    g_ppu_render_last_pal0  = g_ppu_pal[0];
    g_ppu_render_last_chr00 = g_chr_ram[0x1000];
#endif

    /* When rendering is fully disabled (BG + sprites off), keep the previous
     * frame's content rather than blanking.  On real NES the CRT continues
     * displaying the last rendered scanlines, so brief rendering-off windows
     * during nametable loads (ppumask=$06) are invisible.  Without this,
     * those frames flash the universal background color — visible as HUD
     * flicker on LCD displays. */
    if (!(g_ppumask & 0x18)) {
#if RECOMP_RENDER_DIAG
        g_ppu_render_skipped++;
#endif
        return;
    }

    /* Universal background color */
    uint32_t bg = g_nes_palette[g_ppu_pal[0] & 0x3F];
    for (int i = 0; i < g_render_width * 240; i++) framebuf[i] = bg;

    /* Only render if BG rendering is enabled ($2001 bit 3) */
    if (!(g_ppumask & 0x08)) goto render_sprites;

    {
        /* Frame-start sync of v from t — see 81b8a47 commit message for rationale */
        runtime_set_ppuaddr(runtime_get_ppu_t() & 0x3FFF);
        if (runtime_scroll_from_t_valid()) {
            runtime_sync_scroll_from_t();
        }
        {
            extern void runtime_record_frame_start_scroll(void);
            runtime_record_frame_start_scroll();
        }

        /* Split-screen: rows 0..split_y-1 use HUD scroll (captured at sprite-0 hit);
         * rows split_y..239 use the post-split game-area scroll.
         * When no split occurred this frame, all rows use current scroll. */
        /* On real NES, sprite-0 hit always fires when the sprite overlaps
         * a non-transparent BG pixel — it's a hardware signal, not software.
         * Our counter-based $2002 sim can miss, so fall back: if sprite 0 is
         * on-screen and rendering is enabled, assume the split happens.
         * HUD scroll values are pre-captured as (0,0) at VBlank start.
         *
         * split_y is tile-aligned: round the sprite's last scanline (Y+8) up
         * to the next 8-pixel boundary.  This ensures the HUD/gameplay
         * boundary lands on a tile row edge, preventing seam artifacts.
         *   SMB:      OAM[0].Y=$18(24) → (24+15)&~7 = 32  (4 tile rows)
         *   Faxanadu: OAM[0].Y=$17(23) → (23+15)&~7 = 32  (4 tile rows)
         *
         * Hysteresis: if OAM[0] was recently on-screen with a valid split,
         * maintain the split for a few frames even if OAM[0] is temporarily
         * hidden (e.g. during DMA timing or mode transition).  This prevents
         * single-frame HUD disappearances. */
        static int s_last_valid_split_y = 240;
        static int s_split_holdoff      = 0;

        int spr0_y  = (int)(g_ppu_oam[0]);
        /* Sprite-0 scroll split: activate only when the hardware sprite-0 hit
         * detection fired this frame. No game-specific RAM checks.
         * The g_spr0_reads_ctr heuristic was removed — it false-triggers on
         * games that poll $2002 for VBlank detection (e.g. Yoshi's Cookie). */
        int split_y;
        if (g_spr0_split_active) {
            split_y = (spr0_y + 15) & ~7;  /* tile-aligned */
            if (split_y > 240) split_y = 240;
            s_last_valid_split_y = split_y;
            s_split_holdoff      = 6;       /* maintain for up to 6 frames */
        } else if (s_split_holdoff > 0) {
            /* OAM[0] hidden but recently had a valid split — hold it */
            split_y = s_last_valid_split_y;
            s_split_holdoff--;
        } else {
            split_y = 240;
        }

        /* MMC1 mirroring — look up once per frame, not per pixel */
        int mirroring = mapper_get_mirroring();

        /* Canonical PPU vertical state — used only when scroll_y >= 240
         * (the "negative Y scroll" trick: writing $2005 with y in 240..255
         * sets coarse_y to 30 or 31, which on real hardware wraps to 0
         * within the SAME nametable, not the next one).  The default linear
         * nt_y model below does not honor that wrap rule, but is correct
         * for normal scroll values; we keep it as the default path so the
         * frame-by-frame behavior on non-negative-Y screens is unchanged. */
        int v_coarse_y = 0, v_fine_y = 0, v_nt_row = 0;
        int use_canonical = 0;
        int v_initialized = 0;
        int v_last_use_hud = -1;
        int v_last_scroll_y = -1;
        int v_last_ppuctrl_nt = -1;

        /* Incremental absolute nametable Y — models real PPU v register
         * auto-increment so MMC3 IRQ scroll splits don't double-count.
         * Only reset when scroll_y or nt_bit actually changes (IRQ case),
         * NOT on sprite-0 HUD/game transitions where Y stays the same. */
        int abs_nt_y = -1;

        for (int sy = 0; sy < 240; sy++) {
            /* Clock MMC3 scanline counter.  When it fires, run the game's
             * IRQ handler so it can swap CHR banks mid-frame (e.g. MM3
             * status bar vs. playfield use different tile sets). */
            if (mapper_clock_scanline() && !g_disable_render_irq) {
                /* Push PCH, PCL, P — same convention as NMI so RTI can pop them */
                uint8_t p_irq = (uint8_t)((g_cpu.N<<7)|(g_cpu.V<<6)|(1<<5)|
                                           (g_cpu.D<<3)|(g_cpu.I<<2)|(g_cpu.Z<<1)|g_cpu.C);
                g_ram[0x100+g_cpu.S] = 0x00;    g_cpu.S--;  /* PCH */
                g_ram[0x100+g_cpu.S] = 0x00;    g_cpu.S--;  /* PCL */
                g_ram[0x100+g_cpu.S] = p_irq;   g_cpu.S--;  /* P   */
                func_IRQ();
            }

            /* Choose scroll source for this scanline.
             * When split_y==240 (no sprite-0 split), use main game scroll for
             * all scanlines — the HUD values are stale (0,0) from VBlank reset. */
            int use_hud = (split_y < 240 && sy < split_y);
            uint8_t ppuctrl_row = use_hud ? g_ppuctrl_hud  : g_ppuctrl;
            int     scroll_x    = use_hud ? g_ppuscroll_x_hud : g_ppuscroll_x;
            int     scroll_y    = use_hud ? g_ppuscroll_y_hud : g_ppuscroll_y;

            /* BG pattern table: PPUCTRL bit 4 selects $0000 or $1000 */
            int chr_base = (ppuctrl_row & 0x10) ? 0x1000 : 0x0000;

            /* Scroll origin in combined 512×480 nametable space.
             * PPUCTRL bits 0-1 select the base nametable (add 256 or 240 to scroll). */
            int origin_x = scroll_x + ((ppuctrl_row & 0x01) ? 256 : 0);
            int origin_y = scroll_y + ((ppuctrl_row & 0x02) ? 240 : 0);

            /* (Re)initialize the canonical state on first scanline, on
             * use_hud transition, OR if scroll source changed mid-frame. */
            int cur_nt_y_bit = (ppuctrl_row & 0x02) ? 1 : 0;
            if (!v_initialized
                || use_hud != v_last_use_hud
                || scroll_y != v_last_scroll_y
                || cur_nt_y_bit != v_last_ppuctrl_nt) {
                if (scroll_y >= 240) {
                    use_canonical = 1;
                    v_coarse_y = (scroll_y >> 3) & 0x1F;
                    v_fine_y   = scroll_y & 0x07;
                    v_nt_row   = cur_nt_y_bit;
                } else {
                    use_canonical = 0;
                }
                v_initialized     = 1;
                v_last_use_hud    = use_hud;
                /* Reset abs_nt_y only when the Y scroll source actually
                 * changed (IRQ mid-frame split).  A sprite-0 HUD/game
                 * transition with the same scroll_y must NOT reset — the
                 * incremental counter is already correct from prior rows. */
                if (abs_nt_y < 0
                    || scroll_y != v_last_scroll_y
                    || cur_nt_y_bit != v_last_ppuctrl_nt) {
                    abs_nt_y = scroll_y + (cur_nt_y_bit ? 240 : 0);
                }
                v_last_scroll_y   = scroll_y;
                v_last_ppuctrl_nt = cur_nt_y_bit;
            }

            int nt_row, local_ty, tile_row;
            if (use_canonical) {
                nt_row   = v_nt_row;
                local_ty = v_coarse_y;
                tile_row = v_fine_y;
            } else {
                int nt_y = abs_nt_y;
                if (nt_y >= 480) nt_y -= 480;
                int tile_y = nt_y / 8;
                tile_row = nt_y % 8;
                nt_row   = (tile_y >= 30) ? 1 : 0;
                local_ty = tile_y % 30;
            }

            for (int sx = -g_widescreen_left; sx < 256 + g_widescreen_right; sx++) {
                int nt_x      = (origin_x + sx) & 0x1FF;  /* 9-bit wrap (512px nametable) */
                int tile_x    = nt_x / 8;
                int pixel_col = nt_x % 8;
                int nt_col    = (tile_x >= 32) ? 1 : 0;
                int local_tx  = tile_x % 32;

                /* Resolve virtual NT to physical NT using cached mirroring mode */
                int virt_nt = nt_row * 2 + nt_col;
                int phys_nt;
                switch (mirroring) {
                    case 0:  phys_nt = 0;            break; /* one-screen lower */
                    case 1:  phys_nt = 1;            break; /* one-screen upper */
                    case 2:  phys_nt = virt_nt & 1;  break; /* vertical */
                    case 3:  phys_nt = virt_nt >> 1; break; /* horizontal */
                    default: phys_nt = virt_nt & 1;  break;
                }
                int nt_off = phys_nt * 0x400;

                uint8_t tile_id = g_ppu_nt[(nt_off + local_ty * 32 + local_tx) & 0x0FFF];

                int attr_bx = local_tx / 4, attr_by = local_ty / 4;
                uint8_t attr = g_ppu_nt[(nt_off + 0x3C0 + attr_by * 8 + attr_bx) & 0x0FFF];
                int sub_x = (local_tx / 2) & 1, sub_y = (local_ty / 2) & 1;
                int pal_base = (attr >> ((sub_y * 2 + sub_x) * 2)) & 0x03;

                int bit = 7 - pixel_col;
                int chr_off = chr_base + tile_id * 16 + tile_row;
                int color_idx = ((g_chr_ram[chr_off] >> bit) & 1) |
                                (((g_chr_ram[chr_off + 8] >> bit) & 1) << 1);
                int fb_x = sx + g_widescreen_left;
                framebuf[sy * g_render_width + fb_x] = bg_color(pal_base, color_idx);
            }

            /* Canonical-mode per-scanline advance (only when active). */
            if (use_canonical) {
                v_fine_y++;
                if (v_fine_y == 8) {
                    v_fine_y = 0;
                    if (v_coarse_y == 29) {
                        v_coarse_y = 0;
                        v_nt_row ^= 1;
                    } else if (v_coarse_y == 31) {
                        v_coarse_y = 0;          /* same-NT wrap, no toggle */
                    } else {
                        v_coarse_y = (v_coarse_y + 1) & 0x1F;
                    }
                }
            } else {
                abs_nt_y++;
            }
        }
    }

render_sprites:

    /* Phase 2: Sprites (OAM) — skip if sprite rendering disabled */
    if (!(g_ppumask & 0x10)) return;

    /* DEBUG: save OAM tile sheet — 8 cols x 8 rows, each tile at 4x scale (32x32px)
     * Image = 256x256. Magenta BG, grid lines. Transparent pixels = magenta.
     * Also writes a text file with OAM slot info + full palette dump. */
    if (0 && g_frame_count >= 300 && g_frame_count % 60 == 0) {
        #define SCALE 4
        #define CELL (8 * SCALE)   /* 32 */
        #define GCOLS 8
        #define GROWS 8
        #define GW (CELL * GCOLS)  /* 256 */
        #define GH (CELL * GROWS)  /* 256 */
        static uint32_t oam_img[GW * GH];
        int sb = (g_ppuctrl & 0x08) ? 0x1000 : 0x0000;
        /* Magenta background */
        for (int i = 0; i < GW * GH; i++) oam_img[i] = 0xFFFF00FF;
        /* Grid lines (dark gray) */
        for (int gy = 0; gy < GH; gy++)
            for (int gx = 0; gx < GW; gx++)
                if (gx % CELL == 0 || gy % CELL == 0)
                    oam_img[gy * GW + gx] = 0xFF333333;

        for (int si = 0; si < 64; si++) {
            uint8_t sy = g_ppu_oam[si*4+0], st = g_ppu_oam[si*4+1];
            uint8_t sa = g_ppu_oam[si*4+2];
            int col = si % GCOLS, row = si / GCOLS;
            int ox = col * CELL + 1, oy = row * CELL + 1;
            if (sy >= 0xEF) {
                /* Mark hidden slots with dark gray fill */
                for (int py = oy; py < oy + CELL - 1 && py < GH; py++)
                    for (int px = ox; px < ox + CELL - 1 && px < GW; px++)
                        oam_img[py * GW + px] = 0xFF222222;
                continue;
            }
            int sp = (sa & 3) + 4;
            for (int tr = 0; tr < 8; tr++) {
                int co = sb + st * 16 + tr;
                uint8_t lo = g_chr_ram[co], hi = g_chr_ram[co + 8];
                for (int b = 7; b >= 0; b--) {
                    int ci = ((lo >> b) & 1) | (((hi >> b) & 1) << 1);
                    uint32_t color;
                    if (ci == 0)
                        color = 0xFFFF00FF; /* transparent = magenta */
                    else {
                        uint8_t nc = g_ppu_pal[(sp*4+ci) & 0x1F] & 0x3F;
                        color = g_nes_palette[nc];
                    }
                    int px0 = ox + (7 - b) * SCALE;
                    int py0 = oy + tr * SCALE;
                    for (int dy = 0; dy < SCALE; dy++)
                        for (int dx = 0; dx < SCALE; dx++) {
                            int fx = px0 + dx, fy = py0 + dy;
                            if (fx < GW && fy < GH)
                                oam_img[fy * GW + fx] = color;
                        }
                }
            }
        }
        /* Save image */
        {
            static uint8_t rgb[GW * GH * 3];
            for (int i = 0; i < GW * GH; i++) {
                rgb[i*3+0] = (oam_img[i] >> 16) & 0xFF;
                rgb[i*3+1] = (oam_img[i] >>  8) & 0xFF;
                rgb[i*3+2] =  oam_img[i]        & 0xFF;
            }
            char path[80];
            snprintf(path, sizeof(path), "C:/temp/oam_sheet_%04llu.png",
                     (unsigned long long)g_frame_count);
            save_png(path, GW, GH, rgb, GW * 3);
        }
        /* Save text info */
        {
            char path[80];
            snprintf(path, sizeof(path), "C:/temp/oam_info_%04llu.txt",
                     (unsigned long long)g_frame_count);
            FILE *f = fopen(path, "w");
            if (f) {
                fprintf(f, "Frame %llu  PPUCTRL=$%02X  spr_chr=$%04X\n",
                        (unsigned long long)g_frame_count, g_ppuctrl, sb);
                fprintf(f, "Palette: ");
                for (int i = 0; i < 32; i++) fprintf(f, "%02X ", g_ppu_pal[i]);
                fprintf(f, "\n\nSlot  Y    Tile Attr  X   Pal  Colors(1/2/3)\n");
                for (int i = 0; i < 64; i++) {
                    uint8_t y=g_ppu_oam[i*4], t=g_ppu_oam[i*4+1];
                    uint8_t a=g_ppu_oam[i*4+2], x=g_ppu_oam[i*4+3];
                    if (y >= 0xEF) continue;
                    int p=(a&3)+4;
                    fprintf(f, " %2d  %3d   $%02X  $%02X  %3d   %d   $%02X/$%02X/$%02X\n",
                            i, y, t, a, x, p,
                            g_ppu_pal[(p*4+1)&0x1F], g_ppu_pal[(p*4+2)&0x1F],
                            g_ppu_pal[(p*4+3)&0x1F]);
                }
                fclose(f);
            }
        }
    }

    /* PPUCTRL bit 5: 0 = 8x8 sprites, 1 = 8x16 sprites */
    int spr_tall = (g_ppuctrl & 0x20) != 0;
    int spr_height = spr_tall ? 16 : 8;
    /* Sprite pattern table: PPUCTRL bit 3 selects $0000 or $1000 (8x8 mode only) */
    int spr_chr_base = (g_ppuctrl & 0x08) ? 0x1000 : 0x0000;

    /* OAM: 64 sprites × 4 bytes: [Y, tile, attr, X] */
    for (int s = 63; s >= 0; s--) {  /* draw back-to-front so sprite 0 is on top */
        uint8_t spr_y    = g_ppu_oam[s * 4 + 0];
        uint8_t spr_tile = g_ppu_oam[s * 4 + 1];
        uint8_t spr_attr = g_ppu_oam[s * 4 + 2];
        uint8_t spr_x    = g_ppu_oam[s * 4 + 3];

        if (spr_y >= 0xEF) continue; /* off-screen */

        int flip_h   = (spr_attr >> 6) & 1;
        int flip_v   = (spr_attr >> 7) & 1;
        int priority = (spr_attr >> 5) & 1; /* 0=in front, 1=behind BG */
        int spr_pal  = (spr_attr & 0x03) + 4; /* sprite palettes start at $3F10, offset 4 */

        /* 8x16 mode: tile bit 0 selects pattern table, top tile = tile & 0xFE */
        int tile_base, tile_chr_base;
        if (spr_tall) {
            tile_chr_base = (spr_tile & 1) ? 0x1000 : 0x0000;
            tile_base = spr_tile & 0xFE;
        } else {
            tile_chr_base = spr_chr_base;
            tile_base = spr_tile;
        }

        for (int row = 0; row < spr_height; row++) {
            int draw_row = flip_v ? (spr_height - 1 - row) : row;
            int py = spr_y + 1 + row; /* OAM Y is offset by 1 */
            if (py < 0 || py >= 240) continue;

            /* For 8x16: rows 0-7 use top tile, rows 8-15 use bottom tile */
            int tile_row = draw_row;
            int tile_num = tile_base;
            if (spr_tall && tile_row >= 8) {
                tile_num = tile_base + 1;
                tile_row -= 8;
            }

            int chr_off = tile_chr_base + tile_num * 16 + tile_row;
            uint8_t lo = g_chr_ram[chr_off];
            uint8_t hi = g_chr_ram[chr_off + 8];

            for (int bit = 7; bit >= 0; bit--) {
                /* flip_h mirrors which CHR bit we read; screen position is always (7-bit) */
                int chr_bit = flip_h ? (7 - bit) : bit;
                int px = spr_x + (7 - bit);
                if (px < 0 || px >= 256) continue;
                int color_idx = ((lo >> chr_bit) & 1) | (((hi >> chr_bit) & 1) << 1);
                if (color_idx == 0) continue; /* transparent */
                /* Offset sprite X into widescreen framebuffer */
                int fb_x = px + g_widescreen_left;
                /* Priority=1: sprite behind BG — only draw where BG is transparent */
                if (priority && framebuf[py * g_render_width + fb_x] != bg) continue;
                uint8_t nes_color = g_ppu_pal[(spr_pal * 4 + color_idx) & 0x1F] & 0x3F;
                framebuf[py * g_render_width + fb_x] = g_nes_palette[nes_color];
            }
        }
    }

#if 0 /* DEBUG OVERLAY: green boxes at tile >= $90 positions -- kept for reference */
    for (int di = 0; di < 64; di++) {
        uint8_t dy = g_ppu_oam[di*4+0], dt = g_ppu_oam[di*4+1];
        uint8_t dx = g_ppu_oam[di*4+3];
        if (dy >= 0xEF) continue;
        if (dt < 0x90) continue;
        int by = dy + 1, bx = dx;
        for (int ry = by - 2; ry < by + 10; ry++)
            for (int rx = bx - 2; rx < bx + 10; rx++) {
                if (ry < 0 || ry >= 240 || rx < 0 || rx >= 256) continue;
                if (ry < by || ry >= by+8 || rx < bx || rx >= bx+8)
                    framebuf[ry * 256 + rx] = 0xFF00FF00;
            }
    }
#endif
}
