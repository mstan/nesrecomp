#include "vrc6_pin_vectors.inc"
static uint16_t vrc6_addr(unsigned mapper, uint16_t addr) {
    return mapper==26 ? (addr&0xfffc)|((addr&1)<<1)|((addr&2)>>1) : addr;
}
static void test_vrc6(void) {
    for (unsigned mapper=24;mapper<=26;mapper+=2) {
        if (!hw_cart_supports(mapper)) continue;
        cart(mapper,256,256); hw_cart.chr_len=256*1024;
        for (unsigned i=0;i<256;++i) memset(chr+i*1024,i,1024);
        const uint8_t regs[8]={1,8,16,25,32,41,49,56};
        for (unsigned r=0;r<8;++r)
            hw_cart_cpu_write(vrc6_addr(mapper,(uint16_t)(0xd000+(r/4)*4096+r%4)),regs[r]);
        const uint8_t patterns[8][8]={
            {1,8,16,25,32,41,49,56}, {1,1,8,8,16,16,25,25},
            {1,8,16,25,32,32,41,41}, {1,8,16,25,32,32,41,41},
            {1,8,16,25,32,41,49,56}, {0,1,8,9,16,17,24,25},
            {1,8,16,25,32,33,40,41}, {1,8,16,25,32,33,40,41}
        };
        for (unsigned mode=0;mode<64;++mode) {
            hw_cart_cpu_write(0xb003,(uint8_t)mode);
            for (unsigned page=0;page<8;++page) chr_bank(page,patterns[(mode&3)+((mode&32)?4:0)][page],1);
            for (unsigned nt=0;nt<4;++nt) {
                uint16_t addr=(uint16_t)(0x2000+nt*1024);
                uint8_t value=0xa5, pins=vrc6_nt_pins[mode][nt];
                CHECK(hw_cart_nt_read(addr,true,&value)==((pins&0x40)==0));
                CHECK(hw_cart_ciram_a10(addr)==((pins&1)?1024:0));
                if (pins&0x80) {
                    CHECK(value==(pins&63));
                    CHECK(hw_cart_nt_write(addr,0x55));
                    CHECK(hw_cart_nt_read(addr,true,&value)); CHECK(value==(pins&63));
                } else CHECK(value==0xa5);
            }
        }
        hw_cart_cpu_write(0x8ffe,3); hw_cart_cpu_write(0xcfff,5); prg_banks(6,7,5,31);
        hw_cart_cpu_write(0xb003,0xa0); hw_cart_cpu_write(0x6000,0x55);
        uint8_t v=0; CHECK(hw_cart_cpu_read(0x6000,&v)); CHECK(v==0x55);
        hw_cart_cpu_write(0xb003,0x20); CHECK(!hw_cart_cpu_read(0x6000,&v));
        hw_cart_cpu_write(0xf000,255); hw_cart_cpu_write(vrc6_addr(mapper,0xf001),6);
        vrc_cycles(1); CHECK(!hw_cart_irq()); vrc_cycles(1); CHECK(hw_cart_irq());
        hw_cart_cpu_write(vrc6_addr(mapper,0xf002),0); CHECK(!hw_cart_irq());

        /* Exact duty occupancy over one period at all eight pulse duties. */
        for (unsigned duty=0;duty<8;++duty) {
            hw_cart_power_on();
            hw_cart_cpu_write(0x9000,(uint8_t)(duty*16+15));
            hw_cart_cpu_write(vrc6_addr(mapper,0x9002),0x80);
            unsigned high=0;
            for (unsigned i=0;i<16;++i) { vrc_cycles(1); high+=hw_cart_audio_level()<0; }
            CHECK(high==duty+1);
        }
        hw_cart_cpu_write(0x9003,1);
        HwVrc6Audio paused=hw_cart.m.vrc6_audio; vrc_cycles(100);
        CHECK(!memcmp(&paused,&hw_cart.m.vrc6_audio,sizeof(paused)));
        hw_cart_power_on();
        hw_cart_cpu_write(0xb000,8); hw_cart_cpu_write(vrc6_addr(mapper,0xb002),0x80);
        const unsigned saw[]={0,1,1,2,2,3,3,4,4,5,5,6,6,0};
        for (unsigned i=0;i<14;++i) { vrc_cycles(1); CHECK(hw_cart.m.vrc6_audio.accumulator/8==saw[i]); }
        hw_cart_cpu_write(vrc6_addr(mapper,0xb002),0); CHECK(hw_cart_audio_level()==0);
        for (unsigned scale=0;scale<3;++scale) {
            hw_cart_power_on();
            hw_cart_cpu_write(0x9003,(uint8_t)(scale==2?6:scale*2));
            hw_cart_cpu_write(vrc6_addr(mapper,0x9001),0xff);
            hw_cart_cpu_write(vrc6_addr(mapper,0x9002),0x81);
            vrc_cycles(1); CHECK(hw_cart.m.vrc6_audio.step[0]==14);
            unsigned period=scale==0?512:scale==1?32:2;
            vrc_cycles(period-1); CHECK(hw_cart.m.vrc6_audio.step[0]==14);
            vrc_cycles(1); CHECK(hw_cart.m.vrc6_audio.step[0]==13);
        }
    }
}
