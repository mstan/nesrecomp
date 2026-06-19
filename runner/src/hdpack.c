/*
 * hdpack.c — Mesen HD Pack loader, tile index, and HD upscaler.
 *
 * See runner/HDPACK.md for the format spec and integration model. Matching is
 * byte-compatible with Mesen2 Core/NES/HdPacks (HdData.h / HdPackLoader.cpp /
 * HdNesPack.cpp). PNG decode uses stb_image (implemented in chr_codec.c).
 */
#include "hdpack.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "stb_image.h"   /* declarations only; impl lives in chr_codec.c */

/* Runtime state the <condition> engine reads (defined in runtime.c / ppu). */
extern uint8_t  g_ram[0x0800];
extern uint64_t g_frame_count;
extern uint8_t  g_ppuscroll_x, g_ppuscroll_y, g_ppuctrl;

/* ── Tile key + entry ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t palette;       /* PaletteColors (Mesen layout) */
    uint8_t  tile16[16];    /* CHR-RAM tile bytes (content match) */
    int32_t  tile_index;    /* CHR-ROM absolute tile index (index match) */
    int      is_chr_ram;    /* match mode for this key */
} HdKey;

struct HdCond;  /* fwd (defined with the condition engine below) */

typedef struct HdTile {
    HdKey          key;
    uint32_t      *data;            /* w*h premultiplied ARGB8888 */
    int            w, h;            /* 8*scale each */
    int            brightness;      /* 0..255 (or >255 for HDR; rare) */
    int            has_transparent; /* any pixel with alpha != 0xFF */
    int            fully_transparent;
    struct HdCond **conds; int *neg; int ncond;  /* [cond] gate; 0 = unconditional */
    struct HdTile *next;            /* primary hash chain */
} HdTile;

/* Resolve a '&'-separated [cond] prefix into HdCond* + negation arrays (defined
 * with the condition engine below). Returns 1 if all names resolve (0 conds for
 * an empty string), 0 if any condition is undefined. */
static int resolve_cond_list(const char *cond_str, struct HdCond ***out_c,
                             int **out_n, int *out_count);

/* Wildcard-palette alias node: a `defaultTile=Y` entry is reachable both by its
 * exact palette key (primary table) and by a 0xFFFFFFFF wildcard key. */
typedef struct HdAlias {
    HdKey            key;           /* palette == 0xFFFFFFFF */
    HdTile          *tile;
    struct HdAlias  *next;
} HdAlias;

/* ── PNG sheet ────────────────────────────────────────────────────────────── */

typedef struct { uint32_t *px; int w, h; } HdSheet;

/* ── Module state ─────────────────────────────────────────────────────────── */

#define HD_NBUCKETS 4096   /* power of two */

static int        s_active = 0;
static int        s_scale = 1;
static int        s_version = 0;
static int        s_is_chr_ram = 0;
static char       s_dir[512];

static HdSheet   *s_sheets = NULL;
static int        s_sheet_count = 0, s_sheet_cap = 0;

static HdTile    *s_buckets[HD_NBUCKETS];
static HdAlias   *s_alias_buckets[HD_NBUCKETS];

static HdTile   **s_all = NULL;        /* every tile (for cleanup) */
static int        s_all_count = 0, s_all_cap = 0;

static HdPixel   *s_pixels = NULL;     /* side channel, native_w*240 */
static int        s_native_w = 0;

static int        s_skipped_cond = 0;  /* conditional tiles not yet supported (Phase 2) */
static int        s_overscan[4] = {0,0,0,0}; /* top,right,bottom,left (parsed; clip TODO) */
static int        s_has_overscan = 0;
static int        s_disable_orig = 0;  /* <options>disableOriginalTiles */

/* ── <condition> engine (mirrors Mesen HdPackConditions.h) ─────────────────── */

typedef enum {
    CT_MEM_CHECK, CT_MEM_CHECK_CONST,        /* operandA op operandB / const */
    CT_FRAME_RANGE,                          /* frame % A >= B */
    CT_TILE_AT_POS, CT_SPRITE_AT_POS,        /* fixed screen pos (per-frame) */
    CT_TILE_NEARBY, CT_SPRITE_NEARBY,        /* current pixel +- delta (per-pixel) */
    CT_POS_X, CT_POS_Y, CT_ORIGIN_X, CT_ORIGIN_Y,
    CT_HMIRROR, CT_VMIRROR,                  /* built-in: current tile's flip flags */
    CT_UNKNOWN,
} HdCondType;

typedef enum { OP_EQ, OP_NE, OP_GT, OP_LT, OP_LE, OP_GE } HdCondOp;

typedef struct HdCond {
    char        name[64];
    HdCondType  type;
    HdCondOp    op;
    uint32_t    operandA;      /* mem address (low 16) | 0x80000000 for PPU; or frame modulo; or pos operand */
    uint32_t    operandB;      /* mem address / constant / frame threshold */
    uint8_t     mask;          /* memory-check mask (default 0xFF) */
    int         is_ppu;        /* operandA targets PPU memory */
    /* tile/sprite at/nearby: */
    int         x, y;          /* pixel pos (at) or signed tile delta (nearby) */
    int32_t     tile_index;    /* CHR-ROM index, or -1 = CHR-RAM content match */
    uint8_t     tile16[16];
    uint32_t    palette;
    int         ignore_palette;
    int         use_cache;     /* 1 = constant per frame (cache result) */
    /* per-frame cache: */
    uint64_t    cached_frame;
    int         cached_val;
    struct HdCond *next;       /* name hash chain */
} HdCond;

#define HDCOND_NBUCKETS 8192
static HdCond *s_cond_buckets[HDCOND_NBUCKETS];
static HdCond **s_cond_all = NULL; static int s_cond_count = 0, s_cond_cap = 0;
static int     s_missing_cond = 0; /* background referenced an undefined condition */

/* ── <background> layers ───────────────────────────────────────────────────── */

typedef struct {
    char      name[256];
    uint32_t *px;            /* premultiplied ARGB, w*h */
    int       w, h;
} HdBgImg;

typedef struct HdBg {
    HdBgImg  *img;
    int       brightness;    /* 0..255 */
    float     hscroll, vscroll;
    int       priority;      /* 0..39 (default 10) */
    int       left, top;     /* pixel offset into the PNG (NES px) */
    int       blend;         /* 0 alpha, 1 add, 2 subtract */
    HdCond  **conds; int *neg; int ncond;  /* ANDed conditions; neg[i]=='!'  */
    int       active;        /* recomputed per frame */
} HdBg;

static HdBg     *s_bgs = NULL; static int s_bg_count = 0, s_bg_cap = 0;
static HdBgImg **s_bgimgs = NULL; static int s_bgimg_count = 0, s_bgimg_cap = 0;
static HdBg    **s_bg_sorted = NULL;  /* by ascending priority */
static int       s_bg_unrenderable = 0; /* parsed but PNG missing */
static HdBg    **s_actbg = NULL;        /* active this frame (ascending priority) */
static int       s_nact = 0, s_band_mid = 0, s_band_fg = 0;
static int       s_hud_rows = 0;        /* top native rows masked by an opaque HUD overlay */

/* ── Small helpers ────────────────────────────────────────────────────────── */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int hex_nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
static uint32_t hex_u32(const char *s) {
    uint32_t v = 0;
    while (*s && isxdigit((unsigned char)*s)) v = (v << 4) | (uint32_t)hex_nib(*s++);
    return v;
}
static uint8_t hex_byte(const char *s) { return (uint8_t)((hex_nib(s[0]) << 4) | hex_nib(s[1])); }

/* Split `s` (modified in place) on ',' into up to max tokens. Returns count. */
static int split_commas(char *s, char **out, int max) {
    int n = 0;
    char *p = s;
    while (n < max) {
        out[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = '\0';
        p = c + 1;
    }
    return n;
}

static char *rstrip(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = '\0';
    return s;
}

/* ── Key hashing / equality (mirrors HdTileKey) ───────────────────────────── */

static uint32_t key_hash(const HdKey *k) {
    if (k->is_chr_ram) {
        uint8_t buf[20];
        memcpy(buf, &k->palette, 4);
        memcpy(buf + 4, k->tile16, 16);
        uint32_t r = 0;
        for (int i = 0; i < 20; i += 4) {
            uint32_t c; memcpy(&c, buf + i, 4);
            r += c; r = (r << 2) | (r >> 30);
        }
        return r;
    }
    return (uint32_t)k->tile_index ^ k->palette;
}

static int key_eq(const HdKey *a, const HdKey *b) {
    if (a->is_chr_ram != b->is_chr_ram) return 0;
    if (a->is_chr_ram)
        return a->palette == b->palette && memcmp(a->tile16, b->tile16, 16) == 0;
    return a->tile_index == b->tile_index && a->palette == b->palette;
}

/* Evaluate a tile/background's [cond] gate at pixel (x,y) (impl below).
 * is_sprite tells the built-in hmirror/vmirror which layer's flip flags to read. */
static int cond_eval(struct HdCond *c, int x, int y, HdPixel *cur, int is_sprite);

static int tile_conds_pass(const HdTile *t, int x, int y, HdPixel *cur, int is_sprite) {
    for (int i = 0; i < t->ncond; i++) {
        int v = cond_eval(t->conds[i], x, y, cur, is_sprite);
        if (t->neg[i]) v = !v;
        if (!v) return 0;
    }
    return 1;
}

/* Lookup for key `k` at pixel (x,y): a conditional tile whose [cond] gate passes
 * wins; otherwise the unconditional match; otherwise the wildcard-default
 * (palette 0xFFFFFFFF) fallback. */
static HdTile *find_tile(const HdKey *k, int x, int y, HdPixel *cur, int is_sprite) {
    uint32_t h = key_hash(k) & (HD_NBUCKETS - 1);
    HdTile *fallback = NULL;
    for (HdTile *t = s_buckets[h]; t; t = t->next) {
        if (!key_eq(&t->key, k)) continue;
        if (t->ncond == 0) { if (!fallback) fallback = t; continue; }
        if (tile_conds_pass(t, x, y, cur, is_sprite)) return t;
    }
    if (fallback) return fallback;

    HdKey wk = *k;
    wk.palette = 0xFFFFFFFFu;
    uint32_t wh = key_hash(&wk) & (HD_NBUCKETS - 1);
    for (HdAlias *a = s_alias_buckets[wh]; a; a = a->next)
        if (key_eq(&a->key, &wk)) return a->tile;
    return NULL;
}

static void track_tile(HdTile *t) {
    if (s_all_count == s_all_cap) {
        s_all_cap = s_all_cap ? s_all_cap * 2 : 256;
        s_all = (HdTile **)realloc(s_all, s_all_cap * sizeof(HdTile *));
    }
    s_all[s_all_count++] = t;
}

static void register_tile(HdTile *t, int default_tile) {
    uint32_t h = key_hash(&t->key) & (HD_NBUCKETS - 1);
    t->next = s_buckets[h];
    s_buckets[h] = t;
    track_tile(t);

    if (default_tile) {
        HdKey wk = t->key;
        wk.palette = 0xFFFFFFFFu;
        HdAlias *a = (HdAlias *)calloc(1, sizeof(HdAlias));
        a->key = wk; a->tile = t;
        uint32_t wh = key_hash(&wk) & (HD_NBUCKETS - 1);
        a->next = s_alias_buckets[wh];
        s_alias_buckets[wh] = a;
    }
}

/* ── PNG sheet loading ────────────────────────────────────────────────────── */

static uint32_t premul(uint32_t argb) {
    uint32_t a = argb >> 24;
    if (a >= 0xFF) return argb;
    uint32_t af = a + 1;
    uint32_t r = (((argb >> 16) & 0xFF) * af) >> 8;
    uint32_t g = (((argb >> 8) & 0xFF) * af) >> 8;
    uint32_t b = ((argb & 0xFF) * af) >> 8;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* Load dir/name PNG into a new sheet slot (premultiplied). Returns index or -1. */
static int load_sheet(const char *name) {
    char path[768];
    snprintf(path, sizeof(path), "%s/%s", s_dir, name);

    int w, h, comp;
    unsigned char *rgba = stbi_load(path, &w, &h, &comp, 4);  /* force RGBA */
    if (!rgba) {
        fprintf(stderr, "[HDPack] PNG load failed: %s (%s)\n", path, stbi_failure_reason());
        return -1;
    }

    uint32_t *px = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    if (!px) { stbi_image_free(rgba); return -1; }
    for (int i = 0; i < w * h; i++) {
        unsigned char *p = rgba + i * 4;
        uint32_t argb = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
                        ((uint32_t)p[1] << 8) | (uint32_t)p[2];
        px[i] = premul(argb);
    }
    stbi_image_free(rgba);

    if (s_sheet_count == s_sheet_cap) {
        s_sheet_cap = s_sheet_cap ? s_sheet_cap * 2 : 8;
        s_sheets = (HdSheet *)realloc(s_sheets, s_sheet_cap * sizeof(HdSheet));
    }
    s_sheets[s_sheet_count].px = px;
    s_sheets[s_sheet_count].w = w;
    s_sheets[s_sheet_count].h = h;
    return s_sheet_count++;
}

/* ── <tile> processing ────────────────────────────────────────────────────── */

static void process_tile(const char *cond_str, char *rest) {
    char *tok[32];
    int n = split_commas(rest, tok, 32);
    if (n < 7) return;
    for (int i = 0; i < n; i++) tok[i] = rstrip(tok[i]);

    HdKey key; memset(&key, 0, sizeof(key));
    int bitmap_index, X, Y, default_tile, brightness;
    int idx = 0;

    if (s_version < 100) {
        /* legacy: tileIdx, img, palR, palG, palB, X, Y, brightness, default[, 16 dec bytes] */
        if (n < 9) return;
        key.tile_index = atoi(tok[idx++]);
        bitmap_index   = atoi(tok[idx++]);
        int pr = atoi(tok[idx++]), pg = atoi(tok[idx++]), pb = atoi(tok[idx++]);
        key.palette = ((uint32_t)pr << 16) | ((uint32_t)pg << 8) | (uint32_t)pb;
        X = atoi(tok[idx++]); Y = atoi(tok[idx++]);
        brightness = (int)(atof(tok[idx++]) * 255.0);
        default_tile = (tok[idx][0] == 'Y' || tok[idx][0] == 'y'); idx++;
        if (n >= 24) {  /* CHR-RAM: 16 decimal bytes appended */
            key.is_chr_ram = 1;
            for (int i = 0; i < 16 && idx < n; i++) key.tile16[i] = (uint8_t)atoi(tok[idx++]);
        }
    } else {
        /* modern: img, tileData, palData, X, Y, brightness, default[, chrBank, tileIdx] */
        bitmap_index = atoi(tok[idx++]);
        const char *tile_data = tok[idx++];
        const char *pal_data  = tok[idx++];
        if (strlen(tile_data) >= 32) {
            key.is_chr_ram = 1;
            for (int i = 0; i < 16; i++) key.tile16[i] = hex_byte(tile_data + i * 2);
            key.tile_index = -1;
        } else {
            key.is_chr_ram = 0;
            key.tile_index = (s_version <= 102) ? atoi(tile_data) : (int32_t)hex_u32(tile_data);
        }
        key.palette = hex_u32(pal_data);
        X = atoi(tok[idx++]); Y = atoi(tok[idx++]);
        brightness = (int)(atof(tok[idx++]) * 255.0);
        default_tile = (tok[idx][0] == 'Y' || tok[idx][0] == 'y'); idx++;
        /* trailing chrBank/tileIndex (CHR-RAM) are not used for matching */
    }

    if (bitmap_index < 0 || bitmap_index >= s_sheet_count) {
        fprintf(stderr, "[HDPack] tile references missing img %d\n", bitmap_index);
        return;
    }
    if (brightness < 0) brightness = 0;

    HdSheet *sh = &s_sheets[bitmap_index];
    int W = 8 * s_scale, H = 8 * s_scale;
    if (X < 0 || Y < 0 || X + W > sh->w || Y + H > sh->h) {
        fprintf(stderr, "[HDPack] tile block out of bounds (img %d, %d,%d %dx%d in %dx%d)\n",
                bitmap_index, X, Y, W, H, sh->w, sh->h);
        return;
    }

    /* [cond] gate (Phase-2 conditional tiles). Drop the tile if it references an
     * undefined condition (mirrors the <background> handling). */
    struct HdCond **tc = NULL; int *tn = NULL; int tcn = 0;
    if (!resolve_cond_list(cond_str, &tc, &tn, &tcn)) { s_skipped_cond++; return; }

    HdTile *t = (HdTile *)calloc(1, sizeof(HdTile));
    t->key = key;
    t->w = W; t->h = H;
    t->conds = tc; t->neg = tn; t->ncond = tcn;
    t->brightness = brightness;
    t->data = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    t->has_transparent = 0;
    t->fully_transparent = 1;
    for (int y = 0; y < H; y++) {
        const uint32_t *src = sh->px + (size_t)(Y + y) * sh->w + X;
        uint32_t *dst = t->data + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            uint32_t v = src[x];
            dst[x] = v;
            if ((v & 0xFF000000u) != 0xFF000000u) t->has_transparent = 1;
            if (v & 0xFF000000u) t->fully_transparent = 0;
        }
    }

    register_tile(t, default_tile);
}

/* ── <condition> parsing ──────────────────────────────────────────────────── */

static uint32_t name_hash(const char *s) {
    uint32_t h = 5381; while (*s) h = h * 33u + (uint8_t)*s++; return h;
}
static HdCond *cond_find(const char *name) {
    for (HdCond *c = s_cond_buckets[name_hash(name) & (HDCOND_NBUCKETS - 1)]; c; c = c->next)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

/* '&'-separated [cond] string -> HdCond* + negation arrays (see fwd decl). */
static int resolve_cond_list(const char *cond_str, struct HdCond ***out_c,
                             int **out_n, int *out_count) {
    *out_c = NULL; *out_n = NULL; *out_count = 0;
    if (!cond_str || !cond_str[0]) return 1;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", cond_str);
    char *parts[16]; int np = 0; char *p = buf;
    while (np < 16) { parts[np++] = p; char *amp = strchr(p, '&'); if (!amp) break; *amp = '\0'; p = amp + 1; }
    HdCond **cc = (HdCond **)calloc(np, sizeof(HdCond *));
    int     *nn = (int *)calloc(np, sizeof(int));
    for (int i = 0; i < np; i++) {
        char *nm = rstrip(parts[i]);
        int neg = 0;
        if (*nm == '!') { neg = 1; nm++; }
        HdCond *c = cond_find(nm);
        if (!c) { free(cc); free(nn); return 0; }   /* undefined condition */
        cc[i] = c; nn[i] = neg;
    }
    *out_c = cc; *out_n = nn; *out_count = np;
    return 1;
}
static int parse_op(const char *s, HdCondOp *out) {
    if (!strcmp(s, "=="))      *out = OP_EQ;
    else if (!strcmp(s, "!=")) *out = OP_NE;
    else if (!strcmp(s, ">"))  *out = OP_GT;
    else if (!strcmp(s, "<"))  *out = OP_LT;
    else if (!strcmp(s, "<=")) *out = OP_LE;
    else if (!strcmp(s, ">=")) *out = OP_GE;
    else return 0;
    return 1;
}
/* Parse a tile-data token: 32 hex chars = CHR-RAM content (->tile16, index -1);
 * otherwise a hex tile index. */
static void parse_tiledata(HdCond *c, const char *s) {
    if ((int)strlen(s) >= 32) {
        c->tile_index = -1;
        for (int i = 0; i < 16; i++) c->tile16[i] = hex_byte(s + i * 2);
    } else {
        c->tile_index = (int32_t)hex_u32(s);
    }
}

static void cond_register(HdCond *c) {
    if (s_cond_count == s_cond_cap) {
        s_cond_cap = s_cond_cap ? s_cond_cap * 2 : 512;
        s_cond_all = (HdCond **)realloc(s_cond_all, s_cond_cap * sizeof(HdCond *));
    }
    s_cond_all[s_cond_count++] = c;
    uint32_t h = name_hash(c->name) & (HDCOND_NBUCKETS - 1);
    c->next = s_cond_buckets[h];
    s_cond_buckets[h] = c;
}

/* Built-in conditions Mesen exposes without a <condition> line: hmirror/vmirror
 * test the flip flags of the tile currently being drawn. Registered before
 * parsing so [hmirror]/[vmirror] tile rules resolve instead of being dropped. */
static void register_builtin_conds(void) {
    static const struct { const char *name; HdCondType type; } builtins[] = {
        { "hmirror", CT_HMIRROR }, { "vmirror", CT_VMIRROR },
    };
    for (int i = 0; i < (int)(sizeof(builtins) / sizeof(builtins[0])); i++) {
        HdCond *c = (HdCond *)calloc(1, sizeof(HdCond));
        snprintf(c->name, sizeof(c->name), "%s", builtins[i].name);
        c->type = builtins[i].type;
        c->tile_index = -1;
        c->use_cache = 0;   /* per-pixel: depends on the tile being matched */
        cond_register(c);
    }
}

static void process_condition(char *rest) {
    char *tok[16];
    int n = split_commas(rest, tok, 16);
    if (n < 2) return;
    for (int i = 0; i < n; i++) tok[i] = rstrip(tok[i]);

    HdCond *c = (HdCond *)calloc(1, sizeof(HdCond));
    snprintf(c->name, sizeof(c->name), "%s", tok[0]);
    c->mask = 0xFF;
    c->tile_index = -1;
    c->use_cache = 1;
    const char *ty = tok[1];

    if (!strcmp(ty, "memoryCheck") || !strcmp(ty, "ppuMemoryCheck")) {
        if (n < 5 || !parse_op(tok[3], &c->op)) { free(c); return; }
        c->type = CT_MEM_CHECK;
        c->is_ppu = (ty[0] == 'p');
        c->operandA = hex_u32(tok[2]);
        c->operandB = hex_u32(tok[4]);
        if (n >= 6) c->mask = (uint8_t)hex_u32(tok[5]);
    } else if (!strcmp(ty, "memoryCheckConstant") || !strcmp(ty, "ppuMemoryCheckConstant")) {
        if (n < 5 || !parse_op(tok[3], &c->op)) { free(c); return; }
        c->type = CT_MEM_CHECK_CONST;
        c->is_ppu = (ty[0] == 'p');
        c->operandA = hex_u32(tok[2]);
        c->operandB = hex_u32(tok[4]) & 0xFF;
        if (n >= 6) c->mask = (uint8_t)hex_u32(tok[5]);
    } else if (!strcmp(ty, "frameRange")) {
        if (n < 4) { free(c); return; }
        c->type = CT_FRAME_RANGE;
        c->operandA = (uint32_t)atoi(tok[2]);   /* modulo (v102+ decimal) */
        c->operandB = (uint32_t)atoi(tok[3]);   /* threshold */
    } else if (!strcmp(ty, "tileAtPosition") || !strcmp(ty, "spriteAtPosition") ||
               !strcmp(ty, "tileNearby")     || !strcmp(ty, "spriteNearby")) {
        if (n < 6) { free(c); return; }
        int sprite = (ty[0] == 's');
        int nearby = strstr(ty, "Nearby") != NULL;
        c->type = sprite ? (nearby ? CT_SPRITE_NEARBY : CT_SPRITE_AT_POS)
                         : (nearby ? CT_TILE_NEARBY   : CT_TILE_AT_POS);
        c->x = atoi(tok[2]);
        c->y = atoi(tok[3]);
        parse_tiledata(c, tok[4]);
        c->palette = hex_u32(tok[5]);
        if (n >= 7 && (tok[6][0] == 'Y' || tok[6][0] == 'y')) c->ignore_palette = 1;
        c->use_cache = !nearby;   /* nearby depends on the current pixel */
    } else if (!strcmp(ty, "positionCheckX") || !strcmp(ty, "positionCheckY") ||
               !strcmp(ty, "originPositionCheckX") || !strcmp(ty, "originPositionCheckY")) {
        if (n < 4 || !parse_op(tok[2], &c->op)) { free(c); return; }
        c->type = !strcmp(ty, "positionCheckX") ? CT_POS_X :
                  !strcmp(ty, "positionCheckY") ? CT_POS_Y :
                  strstr(ty, "X") ? CT_ORIGIN_X : CT_ORIGIN_Y;
        c->operandA = (uint32_t)atoi(tok[3]);
        c->use_cache = 0;
    } else {
        free(c); return;   /* unknown / unsupported condition type */
    }
    cond_register(c);
}

/* ── <background> parsing ─────────────────────────────────────────────────── */

static HdBgImg *load_bg_img(const char *name) {
    for (int i = 0; i < s_bgimg_count; i++)
        if (!strcmp(s_bgimgs[i]->name, name)) return s_bgimgs[i];   /* dedup */

    char path[768];
    snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    int w, h, comp;
    unsigned char *rgba = stbi_load(path, &w, &h, &comp, 4);
    if (!rgba) {
        fprintf(stderr, "[HDPack] background PNG load failed: %s (%s)\n", path, stbi_failure_reason());
        return NULL;
    }
    HdBgImg *im = (HdBgImg *)calloc(1, sizeof(HdBgImg));
    snprintf(im->name, sizeof(im->name), "%s", name);
    im->w = w; im->h = h;
    im->px = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    for (int i = 0; i < w * h; i++) {
        unsigned char *p = rgba + i * 4;
        uint32_t argb = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
                        ((uint32_t)p[1] << 8) | (uint32_t)p[2];
        im->px[i] = premul(argb);
    }
    stbi_image_free(rgba);

    if (s_bgimg_count == s_bgimg_cap) {
        s_bgimg_cap = s_bgimg_cap ? s_bgimg_cap * 2 : 64;
        s_bgimgs = (HdBgImg **)realloc(s_bgimgs, s_bgimg_cap * sizeof(HdBgImg *));
    }
    s_bgimgs[s_bgimg_count++] = im;
    return im;
}

/* cond_str: the text inside [ ] (may be NULL/empty); rest: after "<background>" */
static void process_background(const char *cond_str, char *rest) {
    char *tok[10];
    int n = split_commas(rest, tok, 10);
    if (n < 1) return;
    for (int i = 0; i < n; i++) tok[i] = rstrip(tok[i]);

    HdBgImg *im = load_bg_img(tok[0]);
    if (!im) { s_bg_unrenderable++; return; }

    if (s_bg_count == s_bg_cap) {
        s_bg_cap = s_bg_cap ? s_bg_cap * 2 : 256;
        s_bgs = (HdBg *)realloc(s_bgs, s_bg_cap * sizeof(HdBg));
    }
    HdBg *b = &s_bgs[s_bg_count];
    memset(b, 0, sizeof(*b));
    b->img = im;
    b->brightness = (n >= 2) ? (int)(atof(tok[1]) * 255.0) : 255;
    if (b->brightness < 0) b->brightness = 0;
    b->hscroll  = (n >= 3) ? (float)atof(tok[2]) : 0.0f;
    b->vscroll  = (n >= 4) ? (float)atof(tok[3]) : 0.0f;
    b->priority = (n >= 5) ? atoi(tok[4]) : 10;
    b->left     = (n >= 6) ? atoi(tok[5]) : 0;
    b->top      = (n >= 7) ? atoi(tok[6]) : 0;
    b->blend    = 0;
    if (n >= 8) {
        if      (!strcmp(tok[7], "Add"))      b->blend = 1;
        else if (!strcmp(tok[7], "Subtract")) b->blend = 2;
    }
    if (b->priority < 0)  b->priority = 0;
    if (b->priority > 39) b->priority = 39;

    /* Resolve condition names ('&'-separated, optional '!' negation). Conditions
     * are defined before backgrounds in the manifest; drop the rule if any name
     * is undefined. */
    if (!resolve_cond_list(cond_str, &b->conds, &b->neg, &b->ncond)) {
        s_missing_cond++;
        return;   /* cannot evaluate -> drop this background */
    }
    s_bg_count++;
}

static int bg_priority_cmp(const void *a, const void *b) {
    return (*(HdBg * const *)a)->priority - (*(HdBg * const *)b)->priority;
}
static void finalize_backgrounds(void) {
    if (s_bg_count == 0) return;
    s_bg_sorted = (HdBg **)malloc(s_bg_count * sizeof(HdBg *));
    for (int i = 0; i < s_bg_count; i++) s_bg_sorted[i] = &s_bgs[i];
    qsort(s_bg_sorted, s_bg_count, sizeof(HdBg *), bg_priority_cmp);
    s_actbg = (HdBg **)malloc(s_bg_count * sizeof(HdBg *));
}

/* ── Parsing driver ───────────────────────────────────────────────────────── */

static void parse_line(char *line) {
    rstrip(line);
    char *L = line;

    /* optional leading [conditions] prefix */
    char *cond_str = NULL;
    if (*L == '[') {
        char *rb = strchr(L, ']');
        if (rb) { *rb = '\0'; cond_str = L + 1; L = rb + 1; }
    }
    if (*L == '\0') return;

    if      (!strncmp(L, "<ver>", 5))        s_version = atoi(L + 5);
    else if (!strncmp(L, "<scale>", 7))      s_scale = clampi(atoi(L + 7), 1, 10);
    else if (!strncmp(L, "<img>", 5))        { char *nm = rstrip(L + 5); load_sheet(nm); }
    else if (!strncmp(L, "<condition>", 11)) process_condition(L + 11);
    else if (!strncmp(L, "<tile>", 6))       process_tile(cond_str, L + 6);
    else if (!strncmp(L, "<background>", 12)) process_background(cond_str, L + 12);
    else if (!strncmp(L, "<options>", 9))    { if (strstr(L + 9, "disableOriginalTiles")) s_disable_orig = 1; }
    else if (!strncmp(L, "<overscan>", 10)) {
        char buf[64]; char *tok[4];
        snprintf(buf, sizeof(buf), "%s", L + 10);
        if (split_commas(buf, tok, 4) == 4) {
            for (int i = 0; i < 4; i++) s_overscan[i] = atoi(tok[i]);
            s_has_overscan = 1;
        }
    }
    /* <bgm>/<sfx>/<patch>/<options>/<addition>/<fallback>/#comments: ignored for now */
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int hdpack_active(void) { return s_active; }
int hdpack_scale(void)  { return s_active ? s_scale : 1; }
HdPixel *hdpack_pixels(void) { return s_active ? s_pixels : NULL; }
int hdpack_recording(void)   { return s_active; }

void hdpack_frame_begin(void) {
    if (s_active && s_pixels)
        memset(s_pixels, 0, (size_t)s_native_w * 240 * sizeof(HdPixel));
}

void hdpack_unload(void) {
    for (int i = 0; i < HD_NBUCKETS; i++) {
        HdAlias *a = s_alias_buckets[i];
        while (a) { HdAlias *nx = a->next; free(a); a = nx; }
        s_alias_buckets[i] = NULL;
        s_buckets[i] = NULL;
    }
    for (int i = 0; i < s_all_count; i++) {
        if (s_all[i]) { free(s_all[i]->conds); free(s_all[i]->neg); free(s_all[i]->data); free(s_all[i]); }
    }
    free(s_all); s_all = NULL; s_all_count = s_all_cap = 0;
    for (int i = 0; i < s_sheet_count; i++) free(s_sheets[i].px);
    free(s_sheets); s_sheets = NULL; s_sheet_count = s_sheet_cap = 0;

    /* conditions */
    for (int i = 0; i < s_cond_count; i++) free(s_cond_all[i]);
    free(s_cond_all); s_cond_all = NULL; s_cond_count = s_cond_cap = 0;
    for (int i = 0; i < HDCOND_NBUCKETS; i++) s_cond_buckets[i] = NULL;
    /* backgrounds */
    for (int i = 0; i < s_bg_count; i++) { free(s_bgs[i].conds); free(s_bgs[i].neg); }
    free(s_bgs); s_bgs = NULL; s_bg_count = s_bg_cap = 0;
    free(s_bg_sorted); s_bg_sorted = NULL;
    free(s_actbg); s_actbg = NULL; s_nact = 0;
    for (int i = 0; i < s_bgimg_count; i++) { free(s_bgimgs[i]->px); free(s_bgimgs[i]); }
    free(s_bgimgs); s_bgimgs = NULL; s_bgimg_count = s_bgimg_cap = 0;
    s_missing_cond = s_bg_unrenderable = 0; s_disable_orig = 0;

    free(s_pixels); s_pixels = NULL;
    s_active = 0; s_scale = 1; s_version = 0;
    s_skipped_cond = 0; s_has_overscan = 0; s_native_w = 0;
    s_dir[0] = '\0';
}

int hdpack_load(const char *dir, int is_chr_ram_game, int native_w) {
    hdpack_unload();
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    s_is_chr_ram = is_chr_ram_game ? 1 : 0;
    s_scale = 1; s_version = 0;

    char path[600];
    snprintf(path, sizeof(path), "%s/hires.txt", dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[HDPack] no hires.txt in %s\n", dir);
        return -1;
    }

    register_builtin_conds();   /* hmirror/vmirror, before any [cond] rules resolve */
    char line[2048];
    while (fgets(line, sizeof(line), f)) parse_line(line);
    fclose(f);
    finalize_backgrounds();

    if (s_all_count == 0 && s_bg_count == 0) {
        fprintf(stderr, "[HDPack] %s: no usable tiles or backgrounds "
                "(skipped %d conditional tiles; %d bg w/ missing PNG, %d w/ missing condition)\n",
                dir, s_skipped_cond, s_bg_unrenderable, s_missing_cond);
        hdpack_unload();
        return -1;
    }

    s_native_w = native_w;
    s_pixels = (HdPixel *)calloc((size_t)native_w * 240, sizeof(HdPixel));
    if (!s_pixels) { hdpack_unload(); return -1; }

    s_active = 1;
    printf("[HDPack] loaded %s: ver=%d scale=%dx imgs=%d tiles=%d (%s match)\n",
           dir, s_version, s_scale, s_sheet_count, s_all_count,
           s_is_chr_ram ? "CHR-RAM content" : "CHR-ROM index");
    printf("[HDPack] conditions=%d backgrounds=%d (%d unique PNGs)%s\n",
           s_cond_count, s_bg_count, s_bgimg_count,
           s_disable_orig ? "  [disableOriginalTiles]" : "");
    if (s_skipped_cond || s_bg_unrenderable || s_missing_cond)
        printf("[HDPack] note: skipped %d conditional tile(s); dropped %d bg (missing PNG), "
               "%d bg (missing condition)\n", s_skipped_cond, s_bg_unrenderable, s_missing_cond);
    return 0;
}

int hdpack_load_from_config(int is_chr_ram_game, int native_w) {
    /* 1. NESRECOMP_HDPACK is an explicit override — always wins. */
    const char *env = getenv("NESRECOMP_HDPACK");
    if (env && env[0]) return hdpack_load(env, is_chr_ram_game, native_w);

    /* 2. Master switch (config.ini HdPackEnabled / the launcher's "Enable HD
     *    pack" toggle). Off => no pack, even if pack files are present. */
    if (!g_nes_config.hdpack_enabled) return -1;

    /* 3. Configured folder, else the default drop-in location <exe_dir>/hdpack.
     *    An empty HdPackDir means "use the default" and is resolved here at load
     *    time (not baked into config), so the build can be moved and still find
     *    its hdpack/ folder. */
    char autobuf[600];
    const char *dir = g_nes_config.hdpack_dir;
    if (!dir || !dir[0]) {
        char exedir[512];
        nesrecomp_exe_dir(exedir, sizeof(exedir));
        snprintf(autobuf, sizeof(autobuf), "%shdpack", exedir);
        dir = autobuf;
    }

    /* 4. Only load when the folder actually holds a pack, so "enabled but no
     *    pack present" stays byte-identical to stock (and avoids error spam). */
    char probe[700];
    snprintf(probe, sizeof(probe), "%s/hires.txt", dir);
    FILE *pf = fopen(probe, "rb");
    if (!pf) return -1;
    fclose(pf);
    return hdpack_load(dir, is_chr_ram_game, native_w);
}

/* ── HD rendering (mirrors HdNesPack::DrawTile sampling) ───────────────────── */

static uint32_t adjust_brightness(uint32_t px, int b) {
    uint32_t a = px >> 24;
    int r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, bl = px & 0xFF;
    r = r * b / 255; if (r > 255) r = 255;
    g = g * b / 255; if (g > 255) g = 255;
    bl = bl * b / 255; if (bl > 255) bl = 255;
    return (a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

/* Premultiplied alpha over an opaque background color. */
static uint32_t blend_over(uint32_t under, uint32_t src) {
    uint32_t a = src >> 24;
    uint32_t inv = 255 - a;
    int r = ((src >> 16) & 0xFF) + (int)((((under >> 16) & 0xFF) * inv) / 255);
    int g = ((src >> 8) & 0xFF) + (int)((((under >> 8) & 0xFF) * inv) / 255);
    int b = (src & 0xFF) + (int)(((under & 0xFF) * inv) / 255);
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Shared HD-tile sampling setup: returns the starting source offset and the
 * per-pixel / per-row increments for the scale*scale block at (off_x,off_y),
 * honoring horizontal/vertical mirroring (mirrors HdNesPack::DrawTile). */
static void blit_setup(const HdTile *t, int off_x, int off_y, int hmir, int vmir,
                       int s, long *base, int *small_inc, int *large_inc) {
    int tileW = t->w;                /* 8*s */
    int src_off_x = hmir ? (7 - off_x) : off_x;
    *base = (long)(off_y * s) * tileW + (long)src_off_x * s;
    *small_inc = 1; *large_inc = tileW - s;
    if (hmir) { *base += s - 1; *small_inc = -1; *large_inc = tileW + s; }
    if (vmir) { *base += (long)tileW * (s - 1); *large_inc = (hmir ? s : -s) - tileW; }
}

/* Background blit: transparent HD pixels resolve to a solid under-color
 * (the backdrop). Opaque -> HD; partial -> HD over backdrop. */
static void blit_under(uint32_t *dst, int hd_w, const HdTile *t,
                       int off_x, int off_y, int hmir, int vmir,
                       uint32_t under, int s) {
    const uint32_t *bm = t->data;
    long bo; int small_inc, large_inc;
    blit_setup(t, off_x, off_y, hmir, vmir, s, &bo, &small_inc, &large_inc);
    uint32_t *out = dst;
    for (int y = 0; y < s; y++) {
        for (int x = 0; x < s; x++) {
            uint32_t px = bm[bo];
            if (t->brightness != 255) px = adjust_brightness(px, t->brightness);
            uint32_t a = px >> 24;
            if (!t->has_transparent || a == 0xFF) out[x] = px;
            else if (a)                            out[x] = blend_over(under, px);
            else                                   out[x] = under;
            bo += small_inc;
        }
        bo += large_inc; out += hd_w;
    }
}

/* Sprite blit: composites over whatever is already in dst (the HD background),
 * so transparent HD-sprite pixels keep the HD background beneath. */
static void blit_over(uint32_t *dst, int hd_w, const HdTile *t,
                      int off_x, int off_y, int hmir, int vmir, int s) {
    const uint32_t *bm = t->data;
    long bo; int small_inc, large_inc;
    blit_setup(t, off_x, off_y, hmir, vmir, s, &bo, &small_inc, &large_inc);
    uint32_t *out = dst;
    for (int y = 0; y < s; y++) {
        for (int x = 0; x < s; x++) {
            uint32_t px = bm[bo];
            if (t->brightness != 255) px = adjust_brightness(px, t->brightness);
            uint32_t a = px >> 24;
            if (a == 0xFF)  out[x] = px;
            else if (a)     out[x] = blend_over(out[x], px);
            /* else fully transparent: keep the HD background already in dst */
            bo += small_inc;
        }
        bo += large_inc; out += hd_w;
    }
}

static void fill_block(uint32_t *dst, int hd_w, uint32_t color, int s) {
    for (int y = 0; y < s; y++) {
        uint32_t *o = dst + (long)y * hd_w;
        for (int x = 0; x < s; x++) o[x] = color;
    }
}

/* Match a layer by (is_sprite, index/content, palette) in the loaded dialect.
 * (x,y,cur) locate the pixel for any per-pixel [cond] gates on conditional tiles. */
static HdTile *match_layer(int is_sprite, int32_t tile_index, const uint8_t *t16,
                           uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3,
                           int x, int y, HdPixel *cur) {
    HdKey k; memset(&k, 0, sizeof(k));
    k.is_chr_ram = s_is_chr_ram;
    if (s_is_chr_ram) { if (t16) memcpy(k.tile16, t16, 16); }
    else              k.tile_index = tile_index;
    if (s_version < 100)
        k.palette = ((uint32_t)p1 << 16) | ((uint32_t)p2 << 8) | (uint32_t)p3;
    else if (is_sprite)
        k.palette = 0xFF000000u | ((uint32_t)p1 << 16) | ((uint32_t)p2 << 8) | (uint32_t)p3;
    else
        k.palette = ((uint32_t)p0 << 24) | ((uint32_t)p1 << 16) |
                    ((uint32_t)p2 << 8) | (uint32_t)p3;
    return find_tile(&k, x, y, cur, is_sprite);
}

static int s_opt_debug = -1;       /* unmatched: BG->magenta, sprite->cyan */
static int s_opt_hide_orig = -1;   /* unmatched tiles show backdrop, not the original */

/* ── <condition> evaluation ───────────────────────────────────────────────── */

static uint8_t read_cpu_mem(uint32_t addr) {
    addr &= 0xFFFF;
    if (addr < 0x2000) return g_ram[addr & 0x07FF];
    return 0;   /* outside internal RAM (PPU regs / ROM): not tracked */
}
static int cond_cmp(int a, HdCondOp op, int b) {
    switch (op) {
        case OP_EQ: return a == b; case OP_NE: return a != b;
        case OP_GT: return a >  b; case OP_LT: return a <  b;
        case OP_LE: return a <= b; case OP_GE: return a >= b;
    }
    return 0;
}
/* Compare the tile/sprite recorded at side-channel index `off` to a condition. */
static int tile_matches(const HdCond *c, int off, int sprite) {
    HdPixel *p = &s_pixels[off];
    const uint8_t *t16; int32_t index; int has; uint32_t pal;
    if (sprite) {
        has = p->sp_has; t16 = p->sp_t16; index = p->sp_index;
        pal = (s_version < 100) ? (((uint32_t)p->sp_p1 << 16) | ((uint32_t)p->sp_p2 << 8) | p->sp_p3)
                                : (0xFF000000u | ((uint32_t)p->sp_p1 << 16) |
                                   ((uint32_t)p->sp_p2 << 8) | p->sp_p3);
    } else {
        has = p->bg_has; t16 = p->bg_t16; index = p->bg_index;
        pal = (s_version < 100) ? (((uint32_t)p->bg_p1 << 16) | ((uint32_t)p->bg_p2 << 8) | p->bg_p3)
                                : (((uint32_t)p->bg_p0 << 24) | ((uint32_t)p->bg_p1 << 16) |
                                   ((uint32_t)p->bg_p2 << 8) | p->bg_p3);
    }
    if (!has) return 0;
    if (!c->ignore_palette && pal != c->palette) return 0;
    if (c->tile_index < 0) return t16 && memcmp(t16, c->tile16, 16) == 0;  /* CHR-RAM */
    return index == c->tile_index;                                          /* CHR-ROM */
}
/* x,y / cur are the current pixel (used by per-pixel conditions only). */
static int cond_eval(HdCond *c, int x, int y, HdPixel *cur, int is_sprite) {
    if (c->use_cache && c->cached_frame == g_frame_count + 1) return c->cached_val;
    int r = 0;
    switch (c->type) {
        case CT_MEM_CHECK_CONST:
            if (!c->is_ppu) r = cond_cmp(read_cpu_mem(c->operandA) & c->mask, c->op, (int)c->operandB);
            break;
        case CT_MEM_CHECK:
            if (!c->is_ppu) r = cond_cmp(read_cpu_mem(c->operandA) & c->mask, c->op,
                                         read_cpu_mem(c->operandB) & c->mask);
            break;
        case CT_FRAME_RANGE:
            r = c->operandA ? ((uint32_t)(g_frame_count % c->operandA) >= c->operandB) : 0;
            break;
        case CT_TILE_AT_POS:
        case CT_SPRITE_AT_POS:
            if (c->x >= 0 && c->x < s_native_w && c->y >= 0 && c->y < 240)
                r = tile_matches(c, c->y * s_native_w + c->x, c->type == CT_SPRITE_AT_POS);
            break;
        case CT_TILE_NEARBY:
        case CT_SPRITE_NEARBY: {
            int sprite = (c->type == CT_SPRITE_NEARBY);
            int xs = (cur && sprite && cur->sp_hm) ? -1 : 1;
            int ys = (cur && sprite && cur->sp_vm) ? -1 : 1;
            int px = x + c->x * xs, py = y + c->y * ys;
            if (px >= 0 && px < s_native_w && py >= 0 && py < 240)
                r = tile_matches(c, py * s_native_w + px, sprite);
            break;
        }
        case CT_POS_X: r = cond_cmp(x, c->op, (int)c->operandA); break;
        case CT_POS_Y: r = cond_cmp(y, c->op, (int)c->operandA); break;
        case CT_ORIGIN_X: if (cur) r = cond_cmp(x - cur->bg_ox, c->op, (int)c->operandA); break;
        case CT_ORIGIN_Y: if (cur) r = cond_cmp(y - cur->bg_oy, c->op, (int)c->operandA); break;
        case CT_HMIRROR:  r = is_sprite && cur && cur->sp_hm; break;  /* NES BG tiles never flip */
        case CT_VMIRROR:  r = is_sprite && cur && cur->sp_vm; break;
        default: break;
    }
    if (c->use_cache) { c->cached_frame = g_frame_count + 1; c->cached_val = r; }
    return r;
}

/* ── Background activation (once per frame) + compositing ──────────────────── */

static void eval_backgrounds_for_frame(void) {
    s_nact = 0;
    if (!s_actbg) return;
    for (int i = 0; i < s_bg_count; i++) {
        HdBg *b = s_bg_sorted[i];
        int act = 1;
        for (int k = 0; k < b->ncond; k++) {
            int v = cond_eval(b->conds[k], 0, 0, NULL, 0);
            if (b->neg[k]) v = !v;
            if (!v) { act = 0; break; }
        }
        if (act) s_actbg[s_nact++] = b;   /* preserves priority order */
    }
    s_band_mid = s_band_fg = s_nact;
    for (int i = 0; i < s_nact; i++) {
        if (s_actbg[i]->priority >= 20 && s_band_mid == s_nact) s_band_mid = i;
        if (s_actbg[i]->priority >= 30 && s_band_fg  == s_nact) { s_band_fg = i; break; }
    }

    /* HUD mask: an opaque foreground overlay (e.g. hud.png at priority 39) sits
     * over the top strip. The scrolling room backgrounds underneath bleed through
     * the overlay's transparent holes (empty item/heart slots). Find the top
     * contiguous span of rows the overlay fully spans, so the compositor can skip
     * the room backgrounds there and let those holes fall back to the backdrop. */
    s_hud_rows = 0;
    int scrolly = g_ppuscroll_y + ((g_ppuctrl & 0x02) ? 240 : 0);
    for (int i = s_band_fg; i < s_nact; i++) {
        HdBg *b = s_actbg[i];
        int offy = (int)(scrolly * b->vscroll);
        int rows = 0;
        for (int sy = 0; sy < 240; sy++) {
            int py = (b->top + sy + offy) * s_scale;
            if (py < 0 || py >= b->img->h) break;
            const uint32_t *row = b->img->px + (long)py * b->img->w;
            int opaque = 0;
            for (int sx = 0; sx < s_native_w; sx++) {
                int px = (b->left + sx) * s_scale;
                if (px >= 0 && px < b->img->w && (row[px] >> 24) == 0xFF) { opaque = 1; break; }
            }
            if (!opaque) break;
            rows = sy + 1;
        }
        if (rows > s_hud_rows && rows < 120) s_hud_rows = rows;  /* top strip only, not a full-screen overlay */
    }
}

static uint32_t sat_add(uint32_t u, uint32_t s) {
    int r = ((u>>16)&0xFF)+((s>>16)&0xFF), g = ((u>>8)&0xFF)+((s>>8)&0xFF), b = (u&0xFF)+(s&0xFF);
    if (r>255) r=255; if (g>255) g=255; if (b>255) b=255;
    return 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)b;
}
static uint32_t sat_sub(uint32_t u, uint32_t s) {
    int r = ((u>>16)&0xFF)-((s>>16)&0xFF), g = ((u>>8)&0xFF)-((s>>8)&0xFF), b = (u&0xFF)-(s&0xFF);
    if (r<0) r=0; if (g<0) g=0; if (b<0) b=0;
    return 0xFF000000u | ((uint32_t)r<<16) | ((uint32_t)g<<8) | (uint32_t)b;
}
/* Composite one background's scale*scale contribution at native pixel (sx,sy). */
static void blend_bg_block(uint32_t *dst, int hd_w, const HdBg *b, int sx, int sy, int s) {
    int scrollx = g_ppuscroll_x + ((g_ppuctrl & 0x01) ? 256 : 0);
    int scrolly = g_ppuscroll_y + ((g_ppuctrl & 0x02) ? 240 : 0);
    int offx = (int)(scrollx * b->hscroll);
    int offy = (int)(scrolly * b->vscroll);
    const HdBgImg *im = b->img;
    for (int dy = 0; dy < s; dy++) {
        int py = (b->top + sy + offy) * s + dy;
        if (py < 0 || py >= im->h) continue;
        const uint32_t *row = im->px + (long)py * im->w;
        uint32_t *out = dst + (long)dy * hd_w;
        for (int dx = 0; dx < s; dx++) {
            int px = (b->left + sx + offx) * s + dx;
            if (px < 0 || px >= im->w) continue;
            uint32_t v = row[px];
            if (b->brightness != 255) v = adjust_brightness(v, b->brightness);
            uint32_t a = v >> 24;
            if (!a) continue;
            if (b->blend == 1)      out[dx] = sat_add(out[dx], v);
            else if (b->blend == 2) out[dx] = sat_sub(out[dx], v);
            else if (a == 0xFF)     out[dx] = v;
            else                    out[dx] = blend_over(out[dx], v);
        }
    }
}

void hdpack_upscale(const uint32_t *native_fb, int native_w, uint32_t *hd_buf) {
    if (!s_active) return;
    if (s_opt_debug < 0)     s_opt_debug = getenv("NESRECOMP_HDPACK_DEBUG") ? 1 : 0;
    if (s_opt_hide_orig < 0) s_opt_hide_orig = getenv("NESRECOMP_HDPACK_HIDE_ORIGINALS") ? 1 : 0;
    int s = s_scale, hd_w = native_w * s;
    int hide_orig = s_disable_orig || s_opt_hide_orig;

    eval_backgrounds_for_frame();

    for (int sy = 0; sy < 240; sy++) {
        for (int sx = 0; sx < native_w; sx++) {
            int idx = sy * native_w + sx;
            uint32_t *dst = hd_buf + (long)(sy * s) * hd_w + (long)sx * s;
            HdPixel *p = &s_pixels[idx];

            /* base: backdrop where BG is enabled, else the universal native pixel */
            fill_block(dst, hd_w, p->bg_has ? p->backdrop : native_fb[idx], s);

            /* under-backgrounds (priority 0..19). Suppressed inside an opaque HUD
             * overlay's top strip so its transparent holes (empty item/heart slots)
             * fall back to the backdrop instead of bleeding the scrolling room art. */
            if (sy >= s_hud_rows)
                for (int i = 0; i < s_band_mid; i++) blend_bg_block(dst, hd_w, s_actbg[i], sx, sy, s);

            /* NES background HD tile (composites over whatever is beneath) */
            HdTile *bt = p->bg_has ? match_layer(0, p->bg_index, p->bg_t16,
                                                 p->bg_p0, p->bg_p1, p->bg_p2, p->bg_p3,
                                                 sx, sy, p)
                                   : NULL;
            if (bt && !bt->fully_transparent) {
                blit_over(dst, hd_w, bt, p->bg_ox, p->bg_oy, 0, 0, s);
            } else if (p->bg_has) {
                if (s_opt_debug)      fill_block(dst, hd_w, 0xFFFF00FFu, s);  /* magenta */
                else if (hide_orig)   { /* leave background/backdrop showing */ }
                else                  fill_block(dst, hd_w, p->bg_argb, s);   /* original */
            }

            /* mid-backgrounds (priority 20..29, over the BG tile, under sprites) */
            for (int i = s_band_mid; i < s_band_fg; i++) blend_bg_block(dst, hd_w, s_actbg[i], sx, sy, s);

            /* sprite HD tile (over the HD background) */
            if (p->sp_has) {
                HdTile *st = match_layer(1, p->sp_index, p->sp_t16,
                                         0, p->sp_p1, p->sp_p2, p->sp_p3,
                                         sx, sy, p);
                if (st && !st->fully_transparent) {
                    blit_over(dst, hd_w, st, p->sp_ox, p->sp_oy, p->sp_hm, p->sp_vm, s);
                } else if (s_opt_debug) {
                    fill_block(dst, hd_w, 0xFF00FFFFu, s);   /* cyan: unmatched sprite */
                } else if (!s_opt_hide_orig) {
                    fill_block(dst, hd_w, p->sp_argb, s);    /* original sprite */
                }
            }

            /* foreground backgrounds (priority 30..39, over everything) */
            for (int i = s_band_fg; i < s_nact; i++) blend_bg_block(dst, hd_w, s_actbg[i], sx, sy, s);
        }
    }
}
