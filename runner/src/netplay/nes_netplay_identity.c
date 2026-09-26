/* See nes_netplay_identity.h. */
#include "nes_netplay_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef NESRECOMP_GAME_VERSION
#define NESRECOMP_GAME_VERSION "dev"
#endif

/* ---- SHA-256 ------------------------------------------------------------ */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh;
    int i;
    for (i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) |
               ((uint32_t)p[4*i+2] << 8) | p[4*i+3];
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=h[0]; b=h[1]; c=h[2]; d=h[3]; e=h[4]; f=h[5]; g=h[6]; hh=h[7];
    for (i = 0; i < 64; ++i) {
        uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void nes_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint8_t tail[128];
    size_t full = len / 64, rem = len % 64, tl, i;
    uint64_t bits = (uint64_t)len * 8u;
    for (i = 0; i < full; ++i) sha_block(h, data + 64 * i);
    memset(tail, 0, sizeof(tail));
    if (rem) memcpy(tail, data + 64 * full, rem);
    tail[rem] = 0x80;
    tl = (rem + 9 <= 64) ? 64 : 128;
    for (i = 0; i < 8; ++i) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha_block(h, tail);
    if (tl == 128) sha_block(h, tail + 64);
    for (i = 0; i < 8; ++i) {
        out[4*i] = (uint8_t)(h[i] >> 24); out[4*i+1] = (uint8_t)(h[i] >> 16);
        out[4*i+2] = (uint8_t)(h[i] >> 8); out[4*i+3] = (uint8_t)h[i];
    }
}

/* ---- identity ----------------------------------------------------------- */
static char     s_version[96];
static char     s_rom_hex[65];
static uint32_t s_build_fp, s_content_fp;

static int exe_bytes(uint8_t **out, size_t *len)
{
    char path[4096];
    FILE *f;
    long n;
#ifdef _WIN32
    DWORD k = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (!k || k >= sizeof(path)) return 0;
#else
    ssize_t k = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (k <= 0) return 0;
    path[k] = 0;
#endif
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    *out = (uint8_t *)malloc((size_t)n);
    if (!*out) { fclose(f); return 0; }
    if (fread(*out, 1, (size_t)n, f) != (size_t)n) { free(*out); fclose(f); return 0; }
    fclose(f);
    *len = (size_t)n;
    return 1;
}

static uint32_t fold(const uint8_t d[32])
{
    uint32_t x = 0;
    int i;
    for (i = 0; i < 32; i += 4)
        x ^= ((uint32_t)d[i] << 24) | ((uint32_t)d[i+1] << 16) |
             ((uint32_t)d[i+2] << 8) | d[i+3];
    return x ? x : 1u;   /* 0 means "not supplied" to the driver */
}

const char *nes_netplay_identity_game_version(void)
{
    if (!s_version[0]) {
        uint8_t *exe = NULL, d[32];
        size_t len = 0;
        if (exe_bytes(&exe, &len)) {
            nes_sha256(exe, len, d);
            free(exe);
            s_build_fp = fold(d);
            /* The lobby carries RNET_LOBBY_VERSION_LEN-1 = 31 characters and
             * a longer string is truncated on one path and not the other
             * (measured: every join refused "version_mismatch"). Keep the
             * whole identity inside 31: <=18 of the release pin, '+', 12 hex
             * (48 bits) of the executable's SHA-256. */
            snprintf(s_version, sizeof(s_version),
                     "%.18s+%02x%02x%02x%02x%02x%02x", NESRECOMP_GAME_VERSION,
                     d[0], d[1], d[2], d[3], d[4], d[5]);
        } else {
            /* Without the image there is no exact identity; say so in the
             * string rather than pass for a stamped build. */
            snprintf(s_version, sizeof(s_version), "%s+unhashed", NESRECOMP_GAME_VERSION);
            s_build_fp = 0;
        }
    }
    return s_version;
}

void nes_netplay_identity_set_rom(const uint8_t *rom, size_t len)
{
    uint8_t d[32];
    int i;
    if (!rom || !len) return;
    nes_sha256(rom, len, d);
    for (i = 0; i < 32; ++i) snprintf(s_rom_hex + 2 * i, 3, "%02x", d[i]);
    s_content_fp = fold(d);
}

int nes_netplay_identity_set_rom_file(const char *path)
{
    FILE *f;
    long n;
    uint8_t *buf;
    if (!path || !path[0] || !(f = fopen(path, "rb"))) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return 0; }
    fclose(f);
    nes_netplay_identity_set_rom(buf, (size_t)n);
    free(buf);
    return 1;
}

const char *nes_netplay_identity_rom_sha256(void) { return s_rom_hex; }

uint32_t nes_netplay_identity_build_fp(void)
{
    (void)nes_netplay_identity_game_version();
    return s_build_fp;
}

uint32_t nes_netplay_identity_content_fp(void) { return s_content_fp; }
