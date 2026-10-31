#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HD_JOB_SKIP = 0, HD_JOB_SOLID = 1, HD_JOB_IMAGE = 2,
    HD_JOB_BG_SHIFT = 0, HD_JOB_SP_SHIFT = 2,
    HD_JOB_MODE_MASK = 3, HD_JOB_HMIRROR = 16, HD_JOB_VMIRROR = 32
};
#define HD_JOB_NO_IMAGE UINT32_MAX

/* Immutable source pixels. Brightness is normalized to [0,65025], which
 * preserves saturated HDR channel results while bounding shader products.
 * Tile images carry tile brightness; background images carry 255, and each
 * background draw supplies its own brightness. IDs index this pack's array. */
typedef struct {
    const uint32_t *pixels;
    uint32_t width, height, brightness;
} HdJobImage;

/* Eight uint32 words, in this order, for two RGBA32UI job texels. Each layer
 * mode is skip/solid/image. An image's uv packs native tile offsets x|(y<<8).
 * H mirror reverses the source cell and subpixel; V mirror reverses only the
 * subpixel row because the native PPU already selected the flipped row. */
typedef struct {
    uint32_t base, bg_image, bg_uv, bg_color;
    uint32_t sp_image, sp_uv, sp_color, flags;
} HdJobPixel;

typedef struct {
    uint32_t image;
    int32_t offset_x, offset_y; /* native position added before source sampling */
    uint32_t brightness, blend; /* premultiplied over=0, saturating add=1/sub=2 */
} HdJobBackground;

typedef struct HdJobFrame {
    uint64_t serial, pack_generation;
    uint32_t width, height, source_scale;
    uint32_t image_count, background_count, under_end, mid_end, hud_rows;
    const HdJobImage *images;
    const HdJobPixel *pixels;
    const HdJobBackground *backgrounds;
} HdJobFrame;

/* Resolves the native side channel and all conditions into a new caller-owned
 * reference. No output-scale work or enhanced framebuffer is produced here.
 * The frame contains no live HdPixel, guest-memory or condition pointers.
 * Retained frames and their immutable source pixels survive pack unload/load.
 * Retain/release run on the producer thread; other threads may read a const
 * frame while its owner keeps a reference. Allocation failure returns NULL. */
const HdJobFrame *hdpack_prepare_jobs(const uint32_t *native, int native_w);
void hdpack_jobs_retain(const HdJobFrame *frame);
void hdpack_jobs_release(const HdJobFrame *frame);

/* Portable consumer of exactly the same published jobs. Output dimensions are
 * width*scale by height*scale, with scale in [1,64]. Source subpixels use
 * floor(output_subpixel*source_scale/scale), sampling authored pixels only.
 * Caller owns bounded output allocation/admission. Returns 0 on success. */
int hdpack_render_jobs(const HdJobFrame *frame, unsigned scale,
                       uint32_t *out, size_t pitch_pixels);

#ifdef __cplusplus
}
#endif
