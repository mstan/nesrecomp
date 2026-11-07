/* Exact independent definition of the existing rollback hash contract. */
#include "nes_rb_state.h"
#include "logical_input.h"
#include "../../src/netplay/nes_netplay_identity.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t reference_hash(const uint8_t *p, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}
static void reference(const uint8_t *img, size_t len, NesRbDigest *out) {
    memset(out, 0, sizeof *out);
    if (!img || len < 48) return;
    out->master = reference_hash(img, len, 2166136261u);
    for (int i = 0; i < 3; ++i) out->part[i] = 2166136261u;
    out->part[0] = reference_hash(img, 32, out->part[0]);
    size_t at = 32, end = len - 16;
    while (end - at >= 8) {
        uint32_t n; memcpy(&n, img + at + 4, 4);
        if (n > end - at - 8) break;
        int part = !memcmp(img+at, "PPU ", 4) || !memcmp(img+at, "PIC", 3) || !memcmp(img+at, "CHRR", 4) ? 1 :
                   !memcmp(img+at, "CPU ", 4) || !memcmp(img+at, "HW  ", 4) || !memcmp(img+at, "CART", 4) ||
                   !memcmp(img+at, "TIME", 4) || !memcmp(img+at, "REGN", 4) ? 0 : 2;
        out->part[part] = reference_hash(img+at, 8+n, out->part[part]); at += 8+n;
    }
    out->part[2] = reference_hash(img+end, 16, out->part[2]);
}
static void check(const uint8_t *image, size_t len) {
    NesRbDigest want, got;
    reference(image, len, &want); nes_rb_digest_image(image, len, &got);
    assert(!memcmp(&want, &got, sizeof got));
}
static uint32_t rng = 0x70925f2bu;
static uint32_t random32(void) { rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return rng; }
static void config_reference(void) {
    char guarded[80];memset(guarded,0x5a,sizeof guarded);
    const char *canonical="nes-session/1\ninput=2:03ff:cycle-lle-c1\nk0=0000000000000000000000000000000000000000000000000000000000000000\nk1=0000000000000000000000000000000000000000000000000000000000000001\nk2=0000000000000000000000000000000000000000000000000000000000000002\nk3=0000000000000000000000000000000000000000000000000000000000000003\n";
    nes_netplay_session_reference(canonical,guarded);
    assert(strlen(guarded)==50);assert(!strcmp(guarded,"sha256:6QmPt6X6ncvahK3Wp3IcUKRR80Cmblu_hqSEMG23dgk"));
    for(int i=51;i<sizeof guarded;i++)assert(guarded[i]==0x5a);
    char changed[512],other[64];snprintf(changed,sizeof changed,"%sx",canonical);
    nes_netplay_session_reference(changed,other);assert(strcmp(guarded,other));
}
extern unsigned rb_test_loaded;
static void input_snapshot(void) {
    uint8_t image[48];size_t n=0;NesRbDigest before,after;
    for(int seat=0;seat<4;seat++){g_logical_input[seat]=(uint8_t)(0x11u<<seat);g_game_input[seat]=(unsigned)seat;}
    nes_rb_state_invalidate();assert(nes_rb_state_save(image,sizeof image,&n)==sizeof image);assert(n==48);
    assert(!memcmp(image+32,"CYCRBIN2",8));
    for(int seat=0;seat<4;seat++)assert(image[44+seat]==seat);
    nes_rb_digest_image(image,n,&before);
    for(int seat=0;seat<4;seat++) {
        image[44+seat]^=1;nes_rb_digest_image(image,n,&after);assert(before.master!=after.master);assert(before.part[2]!=after.part[2]);
        assert(before.part[0]==after.part[0]&&before.part[1]==after.part[1]);image[44+seat]^=1;
    }
    memset(g_logical_input,0,4);memset(g_game_input,0,sizeof g_game_input);
    assert(nes_rb_state_load(image,n));assert(rb_test_loaded==1);
    for(int seat=0;seat<4;seat++){assert(g_logical_input[seat]==(uint8_t)(0x11u<<seat));assert(g_game_input[seat]==seat);}
    image[32]='X';assert(!nes_rb_state_load(image,n));assert(rb_test_loaded==1);
}
int main(void) {
    input_snapshot();config_reference();
    uint8_t *image = malloc(1024 * 1024); assert(image);
    check(NULL, 0); check(NULL, 100);
    for (size_t n = 0; n < 80; ++n) { memset(image, 0xff, 80); check(image, n); }
    const char tags[][5] = { "CPU ", "HW  ", "CART", "TIME", "REGN", "PPU ", "PICI", "PICL", "PICB", "CHRR", "APU ", "MOD ", "END ", "????" };
    for (unsigned test = 0; test < 500; ++test) {
        size_t n = 32;
        for (size_t i = 0; i < 32; ++i) image[i] = (uint8_t)random32();
        for (unsigned chunk = 0, count = random32()%30; chunk < count; ++chunk) {
            uint32_t len = random32()%8192;
            memcpy(image+n, tags[random32()%(sizeof tags/sizeof tags[0])], 4);
            memcpy(image+n+4, &len, 4); n+=8;
            for (uint32_t i = 0; i < len; ++i) image[n++] = (uint8_t)random32();
        }
        for (unsigned i = 0; i < 16; ++i) image[n++] = (uint8_t)random32();
        check(image, n);
        for (unsigned i = 0; i < 5; ++i) check(image, random32()%n);
        if (n >= 52) {
            uint32_t bad = UINT32_MAX; memcpy(image+36, &bad, 4); check(image, n);
        }
    }
    free(image); puts("rollback digest: exact master and partitions, valid/malformed/truncated inputs");
    return 0;
}
