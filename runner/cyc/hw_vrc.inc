/* Konami VRC2/VRC4 board wiring and VRC3/VRC4 CPU-clocked IRQs.
 * https://www.nesdev.org/wiki/VRC2_and_VRC4
 * https://www.nesdev.org/wiki/VRC_IRQ
 * https://www.nesdev.org/wiki/VRC3
 * Included after the cartridge's bank-table helpers. */
static bool vrc2_board(void)
{
    return hw_cart.mapper == 22 ||
        ((hw_cart.mapper == 23 || hw_cart.mapper == 25) && hw_cart.info.submapper == 3);
}

static bool vrc24_board(void)
{
    return hw_cart.mapper == 21 || hw_cart.mapper == 22 ||
           hw_cart.mapper == 23 || hw_cart.mapper == 25;
}

/* Normalize the PCB's register-select pins into bits 0 and 1. Submapper zero
 * retains the iNES union of the two historical address decodes. */
static unsigned vrc_port(uint16_t addr)
{
    unsigned lo, hi, sub = hw_cart.info.submapper;
    if (hw_cart.mapper == 21) {
        lo = (addr >> 1) & 3; hi = (addr >> 6) & 3;
    } else if (hw_cart.mapper == 23) {
        lo = addr & 3; hi = (addr >> 2) & 3;
    } else {
        lo = ((addr >> 1) & 1) | ((addr & 1) << 1);
        hi = ((addr >> 3) & 1) | ((addr >> 1) & 2);
    }
    if (hw_cart.mapper == 22 || sub == 1 || sub == 3) return lo;
    return sub == 2 ? hi : lo | hi;
}

static void vrc24_apply(void)
{
    bool swap = !vrc2_board() && (hw_cart.m.ctrl & 2);
    map_prg8(0, swap ? -2 : hw_cart.m.reg[0]);
    map_prg8(1, hw_cart.m.reg[1]);
    map_prg8(2, swap ? hw_cart.m.reg[0] : -2);
    map_prg8(3, -1);
    for (unsigned i=0; i<8; ++i)
        map_chr1(i, hw_cart.mapper == 22 ? hw_cart.m.vrc_chr[i] >> 1 : hw_cart.m.vrc_chr[i]);
    static const uint8_t mirror[] = {HW_MIRROR_VERTICAL, HW_MIRROR_HORIZONTAL,
                                    HW_MIRROR_SCREEN_A, HW_MIRROR_SCREEN_B};
    hw_cart.mirroring = mirror[hw_cart.m.mirror_reg & (vrc2_board() ? 1 : 3)];
    hw_cart.wram_readable = hw_cart.wram_writable = vrc2_board() || (hw_cart.m.ctrl & 1);
}

static void vrc_irq_control(uint8_t value)
{
    hw_cart.m.irq_mode = value & 7;
    hw_cart.m.irq_enable = (value & 2) != 0;
    hw_cart.m.irq_out = 0;
    hw_cart.m.irq_prescaler = 341;
    if (value & 2) {
        hw_cart.m.irq_counter = hw_cart.m.irq_latch;
        /* The register write's M2 edge loads rather than increments. */
        hw_cart.m.last_write_cycle = hw.cycles + 1;
    }
}

static void vrc_irq_ack(void)
{
    hw_cart.m.irq_out = 0;
    hw_cart.m.irq_enable = hw_cart.m.irq_mode & 1;
}

static void vrc_irq_clock(void)
{
    if (!hw_cart.m.irq_enable || hw.cycles == hw_cart.m.last_write_cycle) return;
    if (!(hw_cart.m.irq_mode & 4)) {
        hw_cart.m.irq_prescaler -= 3;
        if (hw_cart.m.irq_prescaler > 0) return;
        hw_cart.m.irq_prescaler += 341;
    }
    if (hw_cart.m.irq_counter == 255) {
        hw_cart.m.irq_counter = hw_cart.m.irq_latch;
        hw_cart.m.irq_out = 1;
    } else ++hw_cart.m.irq_counter;
}

static void vrc24_write(uint16_t addr, uint8_t value)
{
    unsigned page = addr >> 12, port = vrc_port(addr);
    if (page == 8 || page == 10) {
        hw_cart.m.reg[page == 10] = value & 31;
        vrc24_apply();
    } else if (page == 9) {
        if (vrc2_board() || port < 2) hw_cart.m.mirror_reg = value;
        else hw_cart.m.ctrl = value & 3;
        vrc24_apply();
    } else if (page >= 11 && page <= 14) {
        unsigned index = (page - 11)*2 + port/2;
        uint16_t *r = &hw_cart.m.vrc_chr[index];
        if (port & 1) *r = (*r & 15) | ((value & (vrc2_board() ? 15 : 31)) << 4);
        else *r = (*r & 0x1f0) | (value & 15);
        vrc24_apply();
    } else if (page == 15 && !vrc2_board()) {
        switch (port) {
        case 0: hw_cart.m.irq_latch = (hw_cart.m.irq_latch & 240) | (value & 15); break;
        case 1: hw_cart.m.irq_latch = (hw_cart.m.irq_latch & 15) | (value << 4); break;
        case 2: vrc_irq_control(value); break;
        case 3: vrc_irq_ack(); break;
        }
    }
}

static void vrc3_write(uint16_t addr, uint8_t value)
{
    unsigned page = addr >> 12;
    if (page >= 8 && page <= 11) {
        unsigned shift = (page - 8)*4;
        hw_cart.m.irq_latch16 = (hw_cart.m.irq_latch16 & ~(15u << shift)) | ((value & 15) << shift);
    } else if (page == 12) {
        hw_cart.m.irq_mode = value & 7;
        hw_cart.m.irq_enable = (value & 2) != 0;
        hw_cart.m.irq_out = 0;
        if (value & 2) {
            hw_cart.m.irq_counter16 = hw_cart.m.irq_latch16;
            hw_cart.m.last_write_cycle = hw.cycles + 1;
        }
    } else if (page == 13) vrc_irq_ack();
    else if (page == 15) {
        hw_cart.m.prg = value & 7;
        map_prg16(0, hw_cart.m.prg);
    }
}

static void vrc3_clock(void)
{
    if (!hw_cart.m.irq_enable || hw.cycles == hw_cart.m.last_write_cycle) return;
    uint16_t mask = (hw_cart.m.irq_mode & 4) ? 255 : 65535;
    if ((hw_cart.m.irq_counter16 & mask) == mask) {
        hw_cart.m.irq_counter16 = (hw_cart.m.irq_counter16 & ~mask) | (hw_cart.m.irq_latch16 & mask);
        hw_cart.m.irq_out = 1;
    } else ++hw_cart.m.irq_counter16;
}
