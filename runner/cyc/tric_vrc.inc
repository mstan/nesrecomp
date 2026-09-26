/* Independent VRC connector implementation. See the board specifications
 * linked in hw_vrc.inc; no bank tables or IRQ state are shared with it. */
inline bool TricMapper::VRC2Board() {
    return Cart.Mapper == 22 || ((Cart.Mapper == 23 || Cart.Mapper == 25) && Cart.Info.submapper == 3);
}
inline bool TricMapper::VRC24Board() {
    return Cart.Mapper == 21 || Cart.Mapper == 22 || Cart.Mapper == 23 || Cart.Mapper == 25;
}
inline unsigned TricMapper::VRCPort(ushort Address) {
    unsigned a0=0, a1=1, b0=2, b1=3;
    if (Cart.Mapper == 21) { a0=1; a1=2; b0=6; b1=7; }
    else if (Cart.Mapper == 22 || Cart.Mapper == 25) { a0=1; a1=0; b0=3; b1=2; }
    bool lower = Cart.Info.submapper != 2;
    bool upper = Cart.Info.submapper == 2 || (Cart.Info.submapper == 0 && Cart.Mapper != 22);
    unsigned port = 0;
    if (lower) port = ((Address >> a0) & 1) + ((Address >> a1) & 1)*2;
    if (upper) port |= ((Address >> b0) & 1) + ((Address >> b1) & 1)*2;
    return port;
}
inline void TricMapper::VRCApply() {
    bool swap = !VRC2Board() && (Ctrl & 2);
    MapPRG8(swap ? 2 : 0, Reg[0]);
    MapPRG8(swap ? 0 : 2, -2);
    MapPRG8(1, Reg[1]); MapPRG8(3, -1);
    for (unsigned i=0; i<8; ++i) MapCHR1(i, VrcChr[i] / (Cart.Mapper == 22 ? 2 : 1));
    unsigned mirror = MirrorReg % (VRC2Board() ? 2 : 4);
    Mirroring = mirror == 0 ? TRIC_MIRROR_VERTICAL : mirror == 1 ? TRIC_MIRROR_HORIZONTAL :
                mirror == 2 ? TRIC_MIRROR_SCREEN_A : TRIC_MIRROR_SCREEN_B;
    WRAM_Readable = WRAM_Writable = VRC2Board() || (Ctrl & 1);
}
inline void TricMapper::VRCWrite(ushort Address, byte Input) {
    unsigned page = Address / 4096;
    if (Cart.Mapper == 73) {
        if (page >= 8 && page <= 11) {
            unsigned shift = (page-8)*4;
            VrcLatch16 = (VrcLatch16 & ~(15u << shift)) | ((Input % 16) << shift);
        } else if (page == 12) {
            IrqMode = Input % 8; IrqEnable = (Input & 2) != 0; IrqOut = false;
            if (IrqEnable) { VrcCounter16 = VrcLatch16; LastWriteCycle = totalCycles + 1; }
        } else if (page == 13) { IrqOut = false; IrqEnable = (IrqMode & 1) != 0; }
        else if (page == 15) { PrgReg = Input % 8; MapPRG16(0, PrgReg); }
        return;
    }
    unsigned port = VRCPort(Address);
    switch (page) {
    case 8: Reg[0] = Input % 32; VRCApply(); break;
    case 10: Reg[1] = Input % 32; VRCApply(); break;
    case 9:
        if (VRC2Board() || port == 0) MirrorReg = Input;
        else if (port == 2) Ctrl = Input % 4;
        VRCApply(); break;
    case 11: case 12: case 13: case 14: {
        unsigned r = (page-11)*2 + port/2;
        if (port%2) VrcChr[r] = (VrcChr[r]%16) + (Input%(VRC2Board()?16:32))*16;
        else VrcChr[r] = (VrcChr[r] & 0x1f0) + Input%16;
        VRCApply(); break;
    }
    case 15:
        if (VRC2Board()) break;
        if (port == 0) IrqLatch = (IrqLatch & 240) | (Input & 15);
        else if (port == 1) IrqLatch = (IrqLatch & 15) | (Input << 4);
        else if (port == 2) {
            IrqMode = Input%8; IrqEnable = (Input&2)!=0; IrqOut=false; IrqDivider=341;
            if (IrqEnable) { IrqCounter=IrqLatch; LastWriteCycle=totalCycles+1; }
        } else { IrqOut=false; IrqEnable=(IrqMode&1)!=0; }
        break;
    }
}
inline void TricMapper::CPUClock() {
    if (Cart.Mapper==40) {
        if (IrqEnable && totalCycles!=LastWriteCycle && VrcCounter16!=4096) {
            VrcCounter16++; if (VrcCounter16==4096) IrqOut=true;
        }
        return;
    }
    if (Cart.Mapper==5) { MMC5Clock(); return; }
    if (BandaiBoard()) { BandaiClock(); return; }
    if (!IrqEnable || totalCycles == LastWriteCycle) return;
    if (Cart.Mapper == 73) {
        unsigned width = (IrqMode & 4) ? 256 : 65536;
        if (VrcCounter16 % width == width-1) {
            VrcCounter16 = (VrcCounter16 & ~(width-1)) | (VrcLatch16 & (width-1));
            IrqOut = true;
        } else ++VrcCounter16;
    } else if ((VRC24Board() && !VRC2Board()) || VRC6Board() || Cart.Mapper==85) {
        if (!(IrqMode & 4)) {
            IrqDivider -= 3;
            if (IrqDivider > 0) return;
            IrqDivider += 341;
        }
        if (IrqCounter == 255) { IrqCounter=IrqLatch; IrqOut=true; }
        else ++IrqCounter;
    }
}
