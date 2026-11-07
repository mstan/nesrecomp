/* video_test.c - the presented width (cyc_video.h, common/nes_video_geometry.h):
 * the widths of every mode, Fit's clamp to the window, request queuing after
 * the window exists and the one apply point, and where a compositor placed the
 * native picture (cyc_render.h) (CTest cyc_video_test). */
#include "cyc_video.h"
#include "cyc_render.h"
#include "cyc_ring.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* A compositor whose camera stops at an area's edge: it paints (or leaves the
 * picture to the native one) and may declare where native column 0 went. */
static int s_paint, s_declare, s_origin;
static int anchored(uint32_t *out, int width, int height, int native_x0, const uint32_t *native, void *user)
{
    (void)out; (void)width; (void)height; (void)native_x0; (void)native; (void)user;
    if (s_paint && s_declare) cyc_render_set_native_origin(s_origin);
    return s_paint;
}

/* A stock-width compositor: paints one marked pixel; records what it was given */
static int s_stock_calls, s_stock_paint, s_stock_args_ok, s_stock_native_x0;
static int stock(uint32_t *out, int width, int height, int native_x0, const uint32_t *native, void *user)
{
    (void)user;
    s_stock_calls++;
    s_stock_native_x0 = native_x0;
    s_stock_args_ok = (width == 256 && height == 240 && native_x0 == 0 && native == cyc_frame_argb() && out[5] == 0xFF000000u);
    out[0] = 0xFF123456u;
    return s_stock_paint;
}

int main(void)
{
    /* the numbers: square pixels, 240 rows, even widths, STOCK explicit */
    CHECK(nes_video_geometry_width_for(NES_VIDEO_STOCK, 0, 0) == 256);
    CHECK(nes_video_geometry_width_for(NES_VIDEO_16_9, 0, 0) == 426);
    CHECK(nes_video_geometry_width_for(NES_VIDEO_21_9, 0, 0) == 560);
    CHECK(nes_video_geometry_width_for(NES_VIDEO_32_9, 0, 0) == 854);
    CHECK(nes_video_geometry_width_for(NES_VIDEO_FIT, 0, 0) == 256);          /* no window: 16:15 */
    CHECK(nes_video_geometry_width_for(NES_VIDEO_FIT, 1024, 960) == 256);     /* 16:15 */
    CHECK(nes_video_geometry_width_for(NES_VIDEO_FIT, 640, 480) == 320);      /* 4:3 fills */
    CHECK(nes_video_geometry_width_for(NES_VIDEO_FIT, 1920, 1080) == 426);    /* 16:9 */
    CHECK(nes_video_geometry_width_for(NES_VIDEO_FIT, 3440, 1440) == 574);    /* 43:18 */
    CHECK(nes_video_geometry_width_for(NES_VIDEO_FIT, 5120, 1440) == 854);    /* 32:9 */
    CHECK(nes_video_geometry_width_for(NES_VIDEO_FIT, 8000, 1000) == 854);    /* clamped at 32:9 */
    CHECK(nes_video_geometry_width_for(NES_VIDEO_FIT, 500, 1000) == 256);     /* clamped at 16:15 */
    for (int w = 100; w < 3000; w += 7)
        for (int h = 100; h < 2000; h += 13) {
            int x = nes_video_geometry_width_for(NES_VIDEO_FIT, w, h);
            CHECK(x >= 256 && x <= NES_VIDEO_MAX_WIDTH && !(x & 1));
        }
    int m;
    CHECK(nes_video_geometry_parse("16-9", &m) && m == NES_VIDEO_16_9);
    CHECK(nes_video_geometry_parse("21x9", &m) && m == NES_VIDEO_21_9);
    CHECK(nes_video_geometry_parse("32_9", &m) && m == NES_VIDEO_32_9);
    CHECK(nes_video_geometry_parse("Adaptive", &m) && m == NES_VIDEO_FIT);
    CHECK(nes_video_geometry_parse("off", &m) && m == NES_VIDEO_STOCK);
    CHECK(!nes_video_geometry_parse("17:9", &m));

    /* the state: stock by default; before the window a request applies at once */
    CHECK(cyc_video_width() == 256 && cyc_video_mode() == NES_VIDEO_STOCK && cyc_video_native_x0() == 0);
    cyc_video_set_mode(NES_VIDEO_21_9);
    CHECK(cyc_video_width() == 560 && cyc_video_native_x0() == 152);
    CHECK(!cyc_video_apply_pending());
    /* with a window a request waits for the apply point */
    cyc_video_window_ready();
    cyc_video_window_resized(1920, 1080);
    CHECK(cyc_video_width() == 560);                  /* not Fit: the window does not matter */
    cyc_video_set_mode(NES_VIDEO_FIT);
    CHECK(cyc_video_width() == 560);                  /* queued */
    CHECK(cyc_video_apply_pending() && cyc_video_width() == 426);
    CHECK(!cyc_video_apply_pending());
    cyc_video_window_resized(5120, 1440);
    CHECK(cyc_video_width() == 426 && cyc_video_apply_pending() && cyc_video_width() == 854);
    cyc_video_window_resized(5120, 1440);             /* the same size: nothing queued */
    CHECK(!cyc_video_apply_pending());
    cyc_video_set_mode(NES_VIDEO_FIT);                /* idempotent */
    CHECK(!cyc_video_apply_pending());
    /* a resize and back before the apply point queues nothing net */
    cyc_video_window_resized(1920, 1080);
    cyc_video_window_resized(5120, 1440);
    CHECK(!cyc_video_apply_pending() && cyc_video_width() == 854);
    cyc_video_set_mode(NES_VIDEO_STOCK);
    CHECK(cyc_video_apply_pending() && cyc_video_width() == 256);
    cyc_video_request_width(301);                     /* even, fixed */
    CHECK(cyc_video_apply_pending() && cyc_video_width() == 300 && cyc_video_mode() == NES_VIDEO_STOCK);
    /* every width change is in the ring */
    CHECK(cyc_ring_kind_total(CYC_EV_VIDEO) == 5);

    /* the native origin the host maps the Zapper and crosshair through */
    int w, h;
    cyc_video_request_width(560);
    CHECK(cyc_video_apply_pending() && cyc_video_width() == 560);
    CHECK(cyc_render_native_origin(560) == 152);      /* nothing composed: centered */
    CHECK(cyc_render_native_origin(256) == 0);
    cyc_render_set_compositor(anchored, NULL);
    s_paint = s_declare = 1;
    s_origin = 40;
    cyc_render_present(&w, &h);
    CHECK(w == 560 && cyc_render_native_origin(560) == 40);
    CHECK(cyc_render_native_origin(426) == 85);       /* another width: centered */
    s_origin = 304;                                   /* cached until the next frame */
    cyc_render_present(&w, &h);
    CHECK(cyc_render_native_origin(560) == 40);
    cyc_render_frame_done();
    cyc_render_present(&w, &h);
    CHECK(cyc_render_native_origin(560) == 304);
    s_declare = 0;                                    /* painted, nothing declared */
    cyc_render_frame_done();
    cyc_render_present(&w, &h);
    CHECK(cyc_render_native_origin(560) == 152);
    s_declare = 1;
    s_paint = 0;                                      /* pillarboxed: a declaration is ignored */
    cyc_render_frame_done();
    cyc_render_present(&w, &h);
    CHECK(cyc_render_native_origin(560) == 152);
    cyc_render_set_compositor(NULL, NULL);

    /* stock-width compositing: off, the compositor is not called at 256; on, it is, with origin 0 and the native
     * picture, and a decline presents the native picture itself */
    cyc_video_request_width(256);
    CHECK(cyc_video_apply_pending() && cyc_video_width() == 256);
    cyc_render_set_compositor(stock, NULL);
    s_stock_calls = 0;
    s_stock_paint = 1;
    cyc_render_frame_done();
    const uint32_t *pic = cyc_render_present(&w, &h);
    CHECK(w == 256 && h == 240 && s_stock_calls == 0 && pic == cyc_frame_argb());
    CHECK(!cyc_render_stock_compositing());
    cyc_render_set_stock_compositing(true);
    CHECK(cyc_render_stock_compositing());
    pic = cyc_render_present(&w, &h);
    CHECK(w == 256 && h == 240 && s_stock_calls == 1 && pic != cyc_frame_argb() && pic[0] == 0xFF123456u);
    CHECK(s_stock_args_ok);
    CHECK(cyc_render_present(&w, &h) == pic && s_stock_calls == 1);   /* composed once per frame */
    cyc_render_frame_done();
    s_stock_paint = 0;                                                /* declined: the native picture */
    pic = cyc_render_present(&w, &h);
    CHECK(s_stock_calls == 2 && pic == cyc_frame_argb());
    CHECK(cyc_render_present(&w, &h) == cyc_frame_argb() && s_stock_calls == 2);
    CHECK(cyc_render_native_origin(256) == 0);
    cyc_render_set_stock_compositing(false);
    cyc_render_frame_done();
    CHECK(cyc_render_present(&w, &h) == cyc_frame_argb() && s_stock_calls == 2);
    cyc_render_set_stock_compositing(true);
    cyc_video_request_width(560);                                     /* wide: the same compositor, the wide contract */
    CHECK(cyc_video_apply_pending() && cyc_video_width() == 560);
    s_stock_paint = 1;
    cyc_render_present(&w, &h);
    CHECK(w == 560 && s_stock_native_x0 == 152);
    cyc_render_set_compositor(NULL, NULL);
    cyc_render_set_stock_compositing(false);
    /* Playback restores the committed width, including a Fit width which
     * differs from the default drawable, and cancels stale resize requests. */
    cyc_video_set_mode(NES_VIDEO_FIT);
    CHECK(cyc_video_restore(NES_VIDEO_FIT, 426, 1920, 1080));
    int dw, dh;cyc_video_drawable_size(&dw, &dh);
    CHECK(cyc_video_mode()==NES_VIDEO_FIT && cyc_video_width()==426 && dw==1920 && dh==1080);
    CHECK(!cyc_video_apply_pending());
    CHECK(!cyc_video_restore(-1,426,1920,1080));
    CHECK(!cyc_video_restore(NES_VIDEO_FIT,425,1920,1080));
    CHECK(!cyc_video_restore(NES_VIDEO_FIT,866,1920,1080));
    CHECK(!cyc_video_restore(NES_VIDEO_FIT,426,1920,0));
    CHECK(!cyc_video_restore(NES_VIDEO_FIT,426,-1,1080));
    CHECK(cyc_video_mode()==NES_VIDEO_FIT && cyc_video_width()==426);
    cyc_video_window_resized(3440,1440);
    CHECK(cyc_video_apply_pending() && cyc_video_width()==574);
    printf("%s (%d failures)\n", failures ? "FAILED" : "cyc_video_test passed", failures);
    return failures != 0;
}
