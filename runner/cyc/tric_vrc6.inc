/* Independent connector banking for the VRC6. Audio has no CPU-readable
 * state or IRQ and is checked by the runtime's waveform contracts instead. */
inline unsigned TricMapper::VRC6Nametable(unsigned nt) {
    const unsigned regs[8][4] = {
        {6,6,7,7},{4,5,6,7},{6,7,6,7},{6,7,6,7},
        {6,7,6,7},{4,5,6,7},{6,6,7,7},{6,6,7,7}
    };
    unsigned r = regs[Ctrl & 7][nt], low;
    switch (Ctrl & 0x2f) {
    case 0x20: case 0x27: r=6+nt/2; low=nt%2; break;
    case 0x23: case 0x24: r=6+nt%2; low=nt/2; break;
    case 0x28: case 0x2f: r=6+nt/2; low=0; break;
    case 0x2b: case 0x2c: r=6+nt%2; low=1; break;
    default: return VrcChr[r];
    }
    return (VrcChr[r] & 254) + low;
}
inline void TricMapper::VRC6Apply() {
    MapPRG16(0, PrgReg); MapPRG8(2, Chr0); MapPRG8(3, -1);
    if ((Ctrl & 3) == 0) for (unsigned i=0; i<8; ++i) MapCHR1(i,VrcChr[i]);
    else {
        unsigned first = (Ctrl & 3) == 1 ? 0 : 4;
        for (unsigned i=0; i<first; ++i) MapCHR1(i,VrcChr[i]);
        for (unsigned i=first; i<8; i+=2) {
            unsigned bank=VrcChr[first+(i-first)/2];
            MapCHR1(i, (Ctrl & 32) ? bank & 254 : bank);
            MapCHR1(i+1, (Ctrl & 32) ? bank | 1 : bank);
        }
    }
    WRAM_Readable=WRAM_Writable=(Ctrl&128)!=0;
}
inline void TricMapper::VRC6Write(ushort Address, byte Input) {
    unsigned port=Address%4, page=Address/4096;
    if (Cart.Mapper==26) port=(port%2)*2+port/2;
    switch (page) {
    case 8: PrgReg=Input%16; VRC6Apply(); break;
    case 12: Chr0=Input%32; VRC6Apply(); break;
    case 11: if (port==3) { Ctrl=Input; VRC6Apply(); } break;
    case 13: case 14: VrcChr[(page-13)*4+port]=Input; VRC6Apply(); break;
    case 15:
        if (port==0) IrqLatch=Input;
        else if (port==1) {
            IrqMode=Input%8; IrqEnable=(Input&2)!=0; IrqOut=false; IrqDivider=341;
            if (IrqEnable) { IrqCounter=IrqLatch; LastWriteCycle=totalCycles+1; }
        } else if (port==2) { IrqOut=false; IrqEnable=(IrqMode&1)!=0; }
        break;
    }
}
