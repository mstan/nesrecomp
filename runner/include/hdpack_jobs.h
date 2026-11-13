#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HD_JOB_SKIP = 0, HD_JOB_SOLID = 1, HD_JOB_IMAGE = 2,
    HD_JOB_BG_SHIFT = 0, HD_JOB_SP_SHIFT = 2,
    HD_JOB_MODE_MASK = 3, HD_JOB_HMIRROR = 16, HD_JOB_VMIRROR = 32,
    HD_JOB_BEHIND = 64, HD_JOB_EXTRA_SHIFT = 7, HD_JOB_EXTRA_START = 9
};
#define HD_JOB_NO_IMAGE UINT32_MAX
#define HD_IMAGE_PAGE_SIZE 128u

/* Immutable lossless storage for repeated source regions. Logical pages may
 * share an allocation; ownership is kept by the retained pack, not the GPU. */
typedef struct {
    uint32_t columns,rows,unique_count;
    const uint32_t *const *map;
    uint32_t **unique;
} HdImagePages;

/* Immutable source pixels. Brightness is normalized to [0,65025], which
 * preserves saturated HDR channel results while bounding shader products.
 * Tile images carry tile brightness; background images carry 255, and each
 * background draw supplies its own brightness. IDs index this pack's array. */
typedef struct {
    const uint32_t *pixels;
    uint32_t width, height, brightness;
    const HdImagePages *pages;
} HdJobImage;

static inline int hd_image_storage_valid(const HdJobImage *image) {
    if(!image->width || !image->height)return 0;
    if(!image->pages)return image->pixels!=NULL;
    const HdImagePages *p=image->pages;
    if(image->pixels || !p->map || !p->unique_count ||
       p->columns!=((uint64_t)image->width+127)/128 || p->rows!=((uint64_t)image->height+127)/128)return 0;
    if((uint64_t)p->columns*p->rows>SIZE_MAX/sizeof(*p->map) ||
       p->unique_count>(uint64_t)p->columns*p->rows)return 0;
    for(size_t i=0;i<(size_t)p->columns*p->rows;i++)if(!p->map[i])return 0;
    return 1;
}
static inline uint32_t hd_image_pixel(const uint32_t *flat,const HdImagePages *pages,
                                     uint32_t width,unsigned x,unsigned y) {
    if(!pages)return flat[(size_t)y*width+x];
    return pages->map[(size_t)(y/128)*pages->columns+x/128][(y%128)*128+x%128];
}

/* Eight uint32 words, in this order, for two RGBA32UI job texels. Each layer
 * mode is skip/solid/image. An image's uv packs native tile offsets x|(y<<8).
 * H mirror reverses the source cell and subpixel; V mirror reverses only the
 * subpixel row because the native PPU already selected the flipped row.
 * Flags bits7..8 give the optional extra sprite count (0..3); bits9..31
 * index the first entry in frame.sprites. Bit6 marks a behind-BG sprite.
 * The first and extra descriptors preserve back-to-front OAM order. */
typedef struct {
    uint32_t base, bg_image, bg_uv, bg_color;
    uint32_t sp_image, sp_uv, sp_color, flags;
} HdJobPixel;

/* Optional descriptors preserve the eight-word common pixel representation.
 * Extra layers are in back-to-front OAM order, like the first descriptor. */
typedef struct { uint32_t image,uv,color,flags; } HdJobSprite;
static inline uint32_t hd_job_extra_count(uint32_t flags){return (flags>>HD_JOB_EXTRA_SHIFT)&3u;}
static inline uint32_t hd_job_extra_start(uint32_t flags){return flags>>HD_JOB_EXTRA_START;}

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
    uint32_t sprite_count,behind_end,has_behind;
    const HdJobSprite *sprites;
    uint32_t native_hud_rows,native_hud_columns; /* actual native pixels own this rectangle */
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
