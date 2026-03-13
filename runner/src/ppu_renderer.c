/*
 * ppu_renderer.c — NES PPU background + sprite rendering
 *
 * Phase 1: BG nametable → CHR tiles → palette → ARGB8888 framebuffer
 * Phase 2: OAM sprites
 */
#include "nes_runtime.h"
#include <string.h>

/* NES system palette — 64 colors as ARGB8888 */
static const uint32_t NES_PALETTE[64] = {
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
        return NES_PALETTE[g_ppu_pal[0] & 0x3F];
    }
    uint8_t nes_color = g_ppu_pal[(pal_base * 4 + color_idx) & 0x1F] & 0x3F;
    return NES_PALETTE[nes_color];
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
        if (x < 0 || x >= 256) continue;
        int color_idx = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
        framebuf[px_y * 256 + x] = bg_color(pal_base, color_idx);
    }
}

void ppu_render_frame(uint32_t *framebuf) {
    /* Universal background color */
    uint32_t bg = NES_PALETTE[g_ppu_pal[0] & 0x3F];
    for (int i = 0; i < 256 * 240; i++) framebuf[i] = bg;

    /* Only render if BG rendering is enabled ($2001 bit 3) */
    if (!(g_ppumask & 0x08)) goto render_sprites;

    {
        /* Split-screen: rows 0-15 use HUD scroll (captured at sprite-0 hit);
         * rows 16-239 use the post-split game-area scroll.
         * When no split occurred this frame, all rows use current scroll. */
        int split_y = g_spr0_split_active ? 16 : 240;

        for (int sy = 0; sy < 240; sy++) {
            /* Choose scroll source for this scanline */
            uint8_t ppuctrl_row = (sy < split_y) ? g_ppuctrl_hud  : g_ppuctrl;
            int     scroll_x    = (sy < split_y) ? g_ppuscroll_x_hud : g_ppuscroll_x;
            int     scroll_y    = (sy < split_y) ? g_ppuscroll_y_hud : g_ppuscroll_y;

            /* BG pattern table: PPUCTRL bit 4 selects $0000 or $1000 */
            int chr_base = (ppuctrl_row & 0x10) ? 0x1000 : 0x0000;

            /* Scroll origin in combined 512×480 nametable space.
             * PPUCTRL bits 0-1 select the base nametable (add 256 or 240 to scroll). */
            int origin_x = scroll_x + ((ppuctrl_row & 0x01) ? 256 : 0);
            int origin_y = scroll_y + ((ppuctrl_row & 0x02) ? 240 : 0);

            int nt_y = origin_y + sy;
            if (nt_y >= 480) nt_y -= 480;
            int tile_y   = nt_y / 8;
            int tile_row = nt_y % 8;
            int nt_row   = (tile_y >= 30) ? 1 : 0;
            int local_ty = tile_y % 30;

            for (int sx = 0; sx < 256; sx++) {
                int nt_x      = (origin_x + sx) & 0x1FF;
                int tile_x    = nt_x / 8;
                int pixel_col = nt_x % 8;
                int nt_col    = (tile_x >= 32) ? 1 : 0;
                int local_tx  = tile_x % 32;

                int which_nt = nt_row * 2 + nt_col;
                int nt_off   = which_nt * 0x400;

                uint8_t tile_id = g_ppu_nt[(nt_off + local_ty * 32 + local_tx) & 0x0FFF];

                int attr_bx = local_tx / 4, attr_by = local_ty / 4;
                uint8_t attr = g_ppu_nt[(nt_off + 0x3C0 + attr_by * 8 + attr_bx) & 0x0FFF];
                int sub_x = (local_tx / 2) & 1, sub_y = (local_ty / 2) & 1;
                int pal_base = (attr >> ((sub_y * 2 + sub_x) * 2)) & 0x03;

                int bit = 7 - pixel_col;
                int chr_off = chr_base + tile_id * 16 + tile_row;
                int color_idx = ((g_chr_ram[chr_off] >> bit) & 1) |
                                (((g_chr_ram[chr_off + 8] >> bit) & 1) << 1);
                framebuf[sy * 256 + sx] = bg_color(pal_base, color_idx);
            }
        }
    }

render_sprites:

    /* Phase 2: Sprites (OAM) — skip if sprite rendering disabled */
    if (!(g_ppumask & 0x10)) return;

    /* Sprite pattern table: PPUCTRL bit 3 selects $0000 or $1000 (8x8 mode) */
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

        for (int row = 0; row < 8; row++) {
            int draw_row = flip_v ? (7 - row) : row;
            int py = spr_y + 1 + row; /* OAM Y is offset by 1 */
            if (py < 0 || py >= 240) continue;

            int chr_off = spr_chr_base + spr_tile * 16 + draw_row;
            uint8_t lo = g_chr_ram[chr_off];
            uint8_t hi = g_chr_ram[chr_off + 8];

            for (int bit = 7; bit >= 0; bit--) {
                /* flip_h mirrors which CHR bit we read; screen position is always (7-bit) */
                int chr_bit = flip_h ? (7 - bit) : bit;
                int px = spr_x + (7 - bit);
                if (px < 0 || px >= 256) continue;
                int color_idx = ((lo >> chr_bit) & 1) | (((hi >> chr_bit) & 1) << 1);
                if (color_idx == 0) continue; /* transparent */
                if (priority) {
                    /* Behind BG: only draw where BG pixel is universal background color */
                    uint32_t bg_backdrop = NES_PALETTE[g_ppu_pal[0] & 0x3F];
                    if (framebuf[py * 256 + px] != bg_backdrop) continue;
                }
                uint8_t nes_color = g_ppu_pal[(spr_pal * 4 + color_idx) & 0x1F] & 0x3F;
                framebuf[py * 256 + px] = NES_PALETTE[nes_color];
            }
        }
    }
}
