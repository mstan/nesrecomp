/* Native picture provider: a timestamped background tile-input stream,
 * bulk OAM selection and a precomposed sprite row. No past fetch is rebuilt
 * from current memory. Register writes enter the same bounded input history
 * used by the reference service; build-selected fallback remains intact. */
static struct {
    unsigned sprite_banks,irq_dot;
    bool enabled,sprite_banks_valid,irq_valid;
} fp;
static bool fp_supported(void)
{
    /* PAL qualifies the fixed-PRG boards whose CHR changes flush through CPU
     * writes; mapper IRQ timestamps below are NTSC master-clock formulas. */
    if(hw_pal()) return hw_cart.prg && (hw_cart.mapper==0 || hw_cart.mapper==3);
    return hw_cart.prg && (hw_cart.mapper==0 || hw_cart.mapper==1 || hw_cart.mapper==4 || hw_cart.mapper==66);
}

/* OAM's object search is one scanline operation. The clear and fetch lanes
 * retain their register/bus timing; no per-pixel sprite-unit loop remains. */
static void fp_select_objects(void)
{
    unsigned count=0,height=ppu.sprite16?16:8;
    memset(ppu.oam2,0xff,sizeof(ppu.oam2));ppu.next_has_s0=0;
    for(unsigned i=0;i<64;++i) {
        unsigned row=(unsigned)(ppu.scanline&255)-ppu.oam[i*4];
        if(row>=height)continue;
        if(count<8) {
            memcpy(ppu.oam2+count*4,ppu.oam+i*4,4);
            if(!i)ppu.next_has_s0=1;
        }
        ++count;
    }
    if(count>8)ppu.overflow=1;
    ppu.oam2_addr=(uint8_t)(count<8?count*4:0x20);ppu.oam2_full=count>=8;
    ppu.eval_wrapped=1;ppu.eval_tick=0;ppu.oam_addr=0;
    fp.sprite_banks_valid=fp.irq_valid=false;
}
static void fp_sprite_operation(void)
{
    bool prerender=ppu.scanline==hw_prerender_line();
    if(ppu.dot<=64 || ppu.dot>=257) { sprite_evaluation();return; }
    if((ppu.instant_bg || ppu.instant_spr) && ppu.oamc_pending) {
        ppu.oamc_pending=0;if(!ppu.oamc_reenabled)corrupt_oam();ppu.oamc_reenabled=0;
    }
    if(ppu.dot==65 && eval_rendering() && !prerender)fp_select_objects();
    if(ppu.oamc_disabled || ppu.oamc_disabled_now)eval_objects(prerender);
}
static void fp_compose_sprites(void)
{
    unsigned line=ppu.scanline,height=ppu.sprite16?16:8,count=0,slots[8];
    if(line>=240)return;
    HR_COUNT(++hr_frame_rows);
    for(unsigned i=0;i<64;++i) {
        int row=(int)line-ppu.oam[i*4]-1;
        if(row>=0 && row<(int)height) { if(count<8)slots[count]=i;++count; }
    }
    if(count>8)count=8;
    memset(ppu.frame_sprites,0,sizeof(ppu.frame_sprites));
    for(int slot=(int)count-1;slot>=0;--slot) {
        unsigned i=slots[slot],attr=ppu.oam[i*4+2],row=line-ppu.oam[i*4]-1;
        if(attr&0x80)row=height-1-row;
        unsigned tile=ppu.oam[i*4+1],address=height==16?
            ((tile&1)<<12)|((tile&0xfe)<<4)|(row&7)|((row&8)<<1):
            (ppu.sprite_table<<12)|(tile<<4)|row;
        unsigned lo=hw_cart_chr_read((uint16_t)address),hi=hw_cart_chr_read((uint16_t)(address+8));
#ifdef NESRECOMP_CYCLE_HDPACK
        if(hdpack_recording()) {
            cyc_hdpack_sprite_row((unsigned)slot,(uint16_t)address,(uint8_t)lo,(uint8_t)hi,attr,ppu.oam[i*4+3]);
        }
#endif
        for(unsigned j=0;j<8;++j) {
            unsigned x=ppu.oam[i*4+3]+j;if(x>=256)break;
            unsigned bit=(attr&0x40)?j:7-j;
            unsigned sc=((lo>>bit)&1)|(((hi>>bit)&1)<<1);
            if(sc)ppu.frame_sprites[x]=(uint16_t)(sc|((attr&3)<<2)|((attr&0x20)?16:0)|
                ((!i)?32:0)|((unsigned)slot<<6));
        }
    }
    ppu.frame_sprite_line=(uint16_t)line;
    ++ppu_native_sprite_spans;ppu_native_sprite_dots+=256;
}

static unsigned fp_sprite_banks(void)
{
    if(!fp.sprite_banks_valid) {
        unsigned mask=255,slot=0;
        for(unsigned i=0;i<64 && slot<8;++i) {
            if(((ppu.scanline-ppu.oam[i*4])&255)<16) {
                if(!(ppu.oam[i*4+1]&1)) mask&=~(1u<<slot);
                ++slot;
            }
        }
        fp.sprite_banks=mask;fp.sprite_banks_valid=true;
    }
    return fp.sprite_banks;
}
static unsigned fp_address_value(unsigned dot)
{
    if(!rendering() || (ppu.scanline>=240 && ppu.scanline!=hw_prerender_line())) return ppu.v;
    if(!dot) return ppu.bg_table<<12;
    if(dot>=337 || ((dot-1)&7)<4) return 0x2000;
    if(dot>=257 && dot<=320) {
        unsigned bank=ppu.sprite16 ? ((fp_sprite_banks()>>((dot-257)/8))&1):ppu.sprite_table;
        return bank<<12;
    }
    return ppu.bg_table<<12;
}
static unsigned fp_phase_after(unsigned dot)
{
    if(dot<1) return 1;
    if(dot<257 && !ppu.bg_table) return 257;
    if(dot>=321 && !ppu.bg_table) return 341;
    return dot<337 ? ((dot+3)/4)*4+1:341;
}
static unsigned fp_address_event(unsigned dot,unsigned a12)
{
    if(!hw_cart.watch_ppu_addr || !rendering() ||
       (ppu.scanline>=240 && ppu.scanline!=hw_prerender_line())) return 341;
    /* Identical address phases carry no mapper observation. A constant-low
     * background is one interval, not sixty-four repeated notifications. */
    for(unsigned next=fp_phase_after(dot);next<341;next=fp_phase_after(next))
        if(((fp_address_value(next)>>12)&1)!=a12) return next;
    return 341;
}
static unsigned fp_irq_deadline(void)
{
    if(!fp.irq_valid) {
        unsigned a12=hw_cart.m.a12,counter=hw_cart.m.irq_counter,reload=hw_cart.m.irq_reload;
        uint64_t low=hw_cart.m.a12_low_cycle;
        unsigned dot=ppu.dot,first=1u+((4u-((hw.align+1u)&3u))&3u);
        fp.irq_dot=341;
        if(hw_cart.m.irq_enable) for(;;) {
            unsigned next=fp_address_event(dot,a12);
            if(next>=341) break;
            uint64_t cycle=hw.cycles+(first+4u*(next-ppu.dot-1u))/12u;
            a12^=1u;
            if(!a12) low=cycle;
            else if(cycle-low>=3) {
                if(!counter || reload) counter=hw_cart.m.irq_latch;
                else --counter;
                reload=0;
                if(!counter) { fp.irq_dot=next;break; }
            }
            dot=next;
        }
        fp.irq_valid=true;
    }
    return fp.irq_dot;
}

static void fp_input_pixel(void)
{
    if(ppu.frame_sprite_line!=ppu.scanline)fp_compose_sprites();
    uint8_t color = 0, pal = 0;
#ifdef NESRECOMP_CYCLE_HDPACK
    unsigned hd_bg_color=0,hd_bg_palette=0,hd_sprite_color=0;int hd_sprite=-1;
#endif
    if (ppu.show_bg && (ppu.dot > 8 || ppu.show_bg8)) {
        unsigned fx = ppu.fine_x;
#if NESRECOMP_PPU_HLE
        color = (uint8_t)((ppu.bg_pixels >> (30 - 2 * fx)) & 3);
        pal = (uint8_t)((ppu.attr_pixels >> (14 - 2 * fx)) & 3);
#else
        color = (uint8_t)(((ppu.bg_lo >> (15 - fx)) & 1) | (((ppu.bg_hi >> (15 - fx)) & 1) << 1));
        pal = (uint8_t)(((ppu.attr_lo >> (7 - fx)) & 1) | (((ppu.attr_hi >> (7 - fx)) & 1) << 1));
#endif
#ifdef NESRECOMP_CYCLE_HDPACK
        hd_bg_color=color;hd_bg_palette=pal;
#endif
        if (color == 0) pal = 0;
    }
    hw_frame_bg[ppu.scanline * 256 + ppu.dot - 1] = color != 0;
    uint8_t background_addr=(uint8_t)(pal<<2|color);
    if(ppu.show_spr && (ppu.dot>8 || ppu.show_spr8) && ppu.frame_sprite_line==ppu.scanline) {
        unsigned sample=ppu.frame_sprites[ppu.dot-1],sc=sample&3;
        if(sc) {
            if((sample&32) && ppu.can_s0hit && ppu.show_bg && color && ppu.dot<256) {
                ppu.s0hit_pending1=1;ppu.can_s0hit=0;
            }
            if(color==0 || !(sample&16)) {
#ifdef NESRECOMP_CYCLE_HDPACK
                hd_sprite=(int)((sample>>6)&7);hd_sprite_color=sc;
#endif
                color=(uint8_t)sc;pal=(uint8_t)(((sample>>2)&3)|4);
            }
        }
    }
    uint8_t addr;
    if (rendering() && ppu.scanline < 240) {
        addr = (uint8_t)(pal << 2 | color);
    } else if ((ppu.v & 0x3F1F) >= 0x3F00) {
        /* Rendering off with v in palette RAM: the backdrop is that entry. */
        addr = (uint8_t)(ppu.v & 0x1F);
        if ((addr & 3) == 0) addr &= 0x0F;
    } else {
        addr = 0;
    }
    if (ppu.palc_disabled || ppu.palc_v_left) {
        ppu.palc_disabled = ppu.palc_v_left = 0;
        corrupt_palettes(color);
    }
    ppu.color[0] = ppu.palette[addr] & 0x3F;
    if(cyc_background_enabled)cyc_background_pipe[0]=ppu.palette[
        rendering()&&ppu.scanline<240?background_addr:addr]&0x3f;
#ifdef NESRECOMP_CYCLE_HDPACK
    cyc_hdpack_row_pixel(hd_bg_color,hd_bg_palette,hd_sprite,hd_sprite_color);
    if(!rendering())cyc_hdpack_blank(ppu.color[0]);
#endif
}

static void fp_input_sample(void)
{
    /* $2005 lands. */
    if (ppu.w2005_delay && --ppu.w2005_delay == 0) {
        uint8_t val = ppu.w2005_value;
        if (!ppu.addr_latch) {
            ppu.fine_x = val & 7;
            ppu.t = (uint16_t)((ppu.t & 0x7FE0) | (val >> 3));
        } else {
            ppu.t = (uint16_t)((ppu.t & 0x0C1F) | ((val & 0xF8) << 2) | ((val & 7) << 12));
        }
        ppu.addr_latch = !ppu.addr_latch;
    }

    /* Scroll copies and the vertical increment, at the end of the dot. */
    if (render_line() && rendering() && ppu.render_count >= 1) {
        if (ppu.dot == 256) increment_y();
        else if (ppu.dot == 257) ppu.v = (uint16_t)((ppu.v & 0x7BE0) | (ppu.t & 0x041F));
        if (ppu.dot >= 280 && ppu.dot <= 304 && ppu.scanline == hw_prerender_line())
            ppu.v = (uint16_t)((ppu.v & 0x041F) | (ppu.t & 0x7BE0));
    }

    unsigned previous_line=ppu.scanline;
    advance_dot();
    if(previous_line!=ppu.scanline)fp.irq_valid=fp.sprite_banks_valid=false;
    int sl = ppu.scanline;

    if (!hw_pal() && ppu.odd_frame && rendering() && sl == 0) {
        if (ppu.dot == 0) {
            /* Odd frames skip dot 0 of scanline 0. */
            ppu.dot = 1;
            ppu.skipped_dot = 1;
        } else if (ppu.dot == 2) {
            ppu.skipped_dot = 0;
        }
    }
    int dot = ppu.dot;
    if (dot == 1 && sl < 240) capture_line(sl);

    /* At alignment 1 sprite evaluation sees $2001 one dot later. */
    if (hw.align != 1) {
        ppu.eval_bg = ppu.show_bg;
        ppu.eval_spr = ppu.show_spr;
    }

    data_sm_dot((!ppu.show_bg && !ppu.show_spr) || (sl >= 240 && sl < hw_prerender_line()));
    ppu.oam_latch = ppu.oam_buffer;

    /* $2006 lands. */
    ppu.copy_v = 0;
    if (ppu.w2006_delay && --ppu.w2006_delay == 0) {
        uint16_t old = ppu.v;
        ppu.copy_v = 1;
        ppu.v = ppu.t;
        ppu.vbus = ppu.v;
        if ((old & 0x3FFF) >= 0x3F00 && (ppu.vbus & 0x3FFF) < 0x3F00 && sl < 240 && dot <= 256 && (old & 0xF) != 0)
            ppu.palc_v_left = 1;
    }

    if (ppu.oam2_reset > 0) ppu.oam2_reset--;

    if (render_line()) {
        fp_sprite_operation();
        if (eval_rendering() && (dot == 63 || dot == 255 || dot == 339)) ppu.oam2_reset = 3;
    } else if (pal_oam_refresh()) {
        /* 2C07 refresh clocks the OAM evaluator, not the pattern fetch bus.
         * VBlank lines above 255 cannot select an onscreen sprite by wrapping
         * their Y coordinate. Doing so leaves ghost sprites on line zero. */
        if (dot <= 64) eval_clear(false);
        else if (dot <= 256) eval_objects(false);
        else if (dot <= 320) ppu.oam_addr = 0;
    }

    if (hw.align == 1) {
        ppu.eval_bg = ppu.show_bg;
        ppu.eval_spr = ppu.show_spr;
    }

    if (!rendering()) {
        ppu.render_count = 0;
        ppu.vbus = ppu.v;
    } else if (ppu.render_count < 5) {
        ppu.render_count++;
    }

    /* $2001 lands, in three parts. */
    if (ppu.w2001_delay && --ppu.w2001_delay == 0) {
        fp.irq_valid=false;
        uint8_t val = ppu.w2001_value;
        ppu.show_bg8 = (val & 0x02) != 0;
        ppu.show_spr8 = (val & 0x04) != 0;
        ppu.show_bg = ppu.instant_bg = (val & 0x08) != 0;
        ppu.show_spr = ppu.instant_spr = (val & 0x10) != 0;
    }
    if (ppu.w2001_oam_delay && --ppu.w2001_oam_delay == 0) {
        if (ppu.w2001_was_rendering && !(ppu.w2001_value & 0x18) && render_line() && !ppu.oamc_pending)
            ppu.oamc_disabled = 1;
    }
    if (ppu.w2001_emph_delay && --ppu.w2001_emph_delay == 0) {
        ppu.greyscale = ppu.w2001_value & 1;
        ppu.emphasis = ppu.w2001_value >> 5;
    }

#ifdef NESRECOMP_CYCLE_HDPACK
    cyc_hdpack_clock();
#endif
    if(cyc_background_enabled){cyc_background_pipe[3]=cyc_background_pipe[2];cyc_background_pipe[2]=cyc_background_pipe[1];cyc_background_pipe[1]=cyc_background_pipe[0];}
    ppu.color[3] = ppu.color[2];
    ppu.color[2] = ppu.color[1];
    ppu.color[1] = ppu.color[0];
    if (render_line() || (sl == 240 && dot == 0)) {
        if ((dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 336)) {
            if (eval_rendering()) bg_fetch();
        } else if (dot >= 337 || dot == 0) {
            if (eval_rendering()) bg_fetch_tail();
        }
        if (dot >= 1 && dot <= 256) {
            if (sl < 240) fp_input_pixel();

        }
        if (sl < 240) output_pixel();
    }

    data_sm_dot_end();
    io_bus_decay();
}

static void fp_input_half(void)
{
    if (render_line() && rendering() && ((ppu.dot >= 1 && ppu.dot <= 257) || (ppu.dot >= 321 && ppu.dot <= 336))) {
#if NESRECOMP_PPU_HLE
        ppu.bg_pixels = (ppu.bg_pixels << 2) | 2u;
        ppu.attr_pixels = (ppu.attr_pixels << 2) | ppu.attr_latch;
#else
        ppu.bg_lo = (uint16_t)(ppu.bg_lo << 1);
        ppu.bg_hi = (uint16_t)(ppu.bg_hi << 1 | 1);
        ppu.attr_lo = (uint16_t)(ppu.attr_lo << 1 | (ppu.attr_latch & 1));
        ppu.attr_hi = (uint16_t)(ppu.attr_hi << 1 | ((ppu.attr_latch >> 1) & 1));
#endif
#ifdef NESRECOMP_CYCLE_HDPACK
        cyc_hdpack_bg_shift();
#endif
    }
    if (ppu.oam2_reset > 0 && --ppu.oam2_reset == 0) {
        ppu.oam2_addr = 0;
        ppu.oam2_full = 0;
    }
    if (ppu.commit) bg_commit();
    if (eval_rendering() || rendering()) ppu.oam_buffer = ppu.oam_buffer_in;
    half_dot_status();
    data_sm_half();
}


#include "ppu_hle_kernel.h"

/* A stable packet has no source mutation or pin/IRQ observation inside its
 * admitted interval. Reuse the qualified raster row decoder and output job;
 * retain the canonical planar history at the packet boundary. */
static bool fp_tile_ready(void)
{
    return ppu.scanline<240 && ppu.dot>=8 && ppu.dot<=248 && !(ppu.dot&7) &&
        rendering() && sm_rest && ppu.render_count==5 &&
        ppu.frame_sprite_line==ppu.scanline &&
        ppu.oam_buffer==ppu.oam_buffer_in &&
        ppu.eval_bg==ppu.show_bg && ppu.eval_spr==ppu.show_spr &&
        ppu.instant_bg==ppu.show_bg && ppu.instant_spr==ppu.show_spr &&
        !(ppu.skipped_dot|ppu.w2001_delay|ppu.w2001_oam_delay|ppu.w2001_emph_delay|
          ppu.w2005_delay|ppu.w2006_delay|ppu.oamc_disabled|ppu.oamc_disabled_now|
          ppu.oamc_pending|ppu.palc_disabled|ppu.palc_v_left|ppu.read2002|
          ppu.vset|ppu.vblank_pending|ppu.oam2_reset|ppu.commit) &&
        ppu.vset_latch1 && !ppu.vset_latch2 && !hw_cart.m.pattern_pending
#ifdef NESRECOMP_CYCLE_HDPACK
        && !hdpack_recording()
#endif
        ;
}
static void fp_tile_oam(unsigned first)
{
    if(first<=64) {
        for(unsigned i=0;i<4;++i) { ppu.oam2[ppu.oam2_addr]=0xff;oam2_increment(); }
        ppu.oam_latch=ppu.oam_buffer=ppu.oam_buffer_in=0xff;
        if(first==57) { ppu.oam2_reset=0;ppu.oam2_addr=ppu.oam2_full=0; }
    } else {
        if(first==65)fp_select_objects();
        ppu.oam_latch=ppu.oam_buffer=ppu.oam_buffer_in;
        if(first==249) { ppu.oam2_reset=0;ppu.oam2_addr=ppu.oam2_full=0; }
    }
    ppu.overflow_late=ppu.overflow;ppu.dot=(uint16_t)(first+7);
}

static void fp_input_tile(unsigned tick,uint64_t clock)
{
    unsigned first = (unsigned)ppu.dot + 1;
    uint16_t old_lo=ppu.bg_lo,old_hi=ppu.bg_hi,old_at_lo=ppu.attr_lo,old_at_hi=ppu.attr_hi;
    uint32_t bg=((uint32_t)ppu_hle_row((uint8_t)(old_lo>>8),(uint8_t)(old_hi>>8))<<16)|
        ppu_hle_row((uint8_t)old_lo,(uint8_t)old_hi);
    uint32_t attr=ppu_hle_row((uint8_t)old_at_lo,(uint8_t)old_at_hi);
    unsigned latch = ppu.attr_latch;
    uint32_t bg_pixels = bg << (2 * ppu.fine_x);
    uint32_t attributes = ((attr << 16) | (latch * 0x5555u)) << (2 * ppu.fine_x);
    uint8_t color[11], background[11];
    color[0] = ppu.color[2]; color[1] = ppu.color[1]; color[2] = ppu.color[0];
    if (cyc_background_enabled) {
        background[0] = cyc_background_pipe[2];
        background[1] = cyc_background_pipe[1];
        background[2] = cyc_background_pipe[0];
    }
    unsigned collision = 8;
    bool old_hit = ppu.s0hit || ppu.s0hit_pending1 || ppu.s0hit_pending2;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned c = ppu.show_bg ? bg_pixels >> 30 : 0;
        unsigned pal = c ? attributes >> 30 : 0;
        bg_pixels <<= 2; attributes <<= 2;
        hw_frame_bg[ppu.scanline * 256 + first + i - 1] = c != 0;
        if (cyc_background_enabled) background[i + 3] = ppu.palette[pal * 4 + c] & 0x3f;
        unsigned sample=ppu.show_spr?ppu.frame_sprites[first+i-1]:0,sc=sample&3;
        if(sc) {
            if((sample&32) && ppu.can_s0hit && c && first+i<256) {
                collision=i;ppu.can_s0hit=0;
            }
            if(!c || !(sample&16)) { c=sc;pal=((sample>>2)&3)|4; }
        }
        color[i + 3] = ppu.palette[pal * 4 + c] & 0x3f;
    }

    /* Each nametable/attribute/pattern source is read once for the tile.
     * Materialize connector/register state at the job boundary. */
    uint16_t v = ppu.v;
    uint16_t nt = (uint16_t)(0x2000 | (v & 0x0fff));
    uint16_t at = (uint16_t)(0x23c0 | (v & 0x0c00) | ((v >> 4) & 0x38) | ((v >> 2) & 7));
    uint8_t tile = ppu.ciram[(nt & 0x3ff) | hw_cart_ciram_a10(nt)];
    uint8_t attribute = ppu.ciram[(at & 0x3ff) | hw_cart_ciram_a10(at)];
    attribute >>= ((v & 2) ? 2 : 0) + ((v & 0x40) ? 4 : 0);
    uint16_t pattern = (uint16_t)((ppu.bg_table ? 0x1000 : 0) | (tile << 4) | ((v >> 12) & 7));
    if (hw_cart.watch_ppu_addr) {
        if (hw_cart.mapper == 4) hw.cycles = clock + tick / 12;
        hw_cart_ppu_addr_watched(nt);
        if (hw_cart.mapper == 4) hw.cycles = clock + (tick + 16) / 12;
        hw_cart_ppu_addr_watched(pattern);
    }
    uint8_t lo = hw_cart.chr[hw_cart_chr_index(pattern)];
    uint8_t hi = hw_cart.chr[hw_cart_chr_index(pattern | 8)];
    ppu.par_chr = pattern | 8;
    ppu.octal_latch = (uint8_t)ppu.par_chr;
    ppu.fetch_data = hi;
    ppu.attribute = attribute & 3;
    ppu.lo_plane = lo;
    ppu.vbus = (uint16_t)((ppu.par_chr & 0xff00) | hi);
    ppu.bg_lo=(uint16_t)((old_lo<<8)|lo);ppu.bg_hi=(uint16_t)((old_hi<<8)|hi);
    ppu.attr_lo=(uint16_t)((old_at_lo<<8)|((latch&1)?255:0));
    ppu.attr_hi=(uint16_t)((old_at_hi<<8)|((latch&2)?255:0));
    ppu.hi_plane=hi;ppu.attr_latch=ppu.attribute;ppu.commit=0;increment_x();
    fp_tile_oam(first);
    unsigned halves=8;
    unsigned hit_half = collision + 2;
    ppu.s0hit = old_hit || hit_half <= halves;
    ppu.s0hit_late = old_hit || hit_half < halves;
    ppu.s0hit_pending1 = collision < 8 && hit_half == halves + 2;
    ppu.s0hit_pending2 = collision < 8 && hit_half == halves + 1;
    ppu.vset_latch2 = 0;
    ppu.blnk_latch = ppu.pal_enable = ppu.copy_v = ppu.ale = ppu.wr = 0;
    ppu.rd = 1;
    unsigned out = (unsigned)ppu.scanline * 256 + first - 4;
    unsigned emphasis = ppu.emphasis;
    if (hw_pal()) emphasis = (emphasis & 4) | ((emphasis & 1) << 1) | ((emphasis & 2) >> 1);
    emphasis <<= 6;
    unsigned mask = ppu.greyscale ? 0x30 : 0x3f;
    for (unsigned i = 0; i < 8; ++i) {
        uint16_t c = (uint16_t)((color[i] & mask) | emphasis);
        hw_frame_index[out + i] = c;
        if (cyc_background_enabled) {
            cyc_background_pipe[3] = background[i];
            cyc_render_background_output(out + i, emphasis >> 6, ppu.greyscale != 0);
        }
        if (hw_zapper.port) {
            ++hw_zapper.dots;
            hw_zapper_pixel((int)(first + i - 4), ppu.scanline, c);
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        ppu.color[i] = color[10 - i];
        if (cyc_background_enabled) cyc_background_pipe[i] = background[10 - i];
    }
    io_bus_decay_span(8);
    ++ppu_native_raster_tiles;
}

/* A packet owns at most eight background input dots. Its fetch, pixel and
 * half-phase deadlines remain ordered within the caller's admitted interval;
 * no CPU/device observation can be crossed by a packet. */
static void fp_input_packet(unsigned first,unsigned events,uint64_t cycle)
{
    for(unsigned i=0;i<events;++i) {
        unsigned tick=first+i*2;hw.cycles=cycle+tick/12;
        if(((hw.align+tick)&3)==0) {
            fp_input_sample();hw_cart_ppu_addr(ppu.vbus);
        } else fp_input_half();
    }
}
void frame_ppu_ticks(unsigned first,unsigned end)
{
    if(!fp.enabled) {
        for(unsigned k=first;k<end;++k) {
            unsigned phase=(hw.align+k)&3;
            if(!phase)reference_ppu_dot();else if(phase==2)reference_ppu_half_dot();
        }
        return;
    }
    HR_COUNT(
        unsigned sample=first+((4u-((hw.align+first)&3u))&3u);
        ++hr_frame_calls;
        if(sample<end)hr_frame_dots+=(end-1u-sample)/4u+1u;
    );
    uint64_t cycle=hw.cycles;
    unsigned tick=first+((hw.align+first)&1);
    while(tick<end) {
        unsigned phase=(hw.align+tick)&3,remaining=(end-tick+1)/2;
        if(phase==0 && remaining>=16 && fp_tile_ready()) {
            fp_input_tile(tick,cycle);tick+=32;continue;
        }
        unsigned events=2*((8-(ppu.dot&7))&7)+(phase==2);
        if(!events)events=16;if(events>remaining)events=remaining;
        fp_input_packet(tick,events,cycle);tick+=events*2;
    }
    hw.cycles=cycle;
}
const char *cyc_ppu_implementation(void) { return "FRAME"; }
void ppu_dot(void) { if(!fp.enabled){reference_ppu_dot();return;}HR_COUNT(++hr_frame_calls;++hr_frame_dots);fp_input_sample();hw_cart_ppu_addr(ppu.vbus); }
void ppu_half_dot(void) { if(!fp.enabled){reference_ppu_half_dot();return;}fp_input_half(); }
bool ppu_clock_cpu_tail(unsigned align)
{
    if(!fp.enabled)return reference_ppu_clock_cpu_tail(align);
    frame_ppu_ticks(1,4);cpu_nmi_input(ppu_nmi_output());
    frame_ppu_ticks(4,7);apu_sample_irq();frame_ppu_ticks(7,12);return true;
}
/* Exact zero/nonzero admission proof without calculating a future duration.
 * MMC3 uses the same invalidated IRQ-deadline cache as the actual executor. */
#if NESRECOMP_HEADROOM_COUNTERS
static bool fp_interval_ready(void)
{
    if(!sm_rest)return false;
    if ((ppu.scanline == 241 || ppu.scanline == hw_prerender_line()) && ppu.dot < 2) return false;
    if (ppu.dot >= 338) return false;
    if (hw_cart.mapper == 4) {
        if (ppu.w2006_delay && ppu.w2006_delay < 4) return false;
        if ((!rendering() || (ppu.scanline >= 240 && ppu.scanline != hw_prerender_line())) &&
            ((ppu.v ^ (hw_cart.m.a12 << 12)) & 0x1000)) return false;
        if (rendering() && (ppu.scanline < 240 || ppu.scanline == hw_prerender_line())) {
            unsigned edge = fp_irq_deadline();
            /* The original first-dot phase is 1..4 master clocks. Its
             * before-edge CPU duration is nonzero exactly four dots away. */
            if (edge < 341 && edge < ppu.dot + 4u) return false;
        }
    }
    return true;
}
#endif

unsigned ppu_native_interval_limit(void)
{
    if(!fp.enabled) return reference_ppu_native_interval_limit();
    if(!sm_rest)return 0;
    if ((ppu.scanline == 241 || ppu.scanline == hw_prerender_line()) && ppu.dot < 2) return 0;
    if(hw_pal()) {
        /* From tick 1, N CPU cycles produce floor((phase+16N)/5) dots.
         * Stay inside this scanline, as the NTSC bound below does. */
        if(ppu.dot>=340) return 0;
        return (5u*(340u-ppu.dot)+4u-hw_region_timing.phase)/16u;
    }
    unsigned limit=(340u-ppu.dot)/3u;
    if(hw_cart.mapper==4) {
        if(ppu.w2006_delay) {
            unsigned before=(ppu.w2006_delay-1)/3u;
            if(limit>before) limit=before;
        }
        if((!rendering() || (ppu.scanline>=240 && ppu.scanline!=hw_prerender_line())) &&
           ((ppu.v^(hw_cart.m.a12<<12))&0x1000)) return 0;
    }
    /* Only a filtered edge that can assert IRQ is a CPU deadline. Other
     * pin transitions run inside the native device interval at their original
     * timestamps; register/PPUDATA/OAM writes invalidate this derived proof. */
    if(hw_cart.mapper==4 && rendering() && (ppu.scanline<240 || ppu.scanline==hw_prerender_line())) {
        unsigned edge=fp_irq_deadline();
        if(edge<341) {
            if(edge<=ppu.dot) return 0;
            unsigned first=1u+((4u-((hw.align+1u)&3u))&3u);
            unsigned before=(first+4u*(edge-ppu.dot-1u)-1u)/12u;
            if(limit>before) limit=before;
        }
    }
    return limit;
}

/* PAL interval: dots at phase 0 and half-dots at phase 2 of every five
 * master ticks, from tick 1 of the entry cycle. The event order is the same
 * sample/half alternation as NTSC, so tile jobs and packets are reused. */
static void frame_ppu_pal_interval(unsigned cycles)
{
    unsigned phase=hw_region_timing.phase,ticks=cycles*16u;
    unsigned dot_tick=5u-phase,half_tick=phase<2u?2u-phase:7u-phase;
    uint64_t cycle=hw.cycles;
    while(dot_tick<=ticks || half_tick<=ticks) {
        if(half_tick<dot_tick) {
            hw.cycles=cycle+half_tick/16u;fp_input_half();half_tick+=5u;continue;
        }
        if(half_tick==dot_tick+2u && half_tick+35u<=ticks && fp_tile_ready()) {
            hw.cycles=cycle+dot_tick/16u;fp_input_tile(dot_tick,cycle);
            dot_tick+=40u;half_tick+=40u;continue;
        }
        hw.cycles=cycle+dot_tick/16u;
        fp_input_sample();hw_cart_ppu_addr(ppu.vbus);dot_tick+=5u;
    }
    hw.cycles=cycle;hw.tick=1;
}
void ppu_native_interval_run(unsigned cycles,unsigned align)
{
    if(!fp.enabled){reference_ppu_native_interval_run(cycles,align);return;}
    if(hw_pal()){frame_ppu_pal_interval(cycles);return;}
    frame_ppu_ticks(1,1+cycles*12);
}
void frame_ppu_mapper_changed(void) { fp.irq_valid=fp.sprite_banks_valid=false; }
uint8_t ppu_read(uint16_t address)
{
    uint8_t value=reference_ppu_read(address);fp.irq_valid=fp.sprite_banks_valid=false;return value;
}
void ppu_write(uint16_t address,uint8_t value)
{
    reference_ppu_write(address,value);fp.irq_valid=fp.sprite_banks_valid=false;
}
void ppu_state_reloaded(void)
{
    reference_ppu_state_reloaded();fp.enabled=fp_supported();fp.irq_valid=fp.sprite_banks_valid=false;
}
void ppu_power_on(void)
{
    reference_ppu_power_on();memset(&fp,0,sizeof(fp));fp.enabled=fp_supported();ppu.frame_sprite_line=0xffff;
}
