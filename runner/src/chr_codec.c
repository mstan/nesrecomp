/*
 * chr_codec.c — NES 2bpp CHR encode/decode from PNG.
 *
 * Uses stb_image for PNG loading.  Quantizes to 4 colors by luminance,
 * encodes as NES 2bpp CHR (8x8 tiles, 16 bytes each).  Supports .bin
 * disk caching so PNG decode only happens once.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

#include "chr_codec.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <sys/types.h>
#  include <sys/stat.h>
#  define STAT_CALL _stat
#  define StatBuf   struct _stat
#else
#  include <sys/stat.h>
#  define STAT_CALL stat
#  define StatBuf   struct stat
#endif

/* ── Helpers ───────────────────────────────────────────────────────────────── */

static int cmp_int_asc(const void *a, const void *b) {
    int va = *(const int *)a, vb = *(const int *)b;
    return (va > vb) - (va < vb);
}

/* ── Pixel -> 2bpp CHR encode ─────────────────────────────────────────────── */

/* Luminance of an RGB pixel (fixed-point approximation). */
static int pixel_luma(uint8_t r, uint8_t g, uint8_t b) {
    return (int)(77 * r + 150 * g + 29 * b); /* 0..65536 range */
}

/*
 * Quantize an image to 4 levels by luminance.
 * Returns a malloc'd buffer of w*h uint8_t values (0-3), or NULL on error.
 */
static uint8_t *quantize_4color(const uint8_t *pixels, int w, int h, int comp) {
    /* Collect unique luma values (up to a reasonable limit). */
    int *lumas = (int *)malloc(w * h * sizeof(int));
    if (!lumas) return NULL;

    for (int i = 0; i < w * h; i++) {
        const uint8_t *p = pixels + i * comp;
        lumas[i] = pixel_luma(p[0], p[1], p[2]);
    }

    /* Find unique luma values. */
    int unique[256];
    int num_unique = 0;

    /* Simple approach: sort all lumas, deduplicate. For small tile images
     * (typically < 32K pixels) this is fast enough. */
    /* Actually, let's use a smarter approach: histogram-based. */
    int *sorted = (int *)malloc(w * h * sizeof(int));
    if (!sorted) { free(lumas); return NULL; }
    memcpy(sorted, lumas, w * h * sizeof(int));

    /* Sort for deduplication. */
    qsort(sorted, w * h, sizeof(int), cmp_int_asc);

    num_unique = 0;
    for (int i = 0; i < w * h; i++) {
        if (num_unique == 0 || sorted[i] != unique[num_unique - 1]) {
            if (num_unique >= 256) break;
            unique[num_unique++] = sorted[i];
        }
    }
    free(sorted);

    if (num_unique > 4) {
        /* More than 4 unique luma levels — quantize by nearest.
         * Split luma range into 4 equal bands. */
        int lo = unique[0];
        int hi = unique[num_unique - 1];
        int range = hi - lo;
        if (range == 0) range = 1;

        uint8_t *result = (uint8_t *)malloc(w * h);
        if (!result) { free(lumas); return NULL; }

        for (int i = 0; i < w * h; i++) {
            int band = (lumas[i] - lo) * 3 / range; /* 0..3 */
            if (band > 3) band = 3;
            result[i] = (uint8_t)band;
        }
        free(lumas);
        return result;
    }

    /* <= 4 unique lumas — exact mapping. */
    uint8_t *result = (uint8_t *)malloc(w * h);
    if (!result) { free(lumas); return NULL; }

    for (int i = 0; i < w * h; i++) {
        for (int j = 0; j < num_unique; j++) {
            if (lumas[i] == unique[j]) {
                result[i] = (uint8_t)j;
                break;
            }
        }
    }

    free(lumas);
    return result;
}

/*
 * Encode quantized pixels (0-3) into NES 2bpp CHR data.
 * Image must be a multiple of 8 in both dimensions.
 * Tiles read left-to-right, top-to-bottom.
 */
static uint8_t *encode_chr(const uint8_t *qpix, int w, int h, int *out_size) {
    int cols = w / 8;
    int rows = h / 8;
    int num_tiles = cols * rows;
    int size = num_tiles * 16;
    uint8_t *chr = (uint8_t *)calloc(1, size);
    if (!chr) return NULL;

    for (int tr = 0; tr < rows; tr++) {
        for (int tc = 0; tc < cols; tc++) {
            int tile_idx = tr * cols + tc;
            uint8_t *tile = chr + tile_idx * 16;

            for (int y = 0; y < 8; y++) {
                uint8_t bp0 = 0, bp1 = 0;
                for (int x = 0; x < 8; x++) {
                    int px = qpix[(tr * 8 + y) * w + (tc * 8 + x)];
                    int bit = 7 - x;
                    bp0 |= (px & 1) << bit;
                    bp1 |= ((px >> 1) & 1) << bit;
                }
                tile[y]     = bp0;
                tile[y + 8] = bp1;
            }
        }
    }

    *out_size = size;
    return chr;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int chr_decode_png(const char *png_path, uint8_t **out_data, int *out_size) {
    int w, h, comp;
    uint8_t *pixels = stbi_load(png_path, &w, &h, &comp, 3); /* force RGB */
    if (!pixels) {
        fprintf(stderr, "[ChrCodec] Failed to load PNG: %s (%s)\n",
                png_path, stbi_failure_reason());
        return -1;
    }

    if (w % 8 != 0 || h % 8 != 0) {
        fprintf(stderr, "[ChrCodec] PNG dimensions %dx%d must be multiples of 8: %s\n",
                w, h, png_path);
        stbi_image_free(pixels);
        return -1;
    }

    uint8_t *qpix = quantize_4color(pixels, w, h, 3);
    stbi_image_free(pixels);
    if (!qpix) {
        fprintf(stderr, "[ChrCodec] Quantization failed: %s\n", png_path);
        return -1;
    }

    int size = 0;
    uint8_t *chr = encode_chr(qpix, w, h, &size);
    free(qpix);
    if (!chr) {
        fprintf(stderr, "[ChrCodec] CHR encode failed: %s\n", png_path);
        return -1;
    }

    *out_data = chr;
    *out_size = size;
    return 0;
}

/* ── Disk cache ───────────────────────────────────────────────────────────── */

/* Build cache path: "foo/bar.png" -> "foo/bar.chr.bin" */
static void make_cache_path(const char *png_path, char *out, int max) {
    const char *dot = strrchr(png_path, '.');
    if (dot) {
        int prefix_len = (int)(dot - png_path);
        snprintf(out, max, "%.*s.chr.bin", prefix_len, png_path);
    } else {
        snprintf(out, max, "%s.chr.bin", png_path);
    }
}

static time_t file_mtime(const char *path) {
    StatBuf st;
    if (STAT_CALL(path, &st) != 0) return 0;
    return st.st_mtime;
}

int chr_load_cached(const char *png_path, uint8_t **out_data, int *out_size) {
    char cache_path[512];
    make_cache_path(png_path, cache_path, sizeof(cache_path));

    time_t png_mtime = file_mtime(png_path);
    time_t bin_mtime = file_mtime(cache_path);

    /* If cache exists and is newer, load it directly. */
    if (bin_mtime > 0 && bin_mtime >= png_mtime) {
        FILE *f = fopen(cache_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz % 16 == 0) {
                uint8_t *data = (uint8_t *)malloc(sz);
                if (data && fread(data, 1, sz, f) == (size_t)sz) {
                    fclose(f);
                    *out_data = data;
                    *out_size = (int)sz;
                    printf("[ChrCodec] Loaded cached: %s (%d bytes)\n",
                           cache_path, (int)sz);
                    return 0;
                }
                free(data);
            }
            fclose(f);
        }
    }

    /* Decode from PNG. */
    int rc = chr_decode_png(png_path, out_data, out_size);
    if (rc != 0) return rc;

    /* Write cache. */
    FILE *f = fopen(cache_path, "wb");
    if (f) {
        fwrite(*out_data, 1, *out_size, f);
        fclose(f);
        printf("[ChrCodec] Converted + cached: %s -> %s (%d bytes)\n",
               png_path, cache_path, *out_size);
    } else {
        printf("[ChrCodec] Converted: %s (%d bytes) [cache write failed]\n",
               png_path, *out_size);
    }

    return 0;
}

/* ── CHR -> PNG ───────────────────────────────────────────────────────────── */

/* Grayscale palette for 2bpp NES tiles. */
static const uint8_t s_pal[4][3] = {
    {0x00, 0x00, 0x00},  /* 0 = black */
    {0x55, 0x55, 0x55},  /* 1 = dark gray */
    {0xAA, 0xAA, 0xAA},  /* 2 = light gray */
    {0xFF, 0xFF, 0xFF},  /* 3 = white */
};

int chr_write_png(const char *png_path, const uint8_t *chr_data, int chr_size) {
    if (chr_size < 16 || chr_size % 16 != 0) {
        fprintf(stderr, "[ChrCodec] Invalid CHR size %d for PNG write\n", chr_size);
        return -1;
    }

    int num_tiles = chr_size / 16;
    int cols = 16;
    if (num_tiles < cols) cols = num_tiles;
    int rows = (num_tiles + cols - 1) / cols;
    int img_w = cols * 8;
    int img_h = rows * 8;

    uint8_t *rgb = (uint8_t *)calloc(1, img_w * img_h * 3);
    if (!rgb) return -1;

    for (int t = 0; t < num_tiles; t++) {
        const uint8_t *tile = chr_data + t * 16;
        int tx = (t % cols) * 8;
        int ty = (t / cols) * 8;

        for (int y = 0; y < 8; y++) {
            uint8_t bp0 = tile[y];
            uint8_t bp1 = tile[y + 8];
            for (int x = 0; x < 8; x++) {
                int bit = 7 - x;
                int val = ((bp0 >> bit) & 1) | (((bp1 >> bit) & 1) << 1);
                int px = ((ty + y) * img_w + (tx + x)) * 3;
                rgb[px + 0] = s_pal[val][0];
                rgb[px + 1] = s_pal[val][1];
                rgb[px + 2] = s_pal[val][2];
            }
        }
    }

    int ok = stbi_write_png(png_path, img_w, img_h, 3, rgb, img_w * 3);
    free(rgb);

    if (!ok) {
        fprintf(stderr, "[ChrCodec] Failed to write PNG: %s\n", png_path);
        return -1;
    }
    return 0;
}
