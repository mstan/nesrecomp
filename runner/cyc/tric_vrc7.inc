/* Separate board decoding. The oracle intentionally has no FM synthesis;
 * FM output is validated with channel/patch and host PCM contracts. */
inline void TricMapper::VRC7Apply() {
    MapPRG8(0,Reg[0]); MapPRG8(1,Reg[1]); MapPRG8(2,Reg[2]); MapPRG8(3,-1);
    for (unsigned i=0;i<8;++i) MapCHR1(i,VrcChr[i]);
    Mirroring=(Ctrl&3)==0?TRIC_MIRROR_VERTICAL:(Ctrl&3)==1?TRIC_MIRROR_HORIZONTAL:
        (Ctrl&3)==2?TRIC_MIRROR_SCREEN_A:TRIC_MIRROR_SCREEN_B;
    WRAM_Readable=WRAM_Writable=(Ctrl&128)!=0;
}
inline void TricMapper::VRC7Write(ushort Address, byte Input) {
    bool select=Cart.Info.submapper==1 ? (Address&8)!=0 : Cart.Info.submapper==2 ? (Address&16)!=0 : (Address&24)!=0;
    if (Address&32) return; // data port is audio only
    unsigned page=Address/4096;
    switch(page) {
    case 8: Reg[select?1:0]=Input%64; VRC7Apply(); break;
    case 9: if (!select) { Reg[2]=Input%64; VRC7Apply(); } break;
    case 10: case 11: case 12: case 13:
        VrcChr[(page-10)*2+(select?1:0)]=Input; VRC7Apply(); break;
    case 14:
        if (select) IrqLatch=Input;
        else { Ctrl=Input; VRC7Apply(); }
        break;
    case 15:
        if (select) { IrqOut=false; IrqEnable=(IrqMode&1)!=0; }
        else {
            IrqMode=Input%8; IrqEnable=(Input&2)!=0; IrqOut=false; IrqDivider=341;
            if (IrqEnable) { IrqCounter=IrqLatch; LastWriteCycle=totalCycles+1; }
        }
        break;
    }
}
