#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "hdpack_jobs.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    HD_GPU_PAGE_SIZE = 128,
    HD_GPU_PAGE_MAX_SLOTS = 2048,
    HD_GPU_PAGE_MAX_LOGICAL = 1024 * 1024
};
#define HD_GPU_PAGE_ABSENT UINT32_MAX

/* Metadata only. Source pixels remain owned by the immutable HdJobFrame.
 * Zero-initialize before first prepare; destroy is safe on an empty cache.
 * image_meta stores four uint32 words per image: logical-page base, width,
 * height, brightness. Pages are row-major within each image. */
typedef struct HdGpuPageCache {
    uint64_t pack_generation, tick;
    uint32_t image_count, page_count, slot_count, needed_count;
    uint32_t *image_meta, *page_image, *page_slots, *slot_pages, *needed_ids;
    uint64_t *slot_last;
    uint8_t *needed;
} HdGpuPageCache;

/* Validate/catalog a new pack, then mark the complete current source working
 * set before reserve may evict anything. Pixel jobs mark their source cell;
 * background jobs mark only the clipped visible source rectangle (including
 * the under-background HUD exclusion). This is independent of output scale.
 * max_buffer_texels bounds each uint32 metadata buffer, slot_count is 1..2048,
 * and logical pages are capped at 1M. Refusal performs no page eviction. */
bool cyc_hd_gpu_cache_prepare(HdGpuPageCache *cache, const HdJobFrame *frame,
                              uint32_t slot_count, uint64_t max_buffer_texels);

/* Only pages marked by the last successful prepare may be reserved. Hits do
 * not upload; misses evict the least recently used page outside the complete
 * needed set. Caller uploads the returned logical source page when upload is
 * true, before drawing. The GL array slot is a 128x128 source-pixel page. */
bool cyc_hd_gpu_cache_reserve(HdGpuPageCache *cache, uint32_t page,
                              uint32_t *slot, bool *upload);
void cyc_hd_gpu_cache_destroy(HdGpuPageCache *cache);

#ifdef __cplusplus
}
#endif
