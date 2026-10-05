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
    memset(h,0,sizeof(h)); memcpy(h,"NES\x1a",4); h[4]=8; h[5]=8; h[6]=0xe2; h[7]=0xc0;
    CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==206 && c.prg_nvram==8192);
    h[6]=0xe0; CHECK(nes_cart_header(h,16,&c)); CHECK(c.prg_ram==0 && c.prg_nvram==0);
    /* TxSROM: iNES keeps the MMC3 work-RAM default; no four-screen wiring. */
    memset(h,0,sizeof(h)); memcpy(h,"NES\x1a",4); h[4]=8; h[5]=16; h[6]=0x60; h[7]=0x70;
    CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==118 && c.prg_ram==8192);
    CHECK(nes_cart_variant_supported(&c));
    h[6]=0x68; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    /* TQROM: iNES implies the 8 KiB CHR RAM beside at most 64 KiB CHR ROM. */
    memset(h,0,sizeof(h)); memcpy(h,"NES",3); h[3]=26; h[4]=8; h[5]=8; h[6]=0x70; h[7]=0x70;
    CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==119 && c.chr_size==65536 && c.chr_ram==8192);
    CHECK(c.prg_ram==0 && nes_cart_variant_supported(&c));
    h[5]=16; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    h[5]=8; h[7]=0x78; h[11]=7; CHECK(nes_cart_header(h,16,&c)); CHECK(nes_cart_variant_supported(&c));
    h[11]=8; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    h[11]=0x77; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    h[11]=0; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    h[11]=7; h[5]=0; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    h[6]=0x40; h[7]=0x08; h[5]=8; CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==4 && !nes_cart_variant_supported(&c));
    /* Sunsoft-4: iNES 8 KiB WRAM; CHR ROM required; submapper 1 unsupported. */
    memset(h,0,sizeof(h)); memcpy(h,"NES",3); h[3]=26; h[4]=8; h[5]=32; h[6]=0x40; h[7]=0x40;
    CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==68 && c.prg_ram==8192 && nes_cart_variant_supported(&c));
    h[7]=0x48; h[8]=0x10; CHECK(nes_cart_header(h,16,&c)); CHECK(c.submapper==1 && !nes_cart_variant_supported(&c));
    h[8]=0; h[10]=7; CHECK(nes_cart_header(h,16,&c)); CHECK(nes_cart_variant_supported(&c));
    h[10]=8; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    h[10]=0; h[5]=0; h[11]=7; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    /* FME-7: iNES 8 KiB WRAM. */
    memset(h,0,sizeof(h)); memcpy(h,"NES",3); h[3]=26; h[4]=8; h[5]=16; h[6]=0x50; h[7]=0x40;
    CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==69 && c.prg_ram==8192 && nes_cart_variant_supported(&c));
    /* CRC-32 check value, and the Namco board rules (nesdev INES Mapper 210). */
    CHECK(nes_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);
    memset(h,0,sizeof(h)); memcpy(h,"NES",3); h[3]=26; h[4]=8; h[5]=16; h[6]=0x32; h[7]=0x10;
    CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==19 && c.prg_nvram==8192 && nes_cart_variant_supported(&c));
    h[6]=0x30; CHECK(nes_cart_header(h,16,&c)); CHECK(c.prg_ram==0 && c.prg_nvram==0 && nes_cart_variant_supported(&c));
    { uint8_t data[4]={1,2,3,4};
      h[6]=0x22; h[7]=0xd0; CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==210 && c.submapper==0);
      nes_cart_known_dump(&c,data,4); CHECK(c.submapper==1 && c.prg_nvram==8192 && nes_cart_variant_supported(&c));
      h[6]=0x20; CHECK(nes_cart_header(h,16,&c)); nes_cart_known_dump(&c,data,4);
      CHECK(c.submapper==2 && !c.prg_ram && !c.prg_nvram && nes_cart_variant_supported(&c));
      h[6]=0x30; h[7]=0x10; CHECK(nes_cart_header(h,16,&c)); nes_cart_known_dump(&c,data,4);
      CHECK(c.mapper==19); }                     /* unknown mapper-19 dump stays a 163 */
    h[6]=0x30; h[7]=0x18; h[8]=0x60; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    h[8]=0x10; h[10]=0x70; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c)); /* sub 1: no ext RAM */
    h[10]=0; CHECK(nes_cart_header(h,16,&c)); CHECK(nes_cart_variant_supported(&c));
    h[6]=0x20; h[7]=0xd8; h[8]=0x20; h[10]=0x07; CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==210 && !nes_cart_variant_supported(&c));
    h[10]=0; CHECK(nes_cart_header(h,16,&c)); CHECK(nes_cart_variant_supported(&c));
    h[8]=0x30; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    /* Mapper 185: submappers 0 and 4-7, one 8 KiB CHR ROM, no RAM. */
    memset(h,0,sizeof(h)); memcpy(h,"NES",3); h[3]=26; h[4]=2; h[5]=1; h[6]=0x90; h[7]=0xb8;
    for (unsigned sub=0; sub<16; ++sub) {
        h[8]=(uint8_t)(sub<<4); CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==185);
        CHECK(nes_cart_variant_supported(&c)==(sub==0 || (sub>=4 && sub<=7)));
    }
    h[8]=0x40; h[5]=2; CHECK(nes_cart_header(h,16,&c)); CHECK(!nes_cart_variant_supported(&c));
    { uint8_t data[4]={9,9,9,9}; h[5]=1; h[8]=0; CHECK(nes_cart_header(h,16,&c));
      nes_cart_known_dump(&c,data,4); CHECK(c.mapper==185 && c.submapper==0); }
    /* Generated known-dump table (tools/cyc/known_dumps.py). Each 4-byte input
     * is forged to the CRC-32 of the real dump named beside it. */
    { static const uint8_t contra_j[4]={0x28,0x53,0x4E,0xD5};   /* B27B8CF4: VRC2b, no RAM */
      static const uint8_t dbz2[4]={0x55,0xBD,0x4F,0x98};       /* 99240573: LZ93D50 + 24C02 */
      static const uint8_t akumakun[4]={0x10,0x82,0x48,0x91};   /* 2C4421B2: FCG-2 */
      static const uint8_t lz93d50[4]={0xBA,0x47,0xE3,0xD8};    /* DB05106E: LZ93D50, no EEPROM */
      static const uint8_t x24c01[4]={0xA3,0x30,0x3F,0x08};     /* 0CF42E69: 159 dumped as 16 */
      static const uint8_t vrc4b[4]={0x76,0x36,0x94,0x00};      /* 5ADBF660: VRC4b, 2 KiB WRAM */
      static const uint8_t battle_rush[4]={0x4F,0x71,0xB3,0x0A}; /* 983D8175: Datach + 24C01 */
      static const uint8_t kitarou2[4]={0x20,0xDE,0xBC,0x12};   /* BDA8F8E4: 152 dumped as 70 */
      static const uint8_t devilman[4]={0x37,0x77,0x1D,0x6D};   /* D1691028: 154 dumped as 88 */
      static const uint8_t holydiver[4]={0xAF,0x79,0xFA,0x20};  /* BA51AC6F: 78 submapper 3 */
      static const uint8_t data_unknown[4]={1,2,3,4};
      memset(h,0,sizeof(h)); memcpy(h,"NES",3); h[3]=26; h[4]=8; h[5]=16; h[6]=0x70; h[7]=0x10;
      CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==23 && c.prg_ram==8192);
      nes_cart_known_dump(&c,contra_j,4);
      CHECK(c.mapper==23 && c.submapper==3 && !c.prg_ram && !c.prg_nvram && nes_cart_variant_supported(&c));
      CHECK(nes_cart_header(h,16,&c)); nes_cart_known_dump(&c,data_unknown,4);
      CHECK(c.submapper==0 && c.prg_ram==8192);                 /* unknown dump keeps the default */
      h[7]=0x18; CHECK(nes_cart_header(h,16,&c)); nes_cart_known_dump(&c,contra_j,4);
      CHECK(c.submapper==0 && !c.prg_ram);                      /* NES 2.0 is trusted as written */
      h[6]=0x00; h[7]=0x10; CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==16 && !c.prg_nvram);
      nes_cart_known_dump(&c,dbz2,4);
      CHECK(c.submapper==5 && c.prg_nvram==256 && c.battery && nes_cart_variant_supported(&c));
      h[6]=0x02; CHECK(nes_cart_header(h,16,&c)); CHECK(c.prg_nvram==256);
      nes_cart_known_dump(&c,akumakun,4);
      CHECK(c.submapper==4 && !c.prg_nvram && !c.battery && nes_cart_variant_supported(&c));
      CHECK(nes_cart_header(h,16,&c)); nes_cart_known_dump(&c,lz93d50,4);
      CHECK(c.submapper==5 && !c.prg_nvram && nes_cart_variant_supported(&c));
      h[6]=0x00; CHECK(nes_cart_header(h,16,&c)); nes_cart_known_dump(&c,x24c01,4);
      CHECK(c.mapper==159 && !c.submapper && c.prg_nvram==128 && nes_cart_variant_supported(&c));
      h[5]=0; CHECK(nes_cart_header(h,16,&c)); nes_cart_known_dump(&c,battle_rush,4);
      CHECK(c.mapper==157 && c.prg_nvram==128 && c.chr_ram==8192 && nes_cart_variant_supported(&c));
      h[5]=16;
      h[6]=0x61; h[7]=0x40; CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==70);
      nes_cart_known_dump(&c,kitarou2,4); CHECK(c.mapper==152 && !c.submapper && nes_cart_variant_supported(&c));
      h[6]=0x89; h[7]=0x50; CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==88 && c.four_screen);  /* the dump's own header */
      nes_cart_known_dump(&c,devilman,4); CHECK(c.mapper==154 && !c.four_screen && nes_cart_variant_supported(&c));
      h[6]=0x80;
      h[6]=0xe8; h[7]=0x40; CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==78 && c.four_screen);   /* the dump's header */
      nes_cart_known_dump(&c,holydiver,4); CHECK(c.submapper==3 && !c.four_screen && nes_cart_variant_supported(&c));
      CHECK(nes_cart_header(h,16,&c)); nes_cart_known_dump(&c,data_unknown,4);
      CHECK(c.submapper==3 && !c.four_screen && nes_cart_variant_supported(&c));  /* iNES convention */
      h[6]=0xe0; CHECK(nes_cart_header(h,16,&c)); nes_cart_known_dump(&c,data_unknown,4);
      CHECK(c.submapper==0 && nes_cart_variant_supported(&c));                    /* one-screen */
      h[6]=0x80; h[7]=0x50;
      h[6]=0x90; h[7]=0x10; CHECK(nes_cart_header(h,16,&c)); CHECK(c.mapper==25);
      nes_cart_known_dump(&c,vrc4b,4);
      CHECK(c.submapper==1 && c.prg_ram==2048 && !c.prg_nvram && nes_cart_variant_supported(&c)); }
    /* SNROM's real 8 KiB save chip must accept legacy raw battery files.
     * These synthetic payloads have Zelda USA's database CRC (3FE272FB).
     * Unknown payloads retain the old compatibility allocation; explicit
     * iNES RAM sizes and NES 2.0 metadata take precedence. */
    {
        const uint8_t zelda_crc[4] = {0x44,0x2B,0xA5,0x80};
        uint8_t image[16+16384] = {'N','E','S',26,1,0,0x12};
        const uint8_t tail[4] = {0xC2,0xD6,0x0B,0xB6};
        CHECK(nes_crc32(0,zelda_crc,4)==0x3FE272FBu);
        CHECK(nes_cart_header(image,sizeof(image),&c)); CHECK(c.prg_nvram==32768);
        nes_cart_known_dump(&c,zelda_crc,4);
        CHECK(c.prg_nvram==8192 && !c.prg_ram && c.battery);
        memcpy(image+sizeof(image)-4,tail,4);
        CHECK(nes_cart_image(image,sizeof(image),&c)); CHECK(c.prg_nvram==8192);
        image[8]=2;
        CHECK(nes_cart_image(image,sizeof(image),&c)); CHECK(c.prg_nvram==16384);
        image[7]=8; image[8]=0; image[10]=0x90; image[11]=7;
        CHECK(nes_cart_image(image,sizeof(image),&c)); CHECK(c.prg_nvram==32768);
        image[7]=0; image[10]=image[11]=0; image[16]=1;
        CHECK(nes_cart_image(image,sizeof(image),&c)); CHECK(c.prg_nvram==32768);
        image[16]=0; image[6]=0;
        CHECK(nes_cart_image(image,sizeof(image),&c)); CHECK(c.mapper==0 && !c.prg_nvram);
    }
    puts("cartridge header contracts passed");
    return 0;
}
