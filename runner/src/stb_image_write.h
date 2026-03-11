/*
 * stb_image_write.h — Minimal PNG writer (uncompressed PNG for screenshot use)
 * Not the real STB library — a purpose-built minimal version for NESRecomp.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef STB_IMAGE_WRITE_IMPLEMENTATION

static uint32_t stbi__crc32(const uint8_t *buf, int len) {
    static uint32_t crc_table[256];
    static int table_init = 0;
    if (!table_init) {
        for (int i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            crc_table[i] = c;
        }
        table_init = 1;
    }
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++) crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

static void stbi__write_u32_be(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v };
    fwrite(b, 1, 4, f);
}

static void stbi__write_chunk(FILE *f, const char *type, const uint8_t *data, int len) {
    stbi__write_u32_be(f, (uint32_t)len);
    uint8_t type4[4]; memcpy(type4, type, 4);
    fwrite(type4, 1, 4, f);
    if (data && len > 0) fwrite(data, 1, len, f);
    /* CRC over type+data */
    uint8_t *tmp = (uint8_t*)malloc(4 + len);
    memcpy(tmp, type4, 4);
    if (data && len > 0) memcpy(tmp+4, data, len);
    uint32_t crc = stbi__crc32(tmp, 4+len);
    free(tmp);
    stbi__write_u32_be(f, crc);
}

/* Simple DEFLATE: no compression (stored blocks) */
static uint8_t *stbi__deflate_stored(const uint8_t *src, int src_len, int *out_len) {
    /* zlib header: CMF=0x78, FLG=0x01 (no dict, level 0) */
    /* Actually use 0x78 0x9C for compatibility */
    int block_max = 65535;
    int num_blocks = (src_len + block_max - 1) / block_max;
    if (src_len == 0) num_blocks = 1;

    /* Each stored block: BFINAL+BTYPE(1) + LEN(2) + NLEN(2) + data */
    int out_size = 2 + num_blocks * 5 + src_len + 4; /* +4 for adler32 */
    uint8_t *out = (uint8_t*)malloc(out_size);
    int pos = 0;

    /* zlib header */
    out[pos++] = 0x78;
    out[pos++] = 0x01;

    /* Adler32 running sum */
    uint32_t adler_s1 = 1, adler_s2 = 0;
    for (int i = 0; i < src_len; i++) {
        adler_s1 = (adler_s1 + src[i]) % 65521;
        adler_s2 = (adler_s2 + adler_s1) % 65521;
    }

    int remaining = src_len;
    const uint8_t *p = src;
    for (int b = 0; b < num_blocks; b++) {
        int blen = remaining < block_max ? remaining : block_max;
        int is_last = (b == num_blocks - 1) ? 1 : 0;
        out[pos++] = (uint8_t)(is_last | 0x00); /* BFINAL | BTYPE=00 (no compression) */
        uint16_t blen16 = (uint16_t)blen;
        out[pos++] = blen16 & 0xFF;
        out[pos++] = (blen16 >> 8) & 0xFF;
        out[pos++] = (~blen16) & 0xFF;
        out[pos++] = ((~blen16) >> 8) & 0xFF;
        memcpy(out + pos, p, blen);
        pos += blen;
        p += blen;
        remaining -= blen;
    }

    /* Adler32 checksum (big-endian) */
    uint32_t adler = (adler_s2 << 16) | adler_s1;
    out[pos++] = (adler >> 24) & 0xFF;
    out[pos++] = (adler >> 16) & 0xFF;
    out[pos++] = (adler >>  8) & 0xFF;
    out[pos++] = (adler      ) & 0xFF;

    *out_len = pos;
    return out;
}

static int stbi_write_png(const char *filename, int w, int h, int comp,
                           const void *data, int stride_bytes) {
    (void)comp; /* We assume RGB=3 */
    FILE *f = fopen(filename, "wb");
    if (!f) return 0;

    /* PNG signature */
    const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    fwrite(sig, 1, 8, f);

    /* IHDR */
    uint8_t ihdr[13];
    ihdr[0]=(w>>24)&0xFF; ihdr[1]=(w>>16)&0xFF; ihdr[2]=(w>>8)&0xFF; ihdr[3]=w&0xFF;
    ihdr[4]=(h>>24)&0xFF; ihdr[5]=(h>>16)&0xFF; ihdr[6]=(h>>8)&0xFF; ihdr[7]=h&0xFF;
    ihdr[8]=8;  /* bit depth */
    ihdr[9]=2;  /* color type: RGB */
    ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
    stbi__write_chunk(f, "IHDR", ihdr, 13);

    /* Build raw image data (filter byte per row = 0) */
    int raw_len = h * (1 + w * 3);
    uint8_t *raw = (uint8_t*)malloc(raw_len);
    const uint8_t *src = (const uint8_t *)data;
    for (int y = 0; y < h; y++) {
        raw[y*(1+w*3)] = 0; /* filter type: None */
        memcpy(raw + y*(1+w*3) + 1, src + y*stride_bytes, w*3);
    }

    /* Compress */
    int comp_len = 0;
    uint8_t *comp_data = stbi__deflate_stored(raw, raw_len, &comp_len);
    free(raw);

    stbi__write_chunk(f, "IDAT", comp_data, comp_len);
    free(comp_data);

    stbi__write_chunk(f, "IEND", NULL, 0);
    fclose(f);
    return 1;
}

#endif /* STB_IMAGE_WRITE_IMPLEMENTATION */
