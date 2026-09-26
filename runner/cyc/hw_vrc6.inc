/* VRC6 bank pins and audio dividers. Board contracts:
 * https://www.nesdev.org/wiki/VRC6
 * https://www.nesdev.org/wiki/VRC6_audio */
static bool vrc6_board(void) { return hw_cart.mapper == 24 || hw_cart.mapper == 26; }

static unsigned vrc6_nt_bank(unsigned nt)
{
    unsigned b = hw_cart.m.ctrl, r;
    switch (b & 7) {
    case 0: case 6: case 7: r = 6 + nt/2; break;
    case 1: case 5: r = 4 + nt; break;
    default: r = 6 + (nt & 1); break;
    }
    unsigned bank = hw_cart.m.vrc_chr[r];
    if ((b & 0x20) && ((b & 3) == 0 || (b & 3) == 3)) {
        /* Mode 3 exchanges the horizontal/vertical pairs of mode 0. */
        unsigned arrangement = ((b >> 2) ^ ((b & 3) == 3 ? 1 : 0)) & 3;
        r = 6 + ((arrangement & 1) ? (nt & 1) : nt/2);
        bank = hw_cart.m.vrc_chr[r] & 0xfe;
        bank |= arrangement == 0 ? nt & 1 : arrangement == 1 ? nt/2 : arrangement == 3;
    }
    return bank;
}

static void vrc6_apply(void)
{
    unsigned mode = hw_cart.m.ctrl & 3;
    map_prg16(0, hw_cart.m.prg); map_prg8(2, hw_cart.m.chr0); map_prg8(3, -1);
    for (unsigned page=0; page<8; ++page) {
        bool pair = mode == 1 || (mode >= 2 && page >= 4);
        unsigned reg = mode == 1 ? page/2 : pair ? 4 + (page-4)/2 : page;
        unsigned bank = hw_cart.m.vrc_chr[reg];
        if (pair && (hw_cart.m.ctrl & 0x20)) bank = (bank & 0xfe) | (page & 1);
        map_chr1(page, bank);
    }
    hw_cart.wram_readable = hw_cart.wram_writable = (hw_cart.m.ctrl & 0x80) != 0;
}

static void vrc6_audio_write(unsigned page, unsigned port, uint8_t value)
{
    HwVrc6Audio *a = &hw_cart.m.vrc6_audio;
    if (page == 9 && port == 3) { a->control = value & 7; return; }
    if (port >= 3) return;
    unsigned ch = page - 9;
    a->reg[ch][port] = value;
    if (port == 2 && !(value & 0x80)) {
        a->step[ch] = ch == 2 ? 0 : 15;
        if (ch == 2) a->accumulator = 0;
    }
}

static void vrc6_write(uint16_t addr, uint8_t value)
{
    unsigned port = addr & 3, page = addr >> 12;
    if (hw_cart.mapper == 26) port = ((port & 1) << 1) | (port >> 1);
    if (page == 8) { hw_cart.m.prg = value & 15; vrc6_apply(); }
    else if (page == 12) { hw_cart.m.chr0 = value & 31; vrc6_apply(); }
    else if (page == 11 && port == 3) { hw_cart.m.ctrl = value; vrc6_apply(); }
    else if (page >= 9 && page <= 11) vrc6_audio_write(page, port, value);
    else if (page == 13 || page == 14) {
        hw_cart.m.vrc_chr[(page-13)*4 + port] = value; vrc6_apply();
    } else if (page == 15) {
        if (port == 0) hw_cart.m.irq_latch = value;
        else if (port == 1) vrc_irq_control(value);
        else if (port == 2) vrc_irq_ack();
    }
}

static void vrc6_audio_clock(void)
{
    HwVrc6Audio *a = &hw_cart.m.vrc6_audio;
    if (a->control & 1) return;
    unsigned shift = (a->control & 4) ? 8 : (a->control & 2) ? 4 : 0;
    for (unsigned ch=0; ch<3; ++ch) {
        if (a->timer[ch]) { --a->timer[ch]; continue; }
        unsigned period = a->reg[ch][1] | ((a->reg[ch][2] & 15) << 8);
        a->timer[ch] = period >> shift;
        if (!(a->reg[ch][2] & 0x80)) continue;
        if (ch < 2) a->step[ch] = (a->step[ch] - 1) & 15;
        else if (++a->step[ch] == 14) { a->step[ch] = 0; a->accumulator = 0; }
        else if (!(a->step[ch] & 1)) a->accumulator += a->reg[ch][0] & 63;
    }
}

static unsigned vrc6_audio_dac(void)
{
    HwVrc6Audio *a = &hw_cart.m.vrc6_audio;
    unsigned level = a->accumulator >> 3;
    for (unsigned ch=0; ch<2; ++ch)
        if ((a->reg[ch][2] & 0x80) &&
            ((a->reg[ch][0] & 0x80) || a->step[ch] <= ((a->reg[ch][0] >> 4) & 7)))
            level += a->reg[ch][0] & 15;
    return level;
}
