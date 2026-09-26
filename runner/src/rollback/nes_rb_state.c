/*
 * nes_rb_state.c -- rollback snapshot + digest over one domain.
 * Contract and rationale: include/nes_rb_state.h, docs/NETPLAY.md.
 */
#include "nes_rb_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "savestate.h"
#include "logical_input.h"

/* Image = [u32 v7_len][V7 image][trailer]. */
#define RB_TRAILER_MAGIC "NRBX"
#define RB_TRAILER_VERSION 1u
#define RB_TRAILER_BYTES (4u + 1u + 4u)

static const char *const k_part_names[3] = { "cpu_wram", "ppu", "apu_io_mods" };

const char *nes_rb_part_name(int i)
{
    return (i >= 0 && i < 3) ? k_part_names[i] : "?";
}

/* ---- timing ------------------------------------------------------------ */

#define RB_TIMING_SAMPLES 4096
typedef struct { float v[RB_TIMING_SAMPLES]; uint64_t n; } RbSamples;
static RbSamples s_save_t, s_load_t, s_digest_t;
static size_t s_last_image_bytes;

static void sample_add(RbSamples *s, double us)
{
    s->v[s->n % RB_TIMING_SAMPLES] = (float)us;
    s->n++;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void sample_pct(const RbSamples *s, double *p50, double *p99)
{
    static float tmp[RB_TIMING_SAMPLES];
    size_t n = s->n < RB_TIMING_SAMPLES ? (size_t)s->n : RB_TIMING_SAMPLES;
    *p50 = *p99 = 0.0;
    if (!n) return;
    memcpy(tmp, s->v, n * sizeof(float));
    qsort(tmp, n, sizeof(float), cmp_float);
    *p50 = tmp[(n - 1) / 2];
    *p99 = tmp[(size_t)((double)(n - 1) * 0.99)];
}

static double now_us(void)
{
    return (double)SDL_GetPerformanceCounter() * 1e6 /
           (double)SDL_GetPerformanceFrequency();
}

void nes_rb_state_timing(NesRbStateTiming *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->saves = s_save_t.n;
    out->loads = s_load_t.n;
    out->digests = s_digest_t.n;
    sample_pct(&s_save_t, &out->save_us_p50, &out->save_us_p99);
    sample_pct(&s_load_t, &out->load_us_p50, &out->load_us_p99);
    sample_pct(&s_digest_t, &out->digest_us_p50, &out->digest_us_p99);
    out->image_bytes = s_last_image_bytes;
}

void nes_rb_state_log_timing(const char *tag)
{
    NesRbStateTiming t;
    nes_rb_state_timing(&t);
    fprintf(stderr,
            "RB_STATE_TIMING %s image=%zuB saves=%llu save_ms_p50=%.4f p99=%.4f "
            "loads=%llu load_ms_p50=%.4f p99=%.4f digests=%llu "
            "digest_ms_p50=%.4f p99=%.4f\n",
            tag ? tag : "-", t.image_bytes,
            (unsigned long long)t.saves, t.save_us_p50 / 1000.0,
            t.save_us_p99 / 1000.0, (unsigned long long)t.loads,
            t.load_us_p50 / 1000.0, t.load_us_p99 / 1000.0,
            (unsigned long long)t.digests, t.digest_us_p50 / 1000.0,
            t.digest_us_p99 / 1000.0);
}

/* ---- hash ----------------------------------------------------------------
 * 64-bit multiply/xorshift over little-endian words, folded to 32 bits. Every
 * peer runs the same build (identity-checked), so only determinism matters;
 * CRC32 over ~170 KB per tick was the cost this replaces. */
static uint64_t hash64(const uint8_t *p, size_t n, uint64_t h)
{
    const uint64_t m = 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    h ^= (uint64_t)n * m;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * m;
        h ^= h >> 29;
    }
    if (i < n) {
        uint64_t w = 0;
        memcpy(&w, p + i, n - i);
        h = (h ^ w) * m;
        h ^= h >> 29;
    }
    h ^= h >> 32;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    return h;
}

static uint32_t fold32(uint64_t h) { return (uint32_t)(h ^ (h >> 32)); }

/* ---- serialization ------------------------------------------------------ */

size_t nes_rb_state_save(uint8_t *buf, size_t cap, size_t *need)
{
    size_t inner_need = 0, n;
    uint32_t v7_len;
    double t0 = now_us();

    if (need) *need = 0;
    if (!buf || cap < 4u + RB_TRAILER_BYTES) {
        (void)savestate_serialize_ex(NULL, 0, &inner_need);
        if (need) *need = 4u + inner_need + RB_TRAILER_BYTES;
        return 0;
    }
    n = savestate_serialize_ex(buf + 4, cap - 4 - RB_TRAILER_BYTES, &inner_need);
    if (!n) {
        if (need && inner_need) *need = 4u + inner_need + RB_TRAILER_BYTES;
        return 0;
    }
    v7_len = (uint32_t)n;
    memcpy(buf, &v7_len, 4);
    {
        uint8_t *t = buf + 4 + n;
        memcpy(t, RB_TRAILER_MAGIC, 4);
        t[4] = (uint8_t)RB_TRAILER_VERSION;
        memcpy(t + 5, g_logical_input, 4);
    }
    n = 4u + n + RB_TRAILER_BYTES;
    if (need) *need = n;
    sample_add(&s_save_t, now_us() - t0);
    s_last_image_bytes = n;
    return n;
}

int nes_rb_state_load(const uint8_t *buf, size_t len)
{
    uint32_t v7_len = 0;
    const uint8_t *t;
    double t0 = now_us();

    if (!buf || len < 4u + RB_TRAILER_BYTES) return 0;
    memcpy(&v7_len, buf, 4);
    if ((size_t)v7_len + 4u + RB_TRAILER_BYTES != len) return 0;
    t = buf + 4 + v7_len;
    if (memcmp(t, RB_TRAILER_MAGIC, 4) != 0 || t[4] != RB_TRAILER_VERSION)
        return 0;
    if (!savestate_deserialize(buf + 4, v7_len, SAVESTATE_LOAD_QUIET))
        return 0;
    memcpy(g_logical_input, t + 5, 4);
    nes_rb_state_invalidate();
    sample_add(&s_load_t, now_us() - t0);
    return 1;
}

/* ---- the cached current image ------------------------------------------- */

static uint8_t *s_img;
static size_t   s_img_cap, s_img_len;
static int      s_img_valid, s_dig_valid;
static NesRbDigest s_dig;

void nes_rb_state_invalidate(void)
{
    s_img_valid = 0;
    s_dig_valid = 0;
}

const uint8_t *nes_rb_state_image(size_t *len)
{
    if (!s_img_valid) {
        size_t need = 0, n = 0;
        int tries;
        if (!s_img) {
            s_img_cap = 256u * 1024u;
            s_img = (uint8_t *)malloc(s_img_cap);
            if (!s_img) { s_img_cap = 0; return NULL; }
        }
        for (tries = 0; tries < 3; ++tries) {
            n = nes_rb_state_save(s_img, s_img_cap, &need);
            if (n) break;
            if (need <= s_img_cap) return NULL;   /* a real failure, not space */
            {
                size_t cap = need + need / 4 + 4096;
                uint8_t *nb = (uint8_t *)realloc(s_img, cap);
                if (!nb) return NULL;
                s_img = nb;
                s_img_cap = cap;
            }
        }
        if (!n) return NULL;
        s_img_len = n;
        s_img_valid = 1;
        s_dig_valid = 0;
    }
    if (len) *len = s_img_len;
    return s_img;
}

void nes_rb_digest_image(const uint8_t *img, size_t len, NesRbDigest *out)
{
    size_t cpu_end = 0, ppu_end = 0;
    memset(out, 0, sizeof(*out));
    if (!img || len < 4) return;
    savestate_partitions(&cpu_end, &ppu_end);
    /* Offsets are within the V7 image, which starts after the u32 length. */
    cpu_end += 4;
    ppu_end += 4;
    if (ppu_end > len) ppu_end = len;
    if (cpu_end > ppu_end) cpu_end = ppu_end;
    out->part[0] = fold32(hash64(img, cpu_end, 0x6E65735F63707500ull));
    out->part[1] = fold32(hash64(img + cpu_end, ppu_end - cpu_end, 0x6E65735F70707500ull));
    out->part[2] = fold32(hash64(img + ppu_end, len - ppu_end, 0x6E65735F61707500ull));
    out->master = fold32(hash64(img, len, 0x6E65735F6D737400ull));
}

void nes_rb_state_digest(NesRbDigest *out)
{
    if (!s_dig_valid || !s_img_valid) {
        size_t len = 0;
        const uint8_t *img = nes_rb_state_image(&len);
        double t0 = now_us();
        nes_rb_digest_image(img, img ? len : 0, &s_dig);
        sample_add(&s_digest_t, now_us() - t0);
        s_dig_valid = img != NULL;
    }
    if (out) *out = s_dig;
}

uint32_t nes_rb_state_digest_master(void)
{
    NesRbDigest d;
    nes_rb_state_digest(&d);
    return d.master;
}

long nes_rb_state_first_diff(const uint8_t *a, size_t alen,
                             const uint8_t *b, size_t blen,
                             char *what, size_t what_cap)
{
    size_t n = alen < blen ? alen : blen, i;
    if (what && what_cap) what[0] = 0;
    for (i = 0; i < n; ++i)
        if (a[i] != b[i]) break;
    if (i == n && alen == blen) return -1;
    if (what && what_cap) {
        if (i < 4) snprintf(what, what_cap, "image-length");
        else savestate_describe_offset(a + 4, alen - 4, i - 4, what, what_cap);
    }
    return (long)i;
}

void nes_rb_state_describe(const uint8_t *img, size_t len, size_t off,
                           char *what, size_t cap)
{
    if (!what || !cap) return;
    if (off < 4) { snprintf(what, cap, "image-length"); return; }
    if (img && len >= 4) {
        uint32_t v7 = 0;
        memcpy(&v7, img, 4);
        if (off >= 4u + v7) {
            size_t t = off - 4u - v7;
            snprintf(what, cap, t < 5 ? "rb_trailer_header" : "logical_input+%zu",
                     t < 5 ? t : t - 5);
            return;
        }
    }
    savestate_describe_offset(img + 4, len - 4, off - 4, what, cap);
}
