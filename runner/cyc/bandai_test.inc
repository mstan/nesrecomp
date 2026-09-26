/* Transaction vectors from Xicor's datasheets, not another emulator. */
static NesEeprom ee;
static uint64_t ee_cycle;
static void ee_lines(unsigned clk,unsigned bit) { nes_eeprom_lines(&ee,clk,bit,++ee_cycle); }
static void ee_start(void) { ee_lines(0,1); ee_lines(1,1); ee_lines(1,0); ee_lines(0,0); }
static void ee_stop(void) { ee_lines(0,0); ee_lines(1,0); ee_lines(1,1); }
static unsigned ee_put(unsigned v)
{
    for (unsigned i=0;i<8;++i) { unsigned bit=(v>>(7-i))&1; ee_lines(0,bit); ee_lines(1,bit); ee_lines(0,bit); }
    ee_lines(0,1); ee_lines(1,1); unsigned ack=!nes_eeprom_read(&ee); ee_lines(0,1); return ack;
}
static unsigned ee_get(unsigned ack)
{
    unsigned v=0;
    for (unsigned i=0;i<8;++i) { ee_lines(1,1); v=v*2+nes_eeprom_read(&ee); ee_lines(0,1); }
    ee_lines(0,!ack); ee_lines(1,!ack); ee_lines(0,!ack); ee_lines(0,1); return v;
}
static void ee_seek(unsigned addr,unsigned read)
{
    ee_start();
    if (ee.size==128) CHECK(ee_put(addr*2+read));
    else {
        CHECK(ee_put(0xa0)); CHECK(ee_put(addr));
        if (read) { ee_start(); CHECK(ee_put(0xa1)); }
    }
}
static void test_eeprom(void)
{
    for (unsigned size=128;size<=256;size*=2) {
        nes_eeprom_init(&ee,size); ee_cycle=0;
        for (unsigned a=0;a<size;++a) {
            ee_seek(a,0); CHECK(ee_put((a*73+37)&255)); ee_stop();
            CHECK(ee.data[a]==255); /* uncommitted until write timer expires */
            ee_start(); CHECK(!ee_put(size==128?a*2:0xa0)); ee_stop();
            ee_cycle+=NES_EEPROM_WRITE_CYCLES; nes_eeprom_clock(&ee,ee_cycle);
            CHECK(ee.data[a]==((a*73+37)&255));
        }
        /* MSB ordering, auto-increment, ACK continuation and device wrap. */
        ee_seek(size-2,1);
        for (unsigned i=0;i<size+3;++i) {
            unsigned a=(size-2+i)%size;
            CHECK(ee_get(i!=size+2)==((a*73+37)&255));
        }
        ee_stop();
        /* Five writes starting at 0x23 wrap within the SAME four-byte page. */
        ee_seek(0x23,0);
        for (unsigned i=0;i<5;++i) CHECK(ee_put(0xa0+i));
        ee_stop(); ee_cycle+=NES_EEPROM_WRITE_CYCLES; nes_eeprom_clock(&ee,ee_cycle);
        CHECK(ee.data[0x23]==0xa4 && ee.data[0x20]==0xa1 && ee.data[0x21]==0xa2 && ee.data[0x22]==0xa3);
        CHECK(ee.data[0x24]==((0x24*73+37)&255));
        ee_seek(0x23,0); CHECK(ee_put(0)); ee_stop(); nes_eeprom_reset(&ee);
        ee_cycle+=NES_EEPROM_WRITE_CYCLES; nes_eeprom_clock(&ee,ee_cycle);
        CHECK(ee.data[0x23]==0xa4); /* power interruption retains old committed byte */
        ee_seek(0x23,1); CHECK(ee_get(0)==0xa4); ee_stop();
        if (size==256) { ee_start(); CHECK(!ee_put(0xa2)); ee_stop(); }
    }
}
static void test_bandai(void)
{
    cart(153,512,8);
    prg_banks(0,1,30,31); CHECK(hw_prg_is_stable());
    hw_cart_cpu_write(0x8001,1); CHECK(!hw_prg_is_stable());
    for (unsigned select=0;select<4;++select) {
        hw_cart_ppu_addr_watched(select*1024);
        unsigned high=select==1?32:0;
        prg_banks(high,high+1,high+30,high+31); chr_bank(0,0,8);
    }
    for (unsigned r=0;r<4;++r) hw_cart_cpu_write(0x8000+r,1);
    CHECK(hw_prg_is_stable()); prg_banks(32,33,62,63);
    uint8_t value=0x70;
    CHECK(!hw_cart_cpu_read(0x6000,&value));
    hw_cart_cpu_write(0x800d,32); hw_cart_cpu_write(0x6000,0xa5);
    CHECK(hw_cart_cpu_read(0x6000,&value) && value==0xa5);
    hw_cart_cpu_write(0x800d,128); value=0x70;
    CHECK(hw_cart_cpu_read(0x6000,&value) && value==0x60);
    test_eeprom();
    cart(159,256,128);
    hw_cart_cpu_write(0x8008,5); hw_cart_cpu_write(0x6008,7); prg_banks(10,11,30,31);
    hw_cart_cpu_write(0x8007,127); chr_bank(7,127,1);
    CHECK(!hw_cart.has_wram);
    for (unsigned sub=0;sub<=5;++sub) {
        if (sub && sub!=4 && sub!=5) continue;
        cart(16,256,256); hw_cart.info.submapper=sub;
        unsigned base=sub==4?0x6000:0x8000;
        hw_cart_cpu_write(base+8,5); prg_banks(10,11,30,31);
        for (unsigned r=0;r<8;++r) { hw_cart_cpu_write(base+r,241+r); chr_bank(r,241+r,1); }
        if (sub) { hw_cart_cpu_write((base^0xe000)+8,7); prg_banks(10,11,30,31); }
        hw_cart_cpu_write(base+9,3); CHECK(hw_cart.mirroring==HW_MIRROR_SCREEN_B);
        hw_cart_cpu_write(base+11,2); hw_cart_cpu_write(base+12,0);
        hw_cart_cpu_write(base+10,1);
        hw.cycles+=2; hw_cart_cpu_clock(); CHECK(!hw_cart_irq());
        ++hw.cycles; hw_cart_cpu_clock(); CHECK(!hw_cart_irq());
        ++hw.cycles; hw_cart_cpu_clock(); CHECK(hw_cart_irq());
        CHECK(hw_cart.m.irq_counter16==65535);
        hw_cart_cpu_write(base+10,0); CHECK(!hw_cart_irq());
        CHECK(hw_cart.m.irq_counter16==(sub==4?65535:2));
        uint8_t v=0xa5; hw_cart_cpu_write(base+13,0); CHECK(!hw_cart_cpu_read(0x6000,&v));
        hw_cart_cpu_write(base+13,128);
        CHECK(hw_cart_cpu_read(0x6000,&v)==(sub!=4));
        CHECK(v==(sub==4?0xa5:0xb5));
    }
}
