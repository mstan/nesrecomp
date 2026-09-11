/*
 * nes_video.h — live-resizable output geometry for the NES runner.
 *
 * The runner renders the NES picture into a framebuffer that is
 * g_render_width x 240 pixels (square-pixel presentation, letterboxed by
 * SDL_RenderSetLogicalSize). Historically that width was fixed for the life of
 * the process: a game set g_widescreen_left/right once in game_on_init(),
 * before the SDL window and texture existed, and every buffer was a static
 * 512-wide array.
 *
 * This module makes the width a runtime property so a game can follow the
 * window's aspect ("Fit": 16:15 .. 32:9) or switch presets (16:9 / 21:9 /
 * 32:9) while running, and so title/menu screens can drop back to the stock
 * 256-wide picture per frame.
 *
 * Model
 *   - Geometry requests are queued (nes_video_request_*) and applied by the
 *     main loop at exactly one point per frame, after SDL_RenderPresent and
 *     before the next frame's render (nes_video_apply_pending). Nothing ever
 *     changes g_render_width while a frame is being rendered.
 *   - Before the SDL window exists (game_on_init time) a request applies
 *     immediately, which is what the fixed-width games (SMB widescreen, the
 *     voxel first-person profile) rely on.
 *   - Every width-sized buffer is allocated once at NES_MAX_RENDER_WIDTH, so
 *     pointers handed out earlier (zapper framebuffer, dot-PPU target) stay
 *     valid across a resize. Only the SDL texture(s), the renderer logical
 *     size and the HD-pack side channel are re-created.
 *   - Widths are even and margins symmetric: width = 256 + 2*margin.
 *     Asymmetric margins remain available for games that need them
 *     (nes_video_request_margins).
 *
 * Opt-in. Nothing here runs unless a game calls nes_video_set_aspect_mode():
 * that and nes_video_on_window_resized() (which acts only once the mode is
 * NES_ASPECT_FIT) are the sole callers of nes_video_width_for(). A game that
 * only sets fixed margins -- g_widescreen_left/right or
 * nes_video_request_width/_margins -- never touches the aspect math, so the
 * widths below cannot change its geometry.
 *
 * Aspect math follows snesrecomp's SmCalculateViewport: with square pixels a
 * 240-row picture of aspect A is round_even(240 * A) columns wide, and Fit
 * clamps the window aspect to [16:15, 32:9].
 *
 *   STOCK 256 (explicit; 16:15)   16:9 426   21:9 560   32:9 854
 *
 * NOTE the pixel-shape trade-off. A real NES puts 256 columns into a 4:3
 * raster with 8:7 non-square pixels; this module presents square pixels
 * (logical g_render_width x 240 via SDL_RenderSetLogicalSize) throughout,
 * under which 256x240 is 16:15 and a true 4:3 frame is 320 columns. So:
 *   - STOCK is 256 -- the vanilla picture, letterbox/pillarbox handled by SDL.
 *     It is slightly narrow-looking versus a CRT, which is the price of never
 *     inventing picture the game did not draw.
 *   - Fit's low clamp is 16:15, not 4:3, so a window at or below the vanilla
 *     aspect gets exactly 256 columns rather than a picture narrower than the
 *     game. A 4:3 window still yields 320, which genuinely fills it with
 *     square pixels -- i.e. 32 columns per side of *extra* widescreen content,
 *     not a stretched vanilla frame.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Widest framebuffer the runner allocates: 32:9 at 240 rows is 853.3 px. */
#define NES_MAX_RENDER_WIDTH 864

/* Aspect of the vanilla 256x240 frame under square pixels: 16:15, not 4:3.
 * Also the low clamp for NES_ASPECT_FIT. */
#define NES_STOCK_ASPECT (256.0 / 240.0)

typedef enum {
    NES_ASPECT_STOCK = 0,   /* the vanilla 256-wide picture (16:15 square-pixel) */
    NES_ASPECT_16_9,
    NES_ASPECT_21_9,
    NES_ASPECT_32_9,
    NES_ASPECT_FIT,         /* follow the window drawable aspect, clamped 16:15..32:9 */
    NES_ASPECT_COUNT
} NesAspectMode;

/* Pure geometry. */
int         nes_video_width_for_aspect(double aspect);              /* even, clamped [256, MAX] */
int         nes_video_width_for(NesAspectMode mode, int out_w, int out_h);
const char *nes_video_aspect_name(NesAspectMode mode);              /* "stock" "16:9" ... "fit" */
int         nes_video_aspect_from_name(const char *name, NesAspectMode *out); /* accepts 16:9, 16-9, 16x9, fit, adaptive, stock, 4:3 */

/* Requests (queued; applied at the frame boundary, or immediately before the
 * window exists). Idempotent: a request equal to the current geometry is a
 * no-op, so a game may re-assert its mode every frame. */
void nes_video_set_aspect_mode(NesAspectMode mode);
NesAspectMode nes_video_aspect_mode(void);
void nes_video_request_width(int width);                 /* symmetric margins */
void nes_video_request_margins(int left, int right);     /* explicit margins */

/* Called by the main loop when the window drawable size changed; re-derives
 * the width when the mode is NES_ASPECT_FIT. */
void nes_video_on_window_resized(int out_w, int out_h);

/* Main-loop side (main_runner.c). Returns 1 and fills the new margins when a
 * geometry change is pending; the caller then re-creates its SDL objects and
 * calls nes_video_commit(). */
int  nes_video_take_pending(int *left, int *right);
void nes_video_commit(int left, int right);              /* sets g_render_width/g_widescreen_* */
void nes_video_mark_window_ready(void);                  /* after SDL window/texture creation */
int  nes_video_window_ready(void);

#ifdef __cplusplus
}
#endif
