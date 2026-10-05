/*
 * cyc_render.h - a game's own compositor for the picture the window presents.
 *
 * The machine keeps drawing its 256x240 picture exactly as the console does;
 * nothing here feeds back into it. When the presented width (cyc_video.h) is
 * wider than 256 and a game installed a compositor, the host hands it a
 * width x 240 ARGB canvas (opaque black) and the native picture; the
 * compositor paints the canvas and returns nonzero, or returns 0 to have the
 * native picture pillarboxed at native_x0 (title screens, menus, anything it
 * does not extend). At width 256 the native picture is presented as it is and
 * the compositor is not called.
 *
 * The compositor runs when the window presents (and for headless --present-out),
 * after the frame's end: reading the state the game left then is safe, and
 * what the PPU used to draw that frame is available here:
 *
 *   cyc_render_line_*   per visible line, the scroll, pattern tables and mask
 *                       the PPU actually rendered with (cyc_core.h
 *                       cyc_frame_lines) - not the game's camera variables,
 *                       which the game has usually advanced for the next
 *                       frame by then
 *   cyc_frame_bg_opaque the background's coverage of the native picture
 *   cyc_render_oam      the OAM that frame's sprites came from
 *   cyc_render_chr / cyc_render_nametable / cyc_render_palette
 *                       pattern bytes through the cartridge's current CHR
 *                       mapping, CIRAM through its mirroring, palette RAM
 *
 * and two drawing helpers: an 8x8 tile from a pattern table with a palette,
 * and the frame's OAM drawn into the wide canvas with a placement callback
 * (8x16, flips, background priority, the left-column clip in native columns).
 *
 * cyc_render_set_margins limits what shows of a painted canvas for the
 * current picture: columns further than `left` / `right` pixels outside the
 * native 256 are black (-1: no limit), so a game can pillarbox a screen
 * partly (a menu over the playfield) without changing the width.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cyc_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*CycCompositorFn)(uint32_t *out, int width, int height, int native_x0, const uint32_t *native,
                               void *user);
void cyc_render_set_compositor(CycCompositorFn fn, void *user);
bool cyc_render_has_compositor(void);
void cyc_render_set_margins(int left, int right);
/* Called by a compositor that placed the native picture somewhere other than
 * the native_x0 it was given (a camera anchored at an area's edge): the canvas
 * column of native column 0 in the picture being painted. The host maps the
 * Zapper's aim and draws its crosshair through it, and set_margins counts from
 * it. A picture the compositor leaves to the native one is at native_x0. */
void cyc_render_set_native_origin(int x0);
/* Where native column 0 sits in the presented picture of that width: what the
 * compositor declared for it, else the centered (width - 256) / 2, 0 at 256. */
int  cyc_render_native_origin(int width);
/* Exact background capture for host-drawn sprite replacements. Native sprite
 * evaluation, hits and overflow remain unchanged. The capture travels in a
 * separate presentation savestate record. */
void cyc_render_capture_background(bool enabled);
bool cyc_render_background(uint32_t *out); /* 256x240 ARGB, false if disabled */
extern bool cyc_background_enabled;
extern uint8_t cyc_background_pipe[4];
void cyc_render_background_output(unsigned pixel, unsigned emphasis, bool grey);

/* ---- what the frame used ---- */
/* The background's X at native column 0 of visible line y, 0-511 across the
 * two nametables side by side, and its Y, 0-479 with the lower nametables'
 * rows from 240 (v's nametable, coarse and fine Y as the line fetched them). */
int  cyc_render_line_scroll_x(int y);
int  cyc_render_line_scroll_y(int y);
/* $2000 bit 4 / bit 3 as that line used them: $0000 or $1000. */
uint16_t cyc_render_line_bg_table(int y);
uint16_t cyc_render_line_sprite_table(int y);
bool cyc_render_line_sprite16(int y);
/* $2001 as that line used it (cyc_core.h CycLine.mask). */
uint8_t cyc_render_line_mask(int y);

const uint8_t *cyc_render_oam(void);              /* 256 bytes */
uint8_t  cyc_render_chr(uint16_t ppu_addr);       /* $0000-$1FFF */
uint8_t  cyc_render_nametable(uint16_t ppu_addr); /* $2000-$2FFF */
uint8_t  cyc_render_palette(int entry);           /* palette RAM $3F00 + entry (0-31), 6 bits */
/* A palette RAM entry as ARGB, with the emphasis the picture's last line had. */
uint32_t cyc_render_color(int entry);

/* ---- drawing ---- */
enum {
    CYC_TILE_HFLIP  = 1,
    CYC_TILE_VFLIP  = 2,
    CYC_TILE_BEHIND = 4,   /* only where `behind` (width x height) is 0 */
};
/* The 8x8 tile at pattern address `pattern` (table + tile * 16) at (x, y),
 * palette 0-7 (4-7 sprites), its transparent pixels untouched; `coverage`
 * (width x height, or NULL) gets 1 where it drew. Clipped to the canvas. */
void cyc_render_tile(uint32_t *out, int width, int height, int x, int y, uint16_t pattern, int palette,
                     unsigned flags, const uint8_t *behind, uint8_t *coverage);

/* Where OAM slot `slot`'s sprite goes: *out_x in canvas columns (x is OAM X,
 * y its first line); return 0 to leave it out. */
typedef int (*CycSpritePlaceFn)(int slot, int x, int y, int *out_x, void *user);
/* The frame's OAM, slot 63 first so slot 0 ends on top, each sprite at
 * place()'s column (NULL: native_x0 + x). bg_opaque (width x height, or NULL)
 * decides behind-background sprites. */
void cyc_render_sprites(uint32_t *out, int width, int height, int native_x0, const uint8_t *bg_opaque,
                        CycSpritePlaceFn place, void *user);

/* ---- host ---- */
/* What the window shows now: width x 240. Composed once per machine frame
 * (cyc_render_frame_done) or width change. */
const uint32_t *cyc_render_present(int *width, int *height);
/* The machine finished a frame, or a save state replaced it. */
void cyc_render_frame_done(void);
uint64_t cyc_render_generation(void);

typedef struct {
    uint64_t composed;      /* pictures the compositor painted */
    uint64_t pillarboxed;   /* wide pictures it left to the native one */
    uint64_t native;        /* pictures presented at 256 */
} CycRenderStats;
void cyc_render_stats(CycRenderStats *out);

#ifdef __cplusplus
}
#endif
