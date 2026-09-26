/* Expected values and divider intervals from the NESdev board contracts. */
static void vrc_cycles(unsigned count)
{
    while (count--) { ++hw.cycles; hw_cart_cpu_clock(); }
}

static void test_vrc(void)
{
    const unsigned boards[][4] = {
        {21,1,2,4}, {21,2,64,128}, {22,0,2,1},
        {23,1,1,2}, {23,2,4,8}, {23,3,1,2},
        {25,1,2,1}, {25,2,8,4}, {25,3,2,1}
    };
    for (unsigned b=0; b<sizeof(boards)/sizeof(boards[0]); ++b) {
        unsigned mapper=boards[b][0], sub=boards[b][1], high=boards[b][2], next=boards[b][3];
        if (!hw_cart_supports(mapper)) continue;
        bool is2 = mapper == 22 || sub == 3;
        cart(mapper, 256, 512);
        hw_cart.info.nes2=1; hw_cart.info.submapper=(uint8_t)sub;
        hw_cart.info.prg_size=256*1024; hw_cart.wram_len=is2?0:8192;
        hw_cart_power_on();
        hw_cart_cpu_write(0x8000,3); hw_cart_cpu_write(0xa000,5);
        prg_banks(3,5,30,31);
        hw_cart_cpu_write((uint16_t)(0x9000+next),3);
        if (is2) { prg_banks(3,5,30,31); CHECK(hw_cart.mirroring==HW_MIRROR_HORIZONTAL); }
        else {
            prg_banks(30,5,3,31); CHECK(hw_cart.wram_readable);
            hw_cart_cpu_write(0x9000,3);
            hw_cart_cpu_write((uint16_t)(0x9000+high),0);
            hw_cart_cpu_write((uint16_t)(0x9000+high+next),0);
            CHECK(hw_cart.mirroring==HW_MIRROR_SCREEN_B); CHECK(hw_cart.wram_readable);
            prg_banks(30,5,3,31);
        }
        for (unsigned page=0; page<8; ++page) {
            uint16_t lo=(uint16_t)(0xb000+(page/2)*4096+(page%2)*next);
            hw_cart_cpu_write(lo,10);
            hw_cart_cpu_write((uint16_t)(lo+high),31);
            unsigned expected=is2?250:506;
            if (mapper==22) expected/=2;
            chr_bank(page,expected,1);
        }
        uint8_t value=0xa4;
        hw_cart_cpu_write(0x6000,1);
        CHECK(hw_cart_cpu_read(0x6000,&value));
        CHECK(value==(is2?0xa5:1));
        if (is2) {
            CHECK(!hw_cart_cpu_read(0x7000,&value));
            hw_cart_cpu_write(0xf000,15); hw_cart_cpu_write((uint16_t)(0xf000+high),15);
            hw_cart_cpu_write((uint16_t)(0xf000+next),7);
            vrc_cycles(400); CHECK(!hw_cart_irq());
            continue;
        }
        /* Cycle mode: FF reload trips on the first following clock. */
        hw_cart_cpu_write(0xf000,15); hw_cart_cpu_write((uint16_t)(0xf000+high),15);
        hw_cart_cpu_write((uint16_t)(0xf000+next),6);
        vrc_cycles(1); CHECK(!hw_cart_irq());
        vrc_cycles(1); CHECK(hw_cart_irq());
        hw_cart_cpu_write((uint16_t)(0xf000+high+next),0);
        vrc_cycles(200); CHECK(!hw_cart_irq()); CHECK(!hw_cart.m.irq_enable);
        /* Scanline divider runs with rendering off: 114, 114, 113 cycles. */
        hw_cart_cpu_write((uint16_t)(0xf000+next),3); vrc_cycles(1);
        const unsigned periods[]={114,114,113};
        for (unsigned i=0;i<3;++i) {
            vrc_cycles(periods[i]-1); CHECK(!hw_cart_irq());
            vrc_cycles(1); CHECK(hw_cart_irq());
            hw_cart_cpu_write((uint16_t)(0xf000+high+next),0);
            CHECK(hw_cart.m.irq_enable);
        }
    }
    if (!hw_cart_supports(73)) return;
    cart(73,128,8);
    hw_cart_cpu_write(0x8000,14); hw_cart_cpu_write(0x9000,15);
    hw_cart_cpu_write(0xa000,15); hw_cart_cpu_write(0xb000,15);
    hw_cart_cpu_write(0xc000,2); vrc_cycles(1);
    vrc_cycles(1); CHECK(!hw_cart_irq()); CHECK(hw_cart.m.irq_counter16==65535);
    vrc_cycles(1); CHECK(hw_cart_irq()); CHECK(hw_cart.m.irq_counter16==65534);
    hw_cart_cpu_write(0xd000,0); CHECK(!hw_cart.m.irq_enable);
    hw_cart_cpu_write(0xa000,11); hw_cart_cpu_write(0xb000,10);
    hw_cart_cpu_write(0xc000,7); vrc_cycles(1);
    vrc_cycles(2); CHECK(hw_cart_irq()); CHECK(hw_cart.m.irq_counter16==0xabfe);
    hw_cart_cpu_write(0xa000,1); hw_cart_cpu_write(0xb000,2);
    hw_cart_cpu_write(0xd000,0); vrc_cycles(2);
    CHECK(hw_cart_irq()); CHECK(hw_cart.m.irq_counter16==0xabfe);
    hw_cart_cpu_write(0xf000,3); prg_banks(6,7,14,15);
}
