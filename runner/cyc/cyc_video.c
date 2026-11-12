/* cyc_video.c - the presented width; see cyc_video.h. */
#include "cyc_video.h"

#include "cyc_ring.h"

static int  s_mode = NES_VIDEO_STOCK;
static int  s_width = 256;
static bool s_window;
static bool s_pending;
static int  s_pending_width = 256;
static int  s_draw_w, s_draw_h;
static int  s_override;

static void commit(int width)
{
    if (width == s_width) return;
    s_width = width;
    cyc_ring_push_len(CYC_EV_VIDEO, (uint16_t)width, (uint32_t)s_mode,
                      s_mode == NES_VIDEO_FIT ? (uint32_t)(s_draw_w > 0 ? s_draw_w : 0) : 0);
}

static void request(int width)
{
    width = nes_video_geometry_clamp_width(width);
    if (!s_window) {
        commit(width);
        s_pending = false;
        return;
    }
    s_pending = width != s_width;
    s_pending_width = width;
}

void cyc_video_set_mode(int mode)
{
    if (mode < 0 || mode >= NES_VIDEO_MODE_COUNT) return;
    s_mode = mode;
    request(s_override ? s_override : nes_video_geometry_width_for(mode, s_draw_w, s_draw_h));
}

int  cyc_video_mode(void) { return s_mode; }

void cyc_video_request_width(int width)
{
    s_override = 0;
    s_mode = NES_VIDEO_STOCK;
    request(width);
}

void cyc_video_override_width(int width)
{
    if (width < 0) return;
    s_override = width ? nes_video_geometry_clamp_width(width) : 0;
    request(s_override ? s_override : nes_video_geometry_width_for(s_mode, s_draw_w, s_draw_h));
}

int cyc_video_width(void) { return s_width; }
int cyc_video_native_x0(void) { return (s_width - 256) / 2; }

void cyc_video_window_ready(void) { s_window = true; }

void cyc_video_window_resized(int drawable_w, int drawable_h)
{
    s_draw_w = drawable_w;
    s_draw_h = drawable_h;
    if (s_override) request(s_override);
    else if (s_mode == NES_VIDEO_FIT) request(nes_video_geometry_width_for(s_mode, drawable_w, drawable_h));
}

void cyc_video_drawable_size(int *drawable_w, int *drawable_h)
{
    if (drawable_w) *drawable_w = s_draw_w;
    if (drawable_h) *drawable_h = s_draw_h;
}

bool cyc_video_restore(int mode, int width, int drawable_w, int drawable_h)
{
    if (mode < 0 || mode >= NES_VIDEO_MODE_COUNT || width < 256 ||
        width > CYC_VIDEO_MAX_WIDTH || (width & 1) || drawable_w < 0 || drawable_h < 0 ||
        (!drawable_w != !drawable_h)) return false;
    s_mode = mode;
    s_draw_w = drawable_w;
    s_draw_h = drawable_h;
    s_pending = false;
    commit(width);
    return true;
}

bool cyc_video_apply_pending(void)
{
    if (!s_pending) return false;
    s_pending = false;
    int before = s_width;
    commit(s_pending_width);
    return s_width != before;
}
