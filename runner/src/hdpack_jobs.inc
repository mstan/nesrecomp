/* Shared immutable presentation jobs. Included after the ordinary matcher and
 * layered lookup plan; neither producer nor consumer advances the machine. */
#include <limits.h>

typedef struct HdJobPack {
    unsigned refs;
    uint64_t generation;
    uint32_t count;
    HdJobImage *images;
} HdJobPack;

typedef struct HdOwnedJobFrame {
    HdJobFrame frame;              /* public const view, first member */
    unsigned refs;
    HdJobPack *pack;
    HdJobPixel *pixels;
    HdJobBackground *backgrounds;
    HdJobSprite *sprites;
    size_t pixel_capacity, background_capacity, sprite_capacity;
    struct HdOwnedJobFrame *next;
} HdOwnedJobFrame;

static HdJobPack *s_job_pack;
static HdOwnedJobFrame *s_job_free;
static unsigned s_job_free_count;
static uint64_t s_job_generation, s_job_serial;

static uint32_t job_brightness(int value)
{
    /* Beyond 255*255, every nonzero 8-bit channel already saturates. */
    return value < 0 ? 0u : value > 65025 ? 65025u : (uint32_t)value;
}

static void job_pack_release(HdJobPack *pack)
{
    if (!pack || --pack->refs) return;
    for (uint32_t i = 0; i < pack->count; ++i) {
        free((void *)pack->images[i].pixels);image_pages_free(pack->images[i].pages);
    }
    free(pack->images);
    free(pack);
}

static int hd_jobs_owns_assets(void) { return s_job_pack != NULL; }

static void hd_jobs_unload(void)
{
    HdJobPack *old = s_job_pack;
    s_job_pack = NULL;
    job_pack_release(old);
    while (s_job_free) {
        HdOwnedJobFrame *next = s_job_free->next;
        free(s_job_free->pixels);
        free(s_job_free->backgrounds);
        free(s_job_free->sprites);
        free(s_job_free);
        s_job_free = next;
    }
    s_job_free_count = 0;
}

static HdJobPack *job_pack_get(void)
{
    if (s_job_pack) return s_job_pack;
    size_t count = (size_t)s_all_count + (size_t)s_bgimg_count;
    if (count > UINT32_MAX || count > SIZE_MAX / sizeof(HdJobImage)) return NULL;
    HdJobPack *pack = (HdJobPack *)calloc(1, sizeof(*pack));
    if (!pack) return NULL;
    pack->images = (HdJobImage *)calloc(count ? count : 1, sizeof(*pack->images));
    if (!pack->images) { free(pack); return NULL; }
    /* These immutable allocations are currently owned by HdTile/HdBgImg.
     * Commit ownership only after the complete asset list has been checked. */
    size_t at = 0;
    for (int i = 0; i < s_all_count; ++i) {
        HdTile *t = s_all[i];
        if (!t || !t->data || t->w <= 0 || t->h <= 0) goto fail;
        HdJobImage *im = &pack->images[at++];
        im->pixels = t->data; im->width = (uint32_t)t->w; im->height = (uint32_t)t->h;
        im->brightness = job_brightness(t->brightness);
    }
    for (int i = 0; i < s_bgimg_count; ++i) {
        HdBgImg *b = s_bgimgs[i];
        /* A failed PNG lookup remains in the loader cache, although every
         * background referencing it was dropped. It owns no usable image
         * and must not disable jobs for the rest of a valid pack. */
        if (!b || (!b->px && !b->pages) || b->w <= 0 || b->h <= 0) continue;
        HdJobImage *im = &pack->images[at++];
        im->pixels = b->px; im->pages=b->pages; im->width = (uint32_t)b->w; im->height = (uint32_t)b->h;
        im->brightness = 255;
    }
    for (int i = 0; i < s_all_count; ++i) s_all[i]->job_image = (uint32_t)i;
    uint32_t bg_image = (uint32_t)s_all_count;
    for (int i = 0; i < s_bgimg_count; ++i) {
        HdBgImg *b = s_bgimgs[i];
        if (!b) continue;
        b->job_image = HD_JOB_NO_IMAGE;
        if ((b->px || b->pages) && b->w > 0 && b->h > 0) b->job_image = bg_image++;
    }
    pack->refs = 1;
    if (!++s_job_generation) ++s_job_generation;
    pack->generation = s_job_generation;
    pack->count = (uint32_t)at;
    s_job_pack = pack;
    return pack;
fail:
    free(pack->images); free(pack); return NULL;
}

void hdpack_jobs_retain(const HdJobFrame *frame)
{
    if (frame) ++((HdOwnedJobFrame *)frame)->refs;
}

void hdpack_jobs_release(const HdJobFrame *frame)
{
    if (!frame) return;
    HdOwnedJobFrame *owned = (HdOwnedJobFrame *)frame;
    if (--owned->refs) return;
    job_pack_release(owned->pack);
    owned->pack = NULL;
    /* Reuse only reclaimed frames: a retained publication never changes.
     * Two scratch allocations bound paused/queued-frame recycling overhead. */
    if (s_job_free_count < 2) {
        owned->next = s_job_free; s_job_free = owned; ++s_job_free_count;
    } else {
        free(owned->pixels); free(owned->backgrounds); free(owned->sprites); free(owned);
    }
}

static HdOwnedJobFrame *job_frame_allocate(size_t pixels, size_t backgrounds)
{
    HdOwnedJobFrame *owned = s_job_free;
    if (owned) { s_job_free = owned->next; --s_job_free_count; }
    else owned = (HdOwnedJobFrame *)calloc(1, sizeof(*owned));
    if (!owned) return NULL;
    owned->refs = 1; owned->next = NULL; owned->pack = NULL;
    if (pixels > owned->pixel_capacity) {
        HdJobPixel *next = (HdJobPixel *)realloc(owned->pixels, pixels * sizeof(*next));
        if (!next) goto fail;
        owned->pixels = next; owned->pixel_capacity = pixels;
    }
    if (backgrounds > owned->background_capacity) {
        HdJobBackground *next = (HdJobBackground *)realloc(owned->backgrounds,
                                                         backgrounds * sizeof(*next));
        if (!next) goto fail;
        owned->backgrounds = next; owned->background_capacity = backgrounds;
    }
    if(pixels>SIZE_MAX/3/sizeof(HdJobSprite))goto fail;
    if(pixels*3>owned->sprite_capacity) {
        HdJobSprite *next=realloc(owned->sprites,pixels*3*sizeof(*next));
        if(!next)goto fail;
        owned->sprites=next;owned->sprite_capacity=pixels*3;
    }
    memset(&owned->frame, 0, sizeof(owned->frame));
    return owned;
fail:
    hdpack_jobs_release(&owned->frame); return NULL;
}

static HdTile *job_match(HdSprite *sp,HdPixel *p,int x,int y)
{
    s_current_sprite=sp;
#if defined(NESRECOMP_HDPACK_LAYER_PLANS) && NESRECOMP_HDPACK_LAYER_PLANS
    if(s_use_render_plan)
        return sp?match_layer_planned(1,sp->index,sp->t16,sp->p0,sp->p1,sp->p2,sp->p3,x,y,p)
                 :match_layer_planned(0,p->bg_index,p->bg_t16,p->bg_p0,p->bg_p1,p->bg_p2,p->bg_p3,x,y,p);
#endif
    return sp?match_layer(1,sp->index,sp->t16,sp->p0,sp->p1,sp->p2,sp->p3,x,y,p)
             :match_layer(0,p->bg_index,p->bg_t16,p->bg_p0,p->bg_p1,p->bg_p2,p->bg_p3,x,y,p);
}

const HdJobFrame *hdpack_prepare_jobs(const uint32_t *native, int native_w)
{
    if (!s_active || !native || !s_pixels || native_w <= 0 ||
        native_w != s_native_w || s_scale <= 0 || s_scale > 64 ||
        (size_t)native_w > SIZE_MAX / 240 / sizeof(HdJobPixel) ||
        (uint64_t)native_w*240*3>0x800000u) return NULL;
    if(!resolve_sprites())return NULL;
    if (s_opt_debug < 0) s_opt_debug = getenv("NESRECOMP_HDPACK_DEBUG") ? 1 : 0;
    if (s_opt_hide_orig < 0)
        s_opt_hide_orig = getenv("NESRECOMP_HDPACK_HIDE_ORIGINALS") ? 1 : 0;
    HdJobPack *pack = job_pack_get();
    if (!pack) return NULL;
    if ((size_t)s_bg_count > SIZE_MAX / sizeof(HdJobBackground)) return NULL;
    HdOwnedJobFrame *owned = job_frame_allocate((size_t)native_w * 240,
                                               (size_t)s_bg_count);
    if (!owned) return NULL;
    owned->pack = pack; ++pack->refs;
    HdJobFrame *f = &owned->frame;
    f->width = (uint32_t)native_w; f->height = 240; f->source_scale = (uint32_t)s_scale;
    f->pack_generation = pack->generation; f->images = pack->images;
    f->image_count = pack->count; f->pixels = owned->pixels;
    f->backgrounds = owned->backgrounds;f->sprites=owned->sprites;
    eval_backgrounds_for_frame();
    f->hud_rows = (uint32_t)s_hud_rows;
    for (int i = 0; i < s_nact; ++i) {
        HdBg *b = s_actbg[i];
        if (!b->img || b->img->job_image == HD_JOB_NO_IMAGE ||
            b->img->job_image >= pack->count) goto fail;
        int64_t ox = (int64_t)b->left + b->offx, oy = (int64_t)b->top + b->offy;
        if (ox < INT32_MIN || ox > INT32_MAX || oy < INT32_MIN || oy > INT32_MAX)
            goto fail;
        int first_y = i < s_band_mid ? s_hud_rows : 0;
        /* Omit entirely invisible source rectangles at native/job granularity. */
        if (ox * s_scale >= b->img->w || (ox + native_w) * s_scale <= 0 ||
            (oy + first_y) * s_scale >= b->img->h || (oy + 240) * s_scale <= 0)
            continue;
        HdJobBackground *out = &owned->backgrounds[f->background_count++];
        out->image = b->img->job_image;
        out->offset_x = (int32_t)ox; out->offset_y = (int32_t)oy;
        out->brightness = job_brightness(b->brightness); out->blend = (uint32_t)b->blend;
        if (i < s_band_behind) ++f->behind_end;
        if (i < s_band_mid) ++f->under_end;
        if (i < s_band_fg) ++f->mid_end;
    }
#if defined(NESRECOMP_HDPACK_LAYER_PLANS) && NESRECOMP_HDPACK_LAYER_PLANS
    if (s_use_render_plan && ++s_plan_epoch == 0) {
        ++s_plan_epoch;
        for (int i = 0; i < HD_NBUCKETS; ++i)
            for (HdKeyGroup *g = s_key_groups[i]; g; g = g->next) g->epoch = 0;
    }
#endif
    int hide_bg = s_disable_orig || s_opt_hide_orig;
    for (int y = 0; y < 240; ++y) for (int x = 0; x < native_w; ++x) {
        size_t at = (size_t)y * (unsigned)native_w + (unsigned)x;
        HdPixel *p = &s_pixels[at]; HdJobPixel *j = &owned->pixels[at];
        j->base = p->bg_has ? p->backdrop : native[at];
        j->bg_image = j->sp_image = HD_JOB_NO_IMAGE;
        j->bg_uv = j->sp_uv = j->bg_color = j->sp_color = j->flags = 0;
        if(p->bg_has && (p->bg_ox>=8 || p->bg_oy>=8))goto fail;
        HdSprite *sprites=sprites_at(at);int lowest_behind=HD_MAX_SPRITES;
        for(int k=0;k<HD_MAX_SPRITES;k++)if(sprites[k].has && sprites[k].behind && !sprites[k].transparent){lowest_behind=k;break;}
        unsigned drawn=0,hidden=0;
        for(int k=HD_MAX_SPRITES-1;k>=0;k--) {
            HdSprite *sp=&sprites[k];
            if(!sp->has || (!sp->behind && k>=lowest_behind))continue;
            if(sp->ox>=8 || sp->oy>=8)goto fail;
            HdJobSprite layer={HD_JOB_NO_IMAGE,0,0,0};
            HdTile *tile=job_match(sp,p,x,y);
            if(tile) {
                if(tile->fully_transparent)continue;
                layer.image=tile->job_image;layer.uv=sp->ox|((uint32_t)sp->oy<<8);
                layer.flags=HD_JOB_IMAGE<<HD_JOB_SP_SHIFT;
                if(!sp->behind && !tile->has_transparent)hidden=1;
            } else if(!sp->transparent && (s_opt_debug || !s_opt_hide_orig)) {
                layer.color=s_opt_debug?0xff00ffffu:sp->argb;
                layer.flags=HD_JOB_SOLID<<HD_JOB_SP_SHIFT;
                if(!sp->behind)hidden=1;
            } else continue;
            if(sp->hm)layer.flags|=HD_JOB_HMIRROR;
            if(sp->vm)layer.flags|=HD_JOB_VMIRROR;
            if(sp->behind){layer.flags|=HD_JOB_BEHIND;f->has_behind=1;}
            if(!drawn) {
                j->sp_image=layer.image;j->sp_uv=layer.uv;j->sp_color=layer.color;j->flags=layer.flags;
            } else {
                if(drawn==1)j->flags|=f->sprite_count<<HD_JOB_EXTRA_START;
                owned->sprites[f->sprite_count++]=layer;
            }
            drawn++;
        }
        if(drawn>1)j->flags|=(drawn-1)<<HD_JOB_EXTRA_SHIFT;
        if(p->bg_has && !hidden) {
            HdTile *bg=job_match(NULL,p,x,y);
            if(bg) {
                if(!bg->fully_transparent) {
                    j->bg_image=bg->job_image;j->bg_uv=p->bg_ox|((uint32_t)p->bg_oy<<8);
                    j->flags|=HD_JOB_IMAGE<<HD_JOB_BG_SHIFT;
                }
            } else if(s_opt_debug || (!hide_bg && !p->bg_transparent)) {
                j->bg_color=s_opt_debug?0xffff00ffu:p->bg_argb;
                j->flags|=HD_JOB_SOLID<<HD_JOB_BG_SHIFT;
            }
        }
        s_current_sprite=NULL;
    }
    if (!++s_job_serial) ++s_job_serial;
    f->serial = s_job_serial;
    return f;
fail:
    hdpack_jobs_release(f); return NULL;
}

static uint32_t job_color_over(uint32_t under, uint32_t source, uint32_t brightness,
                               uint32_t blend)
{
    if (brightness != 255) source = adjust_brightness(source, (int)brightness);
    unsigned alpha = source >> 24;
    if (!alpha) return under;
    if (blend == 1) return sat_add(under, source);
    if (blend == 2) return sat_sub(under, source);
    return alpha == 255 ? source : blend_over(under, source);
}

static int job_background_rows(const HdJobFrame *f, const HdJobBackground *b,
                               unsigned first_y, unsigned scale,
                               uint32_t *out, size_t pitch)
{
    if (b->image >= f->image_count) return -1;
    const HdJobImage *im = &f->images[b->image];
    if (!hd_image_storage_valid(im) || b->brightness > 65025 || b->blend > 2)
        return -1;
    int64_t ox = (int64_t)b->offset_x * scale, oy = (int64_t)b->offset_y * scale;
    int64_t left = ox < 0 ? -ox : 0;
    int64_t right = ((int64_t)im->width * scale + f->source_scale - 1) / f->source_scale - ox;
    int64_t top = (int64_t)first_y * scale;
    if (top < -oy) top = -oy;
    int64_t bottom = ((int64_t)im->height * scale + f->source_scale - 1) / f->source_scale - oy;
    int64_t width = (int64_t)f->width * scale, height = (int64_t)f->height * scale;
    if (right > width) right = width;
    if (bottom > height) bottom = height;
    if (left >= right || top >= bottom) return 0;
    for (int64_t y = top; y < bottom; ++y) {
        size_t sy = (size_t)((y + oy) * f->source_scale / scale);
        uint32_t *dst = out + (size_t)y * pitch + (size_t)left;
        for (int64_t x = left; x < right; ++x) {
            size_t sx = (size_t)((x + ox) * f->source_scale / scale);
            *dst = job_color_over(*dst, hd_image_pixel(im->pixels,im->pages,im->width,(unsigned)sx,(unsigned)sy),
                                  b->brightness, b->blend);
            ++dst;
        }
    }
    return 0;
}

static int job_tile_block(const HdJobFrame *f, unsigned scale, uint32_t *dst,
                          size_t pitch, unsigned mode, uint32_t id, uint32_t uv,
                          uint32_t color, unsigned mirrors)
{
    if (mode == HD_JOB_SKIP) return 0;
    if (mode == HD_JOB_SOLID) {
        for (unsigned y = 0; y < scale; ++y)
            for (unsigned x = 0; x < scale; ++x) dst[(size_t)y * pitch + x] = color;
        return 0;
    }
    if (mode != HD_JOB_IMAGE || id >= f->image_count) return -1;
    const HdJobImage *im = &f->images[id];
    unsigned ox = uv & 255, oy = uv >> 8;
    if (ox >= 8 || oy >= 8 || !hd_image_storage_valid(im) || im->width < 8 * f->source_scale ||
        im->height < 8 * f->source_scale || im->brightness > 65025) return -1;
    int hm = (mirrors & HD_JOB_HMIRROR) != 0, vm = (mirrors & HD_JOB_VMIRROR) != 0;
    for (unsigned y = 0; y < scale; ++y) {
        unsigned sy = y * f->source_scale / scale;
        if (vm) sy = f->source_scale - 1 - sy;
        sy += oy * f->source_scale;
        for (unsigned x = 0; x < scale; ++x) {
            unsigned sx = x * f->source_scale / scale;
            if (hm) sx = f->source_scale - 1 - sx;
            sx += (hm ? 7 - ox : ox) * f->source_scale;
            uint32_t *pixel = dst + (size_t)y * pitch + x;
            *pixel = job_color_over(*pixel, hd_image_pixel(im->pixels,im->pages,im->width,sx,sy),
                                    im->brightness, 0);
        }
    }
    return 0;
}

static int job_sprite_pass(const HdJobFrame *f,unsigned scale,uint32_t *out,size_t pitch,int behind)
{
    for(uint32_t y=0;y<f->height;y++)for(uint32_t x=0;x<f->width;x++) {
        const HdJobPixel *j=&f->pixels[(size_t)y*f->width+x];
        uint32_t count=hd_job_extra_count(j->flags),start=hd_job_extra_start(j->flags);
        if((!count && start) || start>f->sprite_count || count>f->sprite_count-start)return -1;
        for(uint32_t k=0;k<=count;k++) {
            HdJobSprite sp=k?f->sprites[start+k-1]:(HdJobSprite){j->sp_image,j->sp_uv,j->sp_color,j->flags&127u};
            if(k && (sp.flags&~124u))return -1;
            if(!!(sp.flags&HD_JOB_BEHIND)!=behind)continue;
            if(job_tile_block(f,scale,out+(size_t)y*scale*pitch+(size_t)x*scale,pitch,
                (sp.flags>>HD_JOB_SP_SHIFT)&HD_JOB_MODE_MASK,sp.image,sp.uv,sp.color,sp.flags))return -1;
        }
    }
    return 0;
}

int hdpack_render_jobs(const HdJobFrame *f, unsigned scale, uint32_t *out, size_t pitch)
{
    if (!f || !out || !f->pixels || !f->width || !f->height || !f->source_scale ||
        f->source_scale > 64 || !scale || scale > 64 ||
        f->width > SIZE_MAX / scale || pitch < (size_t)f->width * scale ||
        (size_t)f->height > SIZE_MAX / sizeof(*out) / scale / pitch ||
        (f->image_count && !f->images) || (f->background_count && !f->backgrounds) ||
        (f->sprite_count && !f->sprites) || f->has_behind>1 ||
        f->behind_end > f->under_end || f->under_end > f->mid_end || f->mid_end > f->background_count ||
        f->hud_rows > f->height) return -1;
    for (uint32_t y = 0; y < f->height; ++y) for (uint32_t x = 0; x < f->width; ++x) {
        uint32_t *dst = out + (size_t)y * scale * pitch + (size_t)x * scale;
        uint32_t color = f->pixels[(size_t)y * f->width + x].base;
        for (unsigned dy = 0; dy < scale; ++dy)
            for (unsigned dx = 0; dx < scale; ++dx) dst[(size_t)dy * pitch + dx] = color;
    }
    for (uint32_t i = 0; i < f->behind_end; ++i)
        if (job_background_rows(f, &f->backgrounds[i], f->hud_rows, scale, out, pitch)) return -1;
    if(f->has_behind && job_sprite_pass(f,scale,out,pitch,1))return -1;
    for(uint32_t i=f->behind_end;i<f->under_end;i++)
        if(job_background_rows(f,&f->backgrounds[i],f->hud_rows,scale,out,pitch))return -1;
    for (uint32_t y = 0; y < f->height; ++y) for (uint32_t x = 0; x < f->width; ++x) {
        const HdJobPixel *j = &f->pixels[(size_t)y * f->width + x];
        if (job_tile_block(f, scale, out + (size_t)y * scale * pitch + (size_t)x * scale,
                           pitch, (j->flags >> HD_JOB_BG_SHIFT) & HD_JOB_MODE_MASK,
                           j->bg_image, j->bg_uv, j->bg_color, 0)) return -1;
    }
    for (uint32_t i = f->under_end; i < f->mid_end; ++i)
        if (job_background_rows(f, &f->backgrounds[i], 0, scale, out, pitch)) return -1;
    if(job_sprite_pass(f,scale,out,pitch,0))return -1;
    for (uint32_t i = f->mid_end; i < f->background_count; ++i)
        if (job_background_rows(f, &f->backgrounds[i], 0, scale, out, pitch)) return -1;
    return 0;
}
