/* Bandai FCG-1/2 and LZ93D50: NESdev mapper 16 and physical PCB tracing at
 * https://seesaawiki.jp/famicomcartridge/d/Bandai%20LZ93D50%20standard */
static bool bandai_board(void) { return hw_cart.mapper==16; }
static void bandai_apply(void)
{
    map_prg16(0, hw_cart.m.prg & 15); map_prg16(1, -1);
    for (unsigned i=0;i<8;++i) map_chr1(i, hw_cart.m.reg[i]);
    static const uint8_t mirror[]={HW_MIRROR_VERTICAL,HW_MIRROR_HORIZONTAL,HW_MIRROR_SCREEN_A,HW_MIRROR_SCREEN_B};
    hw_cart.mirroring=mirror[hw_cart.m.mirror_reg&3];
}
static void bandai_write(uint16_t addr, uint8_t value)
{
    unsigned sub=hw_cart.info.submapper;
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
        nes_eeprom_lines(&hw_cart.eeprom[0], value&32, value&0xc0, hw.cycles);
    }
}
static bool bandai_read(uint16_t addr, uint8_t *value)
{
    if (addr<0x6000 || addr>=0x8000 || hw_cart.info.submapper==4 || !(hw_cart.m.ctrl&128)) return false;
    *value=(*value&0xef) | (nes_eeprom_read(&hw_cart.eeprom[0])<<4);
    return true;
}
static void bandai_clock(void)
{
    nes_eeprom_clock(&hw_cart.eeprom[0],hw.cycles);
    if (!hw_cart.m.irq_enable || hw.cycles==hw_cart.m.last_write_cycle) return;
    if (hw_cart.m.irq_counter16--==0) hw_cart.m.irq_out=1;
}
