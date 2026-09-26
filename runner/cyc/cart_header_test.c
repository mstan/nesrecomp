#include "../../common/nes_cart.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
int main(void)
{
    uint8_t h[16] = {'N','E','S',26,2,1,0,8};
    NesCartInfo c;
    CHECK(nes_cart_header(h,16,&c));
    CHECK(c.prg_size == 32768 && c.chr_size == 8192 && c.prg_ram == 0);
    h[8]=0x51; h[9]=0x21; h[10]=0x76; h[11]=0x87;
    CHECK(nes_cart_header(h,16,&c));
    CHECK(c.mapper==256 && c.submapper==5 && c.prg_size==258*16384 && c.chr_size==513*8192);
    CHECK(c.prg_ram==4096 && c.prg_nvram==8192 && c.chr_ram==8192 && c.chr_nvram==16384);
    h[4]=14*4+1; h[5]=13*4; h[9]=0xff;
    CHECK(nes_cart_header(h,16,&c));
    CHECK(c.prg_size==49152 && c.chr_size==8192);
    h[4]=255; CHECK(!nes_cart_header(h,16,&c));
    h[4]=2; h[5]=0; h[9]=0; h[8]=0; h[10]=0; h[11]=0;
    CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    h[11]=7; CHECK(nes_cart_header(h,16,&c)); CHECK(nes_cart_variant_supported(&c));
    h[7]=0; CHECK(nes_cart_header(h,16,&c)); CHECK(c.chr_ram==8192 && c.prg_ram==0);
    h[6]=0x12; CHECK(nes_cart_header(h,16,&c)); CHECK(c.prg_nvram==32768);
    h[6]=4; CHECK(nes_cart_header(h,16,&c)); CHECK(c.data_offset==528);
    CHECK(!nes_cart_image(h,16,&c)); CHECK(!nes_cart_header(h,15,&c));
    h[0]=0; CHECK(!nes_cart_header(h,16,&c));
    h[0]='N'; h[6]=0x50; h[8]=0;
    CHECK(nes_cart_header(h,16,&c)); CHECK(c.prg_ram==65536);
    CHECK(nes_cart_variant_supported(&c));
    h[7]=8; h[10]=0;
    CHECK(nes_cart_header(h,16,&c)); CHECK(c.prg_ram==0);
    for (unsigned v=0;v<256;++v) {
        h[10]=(uint8_t)v; CHECK(nes_cart_header(h,16,&c));
        unsigned a=v&15,b=v>>4;
        bool expected=a && b ? (a==7 || a==9) && (b==7 || b==9) :
            (a+b==0 || (a+b>=7 && a+b<=11));
        CHECK(nes_cart_variant_supported(&c)==expected);
    }
    h[10]=0; h[6]=0x58; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    memset(&c,0,sizeof(c)); c.mapper=1; c.nes2=1; c.prg_size=524288; c.chr_ram=8192; c.prg_ram=8192;
    c.submapper=1; CHECK(nes_cart_variant_supported(&c));
    c.prg_size=262144; CHECK(!nes_cart_variant_supported(&c));
    c.submapper=2; c.prg_nvram=8192; CHECK(nes_cart_variant_supported(&c));
    c.chr_ram=16384; CHECK(!nes_cart_variant_supported(&c));
    c.submapper=0; CHECK(nes_cart_variant_supported(&c)); /* SZROM */
    c.prg_nvram=32768; CHECK(!nes_cart_variant_supported(&c));
    c.prg_ram=0; c.chr_ram=8192; c.submapper=4; CHECK(nes_cart_variant_supported(&c));
    c.submapper=5; CHECK(!nes_cart_variant_supported(&c));
    c.prg_size=32768; CHECK(nes_cart_variant_supported(&c));
    c.submapper=7; CHECK(nes_cart_variant_supported(&c));
    c.submapper=6; CHECK(!nes_cart_variant_supported(&c));
    puts("cartridge header contracts passed");
    return 0;
}
