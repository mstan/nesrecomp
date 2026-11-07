/*
 * cyc_video.h - the width of the picture the cycle host presents.
 *
 * The machine always draws 256x240 (cyc_core.h). What the window shows is
 * cyc_video_width() x 240 with square pixels: the native picture, or a game
 * compositor's wider one (cyc_render.h). A game that opts in picks a mode:
 *
 *   stock 256   16:9 426   21:9 560   32:9 854   fit: the window's aspect,
 *   clamped to 16:15 .. 32:9 (256 .. 854)
 *
 * with exactly the numbers of the function-level runner's nes_video.h
 * (common/nes_video_geometry.h is both). Requests are queued and applied at one
 * point per presented frame, after the present and before the next picture is
 * built (cyc_video_apply_pending), so a width never changes while a picture is
 * being made. Before the window exists - and in a headless run, which never
 * has one - a request applies at once; a headless run can give Fit a drawable
 * size to follow with cyc_video_window_resized (the host's --present-size).
 *
 * Nothing here runs unless a game calls cyc_video_set_mode: the default is
 * stock 256, and the window then presents exactly what it did before.
 */
#pragma once
#include <stdbool.h>

#include "../../common/nes_video_geometry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CYC_VIDEO_MAX_WIDTH NES_VIDEO_MAX_WIDTH

/* NES_VIDEO_STOCK .. NES_VIDEO_FIT (nes_video_geometry.h). Idempotent. */
void cyc_video_set_mode(int mode);
int  cyc_video_mode(void);
/* A fixed width (symmetric margins, even, 256..864), leaving the mode stock. */
void cyc_video_request_width(int width);

/* The committed width, and where native column 0 sits in it. */
int  cyc_video_width(void);
int  cyc_video_native_x0(void);

/* Host side. */
void cyc_video_window_ready(void);                   /* from now on requests queue */
void cyc_video_window_resized(int drawable_w, int drawable_h);
/* Last host drawable size, also available while a menu owns input events.
 * Zero dimensions mean no window or headless drawable has been supplied. */
void cyc_video_drawable_size(int *drawable_w, int *drawable_h);
/* Diagnostic playback restores the exact committed geometry before guest work.
 * This does not create a window or change the player's saved settings. */
bool cyc_video_restore(int mode, int width, int drawable_w, int drawable_h);
/* Apply a queued request: true when the width changed (the host re-creates
 * its texture). */
bool cyc_video_apply_pending(void);

#ifdef __cplusplus
}
#endif
