/* Bandai FCG-1/2 and LZ93D50: NESdev mapper 16 and physical PCB tracing at
 * https://seesaawiki.jp/famicomcartridge/d/Bandai%20LZ93D50%20standard */
static bool bandai_board(void) { return hw_cart.mapper==16 || hw_cart.mapper==159 || hw_cart.mapper==153; }
static void bandai_apply(void)
{
    if (hw_cart.mapper==153) {
        unsigned outer=(hw_cart.m.reg[hw_cart.m.bank_select]&1)<<4;
        map_prg16(0,outer|(hw_cart.m.prg&15)); map_prg16(1,outer|15); map_chr8(0);
        hw_cart.wram_readable=hw_cart.wram_writable=(hw_cart.m.ctrl&32)!=0;
    } else {
        map_prg16(0, hw_cart.m.prg & 15); map_prg16(1, -1);
        for (unsigned i=0;i<8;++i) map_chr1(i, hw_cart.m.reg[i]);
    }
    static const uint8_t mirror[]={HW_MIRROR_VERTICAL,HW_MIRROR_HORIZONTAL,HW_MIRROR_SCREEN_A,HW_MIRROR_SCREEN_B};
    hw_cart.mirroring=mirror[hw_cart.m.mirror_reg&3];
}
static void bandai_write(uint16_t addr, uint8_t value)
{
    if (hw_cart.mapper==153 && addr>=0x6000 && addr<0x8000) {
        if (hw_cart.wram_writable) hw_cart.wram[addr&8191]=value;
        return;
    }
    unsigned sub=hw_cart.mapper!=16 ? 5 : hw_cart.info.submapper;
    if (addr<0x6000 || (sub==4 && addr>=0x8000) || (sub==5 && addr<0x8000)) return;
    unsigned port=addr&15;
    if (port<8) { hw_cart.m.reg[port]=value; bandai_apply(); }
    else if (port==8) { hw_cart.m.prg=value; bandai_apply(); }
    else if (port==9) { hw_cart.m.mirror_reg=value; bandai_apply(); }
    else if (port==10) {
        hw_cart.m.irq_enable=value&1; hw_cart.m.irq_out=0;
        if (addr>=0x8000) hw_cart.m.irq_counter16=hw_cart.m.irq_latch16;
        hw_cart.m.last_write_cycle=hw.cycles+1;
    } else if (port==11 || port==12) {
        unsigned shift=(port-11)*8;
        uint16_t *r=addr<0x8000 ? &hw_cart.m.irq_counter16 : &hw_cart.m.irq_latch16;
        *r=(*r & ~(255u<<shift)) | (value<<shift);
    } else if (port==13 && addr>=0x8000) {
        hw_cart.m.ctrl=value;
        if (hw_cart.mapper==153) bandai_apply();
        else nes_eeprom_lines(&hw_cart.eeprom[0], value&32, value&0xc0, hw.cycles);
    }
}
static bool bandai_read(uint16_t addr, uint8_t *value)
{
    if (hw_cart.mapper==153 && addr>=0x6000 && addr<0x8000) {
        if (hw_cart.wram_readable) *value=hw_cart.wram[addr&8191];
        /* No pull-up on this PCB: SDA input is low. If software enables
         * both drivers, resolve D4 low (analog contention is unspecified). */
        if (hw_cart.m.ctrl&128) *value &= 0xef;
        return hw_cart.wram_readable || (hw_cart.m.ctrl&128);
    }
    if (addr<0x6000 || addr>=0x8000 || hw_cart.info.submapper==4 || !(hw_cart.m.ctrl&128)) return false;
    *value=(*value&0xef) | (nes_eeprom_read(&hw_cart.eeprom[0])<<4);
    return true;
}
static void bandai_ppu_addr(uint16_t addr)
{
    unsigned select=(addr>>10)&3;
    if (hw_cart.m.bank_select==select) return;
    hw_cart.m.bank_select=select;
    unsigned outer=(hw_cart.m.reg[select]&1)<<4;
    map_prg16(0,outer|(hw_cart.m.prg&15)); map_prg16(1,outer|15);
}
static void bandai_clock(void)
{
    nes_eeprom_clock(&hw_cart.eeprom[0],hw.cycles);
    if (!hw_cart.m.irq_enable || hw.cycles==hw_cart.m.last_write_cycle) return;
    if (hw_cart.m.irq_counter16--==0) hw_cart.m.irq_out=1;
}
