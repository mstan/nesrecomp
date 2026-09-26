#include "vendor/emu2413/emu2413.h"
static void vrc7_reg(unsigned r, uint8_t value) {
    hw_cart_cpu_write(0x9010,(uint8_t)r); hw_cart_cpu_write(0x9030,value);
}
static void test_vrc7(void) {
    for (unsigned sub=0;sub<3;++sub) {
        cart(85,512,256); hw_cart.info.submapper=(uint8_t)sub; hw_cart_power_on();
        unsigned pin=sub==1?8:16;
        hw_cart_cpu_write(0x8000,0x3f); hw_cart_cpu_write((uint16_t)(0x8000+pin),0x3e);
        hw_cart_cpu_write(0x9000,0x3d); prg_banks(63,62,61,63);
        for (unsigned r=0;r<8;++r) {
            hw_cart_cpu_write((uint16_t)(0xa000+(r/2)*4096+(r%2)*pin),(uint8_t)(240+r));
            chr_bank(r,240+r,1);
        }
        hw_cart_cpu_write(0xe000,0x83); CHECK(hw_cart.mirroring==HW_MIRROR_SCREEN_B);
        hw_cart_cpu_write(0x6000,0xa5); uint8_t v=0;
        CHECK(hw_cart_cpu_read(0x6000,&v)); CHECK(v==0xa5);
        hw_cart_cpu_write(0xe000,0); CHECK(!hw_cart_cpu_read(0x6000,&v));
        hw_cart_cpu_write((uint16_t)(0xe000+pin),255); hw_cart_cpu_write(0xf000,7);
        vrc_cycles(1); CHECK(!hw_cart_irq()); vrc_cycles(1); CHECK(hw_cart_irq());
        hw_cart_cpu_write((uint16_t)(0xf000+pin),0); CHECK(!hw_cart_irq()); CHECK(hw_cart.m.irq_enable);
        if (sub) {
            // The other address line is ignored, so this writes bank zero.
            hw_cart_cpu_write((uint16_t)(0x8000+(24-pin)),4); prg_banks(4,62,61,63);
        }
    }
    /* Known instrument bytes dumped from the physical chip's patch ROM. */
    const uint8_t patches[15][8]={
        {3,0x21,5,6,0xe8,0x81,0x42,0x27},{0x13,0x41,0x14,0xd,0xd8,0xf6,0x23,0x12},
        {0x11,0x11,8,8,0xfa,0xb2,0x20,0x12},{0x31,0x61,0xc,7,0xa8,0x64,0x61,0x27},
        {0x32,0x21,0x1e,6,0xe1,0x76,1,0x28},{2,1,6,0,0xa3,0xe2,0xf4,0xf4},
        {0x21,0x61,0x1d,7,0x82,0x81,0x11,7},{0x23,0x21,0x22,0x17,0xa2,0x72,1,0x17},
        {0x35,0x11,0x25,0,0x40,0x73,0x72,1},{0xb5,1,0xf,0xf,0xa8,0xa5,0x51,2},
        {0x17,0xc1,0x24,7,0xf8,0xf8,0x22,0x12},{0x71,0x23,0x11,6,0x65,0x74,0x18,0x16},
        {1,2,0xd3,5,0xc9,0x95,3,2},{0x61,0x63,0xc,0,0x94,0xc0,0x33,0xf6},
        {0x21,0x72,0xd,0,0xc1,0xd5,0x56,6}
    };
    for (unsigned p=0;p<15;++p) {
        OPLL_PATCH patch[2]; uint8_t bytes[8];
        OPLL_getDefaultPatch(OPLL_VRC7_TONE,p+1,patch); OPLL_patchToDump(patch,bytes);
        CHECK(!memcmp(bytes,patches[p],8));
    }
    for (unsigned ch=0;ch<6;++ch) {
        cart(85,512,256);
        vrc7_reg(0x30+ch,0x30); vrc7_reg(0x10+ch,34); vrc7_reg(0x20+ch,0x19);
        unsigned audible=0;
        for (unsigned t=0;t<200000;++t) { vrc_cycles(1); audible+=hw_cart_audio_level()!=0; }
        CHECK(audible>10000);
        hw_cart_cpu_write(0xe000,0x40); CHECK(hw_cart_audio_level()==0);
        for (unsigned r=0;r<64;++r) CHECK(hw_cart.m.vrc7_reg[r]==0);
        vrc7_reg(0x30+ch,0xff); CHECK(hw_cart.m.vrc7_reg[0x30+ch]==0);
        vrc_cycles(4000); CHECK(hw_cart_audio_level()==0);
        hw_cart_cpu_write(0xe000,0);
        vrc7_reg(0x30+ch,0x3f); CHECK(hw_cart.m.vrc7_reg[0x30+ch]==0x3f);
    }
    cart(85,512,256);
    const unsigned ignored[]={8,14,0x16,0x26,0x36,0x3f,0x40,0xff};
    for (unsigned i=0;i<sizeof(ignored)/sizeof(ignored[0]);++i) vrc7_reg(ignored[i],0xff);
    vrc_cycles(10000); CHECK(hw_cart_audio_level()==0);
    vrc7_reg(0x30,0x30); vrc7_reg(0x10,34); vrc7_reg(0x20,0x19);
    vrc7_reg(15,4); vrc_cycles(10000); CHECK(hw_cart_audio_level()==0);
    vrc7_reg(15,0); vrc_cycles(10000); CHECK(hw_cart_audio_level()!=0);
    // VRC7b has no oscillator even when its correctly wired sound ports are written.
    hw_cart.info.submapper=1; hw_cart_power_on();
    hw_cart_cpu_write(0x9008,0x30); hw_cart_cpu_write(0x9028,0x30);
    hw_cart_cpu_write(0x9008,0x10); hw_cart_cpu_write(0x9028,34);
    hw_cart_cpu_write(0x9008,0x20); hw_cart_cpu_write(0x9028,0x19);
    vrc_cycles(10000); CHECK(hw_cart_audio_level()==0);
}
