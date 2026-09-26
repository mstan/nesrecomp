// Independent connector-side MMC5 model. Audio status/PCM IRQ are modeled;
// the oracle does not synthesize cartridge audio (as with VRC6/VRC7).
inline uint TricMapper::MMC5RAM(unsigned Bank) {
    unsigned offset=0;
    Bank%=16;
    if (Cart.Info.prg_ram && Cart.Info.prg_nvram) {
        bool battery=(Bank/4)%2==0;
        unsigned length=battery?Cart.Info.prg_nvram:Cart.Info.prg_ram;
        return MMC5_PRG_RAM+(battery?Cart.Info.prg_ram:0)+(Bank%4)*8192%length;
    }
    switch (WRAM_Length) {
    case 0: return MMC5_PRG_OPEN;
    case 8192: if ((Bank/4)%2) return MMC5_PRG_OPEN; break;
    case 16384:
        offset=((Bank/4)%2)*8192;
        break;
    case 32768: if ((Bank/4)%2) return MMC5_PRG_OPEN; offset=(Bank%4)*8192; break;
    default: offset=Bank*8192%WRAM_Length; break;
    }
    return MMC5_PRG_RAM+offset;
}
inline void TricMapper::MMC5Apply() {
    unsigned count=0;
    while (count<4) {
        unsigned length=M5.prg_mode==0?4:M5.prg_mode==1?2:M5.prg_mode==2&&count==0?2:1;
        unsigned reg=count+length;
        unsigned first=M5.prg[reg]/length*length;
        for (unsigned i=0;i<length;++i) {
            unsigned bank=first+i;
            if (reg==4 || bank>=128) MapPRG8(count+i,bank%128);
            else {
                uint base=MMC5RAM(bank);
                PRG_Off[(count+i)*2]=base;
                PRG_Off[(count+i)*2+1]=base+4096;
            }
        }
        count+=length;
    }
}
inline void TricMapper::MMC5ClearFrame() {
    M5.in_frame=0; M5.line=0; M5.irq_pending=0;
    M5.last_addr=65535; M5.repeats=0; M5.split_y=M5.scroll;
}
inline void TricMapper::MMC5Reset() {
    memset(&M5,0,sizeof(M5));
    M5.prg_mode=3; M5.ex_mode=3; M5.protect[0]=1; M5.protect[1]=2;
    for (unsigned i=1;i<4;++i) M5.prg[i]=(byte)(127+i);
    M5.prg[4]=255; M5.gpio_mode=192; M5.pcm=255;
    MMC5ClearFrame(); MMC5Apply(); MapCHR8(0);
}
inline void TricMapper::MMC5PCM(byte Value) {
    if (Value!=0) M5.pcm=Value;
    M5.pcm_irq=Value==0;
}
inline bool TricMapper::MMC5Read(ushort Address, byte &Value, bool SideEffects) {
    bool drove=true;
    if (Address>=24576) {
        uint index=Address<32768 ? MMC5RAM(M5.prg[0])+(Address%8192) : PRG_Off[(Address/4096)%8]+Address%4096;
        if (index&MMC5_PRG_OPEN) drove=false;
        else if (index&MMC5_PRG_RAM) Value=WRAM[index%131072];
        else Value=Cart.PRGROM[index];
    } else if (Address>=0x5c00) {
        if (M5.ex_mode>=2) Value=ExRAM[Address%1024]; else drove=false;
    } else switch (Address) {
    case 0x5010: Value=(byte)(1+(M5.pcm_irq && M5.pcm_control>=128)*128); if (SideEffects) M5.pcm_irq=0; break;
    case 0x5015: Value=(M5.pulse[0].length!=0)+2*(M5.pulse[1].length!=0); break;
    case 0x5204: Value=M5.in_frame*64+M5.irq_pending*128; if (SideEffects) M5.irq_pending=0; break;
    case 0x5205: Value=(byte)(M5.multiply[0]*M5.multiply[1]); break;
    case 0x5206: Value=(byte)(M5.multiply[0]*M5.multiply[1]/256); break;
    case 0x5208: Value=(Value%64)+(M5.gpio_data&~M5.gpio_mode&192); break;
    case 0x5209: Value=M5.timer_irq*128; if (SideEffects) M5.timer_irq=0; break;
    default: drove=false;
    }
    if (SideEffects) {
        if (Address==0xfffa || Address==0xfffb) MMC5ClearFrame();
        if (Address>=32768 && Address<49152 && M5.pcm_control%2) MMC5PCM(Value);
    }
    return drove;
}
inline void TricMapper::MMC5Write(ushort Address, byte Input) {
    if (Address==0x2000) M5.ctrl=Input;
    if (Address==0x2001) {
        if (!(M5.mask&24) && (Input&24)) MMC5ClearFrame();
        M5.mask=Input;
        if (!(Input&24)) M5.background=M5.split_active=0;
    }
    if (Address>=24576) {
        uint index=Address<32768 ? MMC5RAM(M5.prg[0])+(Address%8192) : PRG_Off[(Address/4096)%8]+Address%4096;
        if ((index&MMC5_PRG_RAM) && M5.protect[0]==2 && M5.protect[1]==1) WRAM[index%131072]=Input;
        return;
    }
    if (Address>=0x5c00) {
        if (M5.ex_mode!=3) ExRAM[Address%1024]=(M5.ex_mode==2 || M5.in_frame)?Input:0;
        return;
    }
    if (Address>=0x5113 && Address<=0x5117) {M5.prg[Address-0x5113]=Input; MMC5Apply(); return;}
    if (Address>=0x5120 && Address<=0x512b) {
        unsigned bank=(Input+M5.upper*256)*(8>>M5.chr_mode);
        M5.chr[Address-0x5120]=(bank%1024)|(Input%2);
        M5.last_b=Address>=0x5128;
        return;
    }
    if (Address>=0x5000 && Address<=0x5007) {
        unsigned ch=(Address/4)%2, r=Address%4;
        if (r==1) return;
        M5.pulse[ch].reg[r]=Input;
        if (r==3 && ((M5.audio_enable>>ch)&1)) {
            static const byte length[32]={10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30};
            M5.pulse[ch].length=length[Input/8];
        }
        return;
    }
    switch (Address) {
    case 0x5010: M5.pcm_control=Input; break;
    case 0x5011: if (!(M5.pcm_control%2)) MMC5PCM(Input); break;
    case 0x5015:
        M5.audio_enable=Input%4;
        if (!(Input&1)) M5.pulse[0].length=0;
        if (!(Input&2)) M5.pulse[1].length=0;
        break;
    case 0x5100: M5.prg_mode=Input%4; MMC5Apply(); break;
    case 0x5101: M5.chr_mode=Input%4; break;
    case 0x5102: case 0x5103: M5.protect[Address-0x5102]=Input%4; break;
    case 0x5104: M5.ex_mode=Input%4; break;
    case 0x5105: M5.nt=Input; break;
    case 0x5106: M5.fill_tile=Input; break;
    case 0x5107: M5.fill_attr=Input%4; break;
    case 0x5130: M5.upper=Input%4; break;
    case 0x5200: M5.split=Input; break;
    case 0x5201: M5.scroll=Input; if (!M5.in_frame) M5.split_y=Input; break;
    case 0x5202: M5.split_bank=Input; break;
    case 0x5203: M5.irq_line=Input; break;
    case 0x5204: M5.irq_enable=Input/128; break;
    case 0x5205: case 0x5206: M5.multiply[Address-0x5205]=Input; break;
    case 0x5207: M5.gpio_mode=Input; break;
    case 0x5208: M5.gpio_data=Input; break;
    case 0x5209:
        M5.timer=(M5.timer/256)*256+Input;
        M5.timer_active=M5.timer!=0; M5.timer_write=totalCycles+1; break;
    case 0x520a: M5.timer=M5.timer%256+Input*256; M5.timer_write=totalCycles+1; break;
    }
}
inline void TricMapper::MMC5Clock() {
    // Connector data pins are refreshed only on an access; /RD changes even
    // when there is no access (especially at vblank). Sample the actual pin.
    if (PPU_READ) M5.idle=0;
    else if (M5.idle<3) {
        ++M5.idle;
        if (M5.idle==3) {
            M5.in_frame=0; M5.repeats=0; M5.background=0; M5.split_active=0;
            M5.last_addr=65535; M5.split_y=M5.scroll;
        }
    }
    if (M5.timer_active && totalCycles!=M5.timer_write) {
        M5.timer=(ushort)(M5.timer-1);
        if (!M5.timer) { M5.timer_irq=1; M5.timer_active=0; }
    }
    if (++M5.audio_divider==7457) {
        M5.audio_divider=0;
        for (unsigned i=0;i<2;++i) if (M5.pulse[i].length && !(M5.pulse[i].reg[0]&32)) --M5.pulse[i].length;
    }
}
inline void TricMapper::MMC5PPURead(ushort Address) {
    if (M5.rd) return;
    static int trace=-1; if (trace<0) trace=getenv("CYC_MMC5_TRACE")!=NULL;
    if (trace) fprintf(stderr,"M5 %04x fetch=%u line=%u frame=%u repeats=%u split=%u y=%u nt=%u\n",Address,M5.fetch,M5.line,M5.in_frame,M5.repeats,M5.split_active,M5.split_y,M5.split_addr);
    M5.rd=1;
    bool line=M5.repeats==3;
    if (line) {
        if (M5.in_frame) {
            M5.line=(byte)(M5.line+1);
            if (M5.line==240) MMC5ClearFrame();
            else if (M5.line==M5.irq_line && M5.irq_line!=0) M5.irq_pending=1;
            M5.split_y=(M5.split_y==239)?0:(byte)(M5.split_y+1);
        } else {
            M5.in_frame=1; M5.line=0; M5.irq_pending=0; M5.split_y=M5.scroll;
        }
        M5.fetch=1; M5.repeats=0;
    } else if (M5.in_frame) M5.fetch=(byte)(M5.fetch+1);
    if (Address/4096==2) M5.repeats=Address==M5.last_addr?M5.repeats+1:1;
    else M5.repeats=0;
    M5.last_addr=Address;
    if (M5.repeats==3) M5.fetch=0;
    M5.background=(M5.mask&24) && (M5.in_frame || M5.repeats==3) && !(M5.fetch>=128 && M5.fetch<160);
    if (M5.background && Address>=8192 && Address%1024<960) {
        unsigned column=M5.fetch>=160?(M5.fetch-160)/4:(M5.fetch/4+2)%32;
        M5.ext_attr=ExRAM[Address%1024];
        bool side=(M5.split&64)?column>=M5.split%32:column<M5.split%32;
        M5.split_active=M5.ex_mode<2 && M5.split>=128 && side;
        unsigned y=M5.split_y;
        bool next=M5.fetch>=160 || (M5.repeats==3 && M5.in_frame);
        if (next) y=(y==239)?0:(y+1)%256;
        M5.split_addr=(y/8)*32+column%32;
        M5.split_tile=ExRAM[M5.split_addr];
    }
}
inline unsigned TricMapper::MMC5CHR(ushort Address) {
    unsigned bank;
    if (M5.background && M5.split_active) bank=M5.split_bank*4+(Address/1024)%4;
    else if (M5.background && M5.ex_mode==1) bank=(M5.upper*64+M5.ext_attr%64)*4+(Address/1024)%4;
    else {
        bool b=(M5.ctrl&32) && ((M5.in_frame && (M5.mask&24))?M5.background:M5.last_b);
        unsigned size=8>>M5.chr_mode, page=Address/1024;
        unsigned reg=page/size*size+size-1;
        if (b) reg=8+reg%4;
        bank=M5.chr[reg]/size*size+page%size;
    }
    unsigned index=bank%Cart.CHRPages*1024+Address%1024;
    return Cart.UsingCHRRAM?index%Cart.CHRROM_Length:index;
}
inline byte TricMapper::MMC5NT(ushort Address, bool Render) {
    unsigned offset=Address%1024;
    if (Render && M5.background) {
        if (M5.split_active) {
            if (offset<960) return M5.split_tile;
            unsigned x=M5.split_addr%32, y=M5.split_addr/32;
            byte attr=ExRAM[960+(y/4)*8+x/4];
            return ((attr>>((y%4/2)*4+(x%4/2)*2))%4)*85;
        }
        if (M5.ex_mode==1 && offset>=960) return (M5.ext_attr/64)*85;
    }
    unsigned source=(M5.nt>>(2*((Address/1024)%4)))%4;
    if (source<2) return VRAM[source*1024+offset];
    if (source==2) return M5.ex_mode<2?ExRAM[offset]:0;
    return offset<960?M5.fill_tile:M5.fill_attr*85;
}
inline void TricMapper::MMC5PPU() {
    bool reading=!SeventyTwoPinConnector[20], writing=!SeventyTwoPinConnector[55];
    if (reading) MMC5PPURead(PPU_AddressIn);
    if (PPU_AddressIn<8192) {
        unsigned index=MMC5CHR(PPU_AddressIn);
        byte value=PPU_NonTiltData;
        if (reading) PPU_NonTiltData=PPU_DataOut=Cart.CHRROM[index];
        if (writing && Cart.UsingCHRRAM) Cart.CHRROM[index]=value;
    } else {
        byte value=PPU_NonTiltData;
        if (reading) PPU_NonTiltData=PPU_DataOut=MMC5NT(PPU_AddressIn,true);
        if (writing) {
            unsigned source=(M5.nt>>(2*((PPU_AddressIn/1024)%4)))%4;
            if (source<2) VRAM[source*1024+PPU_AddressIn%1024]=value;
            else if (source==2 && M5.ex_mode<2) ExRAM[PPU_AddressIn%1024]=value;
        }
    }
}
