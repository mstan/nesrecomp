/*
 * nes_video.c — live-resizable output geometry (see nes_video.h).
 *
 * Owns the requested/pending geometry only. The SDL objects and the pixel
 * buffers live in main_runner.c, which polls nes_video_take_pending() once per
 * frame at the safe point (after present, before the next render).
 */
#include "nes_video.h"
#include "nes_runtime.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static NesAspectMode s_mode = NES_ASPECT_STOCK;
static int s_window_ready = 0;
static int s_pending = 0;
static int s_pending_left = 0, s_pending_right = 0;
static int s_last_out_w = 0, s_last_out_h = 0;

static int clamp_even_width(int w) {
    if (w < 256) w = 256;
    if (w > NES_MAX_RENDER_WIDTH) w = NES_MAX_RENDER_WIDTH;
    /* Even width keeps the stock 256 columns exactly centered. */
    w &= ~1;
    return w;
}

int nes_video_width_for_aspect(double aspect) {
    if (!(aspect > 0.0)) return 256;
    /* Square pixels: the picture is 240 rows tall, so the width that shows a
     * given aspect is 240 * aspect. Round to nearest even. (16:15 -> 256, the
     * vanilla frame; 4:3 -> 320, which genuinely fills a 4:3 window.) */
    int w = 2 * (int)floor((240.0 * aspect) / 2.0 + 0.5);
    return clamp_even_width(w);
}

int nes_video_width_for(NesAspectMode mode, int out_w, int out_h) {
    /* Under the square-pixel model a 256x240 frame is 16:15, NOT 4:3. STOCK
     * therefore has to be an explicit case: routing it through the aspect
     * formula at 4/3 would yield round_even(240 * 4/3) = 320, i.e. 32-px
     * margins of invented picture rather than the vanilla frame. */
    double aspect;
    switch (mode) {
    case NES_ASPECT_16_9: aspect = 16.0 / 9.0; break;
    case NES_ASPECT_21_9: aspect = 21.0 / 9.0; break;
    case NES_ASPECT_32_9: aspect = 32.0 / 9.0; break;
    case NES_ASPECT_FIT:
        /* Clamp low at 16:15 (= NES_STOCK_ASPECT), the aspect of the vanilla
         * frame: a window narrower than that gets the stock 256 columns
         * (pillarboxed by SDL_RenderSetLogicalSize) instead of a picture
         * narrower than the game. A 4:3 window still fits exactly, at 320. */
        if (out_w > 0 && out_h > 0) {
            aspect = (double)out_w / (double)out_h;
            if (aspect < NES_STOCK_ASPECT)  aspect = NES_STOCK_ASPECT;
            if (aspect > 32.0 / 9.0)        aspect = 32.0 / 9.0;
        } else {
            aspect = NES_STOCK_ASPECT;
        }
        break;
    case NES_ASPECT_STOCK:
    default:
        return 256;
    }
    return nes_video_width_for_aspect(aspect);
}

const char *nes_video_aspect_name(NesAspectMode mode) {
    switch (mode) {
    case NES_ASPECT_STOCK: return "stock";
    case NES_ASPECT_16_9:  return "16:9";
    case NES_ASPECT_21_9:  return "21:9";
    case NES_ASPECT_32_9:  return "32:9";
    case NES_ASPECT_FIT:   return "fit";
    default:               return "?";
    }
}

int nes_video_aspect_from_name(const char *name, NesAspectMode *out) {
    if (!name || !out) return 0;
    char n[16];
    size_t i;
    for (i = 0; name[i] && i < sizeof(n) - 1; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '-' || c == 'x' || c == '_') c = ':';
        n[i] = c;
    }
    n[i] = '\0';
    if (!strcmp(n, "stock") || !strcmp(n, "4:3") || !strcmp(n, "off") || !strcmp(n, "0")) {
        *out = NES_ASPECT_STOCK; return 1;
    }
    if (!strcmp(n, "16:9")) { *out = NES_ASPECT_16_9; return 1; }
    if (!strcmp(n, "21:9")) { *out = NES_ASPECT_21_9; return 1; }
    if (!strcmp(n, "32:9")) { *out = NES_ASPECT_32_9; return 1; }
    if (!strcmp(n, "fit") || !strcmp(n, "adaptive") || !strcmp(n, "auto")) {
        *out = NES_ASPECT_FIT; return 1;
    }
    return 0;
}

void nes_video_request_margins(int left, int right) {
    if (left < 0) left = 0;
    if (right < 0) right = 0;
    if (256 + left + right > NES_MAX_RENDER_WIDTH) {
        /* Shrink the larger margin first; keep the request satisfiable. */
        int over = 256 + left + right - NES_MAX_RENDER_WIDTH;
        if (left >= right) { left -= over; if (left < 0) { right += left; left = 0; } }
        else               { right -= over; if (right < 0) { left += right; right = 0; } }
    }
    if (!s_window_ready) {
        /* Pre-window: apply now, exactly as the old game_on_init() assignments
         * did, so the first SDL texture is created at the right size. */
        nes_video_commit(left, right);
        s_pending = 0;
        return;
    }
    if (left == g_widescreen_left && right == g_widescreen_right) {
        s_pending = 0;
        return;
    }
    s_pending = 1;
    s_pending_left = left;
    s_pending_right = right;
}

void nes_video_request_width(int width) {
    int w = clamp_even_width(width);
    int m = (w - 256) / 2;
    nes_video_request_margins(m, m);
}

void nes_video_set_aspect_mode(NesAspectMode mode) {
    if (mode < 0 || mode >= NES_ASPECT_COUNT) return;
    s_mode = mode;
    nes_video_request_width(nes_video_width_for(mode, s_last_out_w, s_last_out_h));
}

NesAspectMode nes_video_aspect_mode(void) { return s_mode; }

void nes_video_on_window_resized(int out_w, int out_h) {
    s_last_out_w = out_w;
    s_last_out_h = out_h;
    if (s_mode == NES_ASPECT_FIT)
        nes_video_request_width(nes_video_width_for(s_mode, out_w, out_h));
}

int nes_video_take_pending(int *left, int *right) {
    if (!s_pending) return 0;
    if (left) *left = s_pending_left;
    if (right) *right = s_pending_right;
    s_pending = 0;
    return 1;
}

void nes_video_commit(int left, int right) {
    g_widescreen_left  = left;
    g_widescreen_right = right;
    g_render_width     = 256 + left + right;
}

void nes_video_mark_window_ready(void) { s_window_ready = 1; }
int  nes_video_window_ready(void)      { return s_window_ready; }
