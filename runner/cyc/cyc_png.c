/*
 * cyc_png.c - minimal PNG writer for cyc_framebuffer screenshots.
 *
 * Writes 8-bit RGB with an uncompressed (stored-block) zlib stream, so it
 * needs no compression library. A 256x240 frame is about 185KB.
 */
#include "cyc_png.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    put32(hdr, len);
    for (int i = 0; i < 4; i++) hdr[4 + i] = (uint8_t)type[i];
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t crc = crc32_update(0, hdr + 4, 4);
    crc = crc32_update(crc, data, len);
    uint8_t tail[4];
    put32(tail, crc);
    fwrite(tail, 1, 4, f);
}

bool cyc_write_png(const char *path, const uint32_t *argb, int width, int height) {
    size_t row = (size_t)width * 3 + 1;
    size_t raw_len = row * (size_t)height;
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    uint8_t *z = (uint8_t *)malloc(z_len);
    if (!raw || !z) {
        free(raw);
        free(z);
        return false;
    }
    for (int y = 0; y < height; y++) {
        uint8_t *r = raw + row * (size_t)y;
        *r++ = 0; /* filter: none */
        for (int x = 0; x < width; x++) {
            uint32_t c = argb[y * width + x];
            *r++ = (uint8_t)(c >> 16);
            *r++ = (uint8_t)(c >> 8);
            *r++ = (uint8_t)c;
        }
    }
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < raw_len;) {
        size_t n = raw_len - pos > 65535 ? 65535 : raw_len - pos;
        z[o++] = pos + n == raw_len ? 1 : 0;
        z[o++] = (uint8_t)n;
        z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n;
        z[o++] = (uint8_t)(~n >> 8);
        for (size_t i = 0; i < n; i++) {
            uint8_t v = raw[pos + i];
            z[o++] = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
        }
        pos += n;
    }
    put32(z + o, (b << 16) | a);
    o += 4;

    FILE *f = fopen(path, "wb");
    bool ok = f != NULL;
    if (f) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        fwrite(sig, 1, 8, f);
        uint8_t ihdr[13];
        put32(ihdr, (uint32_t)width);
        put32(ihdr + 4, (uint32_t)height);
        ihdr[8] = 8;  /* bit depth */
        ihdr[9] = 2;  /* RGB */
        ihdr[10] = 0;
        ihdr[11] = 0;
        ihdr[12] = 0;
        chunk(f, "IHDR", ihdr, 13);
        chunk(f, "IDAT", z, (uint32_t)o);
        chunk(f, "IEND", NULL, 0);
        ok = fclose(f) == 0;
    }
    free(raw);
    free(z);
    return ok;
}
