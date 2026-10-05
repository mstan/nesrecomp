/* cyc_render.c - a game's compositor and its helpers; see cyc_render.h. */
#include "cyc_render.h"

#include "cyc_video.h"
#include "hw_internal.h"

#include <string.h>

static CycCompositorFn s_fn;
static void           *s_user;
static int             s_margin_left = -1, s_margin_right = -1;
static uint32_t        s_canvas[CYC_VIDEO_MAX_WIDTH * 240];
static uint64_t        s_generation = 1, s_composed_generation;
static int             s_composed_width;
static int             s_origin, s_origin_request;
static bool            s_origin_requested;
static CycRenderStats  s_stats;

void cyc_render_set_compositor(CycCompositorFn fn, void *user)
{
    s_fn = fn;
    s_user = user;
    s_generation++;
}

bool cyc_render_has_compositor(void) { return s_fn != NULL; }

void cyc_render_set_margins(int left, int right)
{
    if (left != s_margin_left || right != s_margin_right) s_generation++;
    s_margin_left = left;
    s_margin_right = right;
}

void cyc_render_set_native_origin(int x0)
{
    s_origin_request = x0;
    s_origin_requested = true;
}

int cyc_render_native_origin(int width)
{
    if (width <= 256) return 0;
    if (width == s_composed_width && s_composed_generation) return s_origin;
    return (width - 256) / 2;
}

void cyc_render_frame_done(void) { s_generation++; }
uint64_t cyc_render_generation(void) { return s_generation; }

void cyc_render_stats(CycRenderStats *out) { *out = s_stats; }

/* ---- what the frame used ---- */

static const CycLine *line(int y) { return &hw_frame_lines[y < 0 ? 0 : y > 239 ? 239 : y]; }

int cyc_render_line_scroll_x(int y)
{
    const CycLine *l = line(y);
    /* dot 1: the line's first two tiles were fetched, coarse X is two on */
    unsigned x6 = ((l->v >> 5) & 0x20u) | (l->v & 0x1Fu);
    return (int)((((x6 - 2u) & 63u) << 3) | l->fine_x);
}

int cyc_render_line_scroll_y(int y)
{
    const CycLine *l = line(y);
    return (int)(((l->v >> 11) & 1u) * 240u + ((l->v >> 5) & 31u) * 8u + ((l->v >> 12) & 7u));
}

uint16_t cyc_render_line_bg_table(int y) { return (line(y)->ctrl & 0x10) ? 0x1000 : 0; }
uint16_t cyc_render_line_sprite_table(int y) { return (line(y)->ctrl & 0x08) ? 0x1000 : 0; }
bool     cyc_render_line_sprite16(int y) { return (line(y)->ctrl & 0x20) != 0; }
uint8_t  cyc_render_line_mask(int y) { return line(y)->mask; }

const uint8_t *cyc_render_oam(void) { return ppu.oam; }

uint8_t cyc_render_chr(uint16_t a)
{
    uint32_t i = hw_cart_chr_index((uint16_t)(a & 0x1FFF));
    if (!hw_cart.chr || (!hw_cart.chr_ram && i >= hw_cart.chr_len)) return 0;
    return hw_cart.chr[i];
}

uint8_t cyc_render_nametable(uint16_t a)
{
    return ppu.ciram[(a & 0x3FF) | hw_cart_ciram_a10((uint16_t)(0x2000 | (a & 0xFFF)))];
}

uint8_t cyc_render_palette(int entry) { return ppu.palette[entry & 31] & 0x3F; }

uint32_t cyc_render_color(int entry)
{
    unsigned emphasis = hw_frame_lines[239].mask >> 5;
    return hw_palette_argb[(ppu.palette[entry & 31] & 0x3F) | emphasis << 6];
}

/* ---- drawing ---- */

void cyc_render_tile(uint32_t *out, int width, int height, int x, int y, uint16_t pattern, int palette,
                     unsigned flags, const uint8_t *behind, uint8_t *coverage)
{
    for (int r = 0; r < 8; ++r) {
        int py = y + r;
        if (py < 0 || py >= height) continue;
        int row = (flags & CYC_TILE_VFLIP) ? 7 - r : r;
        uint8_t lo = cyc_render_chr((uint16_t)(pattern + row)), hi = cyc_render_chr((uint16_t)(pattern + row + 8));
        for (int c = 0; c < 8; ++c) {
            int px = x + c;
            if (px < 0 || px >= width) continue;
            int bit = (flags & CYC_TILE_HFLIP) ? c : 7 - c;
            int v = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
            if (!v) continue;
            size_t at = (size_t)py * (size_t)width + (size_t)px;
            if ((flags & CYC_TILE_BEHIND) && behind && behind[at]) continue;
            out[at] = cyc_render_color(palette * 4 + v);
            if (coverage) coverage[at] = 1;
        }
    }
}

void cyc_render_sprites(uint32_t *out, int width, int height, int native_x0, const uint8_t *bg_opaque,
                        CycSpritePlaceFn place, void *user)
{
    for (int slot = 63; slot >= 0; --slot) {
        const uint8_t *o = &ppu.oam[slot * 4];
        int y = o[0] + 1;
        if (o[0] >= 0xEF) continue;
        bool tall = cyc_render_line_sprite16(y < 240 ? y : 239);
        uint8_t mask = cyc_render_line_mask(y < 240 ? y : 239);
        if (!(mask & 0x10)) continue;
        int x = o[3], ox = native_x0 + x;
        if (place && !place(slot, x, y, &ox, user)) continue;
        uint16_t table = cyc_render_line_sprite_table(y < 240 ? y : 239);
        unsigned attr = o[2];
        int rows = tall ? 16 : 8;
        for (int r = 0; r < rows; ++r) {
            int py = y + r;
            if (py < 0 || py >= height || py >= 240) continue;
            int row = (attr & 0x80) ? rows - 1 - r : r;
            uint16_t pattern = tall ? (uint16_t)(((o[1] & 1) ? 0x1000 : 0) + (o[1] & 0xFE) * 16 + (row >= 8 ? 16 : 0))
                                    : (uint16_t)(table + o[1] * 16);
            uint8_t lo = cyc_render_chr((uint16_t)(pattern + (row & 7)));
            uint8_t hi = cyc_render_chr((uint16_t)(pattern + (row & 7) + 8));
            for (int c = 0; c < 8; ++c) {
                int px = ox + c;
                if (px < 0 || px >= width) continue;
                /* the left-column clip is in native columns */
                if (x + c < 8 && !(cyc_render_line_mask(py) & 0x04) && px - native_x0 == x + c) continue;
                int bit = (attr & 0x40) ? c : 7 - c;
                int v = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
                if (!v) continue;
                size_t at = (size_t)py * (size_t)width + (size_t)px;
                if ((attr & 0x20) && bg_opaque && bg_opaque[at]) continue;
                out[at] = cyc_render_color(16 + (attr & 3) * 4 + v);
            }
        }
    }
}

/* ---- presenting ---- */

const uint32_t *cyc_render_present(int *width, int *height)
{
    int w = cyc_video_width();
    *height = 240;
    if (w <= 256) {
        *width = 256;
        if (s_composed_generation != s_generation || s_composed_width != 256) {
            s_stats.native++;
            s_composed_generation = s_generation;
            s_composed_width = 256;
        }
        return cyc_frame_argb();
    }
    *width = w;
    if (s_composed_generation == s_generation && s_composed_width == w) return s_canvas;
    const uint32_t *native = cyc_frame_argb();
    int x0 = (w - 256) / 2;
    for (int i = 0; i < w * 240; ++i) s_canvas[i] = 0xFF000000u;
    s_origin_requested = false;
    int painted = s_fn ? s_fn(s_canvas, w, 240, x0, native, s_user) : 0;
    s_origin = x0;
    if (!painted) {
        for (int y = 0; y < 240; ++y) memcpy(&s_canvas[y * w + x0], &native[y * 256], 256 * sizeof(uint32_t));
        s_stats.pillarboxed++;
    } else {
        s_stats.composed++;
        if (s_origin_requested) s_origin = s_origin_request;
        if (s_margin_left >= 0 || s_margin_right >= 0) {
            int o = s_origin;
            int left = s_margin_left >= 0 && s_margin_left < o ? o - s_margin_left : 0;
            int right = s_margin_right >= 0 && o + 256 + s_margin_right < w ? o + 256 + s_margin_right : w;
            for (int y = 0; y < 240; ++y) {
                for (int x = 0; x < left; ++x) s_canvas[y * w + x] = 0xFF000000u;
                for (int x = right; x < w; ++x) s_canvas[y * w + x] = 0xFF000000u;
            }
        }
    }
    s_composed_generation = s_generation;
    s_composed_width = w;
    return s_canvas;
}
