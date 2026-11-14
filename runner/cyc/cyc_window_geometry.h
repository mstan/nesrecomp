/* Initial SDL window geometry; no change to the machine's video geometry. */
#pragma once
#include "../../common/nes_video_geometry.h"

/* A title's optional initial Fit hint is expressed in 240-row pixels.
 * Scale it with a validated presentation height (including HD textures).
 * Zero, stock and fixed modes retain the original picture-sized window. */
static inline int cyc_window_initial_fit_width(int mode, int preferred,
                                               int picture_w, int picture_h)
{
    if (mode != NES_VIDEO_FIT || preferred <= 0 || picture_h <= 0) return picture_w;
    return (int)(((long long)nes_video_geometry_clamp_width(preferred) * picture_h + 120) / 240);
}
