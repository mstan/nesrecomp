/* Nintendo MMC5 / ExROM. Written from the hardware documentation:
 * https://www.nesdev.org/wiki/MMC5 and /MMC5_audio
 * https://sourceforge.net/p/fceultra/bugs/787/ (CHR tests on real hardware).
 * The CL board wiring is used; unspecified power-on registers start at zero.
 */
#define M5 (hw_cart.m.mmc5)

static uint32_t mmc5_ram_bank(unsigned bank)
{
    unsigned size=hw_cart.wram_len, offset;
    bank &= 15;
    if (!size) return MMC5_PRG_OPEN;
    if (hw_cart.info.prg_ram && hw_cart.info.prg_nvram) {
        /* Two populated chip selects; chip 0 carries the battery. */
        offset=bank&4 ? (bank&3)*8192%hw_cart.info.prg_ram :
            hw_cart.info.prg_ram+(bank&3)*8192%hw_cart.info.prg_nvram;
        return MMC5_PRG_RAM|offset;
    }
    if (size==8192) { if (bank&4) return MMC5_PRG_OPEN; offset=0; }
    else if (size==16384) {
        /* ETROM: two 8 KiB chips; chip 0 is the battery-backed chip. */
        offset=(bank&4)?8192:0;
    } else if (size==32768) { if (bank&4) return MMC5_PRG_OPEN; offset=(bank&3)*8192; }
    else offset=(bank*8192)%size;
    return MMC5_PRG_RAM|offset;
}

static void mmc5_prg_apply(void)
{
    unsigned mode=M5.prg_mode;
    for (unsigned slot=0;slot<4;++slot) {
        unsigned r, mask;
        if (!mode) { r=4; mask=3; }
        else if (mode==1 || (mode==2 && slot<2)) { r=(slot<2)?2:4; mask=1; }
        else { r=slot+1; mask=0; }
        unsigned bank=(M5.prg[r]&~mask)|(slot&mask);
        if (r==4 || (bank&128)) map_prg8(slot,bank&127);
        else {
            uint32_t offset=mmc5_ram_bank(bank);
            hw_cart.prg_off[slot*2]=offset;
            hw_cart.prg_off[slot*2+1]=offset|4096;
        }
    }
}

static void mmc5_frame_clear(void)
{
    M5.in_frame=M5.line=M5.irq_pending=M5.repeats=0;
    M5.last_addr=0xffff;
    M5.split_y=M5.scroll;
}

/* Called once per /RD assertion. Repeated $2007 samples are one read. The
 * attribute fetch AFTER the third matching nametable fetch clocks the line. */
static void mmc5_ppu_read(uint16_t addr)
{
    if (M5.rd) return;
    static int trace=-1; if (trace<0) trace=getenv("CYC_MMC5_TRACE")!=NULL;
    if (trace) fprintf(stderr,"M5 %04x fetch=%u line=%u frame=%u repeats=%u split=%u y=%u nt=%u\n",addr,M5.fetch,M5.line,M5.in_frame,M5.repeats,M5.split_active,M5.split_y,M5.split_addr);
    M5.rd=1;
    if (M5.repeats==3) {
        if (!M5.in_frame) { M5.in_frame=1; M5.line=0; M5.irq_pending=0; M5.split_y=M5.scroll; }
        else {
            ++M5.line;
            if (M5.line==240) mmc5_frame_clear();
            else if (M5.irq_line && M5.line==M5.irq_line) M5.irq_pending=1;
            M5.split_y=M5.split_y==239 ? 0 : (uint8_t)(M5.split_y+1);
        }
        M5.fetch=1;
        M5.repeats=0;
    } else if (M5.in_frame) ++M5.fetch;
    if ((addr&0xf000)==0x2000 && addr==M5.last_addr) ++M5.repeats;
    else M5.repeats=(addr&0xf000)==0x2000 ? 1 : 0;
    M5.last_addr=addr;
    if (M5.repeats==3) M5.fetch=0;
    M5.background=(M5.mask&24) && (M5.in_frame || M5.repeats==3) &&
        (M5.fetch<128 || M5.fetch>=160);
    if (addr>=0x2000 && (addr&0x3ff)<0x3c0 && M5.background) {
        unsigned column=M5.fetch<128 ? (M5.fetch/4+2)&31 : (M5.fetch-160)/4;
        M5.ext_attr=hw_cart.exram[addr&1023];
        M5.split_active=(M5.split&128) && M5.ex_mode<2 &&
            ((M5.split&64) ? column>=(M5.split&31) : column<(M5.split&31));
        /* The two prefetch tiles belong to the next scanline. Its first
         * visible fetch also precedes the IRQ counter's attribute-read edge. */
        unsigned row=M5.split_y;
        if (M5.fetch>=160 || (M5.in_frame && M5.repeats==3)) row=row==239?0:(uint8_t)(row+1);
        M5.split_addr=(row/8)*32+(column&31);
        M5.split_tile=hw_cart.exram[M5.split_addr];
    }
}

uint32_t hw_cart_mmc5_chr_index(uint16_t addr)
{
    unsigned bank;
    if (M5.background && M5.split_active) bank=M5.split_bank*4+(addr>>10&3);
    else if (M5.background && M5.ex_mode==1) bank=((M5.upper<<6)|(M5.ext_attr&63))*4+(addr>>10&3);
    else {
        bool b=(M5.ctrl&32) && ((M5.in_frame && (M5.mask&24)) ? M5.background : M5.last_b);
        unsigned page=(addr>>10)&(b?3:7), mask=(1u<<(3-M5.chr_mode))-1;
        /* Background's four registers repeat in both pattern tables. */
        unsigned r=b ? 8+((page|mask)&3) : page|mask;
        bank=(M5.chr[r]&~mask)|((addr>>10)&mask);
    }
    unsigned index=(bank&(hw_cart.chr_pages-1))*1024+(addr&1023);
    return hw_cart.chr_ram && hw_cart.chr_len ? index%hw_cart.chr_len : index;
}

static uint8_t mmc5_nt_read(uint16_t addr, bool reading)
{
    if (reading) mmc5_ppu_read(addr);
    unsigned offset=addr&1023;
    if (reading && M5.background) {
        if (M5.split_active) {
            if (offset<960) return M5.split_tile;
            unsigned x=M5.split_addr&31, y=M5.split_addr>>5;
            unsigned attr=hw_cart.exram[960+(y/4)*8+x/4];
            return ((attr>>((y&2)*2+(x&2)))&3)*0x55;
        }
        if (M5.ex_mode==1 && offset>=960) return (M5.ext_attr>>6)*0x55;
    }
    unsigned source=(M5.nt>>((addr>>10&3)*2))&3;
    if (source<2) return ppu.ciram[source*1024+offset];
    if (source==2) return M5.ex_mode<2 ? hw_cart.exram[offset] : 0;
    return offset<960 ? M5.fill_tile : M5.fill_attr*0x55;
}

static void mmc5_nt_write(uint16_t addr, uint8_t value)
{
    unsigned source=(M5.nt>>((addr>>10&3)*2))&3;
    if (source<2) ppu.ciram[source*1024+(addr&1023)]=value;
    else if (source==2 && M5.ex_mode<2) hw_cart.exram[addr&1023]=value;
}

static void mmc5_pcm(uint8_t value)
{
    if (value) M5.pcm=value;
    M5.pcm_irq=value==0;
}

void hw_cart_cpu_read_snoop(uint16_t addr, uint8_t value)
{
    if (hw_cart.mapper!=5) return;
    if (addr==0xfffa || addr==0xfffb) mmc5_frame_clear();
    if (addr>=0x8000 && addr<0xc000 && (M5.pcm_control&1)) mmc5_pcm(value);
}

static bool mmc5_read(uint16_t addr, uint8_t *value)
{
    if (addr>=0x6000) {
        uint32_t offset=addr<0x8000 ? mmc5_ram_bank(M5.prg[0])|(addr&8191) :
            hw_cart.prg_off[addr>>12&7]|(addr&4095);
        if (offset&MMC5_PRG_OPEN) return false;
        *value=offset&MMC5_PRG_RAM ? hw_cart.wram[offset&0x1ffff] : hw_cart.prg[offset];
        return true;
    }
    if (addr>=0x5c00) {
        if (M5.ex_mode<2) return false;
        *value=hw_cart.exram[addr&1023]; return true;
    }
    switch (addr) {
    case 0x5010: *value=(M5.pcm_irq && (M5.pcm_control&128)?128:0)|1; M5.pcm_irq=0; return true;
    case 0x5015: *value=(M5.pulse[0].length?1:0)|(M5.pulse[1].length?2:0); return true;
    case 0x5204: *value=(M5.in_frame?64:0)|(M5.irq_pending?128:0); M5.irq_pending=0; return true;
    case 0x5205: *value=(uint8_t)(M5.multiply[0]*M5.multiply[1]); return true;
    case 0x5206: *value=(uint8_t)((M5.multiply[0]*M5.multiply[1])>>8); return true;
    case 0x5208: *value=(*value&63)|(M5.gpio_data&~M5.gpio_mode&0xc0); return true;
    case 0x5209: *value=M5.timer_irq?128:0; M5.timer_irq=0; return true;
    default: return false;
    }
}

static void mmc5_write(uint16_t addr, uint8_t value)
{
    if (addr==0x2000) M5.ctrl=value;
    else if (addr==0x2001) {
        if (!(M5.mask&24) && (value&24)) mmc5_frame_clear();
        M5.mask=value;
        if (!(value&24)) { M5.background=0; M5.split_active=0; }
    }
    if (addr>=0x6000) {
        uint32_t offset=addr<0x8000 ? mmc5_ram_bank(M5.prg[0])|(addr&8191) :
            hw_cart.prg_off[addr>>12&7]|(addr&4095);
        if ((offset&MMC5_PRG_RAM) && M5.protect[0]==2 && M5.protect[1]==1)
            hw_cart.wram[offset&0x1ffff]=value;
        return;
    }
    if (addr>=0x5c00) {
        if (M5.ex_mode<3) hw_cart.exram[addr&1023]=M5.ex_mode==2 || M5.in_frame ? value : 0;
        return;
    }
    if (addr>=0x5113 && addr<=0x5117) { M5.prg[addr-0x5113]=value; mmc5_prg_apply(); return; }
    if (addr>=0x5120 && addr<=0x512b) {
        /* Register contents are shifted WHEN WRITTEN; changing CHR mode does
         * not retroactively shift them. Low unknown lines resolve to zero. */
        M5.chr[addr-0x5120]=(uint16_t)((((M5.upper*256+value)<<(3-M5.chr_mode))&1023)|(value&1));
        M5.last_b=addr>=0x5128;
        return;
    }
    if (addr>=0x5000 && addr<=0x5007) {
        unsigned ch=(addr>>2)&1, r=addr&3;
        if (r==1) return; /* no sweep unit */
        Mmc5Pulse *p=&M5.pulse[ch]; p->reg[r]=value;
        if (r==3) {
            static const uint8_t lengths[32]={10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30};
            if (M5.audio_enable&(1u<<ch)) p->length=lengths[value>>3];
            p->step=0; p->restart=1;
        }
        return;
    }
    switch (addr) {
    case 0x5010: M5.pcm_control=value; break;
    case 0x5011: if (!(M5.pcm_control&1)) mmc5_pcm(value); break;
    case 0x5015:
        M5.audio_enable=value&3;
        for (unsigned ch=0;ch<2;++ch) if (!(value&(1u<<ch))) M5.pulse[ch].length=0;
        break;
    case 0x5100: M5.prg_mode=value&3; mmc5_prg_apply(); break;
    case 0x5101: M5.chr_mode=value&3; break;
    case 0x5102: case 0x5103: M5.protect[addr-0x5102]=value&3; break;
    case 0x5104: M5.ex_mode=value&3; break;
    case 0x5105: M5.nt=value; break;
    case 0x5106: M5.fill_tile=value; break;
    case 0x5107: M5.fill_attr=value&3; break;
    case 0x5130: M5.upper=value&3; break;
    case 0x5200: M5.split=value; break;
    case 0x5201: M5.scroll=value; if (!M5.in_frame) M5.split_y=value; break;
    case 0x5202: M5.split_bank=value; break;
    case 0x5203: M5.irq_line=value; break;
    case 0x5204: M5.irq_enable=value>>7; break;
    case 0x5205: case 0x5206: M5.multiply[addr-0x5205]=value; break;
    case 0x5207: M5.gpio_mode=value; break;
    case 0x5208: M5.gpio_data=value; break;
    case 0x5209:
        M5.timer=(M5.timer&0xff00)|value; M5.timer_active=M5.timer!=0;
        M5.timer_write=hw.cycles+1; break;
    case 0x520a: M5.timer=(M5.timer&255)|(value<<8); M5.timer_write=hw.cycles+1; break;
    }
}

static void mmc5_clock(void)
{
    if (ppu.rd) M5.idle=0;
    else if (M5.idle<3 && ++M5.idle==3) {
        M5.in_frame=M5.repeats=M5.background=M5.split_active=0;
        M5.last_addr=0xffff; M5.split_y=M5.scroll;
    }
    if (M5.timer_active && hw.cycles!=M5.timer_write && --M5.timer==0) {
        M5.timer_irq=1; M5.timer_active=0;
    }
    bool envelope=++M5.audio_divider==7457;
    if (envelope) M5.audio_divider=0;
    for (unsigned ch=0;ch<2;++ch) {
        Mmc5Pulse *p=&M5.pulse[ch];
        if (p->timer) --p->timer;
        else { p->timer=(uint16_t)(2*(p->reg[2]+((p->reg[3]&7)<<8)+1)-1); p->step=(p->step+1)&7; }
        if (!envelope) continue;
        if (p->length && !(p->reg[0]&32)) --p->length;
        if (p->restart) { p->restart=0; p->envelope=15; p->divider=p->reg[0]&15; }
        else if (p->divider) --p->divider;
        else {
            p->divider=p->reg[0]&15;
            if (p->envelope) --p->envelope;
            else if (p->reg[0]&32) p->envelope=15;
        }
    }
}

static double mmc5_audio(void)
{
    static const uint8_t duty[4]={0x02,0x06,0x1e,0xf9};
    unsigned pulse=0;
    for (unsigned ch=0;ch<2;++ch) {
        Mmc5Pulse *p=&M5.pulse[ch];
        if (p->length && ((duty[p->reg[0]>>6]>>p->step)&1))
            pulse+=(p->reg[0]&16)?p->reg[0]&15:p->envelope;
    }
    /* Nominal per-code PCM gain, anchored to this core's DMC full scale at
     * 127. The MMC5's eighth bit doubles the range; board resistors vary. */
    return -(pulse*(0.1488/15.0)+M5.pcm*((163.67/(24329.0/127+100))/127));
}

static void mmc5_reset(void)
{
    memset(&M5,0,sizeof(M5));
    M5.prg_mode=3; M5.ex_mode=3; M5.protect[0]=1; M5.protect[1]=2;
    M5.prg[1]=0x80; M5.prg[2]=0x81; M5.prg[3]=0x82; M5.prg[4]=0xff;
    M5.gpio_mode=0xc0; M5.pcm=0xff;
    mmc5_frame_clear(); mmc5_prg_apply(); map_chr8(0);
}
#undef M5
