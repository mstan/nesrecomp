#include "cyc_hd_gpu_cache.h"
#include <stdlib.h>
#include <string.h>

void cyc_hd_gpu_cache_destroy(HdGpuPageCache *cache)
{
    if (!cache) return;
    free(cache->image_meta); free(cache->page_image); free(cache->page_slots);
    free(cache->slot_pages); free(cache->needed_ids); free(cache->slot_last);
    free(cache->needed);
    memset(cache, 0, sizeof(*cache));
}

static void clear_needed(HdGpuPageCache *cache)
{
    if (cache->needed) memset(cache->needed, 0, cache->page_count);
    cache->needed_count = 0;
}

static bool catalog(HdGpuPageCache *next, const HdJobFrame *frame,
                     uint32_t slots, uint64_t max_buffer_texels)
{
    uint64_t pages = 0;
    if (frame->image_count > HD_GPU_PAGE_MAX_LOGICAL ||
        (uint64_t)frame->image_count * 4 > max_buffer_texels) return false;
    for (uint32_t i = 0; i < frame->image_count; ++i) {
        const HdJobImage *im = &frame->images[i];
        if (!hd_image_storage_valid(im) || im->brightness > 65025 ||
            (uint64_t)im->width * im->height > SIZE_MAX / sizeof(uint32_t)) return false;
        uint64_t across = ((uint64_t)im->width + HD_GPU_PAGE_SIZE - 1) / HD_GPU_PAGE_SIZE;
        uint64_t down = ((uint64_t)im->height + HD_GPU_PAGE_SIZE - 1) / HD_GPU_PAGE_SIZE;
        uint64_t count = across * down;
        if (count > HD_GPU_PAGE_MAX_LOGICAL || pages + count > HD_GPU_PAGE_MAX_LOGICAL ||
            pages + count > max_buffer_texels) return false;
        pages += count;
    }
    next->pack_generation = frame->pack_generation;
    next->image_count = frame->image_count; next->page_count = (uint32_t)pages;
    next->slot_count = slots;
    size_t image_words = (size_t)frame->image_count * 4;
    size_t page_words = pages ? (size_t)pages : 1;
    next->image_meta = (uint32_t *)malloc((image_words ? image_words : 1) * sizeof(uint32_t));
    next->page_image = (uint32_t *)malloc(page_words * sizeof(uint32_t));
    next->page_slots = (uint32_t *)malloc(page_words * sizeof(uint32_t));
    next->slot_pages = (uint32_t *)malloc((size_t)slots * sizeof(uint32_t));
    next->needed_ids = (uint32_t *)malloc((size_t)slots * sizeof(uint32_t));
    next->slot_last = (uint64_t *)calloc(slots, sizeof(uint64_t));
    next->needed = (uint8_t *)calloc(page_words, 1);
    if (!next->image_meta || !next->page_image || !next->page_slots || !next->slot_pages ||
        !next->needed_ids || !next->slot_last || !next->needed) return false;
    memset(next->page_slots, 0xff, page_words * sizeof(uint32_t));
    memset(next->slot_pages, 0xff, (size_t)slots * sizeof(uint32_t));
    uint32_t first = 0;
    for (uint32_t i = 0; i < frame->image_count; ++i) {
        const HdJobImage *im = &frame->images[i];
        uint32_t *meta = next->image_meta + (size_t)i * 4;
        meta[0] = first; meta[1] = im->width; meta[2] = im->height; meta[3] = im->brightness;
        uint64_t across = ((uint64_t)im->width + HD_GPU_PAGE_SIZE - 1) / HD_GPU_PAGE_SIZE;
        uint64_t down = ((uint64_t)im->height + HD_GPU_PAGE_SIZE - 1) / HD_GPU_PAGE_SIZE;
        uint32_t end = first + (uint32_t)(across * down);
        for (uint32_t p = first; p < end; ++p) next->page_image[p] = i;
        first = end;
    }
    return true;
}

static bool need_page(HdGpuPageCache *cache, uint32_t page)
{
    if (page >= cache->page_count) return false;
    if (cache->needed[page]) return true;
    if (cache->needed_count >= cache->slot_count) return false;
    cache->needed[page] = 1;
    cache->needed_ids[cache->needed_count++] = page;
    return true;
}

/* Source coordinates are signed until clipping is complete. Bounds are
 * exclusive, so one native cell never pulls in its neighboring source page. */
static bool need_rect(HdGpuPageCache *cache, uint32_t image,
                      int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    if (image >= cache->image_count) return false;
    const uint32_t *meta = cache->image_meta + (size_t)image * 4;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > meta[1]) x1 = meta[1];
    if (y1 > meta[2]) y1 = meta[2];
    if (x0 >= x1 || y0 >= y1) return true;
    uint64_t across = ((uint64_t)meta[1] + HD_GPU_PAGE_SIZE - 1) / HD_GPU_PAGE_SIZE;
    uint64_t first_x = (uint64_t)x0 / HD_GPU_PAGE_SIZE;
    uint64_t last_x = (uint64_t)(x1 - 1) / HD_GPU_PAGE_SIZE;
    uint64_t first_y = (uint64_t)y0 / HD_GPU_PAGE_SIZE;
    uint64_t last_y = (uint64_t)(y1 - 1) / HD_GPU_PAGE_SIZE;
    for (uint64_t y = first_y; y <= last_y; ++y)
        for (uint64_t x = first_x; x <= last_x; ++x) {
            uint64_t page = meta[0] + y * across + x;
            if (page >= cache->page_count || !need_page(cache, (uint32_t)page)) return false;
        }
    return true;
}

static bool need_tile(HdGpuPageCache *cache, const HdJobFrame *frame,
                      uint32_t flags, uint32_t image, uint32_t uv, bool sprite)
{
    unsigned shift = sprite ? HD_JOB_SP_SHIFT : HD_JOB_BG_SHIFT;
    unsigned mode = (flags >> shift) & HD_JOB_MODE_MASK;
    if (mode == HD_JOB_SKIP || mode == HD_JOB_SOLID) return true;
    if (mode != HD_JOB_IMAGE || image >= cache->image_count) return false;
    uint32_t x = uv & 255, y = uv >> 8;
    const uint32_t *meta = cache->image_meta + (size_t)image * 4;
    if (x >= 8 || y >= 8 || meta[1] < 8 * frame->source_scale ||
        meta[2] < 8 * frame->source_scale) return false;
    if (sprite && (flags & HD_JOB_HMIRROR)) x = 7 - x;
    /* V mirror changes only the order inside this already-fetched row. */
    int64_t x0 = (int64_t)x * frame->source_scale;
    int64_t y0 = (int64_t)y * frame->source_scale;
    return need_rect(cache, image, x0, y0,
                     x0 + frame->source_scale, y0 + frame->source_scale);
}

static bool mark_frame(HdGpuPageCache *cache, const HdJobFrame *frame)
{
    clear_needed(cache);
    for (uint32_t i = 0; i < frame->background_count; ++i) {
        const HdJobBackground *b = &frame->backgrounds[i];
        if (b->image >= cache->image_count || b->brightness > 65025 || b->blend > 2)
            return false;
        uint32_t top = i < frame->under_end ? frame->hud_rows : 0;
        int64_t x0 = (int64_t)b->offset_x * frame->source_scale;
        int64_t y0 = ((int64_t)top + b->offset_y) * frame->source_scale;
        int64_t x1 = ((int64_t)frame->width + b->offset_x) * frame->source_scale;
        int64_t y1 = ((int64_t)frame->height + b->offset_y) * frame->source_scale;
        if (!need_rect(cache, b->image, x0, y0, x1, y1)) return false;
    }
    size_t pixels = (size_t)frame->width * frame->height;
    for (size_t i = 0; i < pixels; ++i) {
        const HdJobPixel *p = &frame->pixels[i];
        uint32_t count=hd_job_extra_count(p->flags),start=hd_job_extra_start(p->flags);
        if((!count && start) || start>frame->sprite_count || count>frame->sprite_count-start)return false;
        for(uint32_t k=0;k<count;k++) {
            const HdJobSprite *sp=&frame->sprites[start+k];
            if((sp->flags&~124u) || !need_tile(cache,frame,sp->flags,sp->image,sp->uv,true))return false;
        }
        if (!need_tile(cache, frame, p->flags, p->bg_image, p->bg_uv, false) ||
            !need_tile(cache, frame, p->flags, p->sp_image, p->sp_uv, true)) return false;
    }
    return true;
}

bool cyc_hd_gpu_cache_prepare(HdGpuPageCache *cache, const HdJobFrame *frame,
                              uint32_t slots, uint64_t max_buffer_texels)
{
    if (!cache) return false;
    if (!frame || !slots || slots > HD_GPU_PAGE_MAX_SLOTS || !max_buffer_texels ||
        !frame->width || !frame->height || !frame->pixels ||
        !frame->source_scale || frame->source_scale > 64 ||
        (uint64_t)frame->width * frame->height > SIZE_MAX / sizeof(HdJobPixel) ||
        (frame->image_count && !frame->images) ||
        (frame->background_count && !frame->backgrounds) ||
        (frame->sprite_count && !frame->sprites) || frame->has_behind>1 ||
        frame->behind_end > frame->under_end || frame->under_end > frame->mid_end || frame->mid_end > frame->background_count ||
        frame->hud_rows > frame->height) {
        clear_needed(cache); return false;
    }
    bool rebuild = !cache->image_meta || cache->pack_generation != frame->pack_generation ||
                   cache->image_count != frame->image_count || cache->slot_count != slots;
    if (rebuild) {
        HdGpuPageCache next = {0};
        if (!catalog(&next, frame, slots, max_buffer_texels) || !mark_frame(&next, frame)) {
            cyc_hd_gpu_cache_destroy(&next); clear_needed(cache); return false;
        }
        cyc_hd_gpu_cache_destroy(cache);
        *cache = next;
        return true;
    }
    if ((uint64_t)cache->image_count * 4 > max_buffer_texels ||
        cache->page_count > max_buffer_texels || !mark_frame(cache, frame)) {
        clear_needed(cache); return false;
    }
    return true;
}

static uint64_t stamp(HdGpuPageCache *cache)
{
    if (cache->tick == UINT64_MAX) {
        /* Rare wrap preserves exact resident LRU order, with slot order as a
         * stable tie-break. Work is bounded by the 2048-slot hardware cap. */
        uint64_t old[HD_GPU_PAGE_MAX_SLOTS];
        memcpy(old, cache->slot_last, (size_t)cache->slot_count * sizeof(uint64_t));
        for (uint32_t i = 0; i < cache->slot_count; ++i) {
            uint64_t rank = 1;
            if (cache->slot_pages[i] == HD_GPU_PAGE_ABSENT) {
                cache->slot_last[i] = 0; continue;
            }
            for (uint32_t j = 0; j < cache->slot_count; ++j)
                if (cache->slot_pages[j] != HD_GPU_PAGE_ABSENT &&
                    (old[j] < old[i] || (old[j] == old[i] && j < i))) ++rank;
            cache->slot_last[i] = rank;
        }
        cache->tick = cache->slot_count;
    }
    return ++cache->tick;
}

bool cyc_hd_gpu_cache_reserve(HdGpuPageCache *cache, uint32_t page,
                              uint32_t *slot, bool *upload)
{
    if (!cache || !slot || !upload || page >= cache->page_count ||
        !cache->needed || !cache->needed[page] || !cache->needed_count) return false;
    uint32_t hit = cache->page_slots[page];
    if (hit != HD_GPU_PAGE_ABSENT) {
        if (hit >= cache->slot_count || cache->slot_pages[hit] != page) return false;
        cache->slot_last[hit] = stamp(cache);
        *slot = hit; *upload = false; return true;
    }
    uint32_t selected = HD_GPU_PAGE_ABSENT;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < cache->slot_count; ++i) {
        uint32_t old = cache->slot_pages[i];
        if (old == HD_GPU_PAGE_ABSENT) { selected = i; break; }
        if (old >= cache->page_count) return false;
        if (!cache->needed[old] &&
            (selected == HD_GPU_PAGE_ABSENT || cache->slot_last[i] < oldest)) {
            selected = i; oldest = cache->slot_last[i];
        }
    }
    if (selected == HD_GPU_PAGE_ABSENT) return false;
    uint32_t old = cache->slot_pages[selected];
    if (old != HD_GPU_PAGE_ABSENT) cache->page_slots[old] = HD_GPU_PAGE_ABSENT;
    cache->page_slots[page] = selected; cache->slot_pages[selected] = page;
    cache->slot_last[selected] = stamp(cache);
    *slot = selected; *upload = true; return true;
}
