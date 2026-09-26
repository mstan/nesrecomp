static void mmc1_serial(unsigned addr, unsigned value)
{
    for (unsigned i=0;i<5;++i) { hw.cycles+=2; hw_cart_cpu_write((uint16_t)addr,(value>>i)&1); }
}

static void test_mmc1a(void)
{
    cart(155,256,128);
    for (unsigned mode=0;mode<4;++mode) for (unsigned prg_value=0;prg_value<32;++prg_value) {
        mmc1_serial(0x8000,mode*4); mmc1_serial(0xe000,prg_value);
        unsigned lo=prg_value&15, hi=lo;
        if (mode<2) { lo&=14; hi=lo+1; }
        if (mode==2) lo=prg_value&16 ? prg_value&8 : 0;
        if (mode==3) hi=prg_value&16 ? (prg_value&8)|7 : 15;
        prg_banks(lo*2,lo*2+1,hi*2,hi*2+1);
        CHECK(hw_cart.wram_readable && hw_cart.wram_writable);
    }
}

static void test_mmc1_boards(void)
{
    /* Exhaustive ROM output pins: all modes, inner/outer bank values and
     * PPU A12, with no reads required to propagate the CHR outputs. */
    cart(1,512,8);
    for (unsigned mode=0;mode<4;++mode) for (unsigned inner=0;inner<16;++inner)
        for (unsigned outer=0;outer<2;++outer) for (unsigned a12=0;a12<2;++a12) {
            mmc1_serial(0x8000,16+mode*4);
            mmc1_serial(0xa000,outer*16); mmc1_serial(0xc000,(1-outer)*16);
            mmc1_serial(0xe000,inner);
            hw_cart_ppu_addr_watched((uint16_t)(a12*4096));
            unsigned group=(a12?1-outer:outer)*32;
            unsigned lo=mode<2?(inner&14)*2:mode==2?0:inner*2;
            unsigned hi=mode<2?lo+2:mode==2?inner*2:30;
            prg_banks(group+lo,group+lo+1,group+hi,group+hi+1);
            CHECK(!hw_prg_is_stable());
        }
    mmc1_serial(0x8000,12); CHECK(hw_prg_is_stable());
    mmc1_serial(0xa000,16); prg_banks(62,63,62,63);
    /* SNROM's second /CE is driven by the selected CHR register. */
    cart(1,256,8); hw_cart.info.prg_size=262144; hw_cart.info.prg_ram=8192;
    hw_cart.wram_len=8192; hw_cart_power_on();
    mmc1_serial(0x8000,28); mmc1_serial(0xa000,16); mmc1_serial(0xc000,0);
    hw_cart_ppu_addr_watched(0); CHECK(!hw_cart.wram_readable);
    hw_cart_ppu_addr_watched(4096); CHECK(hw_cart.wram_readable);
    mmc1_serial(0xe000,16); CHECK(!hw_cart.wram_readable);
    mmc1_serial(0xe000,0); CHECK(hw_cart.wram_readable);
    /* Test physical RAM chips/banks, including save-compatible ordering. */
    for (unsigned size=16384;size<=32768;size*=2) for (unsigned chr_kb=8;chr_kb<=64;chr_kb*=8) {
        if (size==32768 && chr_kb>8) continue;
        cart(1,256,chr_kb); hw_cart.info.prg_size=262144;
        hw_cart.info.prg_ram=size==16384?8192:0; hw_cart.info.prg_nvram=size-hw_cart.info.prg_ram;
        hw_cart.wram_len=size; hw_cart_power_on();
        for (unsigned bank=0;bank<size/8192;++bank) {
            unsigned shift=chr_kb>8?4:size==32768?2:3;
            mmc1_serial(0xa000,bank<<shift);
            CHECK(hw_cart.wram_bank==bank*8192);
            hw_cart_cpu_write(0x6000,(uint8_t)(0x50+bank));
        }
        for (unsigned bank=0;bank<size/8192;++bank) CHECK(hw_cart.wram[bank*8192]==0x50+bank);
        hw_cart_power_on();
        CHECK(hw_cart.wram[hw_cart.info.prg_ram]==(size==16384?0x51:0x50));
        if (hw_cart.info.prg_ram) CHECK(hw_cart.wram[0]==0);
    }
    cart(1,256,64); hw_cart.info.submapper=7;
    for (unsigned vertical=0;vertical<2;++vertical) {
        hw_cart.info.vertical=vertical;
        for (unsigned m=0;m<4;++m) { mmc1_serial(0x8000,12+m); CHECK(hw_cart.mirroring==vertical); }
    }
}
