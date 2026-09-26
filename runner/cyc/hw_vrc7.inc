/* VRC7 mapper and the six-channel OPLL subset, using MIT-licensed emu2413.
 * https://www.nesdev.org/wiki/VRC7
 * https://www.nesdev.org/wiki/VRC7_audio */
#include "vendor/emu2413/emu2413.h"
static OPLL *vrc7_opll;

static void vrc7_sound_reset(bool cold)
{
    if (cold) {
        if (vrc7_opll) OPLL_delete(vrc7_opll);
        vrc7_opll = hw_cart.mapper == 85 ? OPLL_new(3579545, 49716) : NULL;
        if (hw_cart.mapper == 85 && !vrc7_opll) {
            fputs("Unable to allocate VRC7 sound core\n",stderr); exit(1);
        }
    }
    if (!vrc7_opll) return;
    uint32_t vibrato = cold ? 0 : vrc7_opll->pm_phase;
    OPLL_setChipType(vrc7_opll,1);
    OPLL_reset(vrc7_opll);
    OPLL_resetPatch(vrc7_opll,OPLL_VRC7_TONE);
    OPLL_setMask(vrc7_opll,OPLL_MASK_CH(6)|OPLL_MASK_CH(7)|OPLL_MASK_CH(8)|OPLL_MASK_RHYTHM);
    vrc7_opll->pm_phase = vibrato;
    /* Our rational oscillator clocks exactly one OPLL update per 72 clocks;
     * disable the library's additional integer-rate scheduling adjustment. */
    vrc7_opll->inp_step = vrc7_opll->out_step;
    memset(hw_cart.m.vrc7_reg,0,sizeof(hw_cart.m.vrc7_reg));
    hw_cart.m.vrc7_address=0; hw_cart.m.vrc7_output=0;
}

static unsigned vrc7_port(uint16_t addr)
{
    unsigned sub=hw_cart.info.submapper;
    return ((addr & (sub == 1 ? 8 : sub == 2 ? 16 : 24)) != 0) | ((addr & 32) ? 2 : 0);
}

static void vrc7_apply(void)
{
    for (unsigned i=0;i<3;++i) map_prg8(i,hw_cart.m.reg[i]);
    map_prg8(3,-1);
    for (unsigned i=0;i<8;++i) map_chr1(i,hw_cart.m.vrc_chr[i]);
    static const uint8_t mir[]={HW_MIRROR_VERTICAL,HW_MIRROR_HORIZONTAL,HW_MIRROR_SCREEN_A,HW_MIRROR_SCREEN_B};
    hw_cart.mirroring=mir[hw_cart.m.ctrl&3];
    hw_cart.wram_readable=hw_cart.wram_writable=(hw_cart.m.ctrl&128)!=0;
}

static void vrc7_write(uint16_t addr, uint8_t value)
{
    unsigned page=addr>>12, port=vrc7_port(addr);
    if (page==9 && (port&1)) {
        if (hw_cart.m.ctrl&64) return;
        if (port==1) hw_cart.m.vrc7_address=value;
        else {
            unsigned r=hw_cart.m.vrc7_address;
            if (r<8 || r==15 || (r>=16 && r<=21) || (r>=32 && r<=37) || (r>=48 && r<=53)) {
                hw_cart.m.vrc7_reg[r]=value;
                if (vrc7_opll) OPLL_writeReg(vrc7_opll,r,value);
            }
        }
        return;
    }
    if (port>=2) return;
    if (page==8 || (page==9 && port==0)) {
        hw_cart.m.reg[page==8 ? port : 2]=value&63; vrc7_apply();
    } else if (page>=10 && page<=13) {
        hw_cart.m.vrc_chr[(page-10)*2+port]=value; vrc7_apply();
    } else if (page==14) {
        if (port) hw_cart.m.irq_latch=value;
        else {
            if ((value&64) && !(hw_cart.m.ctrl&64)) vrc7_sound_reset(false);
            hw_cart.m.ctrl=value; vrc7_apply();
        }
    } else if (page==15) {
        if (port) vrc_irq_ack(); else vrc_irq_control(value);
    }
}

static void vrc7_audio_clock(void)
{
    /* NTSC CPU = 19,687,500/11 Hz; LP resonator = 3,579,545 Hz.
     * Board 85.1 lacks the resonator and cannot produce expansion sound. */
    if (hw_cart.info.submapper==1 || !vrc7_opll) return;
    hw_cart.m.vrc7_phase += 3579545ull*11;
    if (hw_cart.m.vrc7_phase < 19687500ull*72) return;
    hw_cart.m.vrc7_phase -= 19687500ull*72;
    if (hw_cart.m.ctrl&64) { vrc7_opll->am_phase=0; vrc7_opll->lfo_am=0; }
    int16_t output=OPLL_calc(vrc7_opll);
    /* VRC7's diagnostic phase hold has silent output while envelopes run.
     * emu2413's generic OPLL phase-reset path leaves a DC residue. */
    hw_cart.m.vrc7_output=((hw_cart.m.ctrl&64) || (hw_cart.m.vrc7_reg[15]&4)) ? 0 : output;
}

static uint64_t vrc7_sound_hash(uint64_t acc)
{
    acc=acc*131+hw_cart.m.vrc7_address; acc=acc*131+hw_cart.m.vrc7_phase;
    acc=acc*131+(uint16_t)hw_cart.m.vrc7_output;
    for (unsigned r=0;r<64;++r) acc=acc*131+hw_cart.m.vrc7_reg[r];
    if (!vrc7_opll) return acc;
    acc=acc*131+vrc7_opll->pm_phase; acc=acc*131+vrc7_opll->am_phase;
    acc=acc*131+vrc7_opll->eg_counter; acc=acc*131+vrc7_opll->noise;
    for (unsigned i=0;i<12;++i) {
        const OPLL_SLOT *s=&vrc7_opll->slot[i];
        acc=acc*131+s->pg_phase; acc=acc*131+s->eg_out; acc=acc*131+s->eg_state;
        acc=acc*131+s->output[0]; acc=acc*131+s->output[1];
        acc=acc*131+s->key_flag; acc=acc*131+s->update_requests;
    }
    return acc;
}
