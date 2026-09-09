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
 * window's aspect ("Fit": 4:3 .. 32:9) or switch presets (16:9 / 21:9 / 32:9)
 * while running, and so title/menu screens can drop back to 4:3 per frame.
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
 * Aspect math follows snesrecomp's SmCalculateViewport: the stock 256-wide
 * picture is 4:3 at 240 rows, so width = round_even(240 * aspect), and Fit
 * clamps the window aspect to [4:3, 32:9].
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Widest framebuffer the runner allocates: 32:9 at 240 rows is 853.3 px. */
#define NES_MAX_RENDER_WIDTH 864

typedef enum {
    NES_ASPECT_STOCK = 0,   /* 4:3, the vanilla 256-wide picture */
    NES_ASPECT_16_9,
    NES_ASPECT_21_9,
    NES_ASPECT_32_9,
    NES_ASPECT_FIT,         /* follow the window drawable aspect, clamped 4:3..32:9 */
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
