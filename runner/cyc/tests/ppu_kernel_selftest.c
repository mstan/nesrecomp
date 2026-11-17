#include "ppu_hle_kernel.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

unsigned portable_sprite_mask(const uint8_t[8],const uint8_t[8],const uint8_t[8],bool);
unsigned portable_sprite_shift(uint8_t[8],uint8_t[8],uint8_t[8],bool,bool);
static uint32_t rng=0x1f123bb5u;
static uint32_t random32(void) {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void check(const uint8_t x[8],const uint8_t lo[8],const uint8_t hi[8]) {
    for(unsigned flags=0;flags<4;++flags) {
        bool render=(flags&1)!=0,skip=(flags&2)!=0;
        uint8_t nx[8],nl[8],nh[8],px[8],pl[8],ph[8];
        memcpy(nx,x,8);memcpy(nl,lo,8);memcpy(nh,hi,8);
        memcpy(px,x,8);memcpy(pl,lo,8);memcpy(ph,hi,8);
        unsigned ready=0,pixels=0;
        for(unsigned i=0;i<8;++i) {
            bool active=skip||x[i]==0;
            if(active&&((lo[i]>>7)|(hi[i]>>7)))pixels|=1u<<i;
            if(render&&active)ready|=1u<<i;
        }
        assert(ppu_hle_sprite_mask(x,lo,hi,skip)==pixels);
        assert(portable_sprite_mask(x,lo,hi,skip)==pixels);
        assert(ppu_hle_sprite_shift(nx,nl,nh,render,skip)==ready);
        assert(portable_sprite_shift(px,pl,ph,render,skip)==ready);
        assert(!memcmp(nx,px,8)&&!memcmp(nl,pl,8)&&!memcmp(nh,ph,8));
        for(unsigned i=0;i<8;++i) {
            assert(nx[i]==(uint8_t)(x[i]-(!skip&&x[i]!=0)));
            assert(nl[i]==(uint8_t)((ready>>i&1)?lo[i]<<1:lo[i]));
            assert(nh[i]==(uint8_t)((ready>>i&1)?hi[i]<<1:hi[i]));
        }
    }
}
int main(void) {
    for(unsigned lo=0;lo<256;++lo)for(unsigned hi=0;hi<256;++hi) {
        uint16_t want=0;
        for(unsigned bit=0;bit<8;++bit)want|=(uint16_t)(((lo>>bit&1)|((hi>>bit&1)<<1))<<(2*bit));
        assert(ppu_hle_row((uint8_t)lo,(uint8_t)hi)==want);
    }
    for(unsigned mask=1;mask<256;++mask) {
        unsigned bit=0;while(!(mask>>bit&1))++bit;
        assert(ppu_hle_first_sprite(mask)==bit);
    }
    /* Every simultaneous lane mask, including all eight high bits: catches
     * horizontal-pack/overflow mistakes that sparse random lanes can miss. */
    for(unsigned mask=0;mask<256;++mask) {
        uint8_t x[8],lo[8],hi[8]={0};
        for(unsigned i=0;i<8;++i) {
            x[i]=(mask>>i&1)?0:1;lo[i]=(mask>>i&1)?0x80:0;
        }
        check(x,lo,hi);
    }
    for(unsigned byte=0;byte<256;++byte)for(unsigned lane=0;lane<8;++lane) {
        uint8_t x[8]={0},lo[8]={0},hi[8]={0};
        x[lane]=(uint8_t)byte;lo[lane]=0x81;hi[(lane+1)&7]=(uint8_t)~byte;check(x,lo,hi);
    }
    for(unsigned test=0;test<250000;++test) {
        uint8_t x[8],lo[8],hi[8];
        for(unsigned i=0;i<8;++i) {x[i]=(uint8_t)random32();lo[i]=(uint8_t)random32();hi[i]=(uint8_t)random32();}
        check(x,lo,hi);
    }
    printf("PPU kernels: exact row decode, priority and shift; native/portable paths (NEON=%d SSE2=%d)\n",
           NESRECOMP_PPU_HLE_NEON, NESRECOMP_PPU_HLE_SSE2);
    return 0;
}
