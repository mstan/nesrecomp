/* Expectations from MMC5 board wiring and hardware register descriptions. */
static uint8_t m5read(unsigned addr)
{
    uint8_t v=0x35; hw_cart_cpu_read((uint16_t)addr,&v); return v;
}
static uint8_t m5ppu(unsigned addr)
{
    uint8_t v=0;
    if (addr<8192) v=hw_cart_chr_read((uint16_t)addr);
    else CHECK(hw_cart_nt_read((uint16_t)addr,true,&v));
    hw_cart_ppu_rd(false);
    return v;
}
static void m5line(void)
{
    m5ppu(0x2000); m5ppu(0x2000); m5ppu(0x2000); m5ppu(0x23c0);
}
static void test_mmc5(void)
{
    cart(5,512,512);
    for (unsigned i=0;i<64;++i) memset(prg+i*8192,(int)i,8192);
    for (unsigned i=0;i<512;++i) memset(chr+i*1024,(int)i,1024);
    prg_banks(0,1,2,63);
    /* Each mode's register selection, alignment and last-register ROM bit. */
    for (unsigned mode=0;mode<4;++mode) {
        hw_cart_cpu_write(0x5100,(uint8_t)mode);
        for (unsigned v=0;v<128;++v) {
            for (unsigned r=0;r<4;++r) hw_cart_cpu_write((uint16_t)(0x5114+r),(uint8_t)(128|((v+r*7)&127)));
            for (unsigned slot=0;slot<4;++slot) {
                unsigned r=mode==0?3:mode==1?(slot<2?1:3):mode==2&&slot<2?1:slot;
                unsigned mask=mode==0?3:mode==1 || (mode==2&&slot<2)?1:0;
                CHECK(hw_cart_prg_read((uint16_t)(0x8000+slot*8192))==(((v+r*7)&~mask)|(slot&mask))%64);
            }
        }
    }
    /* All multiplier operands, not merely a few products. */
    for (unsigned a=0;a<256;++a) for (unsigned b=0;b<256;++b) {
        hw_cart_cpu_write(0x5205,(uint8_t)a); hw_cart_cpu_write(0x5206,(uint8_t)b);
        CHECK(m5read(0x5205)==(uint8_t)(a*b)); CHECK(m5read(0x5206)==(uint8_t)(a*b>>8));
    }
    /* Physical chip selects: 8K/32K boards have holes, ETROM has two chips. */
    unsigned sizes[]={0,8192,16384,32768,65536,131072};
    for (unsigned s=0;s<6;++s) {
        hw_cart.wram_len=sizes[s];
        hw_cart_cpu_write(0x5102,2); hw_cart_cpu_write(0x5103,1);
        for (unsigned b=0;b<16;++b) memset(hw_cart.wram+b*8192,(int)(0x80+b),8192);
        for (unsigned b=0;b<16;++b) {
            hw_cart_cpu_write(0x5113,(uint8_t)b);
            bool hole=!sizes[s] || ((sizes[s]==8192 || sizes[s]==32768)&&(b&4));
            unsigned bank=sizes[s]==8192?0:sizes[s]==16384?(b&4?1:0):sizes[s]==32768?b&3:sizes[s]==65536?b&7:b;
            CHECK(m5read(0x6000)==(hole?0x35:0x80+bank));
            hw_cart_cpu_write(0x5114,(uint8_t)b);
            CHECK(!hw_prg_is_rom(0x8000));
            CHECK(m5read(0x8000)==(hole?0x35:0x80+bank));
        }
    }
    hw_cart.wram_len=16384; hw_cart.info.prg_ram=hw_cart.info.prg_nvram=8192;
    hw_cart_cpu_write(0x5113,0); CHECK(m5read(0x6000)==0x81);
    hw_cart_cpu_write(0x5113,4); CHECK(m5read(0x6000)==0x80);
    hw_cart_cpu_write(0x5102,0); hw_cart_cpu_write(0x6000,0x42); CHECK(m5read(0x6000)==0x80);
    hw_cart_cpu_write(0x5102,2); hw_cart_cpu_write(0x6000,0x42); CHECK(m5read(0x6000)==0x42);
    /* Extended RAM modes, fill palette replication, CIRAM routing. */
    hw_cart_cpu_write(0x5104,2); hw_cart_cpu_write(0x5c23,0x6d); CHECK(m5read(0x5c23)==0x6d);
    hw_cart_cpu_write(0x5104,3); hw_cart_cpu_write(0x5c23,0xff); CHECK(m5read(0x5c23)==0x6d);
    hw_cart_cpu_write(0x5105,0xe4); hw_cart_cpu_write(0x5106,0x42); hw_cart_cpu_write(0x5107,2);
    CHECK(m5ppu(0x2800)==0); CHECK(m5ppu(0x2c00)==0x42); CHECK(m5ppu(0x2fc0)==0xaa);
    CHECK(hw_cart_nt_write(0x2010,0x25)); CHECK(hw_cart_nt_write(0x2410,0x37));
    CHECK(m5ppu(0x2010)==0x25); CHECK(m5ppu(0x2410)==0x37);
    hw_cart_cpu_write(0x5104,0); CHECK(m5read(0x5c23)==0x35); CHECK(m5ppu(0x2823)==0x6d);
    hw_cart_cpu_write(0x5c23,0x77); CHECK(m5ppu(0x2823)==0);
    /* Scanline assertion is the FOURTH read, then pending survives disable. */
    hw_cart_cpu_read_snoop(0xfffa,0);
    hw_cart_cpu_write(0x5203,1); hw_cart_cpu_write(0x5204,0x80);
    m5ppu(0x2000); m5ppu(0x2000); m5ppu(0x2000); CHECK(m5read(0x5204)==0);
    m5ppu(0x23c0); CHECK(m5read(0x5204)==64);
    m5line(); CHECK(hw_cart_irq()); hw_cart_cpu_write(0x5204,0); CHECK(!hw_cart_irq());
    hw_cart_cpu_write(0x5204,128); CHECK(hw_cart_irq()); CHECK(m5read(0x5204)==192); CHECK(!hw_cart_irq());
    hw_cart_cpu_write(0x5c23,0x77); CHECK(m5ppu(0x2823)==0x77);
    ppu.rd=0; hw_cart_cpu_clock(); hw_cart_cpu_clock(); CHECK(m5read(0x5204)==64);
    hw_cart_cpu_clock(); CHECK(m5read(0x5204)==0);
    /* A held /RD strobe must not fabricate matching reads. */
    uint8_t dummy=0; hw_cart_nt_read(0x2000,true,&dummy);
    for (unsigned i=0;i<10;++i) hw_cart_nt_read(0x2000,true,&dummy);
    CHECK(m5read(0x5204)==0); hw_cart_ppu_rd(false);
    /* CHR packing is write-time, upper bits latch, 8x8 ignores group B. */
    hw_cart_cpu_write(0x2000,0); hw_cart_cpu_write(0x5101,1);
    hw_cart_cpu_write(0x5123,5); hw_cart_cpu_write(0x5127,7);
    CHECK(m5ppu(0x0000)==20); CHECK(m5ppu(0x1000)==28);
    hw_cart_cpu_write(0x5101,3); CHECK(m5ppu(0xc00)==21); CHECK(m5ppu(0x1c00)==29);
    hw_cart_cpu_write(0x512b,42); CHECK(m5ppu(0x1c00)==29);
    hw_cart_cpu_write(0x2000,32); CHECK(m5ppu(0x1c00)==42);
    hw_cart_cpu_write(0x5130,1); hw_cart_cpu_write(0x5120,9); hw_cart_cpu_write(0x5130,0);
    CHECK(hw_cart_mmc5_chr_index(0)==265*1024);
    /* PCM read side effects include zero IRQ, hold DAC, acknowledge and mode. */
    hw_cart_cpu_write(0x5010,0x81); hw_cart_cpu_read_snoop(0x8000,0x43);
    CHECK(hw_cart.m.mmc5.pcm==0x43); hw_cart_cpu_write(0x5011,99); CHECK(hw_cart.m.mmc5.pcm==0x43);
    hw_cart_cpu_read_snoop(0xc000,0); CHECK(!hw_cart_irq());
    hw_cart_cpu_read_snoop(0x8000,0); CHECK(hw_cart_irq()); CHECK(hw_cart.m.mmc5.pcm==0x43);
    CHECK(m5read(0x5010)==129); CHECK(!hw_cart_irq()); CHECK(m5read(0x5010)==1);
    hw_cart_cpu_write(0x5010,128); hw_cart_cpu_write(0x5011,0); CHECK(hw_cart_irq());
    hw_cart_cpu_write(0x5010,0); CHECK(!hw_cart_irq());
    hw_cart_cpu_write(0x5010,128); CHECK(hw_cart_irq());
    hw_cart_cpu_write(0x5011,2); CHECK(!hw_cart_irq());
    /* MMC5A independent timer: high write doesn't start; low write does. */
    hw.cycles=100; hw_cart_cpu_write(0x520a,1); CHECK(!hw_cart.m.mmc5.timer_active);
    hw_cart_cpu_write(0x5209,0); ++hw.cycles; hw_cart_cpu_clock(); CHECK(hw_cart.m.mmc5.timer==256);
    for (unsigned i=0;i<256;++i) {++hw.cycles; hw_cart_cpu_clock();}
    CHECK(hw_cart_irq()); CHECK(m5read(0x5204)==0); CHECK(hw_cart_irq());
    CHECK(m5read(0x5209)==128); CHECK(!hw_cart_irq());
    /* The envelope and length units keep running with host audio disabled. */
    cart(5,512,512);
    hw_cart_cpu_write(0x5015,1); hw_cart_cpu_write(0x5000,0x1f);
    hw_cart_cpu_write(0x5002,0); hw_cart_cpu_write(0x5003,0x18);
    CHECK(m5read(0x5015)==1); /* length table entry 3 = 2 */
    for (unsigned i=0;i<7457;++i) {++hw.cycles;hw_cart_cpu_clock();}
    CHECK(m5read(0x5015)==1);
    for (unsigned i=0;i<7457;++i) {++hw.cycles;hw_cart_cpu_clock();}
    CHECK(m5read(0x5015)==0);
    hw_cart_cpu_write(0x5000,0x3f);hw_cart_cpu_write(0x5003,0x18);
    unsigned transitions=0,previous=hw_cart.m.mmc5.pulse[0].step;
    for (unsigned i=0;i<32;++i) {++hw.cycles;hw_cart_cpu_clock();unsigned step=hw_cart.m.mmc5.pulse[0].step;transitions+=step!=previous;previous=step;}
    CHECK(transitions==16);CHECK(m5read(0x5015)==1); /* period 0 is not muted */
    hw_cart_cpu_write(0x5015,0);CHECK(m5read(0x5015)==0);
    /* At the row 7/8 boundary the last two fetches and the next line's
     * first NT fetch must all use row 8. This precedes the IRQ clock. */
    cart(5,512,512);
    hw_cart_cpu_write(0x5104,2);
    for (unsigned i=0;i<1024;++i) hw_cart_cpu_write((uint16_t)(0x5c00+i),(uint8_t)(i/32));
    hw_cart_cpu_write(0x5104,0);hw_cart_cpu_write(0x5200,0x9f);
    hw_cart_cpu_write(0x5201,7);hw_cart_cpu_write(0x2001,0x08);
    m5line();
    for (unsigned fetch=2;fetch<160;++fetch) m5ppu(fetch&1?0x0008:0x0000);
    CHECK(m5ppu(0x2000)==1); /* prefetch column 0 of row 8 */
    m5ppu(0x23c0);m5ppu(0);m5ppu(8);
    CHECK(m5ppu(0x2001)==1); /* prefetch column 1 */
    m5ppu(0x23c0);m5ppu(0);m5ppu(8);
    m5ppu(0x2002);m5ppu(0x2002);
    CHECK(m5ppu(0x2002)==1); /* next visible fetch, IRQ has not clocked yet */
    CHECK(hw_cart.m.mmc5.line==0);
    m5ppu(0x23c0);CHECK(hw_cart.m.mmc5.line==1);
}
