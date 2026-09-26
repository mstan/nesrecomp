/* NTDEC 2722 PCB tracing: https://forums.nesdev.org/viewtopic.php?t=19744 */
static void test_mapper40(void)
{
    cart(40,64,8); prg_banks(4,5,0,7);
    for (unsigned b=0;b<8;++b) memset(prg+b*8192,b,8192);
    for (unsigned address=0x6000;address<0x8000;++address) {
        uint8_t value=0; CHECK(hw_cart_cpu_read((uint16_t)address,&value)); CHECK(value==6);
        hw_cart_cpu_write((uint16_t)address,0xff);
        CHECK(prg[6*8192+(address&8191)]==6);
    }
    for (unsigned value=0;value<256;++value) {
        hw_cart_cpu_write((uint16_t)(0xe000+value*31),(uint8_t)value);
        prg_banks(4,5,value&7,7);
        hw_cart_cpu_write(0xdfff,0); prg_banks(4,5,value&7,7);
    }
    hw_cart_cpu_write(0xbfff,0x55);
    ++hw.cycles; hw_cart_cpu_clock(); CHECK(hw_cart.m.irq_counter16==0);
    for (unsigned edge=1;edge<=4096;++edge) {
        ++hw.cycles; hw_cart_cpu_clock(); CHECK(hw_cart_irq()==(edge==4096));
    }
    for (unsigned i=0;i<20;++i) { ++hw.cycles; hw_cart_cpu_clock(); CHECK(hw_cart_irq()); }
    hw_cart_cpu_write(0xafff,0); CHECK(hw_cart_irq()); /* enable does not acknowledge */
    hw_cart_cpu_write(0x9fff,255); CHECK(!hw_cart_irq()); CHECK(hw_cart.m.irq_counter16==0);
    for (unsigned i=0;i<5000;++i) { ++hw.cycles; hw_cart_cpu_clock(); CHECK(!hw_cart_irq()); }
    hw_cart_cpu_write(0xa000,0); ++hw.cycles; hw_cart_cpu_clock();
    for (unsigned i=0;i<4000;++i) { ++hw.cycles; hw_cart_cpu_clock(); }
    hw_cart_cpu_write(0xb000,255); ++hw.cycles; hw_cart_cpu_clock();
    CHECK(hw_cart.m.irq_counter16==4001); /* an already enabled counter keeps clocking */
    for (unsigned i=0;i<94;++i) { ++hw.cycles; hw_cart_cpu_clock(); CHECK(!hw_cart_irq()); }
    ++hw.cycles; hw_cart_cpu_clock(); CHECK(hw_cart_irq());
}
