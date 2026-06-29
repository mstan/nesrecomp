/*
 * ppu_renderer.c — shared PPU assets + the OAM debug view.
 *
 * The per-frame software compositor that used to live here (ppu_render_frame +
 * its sprite-0 split heuristics) was retired once the cycle-driven per-scanline
 * renderer (ppu_dot.c) became the sole renderer. What remains is shared by the
 * dot renderer and the debug UI:
 *   - g_nes_palette  : the 64-entry NES system palette (ARGB8888)
 *   - g_disable_render_irq : debug toggle, read by ppu_dot.c's MMC3 IRQ path
 *   - ppu_render_oam_debug : the OAM grid view for the debug window
 */
#include "nes_runtime.h"

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
