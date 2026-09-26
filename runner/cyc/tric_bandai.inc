// Independent Bandai board decoding. The serial EEPROM chip is shared.
inline bool TricMapper::BandaiBoard() { return Cart.Mapper==16 || Cart.Mapper==159; }
inline void TricMapper::BandaiApply() {
    MapPRG16(0,PrgReg%16); MapPRG16(1,-1);
    for (unsigned page=0;page<8;++page) MapCHR1(page,Reg[page]);
    Mirroring=MirrorReg%4==0?TRIC_MIRROR_VERTICAL:MirrorReg%4==1?TRIC_MIRROR_HORIZONTAL:
              MirrorReg%4==2?TRIC_MIRROR_SCREEN_A:TRIC_MIRROR_SCREEN_B;
}
inline void TricMapper::BandaiWrite(ushort Address, byte Input) {
    if (Address<0x6000 || (Cart.Mapper==159 && Address<0x8000)) return;
    if (Cart.Info.submapper==4 && Address>=0x8000) return;
    if (Cart.Info.submapper==5 && Address<0x8000) return;
    switch (Address%16) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        Reg[Address%8]=Input; BandaiApply(); break;
    case 8: PrgReg=Input; BandaiApply(); break;
    case 9: MirrorReg=Input; BandaiApply(); break;
    case 10:
        IrqEnable=(Input%2)!=0; IrqOut=false;
        if (Address>=0x8000) VrcCounter16=VrcLatch16;
        LastWriteCycle=totalCycles+1; break;
    case 11:
        if (Address<0x8000) VrcCounter16=(VrcCounter16&0xff00)+Input;
        else VrcLatch16=(VrcLatch16&0xff00)+Input;
        break;
    case 12:
        if (Address<0x8000) VrcCounter16=(VrcCounter16&255)+Input*256;
        else VrcLatch16=(VrcLatch16&255)+Input*256;
        break;
    case 13:
        if (Address>=0x8000) {
            Ctrl=Input;
            nes_eeprom_lines(&Eeprom[0],(Input/32)%2,(Input&192)!=0,totalCycles);
        }
        break;
    }
}
inline bool TricMapper::BandaiRead(ushort Address, byte &Value) {
    if (Address<0x6000 || Address>=0x8000 || Cart.Info.submapper==4 || !(Ctrl&128)) return false;
    Value=(Value&0xef)+16*nes_eeprom_read(&Eeprom[0]);
    return true;
}
inline void TricMapper::BandaiClock() {
    nes_eeprom_clock(&Eeprom[0],totalCycles);
    if (IrqEnable && totalCycles!=LastWriteCycle) {
        if (VrcCounter16==0) IrqOut=true;
        --VrcCounter16;
    }
}
