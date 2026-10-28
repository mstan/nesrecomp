/* Layered-pack render service. Included only by qualified title builds.
 * The default/direct hdpack.c translation unit keeps its original tile
 * representation and operators. Derived records are never serialized. */
typedef struct HdTilePlan {
    HdTile *tile;
    struct HdTilePlan *key_next, *plan_next;
    int dynamic_conds;
} HdTilePlan;

/* Immutable key ownership and a render-local condition plan. A key's tiles
 * keep definition priority; frame-only gates are resolved once for the whole
 * picture, while position/nearby/mirror gates still use each actual pixel. */
typedef struct HdKeyGroup {
    HdKey key;
    HdTilePlan *first, *plan;
    HdTile *alias, *fallback;
    uint64_t epoch;
    struct HdKeyGroup *next;
} HdKeyGroup;

static HdKeyGroup *s_key_groups[HD_NBUCKETS];
static int        s_groups_ready = 1;
static int        s_use_render_plan;
static uint64_t   s_plan_epoch;
typedef struct {
    HdKey key;
    HdKeyGroup *group;
    HdTile *alias;
    int valid;
} HdLookup;
#define HD_LOOKUP_SIZE 1024
static HdLookup s_lookup[HD_LOOKUP_SIZE];

static HdKeyGroup *key_group(const HdKey *key, uint32_t hash, int create) {
    unsigned bucket = hash & (HD_NBUCKETS - 1);
    for (HdKeyGroup *g = s_key_groups[bucket]; g; g = g->next)
        if (key_eq(&g->key, key)) return g;
    if (!create) return NULL;
    HdKeyGroup *g = (HdKeyGroup *)calloc(1, sizeof(*g));
    if (!g) { s_groups_ready = 0; return NULL; }
    g->key = *key; g->next = s_key_groups[bucket]; s_key_groups[bucket] = g;
    return g;
}

static void prepare_tile_plan(HdKeyGroup *g) {
    g->epoch = s_plan_epoch; g->plan = NULL; g->fallback = NULL;
    HdTilePlan **tail = &g->plan;
    for (HdTilePlan *p = g->first; p; p = p->key_next) {
        HdTile *t = p->tile;
        if (!t->ncond) { if (!g->fallback) g->fallback = t; continue; }
        int pass = 1;
        for (int i = 0; i < t->ncond; ++i) {
            if (!t->conds[i]->use_cache) continue;
            int v = cond_eval(t->conds[i], 0, 0, NULL, 0);
            if (t->neg[i]) v = !v;
            if (!v) { pass = 0; break; }
        }
        if (!pass) continue;
        *tail = p; tail = &p->plan_next;
        /* No lower-priority candidate can beat this constant winner. Earlier
         * dynamic candidates may still win at their own pixels. */
        if (!p->dynamic_conds) break;
    }
    *tail = NULL;
}

static HdTile *find_tile_planned(const HdKey *k, int x, int y, HdPixel *cur, int is_sprite) {
#if defined(NESRECOMP_HDPACK_PLAN_REFERENCE) && NESRECOMP_HDPACK_PLAN_REFERENCE
    return find_tile(k, x, y, cur, is_sprite);
#else
    if (!s_groups_ready) return find_tile(k, x, y, cur, is_sprite);
    uint32_t hash = key_hash(k);
    HdLookup *entry = &s_lookup[hash & (HD_LOOKUP_SIZE - 1)];
    if (!entry->valid || !key_eq(&entry->key, k)) {
        entry->key = *k; entry->valid = 1;
        entry->group = key_group(k, hash, 0);
        HdKey wildcard = *k; wildcard.palette = 0xffffffffu;
        HdKeyGroup *alias = key_group(&wildcard, key_hash(&wildcard), 0);
        entry->alias = alias ? alias->alias : NULL;
    }
    HdKeyGroup *g = entry->group;
    if (!g) return entry->alias;
    if (g->epoch != s_plan_epoch) prepare_tile_plan(g);
    for (HdTilePlan *p = g->plan; p; p = p->plan_next) {
        HdTile *t = p->tile;
        int pass = 1;
        for (int i = 0; i < t->ncond; ++i) {
            if (t->conds[i]->use_cache) continue;
            int v = cond_eval(t->conds[i], x, y, cur, is_sprite);
            if (t->neg[i]) v = !v;
            if (!v) { pass = 0; break; }
        }
        if (pass) return t;
    }
    return g->fallback ? g->fallback : entry->alias;
#endif
}

static void build_key_groups(void) {
    for (int i = 0; i < s_all_count && s_groups_ready; ++i) {
        HdTile *t = s_all[i];
        HdKeyGroup *g = key_group(&t->key, key_hash(&t->key), 1);
        if (!g) break;
        HdTilePlan *p = (HdTilePlan *)calloc(1, sizeof(*p));
        if (!p) { s_groups_ready = 0; break; }
        p->tile = t; p->key_next = g->first; g->first = p;
        for (int k = 0; k < t->ncond; ++k)
            if (!t->conds[k]->use_cache) ++p->dynamic_conds;
    }
    for (int i = 0; i < HD_NBUCKETS && s_groups_ready; ++i)
        for (HdAlias *a = s_alias_buckets[i]; a && s_groups_ready; a = a->next) {
            HdKeyGroup *g = key_group(&a->key, key_hash(&a->key), 1);
            if (g && !g->alias) g->alias = a->tile;
        }
}

static HdTile *match_layer_planned(int is_sprite, int32_t tile_index, const uint8_t *t16,
                                  uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3,
                                  int x, int y, HdPixel *cur) {
    HdKey k; memset(&k, 0, sizeof(k));
    k.is_chr_ram = s_is_chr_ram;
    if (s_is_chr_ram) { if (t16) memcpy(k.tile16, t16, 16); }
    else k.tile_index = tile_index;
    if (s_version < 100)
        k.palette = ((uint32_t)p1 << 16) | ((uint32_t)p2 << 8) | p3;
    else if (is_sprite)
        k.palette = 0xff000000u | ((uint32_t)p1 << 16) | ((uint32_t)p2 << 8) | p3;
    else
        k.palette = ((uint32_t)p0 << 24) | ((uint32_t)p1 << 16) | ((uint32_t)p2 << 8) | p3;
    return find_tile_planned(&k, x, y, cur, is_sprite);
}

static void blend_bg_rows(uint32_t *dst, int hd_w, const HdBg *b, int first_y, int s) {
    const HdBgImg *im = b->img;
    int ox = (b->left + b->offx) * s, oy = (b->top + b->offy) * s;
    int left = ox < 0 ? -ox : 0;
    int right = im->w - ox < hd_w ? im->w - ox : hd_w;
    int top = first_y * s;
    if (top < -oy) top = -oy;
    int bottom = im->h - oy < 240 * s ? im->h - oy : 240 * s;
    if (left >= right || top >= bottom) return;
    for (int y = top; y < bottom; ++y) {
        const uint32_t *src = im->px + (long)(y + oy) * im->w + ox + left;
        uint32_t *out = dst + (long)y * hd_w + left;
        for (int x = left; x < right; ++x) {
            uint32_t v = *src++;
            if (b->brightness != 255) v = adjust_brightness(v, b->brightness);
            uint32_t a = v >> 24;
            if (a) {
                if (b->blend == 1) *out = sat_add(*out, v);
                else if (b->blend == 2) *out = sat_sub(*out, v);
                else if (a == 0xff) *out = v;
                else *out = blend_over(*out, v);
            }
            ++out;
        }
    }
}

static void upscale_planned(const uint32_t *native_fb, int native_w, uint32_t *hd_buf,
                            int s, int hd_w, int hide_orig) {
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < native_w; ++x) {
            int i = y * native_w + x;
            HdPixel *p = &s_pixels[i];
            fill_block(hd_buf + (long)(y * s) * hd_w + (long)x * s,
                       hd_w, p->bg_has ? p->backdrop : native_fb[i], s);
        }
    for (int i = 0; i < s_band_mid; ++i)
        blend_bg_rows(hd_buf, hd_w, s_actbg[i], s_hud_rows, s);
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < native_w; ++x) {
            HdPixel *p = &s_pixels[y * native_w + x];
            if (!p->bg_has) continue;
            uint32_t *dst = hd_buf + (long)(y * s) * hd_w + (long)x * s;
            HdTile *t = match_layer_planned(0, p->bg_index, p->bg_t16,
                                    p->bg_p0, p->bg_p1, p->bg_p2, p->bg_p3, x, y, p);
            if (t && !t->fully_transparent)
                blit_over(dst, hd_w, t, p->bg_ox, p->bg_oy, 0, 0, s);
            else if (s_opt_debug) fill_block(dst, hd_w, 0xffff00ffu, s);
            else if (!hide_orig) fill_block(dst, hd_w, p->bg_argb, s);
        }
    for (int i = s_band_mid; i < s_band_fg; ++i)
        blend_bg_rows(hd_buf, hd_w, s_actbg[i], 0, s);
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < native_w; ++x) {
            HdPixel *p = &s_pixels[y * native_w + x];
            if (!p->sp_has) continue;
            uint32_t *dst = hd_buf + (long)(y * s) * hd_w + (long)x * s;
            HdTile *t = match_layer_planned(1, p->sp_index, p->sp_t16,
                                    0, p->sp_p1, p->sp_p2, p->sp_p3, x, y, p);
            if (t && !t->fully_transparent)
                blit_over(dst, hd_w, t, p->sp_ox, p->sp_oy, p->sp_hm, p->sp_vm, s);
            else if (s_opt_debug) fill_block(dst, hd_w, 0xff00ffffu, s);
            else if (!s_opt_hide_orig) fill_block(dst, hd_w, p->sp_argb, s);
        }
    for (int i = s_band_fg; i < s_nact; ++i)
        blend_bg_rows(hd_buf, hd_w, s_actbg[i], 0, s);
}

static void hd_layer_prepare(void) {
    s_use_render_plan = s_bg_count > 0;
    if (s_use_render_plan) build_key_groups();
    printf("[HDPack] render service: %s\n", s_use_render_plan ? "layer-row plan" : "direct pixels");
}
static void hd_layer_unload(void) {
    for (int i = 0; i < HD_NBUCKETS; ++i) {
        HdKeyGroup *g = s_key_groups[i];
        while (g) {
            HdKeyGroup *next = g->next;
            HdTilePlan *p = g->first;
            while (p) { HdTilePlan *pn = p->key_next; free(p); p = pn; }
            free(g); g = next;
        }
        s_key_groups[i] = NULL;
    }
    memset(s_lookup, 0, sizeof(s_lookup));
    s_groups_ready = 1; s_plan_epoch = 0; s_use_render_plan = 0;
}
static int hd_layer_upscale(const uint32_t *native, int width, uint32_t *out,
                            int scale, int hd_width, int hide_original) {
#if defined(NESRECOMP_HDPACK_PLAN_REFERENCE) && NESRECOMP_HDPACK_PLAN_REFERENCE
    (void)native;(void)width;(void)out;(void)scale;(void)hd_width;(void)hide_original;
    return 0;
#else
    if (!s_use_render_plan) return 0;
    if (++s_plan_epoch == 0) {
        ++s_plan_epoch;
        for (int i = 0; i < HD_NBUCKETS; ++i)
            for (HdKeyGroup *g = s_key_groups[i]; g; g = g->next) g->epoch = 0;
    }
    upscale_planned(native, width, out, scale, hd_width, hide_original);
    return 1;
#endif
}
