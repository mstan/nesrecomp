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
    if (!g) return find_tile(k,x,y,cur,is_sprite);
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
    return NULL;
#endif
}

static void build_key_groups(void) {
    for (int i = s_all_count-1; i >= 0 && s_groups_ready; --i) {
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
    else
        k.palette = ((uint32_t)p0 << 24) | ((uint32_t)p1 << 16) | ((uint32_t)p2 << 8) | p3;
    return native_hud_tile(find_tile_planned(&k, x, y, cur, is_sprite),y);
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
