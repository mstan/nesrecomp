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

/* ── Tile key + entry ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t palette;       /* PaletteColors (Mesen layout) */
    uint8_t  tile16[16];    /* CHR-RAM tile bytes (content match) */
    int32_t  tile_index;    /* CHR-ROM absolute tile index (index match) */
    int      is_chr_ram;    /* match mode for this key */
} HdKey;

typedef struct HdTile {
    HdKey          key;
    uint32_t      *data;            /* w*h premultiplied ARGB8888 */
    int            w, h;            /* 8*scale each */
    int            brightness;      /* 0..255 (or >255 for HDR; rare) */
    int            has_transparent; /* any pixel with alpha != 0xFF */
    int            fully_transparent;
    struct HdTile *next;            /* primary hash chain */
} HdTile;

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

static int        s_skipped_cond = 0;  /* conditional tiles not yet supported */
static int        s_skipped_bg = 0;    /* <background> layers not yet supported */
static int        s_overscan[4] = {0,0,0,0}; /* top,right,bottom,left (parsed; clip TODO) */
static int        s_has_overscan = 0;

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

/* Exact lookup, then the wildcard-default fallback (palette 0xFFFFFFFF). */
static HdTile *find_tile(const HdKey *k) {
    uint32_t h = key_hash(k) & (HD_NBUCKETS - 1);
    for (HdTile *t = s_buckets[h]; t; t = t->next)
        if (key_eq(&t->key, k)) return t;

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

static void process_tile(char *rest) {
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

    HdTile *t = (HdTile *)calloc(1, sizeof(HdTile));
    t->key = key;
    t->w = W; t->h = H;
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

/* ── Parsing driver ───────────────────────────────────────────────────────── */

static void parse_line(char *line) {
    rstrip(line);
    char *L = line;

    /* optional leading [conditions] prefix */
    int has_cond = 0;
    if (*L == '[') {
        char *rb = strchr(L, ']');
        if (rb) { has_cond = 1; L = rb + 1; }
    }
    if (*L == '\0') return;

    if      (!strncmp(L, "<ver>", 5))        s_version = atoi(L + 5);
    else if (!strncmp(L, "<scale>", 7))      s_scale = clampi(atoi(L + 7), 1, 10);
    else if (!strncmp(L, "<img>", 5))        { char *nm = rstrip(L + 5); load_sheet(nm); }
    else if (!strncmp(L, "<tile>", 6))       { if (has_cond) s_skipped_cond++; else process_tile(L + 6); }
    else if (!strncmp(L, "<background>", 12)) s_skipped_bg++;
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
        if (s_all[i]) { free(s_all[i]->data); free(s_all[i]); }
    }
    free(s_all); s_all = NULL; s_all_count = s_all_cap = 0;
    for (int i = 0; i < s_sheet_count; i++) free(s_sheets[i].px);
    free(s_sheets); s_sheets = NULL; s_sheet_count = s_sheet_cap = 0;
    free(s_pixels); s_pixels = NULL;
    s_active = 0; s_scale = 1; s_version = 0;
    s_skipped_cond = s_skipped_bg = 0; s_has_overscan = 0; s_native_w = 0;
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

    char line[2048];
    while (fgets(line, sizeof(line), f)) parse_line(line);
    fclose(f);

    if (s_all_count == 0) {
        fprintf(stderr, "[HDPack] %s: no usable tiles (skipped %d conditional, %d background)\n",
                dir, s_skipped_cond, s_skipped_bg);
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
    if (s_skipped_cond || s_skipped_bg)
        printf("[HDPack] note: skipped %d conditional tile(s), %d background layer(s) "
               "(not yet supported)\n", s_skipped_cond, s_skipped_bg);
    return 0;
}

int hdpack_load_from_config(int is_chr_ram_game, int native_w) {
    const char *dir = getenv("NESRECOMP_HDPACK");
    char autobuf[600];
    if ((!dir || !dir[0]) && g_nes_config.hdpack_enabled && g_nes_config.hdpack_dir[0])
        dir = g_nes_config.hdpack_dir;
    if (!dir || !dir[0]) {
        /* Zero-config per-game pack: <exe_dir>/hdpack/hires.txt (drop-in). */
        char exedir[512], probe[700];
        nesrecomp_exe_dir(exedir, sizeof(exedir));
        snprintf(autobuf, sizeof(autobuf), "%shdpack", exedir);
        snprintf(probe, sizeof(probe), "%s/hires.txt", autobuf);
        FILE *pf = fopen(probe, "rb");
        if (pf) { fclose(pf); dir = autobuf; }
    }
    if (!dir || !dir[0]) return -1;
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

/* Match a layer by (is_sprite, index/content, palette) in the loaded dialect. */
static HdTile *match_layer(int is_sprite, int32_t tile_index, const uint8_t *t16,
                           uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3) {
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
    return find_tile(&k);
}

static int s_opt_debug = -1;       /* unmatched: BG->magenta, sprite->cyan */
static int s_opt_hide_orig = -1;   /* unmatched tiles show backdrop, not the original */

void hdpack_upscale(const uint32_t *native_fb, int native_w, uint32_t *hd_buf) {
    if (!s_active) return;
    if (s_opt_debug < 0)     s_opt_debug = getenv("NESRECOMP_HDPACK_DEBUG") ? 1 : 0;
    if (s_opt_hide_orig < 0) s_opt_hide_orig = getenv("NESRECOMP_HDPACK_HIDE_ORIGINALS") ? 1 : 0;
    int s = s_scale, hd_w = native_w * s;

    for (int sy = 0; sy < 240; sy++) {
        for (int sx = 0; sx < native_w; sx++) {
            int idx = sy * native_w + sx;
            uint32_t *dst = hd_buf + (long)(sy * s) * hd_w + (long)sx * s;
            HdPixel *p = &s_pixels[idx];

            /* ── Background layer ─────────────────────────────────────────── */
            HdTile *bt = p->bg_has ? match_layer(0, p->bg_index, p->bg_t16,
                                                 p->bg_p0, p->bg_p1, p->bg_p2, p->bg_p3)
                                   : NULL;
            if (bt && !bt->fully_transparent) {
                blit_under(dst, hd_w, bt, p->bg_ox, p->bg_oy, 0, 0, p->backdrop, s);
            } else if (p->bg_has) {
                if (s_opt_debug)          fill_block(dst, hd_w, 0xFFFF00FFu, s); /* magenta */
                else if (s_opt_hide_orig) fill_block(dst, hd_w, p->backdrop, s);
                else                      fill_block(dst, hd_w, p->bg_argb, s);  /* original */
            } else {
                fill_block(dst, hd_w, native_fb[idx], s);  /* BG disabled: universal bg */
            }

            /* ── Sprite layer (over the HD background) ────────────────────── */
            if (p->sp_has) {
                HdTile *st = match_layer(1, p->sp_index, p->sp_t16,
                                         0, p->sp_p1, p->sp_p2, p->sp_p3);
                if (st && !st->fully_transparent) {
                    blit_over(dst, hd_w, st, p->sp_ox, p->sp_oy, p->sp_hm, p->sp_vm, s);
                } else if (s_opt_debug) {
                    fill_block(dst, hd_w, 0xFF00FFFFu, s);   /* cyan: unmatched sprite */
                } else if (!s_opt_hide_orig) {
                    fill_block(dst, hd_w, p->sp_argb, s);    /* original sprite */
                }
                /* hide_orig + unmatched sprite: leave the HD background showing */
            }
        }
    }
}
