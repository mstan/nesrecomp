/* Experimental native picture service.
 * Inputs are the register/memory writes observed between timestamp advances.
 * Rows are prepared as pictures, not discovered by clocking fetch/shifter/OAM
 * machinery. Native CPU intervals and the coupled I/O clock share this cursor.
 * The build selects this provider; unsupported boards/regions retain the
 * reference provider in the same build, without a player-facing switch. */
static struct {
    uint16_t row[256], background[256];
    uint8_t opaque[256];
    int line, valid, hit_x, half;
    unsigned sprite_banks, irq_dot;
    bool sprite_banks_valid, irq_valid;
    bool enabled;
} fp;

static bool fp_supported(void)
{
    if(hw_pal() || !hw_cart.prg) return false;
    return hw_cart.mapper==0 || hw_cart.mapper==1 || hw_cart.mapper==4 || hw_cart.mapper==66;
}

static uint8_t fp_read(uint16_t address)
{
    address &= 0x3fff;
    if (address < 0x2000) return hw_cart_chr_read(address);
    if (address >= 0x3f00) return ppu.palette[address & ((address & 3) ? 31 : 15)];
    uint8_t value;
    if (hw_cart_nt_read(address, true, &value)) return value;
    return ppu.ciram[(address & 0x3ff) | hw_cart_ciram_a10(address)];
}

static void fp_store(uint16_t address, uint8_t value)
{
    address &= 0x3fff;
    if (address >= 0x3f00) ppu.palette[address & ((address & 3) ? 31 : 15)] = value & 0x3f;
    else if (address >= 0x2000) {
        if (!hw_cart_nt_write(address, value))
            ppu.ciram[(address & 0x3ff) | hw_cart_ciram_a10(address)] = value;
    } else if (hw_cart.chr_write[address >> 10]) hw_cart.chr[hw_cart_chr_index(address)] = value;
}

/* Retain pattern and attribute inputs when blanking interrupts a row.
 * The picture operation derives its short queued prefix once; it never
 * executes the dot/fetch/shifter machine to keep that prefix alive. */
static void fp_background_input(unsigned v,unsigned x,unsigned *color,unsigned *palette,unsigned slot)
{
    unsigned world=((v&31)<<3)+x, tx=(world>>3)&31;
    unsigned nt=(v&0xc00)^((world&256)?0x400:0);
    unsigned ty=(v>>5)&31, fy=(v>>12)&7;
    unsigned tile=fp_read((uint16_t)(0x2000|nt|(ty<<5)|tx));
    unsigned address=(ppu.bg_table<<12)|(tile<<4)|fy,bit=7-(world&7);
    unsigned lo=fp_read((uint16_t)address),hi=fp_read((uint16_t)(address+8));
    ppu.par_chr=(uint16_t)(address|8);ppu.lo_plane=(uint8_t)lo;ppu.hi_plane=(uint8_t)hi;
    *color=((lo>>bit)&1)|(((hi>>bit)&1)<<1);
#ifdef NESRECOMP_CYCLE_HDPACK
    if(hdpack_recording()) {
        cyc_hdpack_native_tile((uint16_t)address,(uint8_t)lo,(uint8_t)hi,false);
        cyc_hdpack_native_hold(slot,world&7);
    }
#else
    (void)slot;
#endif
    unsigned attr=fp_read((uint16_t)(0x23c0|nt|((ty>>2)<<3)|(tx>>2)));
    *palette=(attr>>(((ty&2)<<1)|(tx&2)))&3;
}
static void fp_complete_tile(unsigned v,bool retained,bool attribute)
{
    /* Restart can miss the nametable-read phase. Its selected pattern tile
     * then survives blanking, while the actual plane reads use today's CHR
     * mapping. Re-reading the nametable would choose a different tile. */
    unsigned tile=retained?(ppu.par_chr>>4)&255:fp_read((uint16_t)(0x2000|(v&0xfff)));
    unsigned address=(ppu.bg_table<<12)|(tile<<4)|((v>>12)&7);
    unsigned lo=fp_read((uint16_t)address),hi=fp_read((uint16_t)(address+8)),bits=0;
    for(unsigned x=0;x<8;++x)bits=(bits<<2)|((lo>>(7-x))&1)|(((hi>>(7-x))&1)<<1);
    ppu.frame_bg_hold=(ppu.frame_bg_hold&0xffff0000u)|bits;
    ppu.par_chr=(uint16_t)(address|8);ppu.lo_plane=(uint8_t)lo;ppu.hi_plane=(uint8_t)hi;
    if(attribute) {
        unsigned attr=fp_read((uint16_t)(0x23c0|(v&0xc00)|((v>>7)&0x38)|((v>>2)&7)));
        ppu.attr_latch=(uint8_t)((attr>>(((v>>4)&4)|(v&2)))&3);
    }
#ifdef NESRECOMP_CYCLE_HDPACK
    if(hdpack_recording()) {
        cyc_hdpack_native_tile((uint16_t)address,(uint8_t)lo,(uint8_t)hi,false);
        for(unsigned x=0;x<8;++x)cyc_hdpack_native_hold(8+x,x);
    }
#endif
}
static void fp_shift_prefix(unsigned count)
{
    if(!count)return;
    unsigned fill=(1u<<(count*2))-1u;
    ppu.frame_bg_hold=(ppu.frame_bg_hold<<(count*2))|(0xaaaaaaaau&fill);
    ppu.frame_at_hold=(ppu.frame_at_hold<<(count*2))|((0x55555555u*ppu.attr_latch)&fill);
#ifdef NESRECOMP_CYCLE_HDPACK
    if(hdpack_recording())cyc_hdpack_native_shift(count);
#endif
}
static void fp_oam_mask(uint8_t value,unsigned delay)
{
    if(rendering() && !(value&0x18) && ppu.scanline<240 && ppu.dot<64) {
        /* Disabling in the secondary-OAM clear publishes a row copy at the
         * next eligible observation, then retains the final clear position
         * for restart. Compute the operation once from the write timestamp. */
        unsigned first=(ppu.dot&1)?3:2;
        ppu.oam2_addr=(uint8_t)(((ppu.dot+delay)/2)&31);
        ppu.oamc_index=ppu.oam2_addr;ppu.oamc_pending=1;ppu.oamc_reenabled=0;
        if(first<=delay) {
            ppu.oamc_index=(uint8_t)(((ppu.dot+1)/2)&31);
            ppu.w2001_oam_delay=(uint8_t)first;
        }
    } else if(!rendering() && (value&0x18) && ppu.oamc_pending) {
        ppu.oamc_reenabled=hw.align==1 || hw.align==2;
        ppu.w2001_delay=(uint8_t)(delay+1);
    }
}
static void fp_mask_prefix(uint8_t value)
{
    static const unsigned delay[4]={2,2,3,2};
    unsigned settle=ppu.dot+delay[hw.align];
    fp_oam_mask(value,delay[hw.align]);
    if(rendering() && !(value&0x18) && ppu.scanline<240 && settle<=256) {
        unsigned base=settle-1,valid=16-(base&7),color=0,palette=0;
        uint32_t bits=0,attributes=0;
#ifdef NESRECOMP_CYCLE_HDPACK
        if(hdpack_recording())cyc_hdpack_native_shift(16);
#endif
        for(unsigned i=0;i<16;++i) {
            if(i<valid) fp_background_input(ppu.frame_line_v,base+i,&color,&palette,i);
            else color=2; /* the background input's pull-up level */
            bits=(bits<<2)|color;attributes=(attributes<<2)|palette;
        }
        ppu.frame_bg_hold=bits;ppu.frame_at_hold=attributes;ppu.frame_hold_status=1;
        ppu.attr_latch=(uint8_t)palette;
        /* A high-plane read already issued before blanking still completes
         * and replaces the queued tile, even though further shifts stop. */
        if(!(settle&7))fp_complete_tile(ppu.v,false,true);
    } else if(!rendering() && (value&0x18)) {
        /* A late HBlank resume cannot fetch a complete new prefix. Advance
         * the retained input only through the remaining active BG positions.
         * Palette lookup stays at picture output time. */
        if((ppu.frame_hold_status&1) && ppu.scanline<239 && settle>=329 && settle<=343) {
            /* A write on the final dot can settle on the next row's first
             * two dots. Those dots consume the same retained prefix. */
            fp_shift_prefix(settle<=336?337-settle:0);
            bool fetched=settle<=332;
            if(fetched)fp_complete_tile(ppu.v,settle>=330,settle<332);
            ppu.frame_prefix_line=(uint16_t)(ppu.scanline+1);ppu.frame_hold_status=(uint8_t)(2|(fetched?4:0));
        } else ppu.frame_hold_status=0;
    }
}

static unsigned fp_line_input_v(void)
{
    unsigned v=ppu.frame_line_v;
    if((ppu.frame_hold_status&2) && ppu.scanline==ppu.frame_prefix_line) {
        unsigned coarse=((v&31)|((v&0x400)>>5));coarse=(coarse+((ppu.frame_hold_status&4)?63:62))&63;
        v=(v&~0x41fu)|(coarse&31)|((coarse&32)<<5);
    }
    return v;
}
static void fp_capture_line(unsigned line)
{
    capture_line((int)line);
    if(rendering()) {
        /* CycLine is a renderer input contract: v includes the two tiles
         * already prefetched. The native row itself starts before those
         * tiles; publish the same observation without fetching them. */
        unsigned v=fp_line_input_v();
        unsigned coarse=((v&31)|((v&0x400)>>5));coarse=(coarse+2)&63;
        hw_frame_lines[line].v=(uint16_t)((v&~0x41fu)|(coarse&31)|((coarse&32)<<5));
    }
}
static void fp_prepare(void)
{
#ifdef NESRECOMP_CYCLE_HDPACK
    bool record_hd=hdpack_recording();
    if(record_hd)cyc_hdpack_native_begin();
#endif
    unsigned v = fp_line_input_v();
    bool prefix=(ppu.frame_hold_status&2) && ppu.scanline==ppu.frame_prefix_line;
    unsigned ty = (v >> 5) & 31, fy = (v >> 12) & 7;
    for (unsigned x=0;x<256;) {
        unsigned world=((v&31)<<3)+ppu.fine_x+x;
        unsigned tx=(world>>3)&31;
        unsigned nt=(v&0xc00)^((world&256)?0x400:0);
        unsigned skip=world&7, count=8-skip;
        if(count>256-x) count=256-x;
        unsigned lo=0,hi=0,pal=0;
        if(ppu.show_bg) {
            unsigned tile=fp_read((uint16_t)(0x2000|nt|(ty<<5)|tx));
            unsigned address=(ppu.bg_table<<12)|(tile<<4)|fy;
            lo=fp_read((uint16_t)address);hi=fp_read((uint16_t)(address+8));
#ifdef NESRECOMP_CYCLE_HDPACK
            if(record_hd)cyc_hdpack_native_tile((uint16_t)address,(uint8_t)lo,(uint8_t)hi,false);
#endif
            unsigned attr=fp_read((uint16_t)(0x23c0|nt|((ty>>2)<<3)|(tx>>2)));
            pal=(attr>>(((ty&2)<<1)|(tx&2)))&3;
        }
        for(unsigned j=0;j<count;++j) {
            unsigned bit=7-skip-j, px=x+j;
            unsigned color=((lo>>bit)&1)|(((hi>>bit)&1)<<1);
            if(!ppu.show_bg || (px<8&&!ppu.show_bg8)) color=0;
            unsigned address=color ? (pal<<2)|color : 0;
            if(!rendering() && (ppu.v&0x3f00)==0x3f00)
                address=ppu.v&((ppu.v&3)?31:15);
            fp.row[px]=fp.background[px]=ppu.palette[address]&0x3f;
            fp.opaque[px]=color!=0;
#ifdef NESRECOMP_CYCLE_HDPACK
            if(record_hd)cyc_hdpack_native_background(px,skip+j,pal,color,(uint8_t)fp.background[px]);
#endif
        }
        x+=count;
    }
    if(prefix) for(unsigned x=0;x<16u-ppu.fine_x;++x) {
        unsigned shift=30u-2u*(x+ppu.fine_x);
        unsigned color=(ppu.frame_bg_hold>>shift)&3u,palette=(ppu.frame_at_hold>>shift)&3u;
        if(!ppu.show_bg || (x<8&&!ppu.show_bg8)) color=0;
        unsigned address=color ? (palette<<2)|color:0;
        fp.row[x]=fp.background[x]=ppu.palette[address]&0x3f;fp.opaque[x]=color!=0;
#ifdef NESRECOMP_CYCLE_HDPACK
        if(record_hd)cyc_hdpack_native_prefix(x,x+ppu.fine_x,palette,color);
#endif
    }
    unsigned slots[8], count = 0, height = ppu.sprite16 ? 16 : 8;
    for (unsigned i = 0; i < 64; ++i) {
        int y = (int)ppu.scanline - (int)ppu.oam[i * 4] - 1;
        if (y >= 0 && y < (int)height) {
            if (count < 8) slots[count] = i;
            ++count;
        }
    }
    fp.hit_x = -1;
    if (count > 8 && rendering()) ppu.overflow = ppu.overflow_late = 1;
    if (count > 8) count = 8;
    for (int s = (int)count - 1; s >= 0; --s) {
        unsigned i = slots[s], attr = ppu.oam[i * 4 + 2];
        unsigned row = ppu.scanline - ppu.oam[i * 4] - 1;
        if (attr & 0x80) row = height - 1 - row;
        unsigned tile = ppu.oam[i * 4 + 1], address;
        if (height == 16) address = ((tile & 1) << 12) | ((tile & 0xfe) << 4) |
                                   (row & 7) | ((row & 8) << 1);
        else address = (ppu.sprite_table << 12) | (tile << 4) | row;
        unsigned lo = fp_read((uint16_t)address), hi = fp_read((uint16_t)(address + 8));
#ifdef NESRECOMP_CYCLE_HDPACK
        if(record_hd)cyc_hdpack_native_tile((uint16_t)address,(uint8_t)lo,(uint8_t)hi,true);
#endif
        for (unsigned j = 0; j < 8; ++j) {
            unsigned x = ppu.oam[i * 4 + 3] + j;
            if (x >= 256 || !ppu.show_spr || (x < 8 && !ppu.show_spr8)) continue;
            unsigned bit = attr & 0x40 ? j : 7 - j;
            unsigned color = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
            if (!color) continue;
            if (!i && fp.opaque[x] && x < 255 && (fp.hit_x < 0 || x < (unsigned)fp.hit_x))
                fp.hit_x = (int)x;
            if (!(attr & 0x20) || !fp.opaque[x]) {
                fp.row[x] = ppu.palette[16 | ((attr & 3) << 2) | color] & 0x3f;
#ifdef NESRECOMP_CYCLE_HDPACK
                if(record_hd)cyc_hdpack_native_sprite(x,j,attr,color);
#endif
            }
        }
    }
    fp.valid = 1;
}

static void fp_output(unsigned first, unsigned end)
{
    if (ppu.scanline >= 240 || first >= end) return;
    if (!fp.valid) fp_prepare();
    for (unsigned x = first; x < end; ++x) {
        unsigned index = ppu.scanline * 256 + x;
        unsigned color = fp.row[x];
        if (ppu.greyscale) color &= 0x30;
        hw_frame_index[index] = (uint16_t)(color | (ppu.emphasis << 6));
        hw_frame_bg[index] = fp.opaque[x];
        if (hw_zapper.port) {
            /* The optical observation belongs to this pixel's timestamp,
             * even when one native operation publishes a larger row range. */
            unsigned offset=x+4u-ppu.dot;
            hw_zapper.dots+=offset;
            hw_zapper_pixel((int)x,(int)ppu.scanline,hw_frame_index[index]);
            hw_zapper.dots-=offset;
        }
        if (cyc_background_enabled) {
            cyc_background_pipe[3] = (uint8_t)fp.background[x];
            cyc_render_background_output(index, ppu.emphasis, ppu.greyscale != 0);
        }
    }
#ifdef NESRECOMP_CYCLE_HDPACK
    if(hdpack_recording())cyc_hdpack_native_output(first,end,ppu.scanline);
#endif
}

/* Mapper address events are independent of picture generation. Only address
 * phase transitions are scheduled; no pattern fetch, shifter or OAM machine
 * runs to discover them. Timestamped filtering keeps native CPU intervals
 * from moving a mapper interrupt to the end of a batch. */
extern void hw_cart_picture_addr(uint16_t address, uint64_t cycle);
static unsigned fp_dot_tick;
static uint64_t fp_cycle;
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
    if(!rendering() || (ppu.scanline>=240 && ppu.scanline!=261)) return ppu.v;
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
       (ppu.scanline>=240 && ppu.scanline!=261)) return 341;
    /* Identical address phases carry no mapper observation. A constant-low
     * background is one interval, not sixty-four repeated notifications. */
    for(unsigned next=fp_phase_after(dot);next<341;next=fp_phase_after(next))
        if(((fp_address_value(next)>>12)&1)!=a12) return next;
    return 341;
}
static void fp_address(unsigned dot,unsigned elapsed)
{
    if(!hw_cart.watch_ppu_addr) return;
    unsigned address=fp_address_value(dot);ppu.vbus=(uint16_t)address;
    hw_cart_picture_addr((uint16_t)address,fp_cycle+(fp_dot_tick+4u*elapsed)/12u);
}
void frame_ppu_mapper_changed(void) { fp.irq_valid=false;fp.valid=0; }
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

static unsigned fp_x(unsigned dot) { return dot < 4 ? 0 : dot > 259 ? 256 : dot - 3; }
static void fp_status(void)
{
    if (ppu.scanline < 240 && !fp.valid) fp_prepare();
    if (fp.hit_x >= 0 && !ppu.s0hit_late && ppu.scanline < 240) {
        unsigned half = 2u * ppu.dot + fp.half;
        if (half >= 2u * ((unsigned)fp.hit_x + 2u) + 1u) ppu.s0hit = 1;
        if (half >= 2u * ((unsigned)fp.hit_x + 3u) + 1u) ppu.s0hit_late = 1;
    }
}

static void fp_scroll_before(void)
{
    if(ppu.w2005_delay==1) {
        unsigned value=ppu.w2005_value;
        if(!ppu.addr_latch) {
            ppu.t=(uint16_t)((ppu.t&0x7fe0)|(value>>3));
            /* Fine X feeds the picture three dots before its output. */
            ppu.frame_fine_x_value=(uint8_t)(value&7);
            ppu.frame_fine_x_delay=3;
        } else ppu.t=(uint16_t)((ppu.t&0xc1f)|((value&0xf8)<<2)|((value&7)<<12));
        ppu.addr_latch^=1;ppu.w2005_delay=0;
    }
}
static void fp_scroll_after(unsigned dots)
{
    if(ppu.w2001_oam_delay) {
        ppu.w2001_oam_delay=(uint8_t)(ppu.w2001_oam_delay-dots);
        if(!ppu.w2001_oam_delay) {
            corrupt_oam();ppu.oamc_index=ppu.oam2_addr;
        }
    }
    if(ppu.w2001_delay)ppu.w2001_delay=(uint8_t)(ppu.w2001_delay-dots);
    if(ppu.oamc_pending && !ppu.w2001_delay && !ppu.w2001_oam_delay && rendering() &&
       (ppu.scanline<241 || ppu.scanline==261)) {
        if(!ppu.oamc_reenabled)corrupt_oam();
        ppu.oamc_pending=ppu.oamc_reenabled=0;
    }
    if(ppu.w2005_delay) ppu.w2005_delay=(uint8_t)(ppu.w2005_delay-dots);
    ppu.copy_v=0;
    if(ppu.w2006_delay) {
        ppu.w2006_delay=(uint8_t)(ppu.w2006_delay-dots);
        if(!ppu.w2006_delay) { ppu.copy_v=1;ppu.v=ppu.t;fp.valid=0; }
    }
    if(ppu.frame_fine_x_delay) {
        if(dots>=ppu.frame_fine_x_delay) {
            ppu.frame_fine_x_delay=0;ppu.fine_x=ppu.frame_fine_x_value;fp.valid=0;
        } else ppu.frame_fine_x_delay=(uint8_t)(ppu.frame_fine_x_delay-dots);
    }
}

static void fp_dots(unsigned count, bool half)
{
    io_bus_decay_span(count);
    unsigned elapsed=0;
    while (count) {
        fp_scroll_before();
        if (ppu.dot == 340) {
            --count;
            if(hw_zapper.port) ++hw_zapper.dots;
            ppu.dot = 0;
            if(ppu.scanline==240 && hw_extra_timing.line<hw_extra_timing.extra_scanlines) {
                ++hw_extra_timing.line;hw_extra_timing.active=1;
            } else {
                hw_extra_timing.line=0;hw_extra_timing.active=0;
                ppu.scanline = (uint16_t)((ppu.scanline + 1) % 262);
            }
            if (!ppu.scanline && ppu.odd_frame && rendering()) ppu.dot = 1;
            fp.line = ppu.scanline; ppu.frame_line_v = ppu.v; fp.valid = 0; fp.hit_x = -1;
            fp.irq_valid=fp.sprite_banks_valid=false;
            if((ppu.frame_hold_status&2) && ppu.scanline!=ppu.frame_prefix_line) ppu.frame_hold_status=0;
            if (!hw_extra_timing.active && ppu.scanline == hw_observe_line) hw_observe_hit = hw_frame_done = true;
            if (ppu.dot == 1 && ppu.scanline < 240) fp_capture_line(ppu.scanline);
            /* A status read clears on the first full dot, including dot0.
             * Keeping it through the wrap would wrongly suppress VBlank at
             * dot1 when the read began at the end of the preceding line. */
            if (ppu.read2002) { ppu.vblank = 0; ppu.read2002 = 0; }
            fp_scroll_after(1);
            fp_address(ppu.dot,elapsed++);
            continue;
        }
        unsigned old = ppu.dot, n = 340 - old;
        if (n > count) n = count;
        if(ppu.w2005_delay>1 && n>=ppu.w2005_delay) n=ppu.w2005_delay-1;
        if(ppu.w2001_delay && n>ppu.w2001_delay)n=ppu.w2001_delay;
        if(ppu.w2001_oam_delay && n>ppu.w2001_oam_delay)n=ppu.w2001_oam_delay;
        if(ppu.w2006_delay && n>ppu.w2006_delay) n=ppu.w2006_delay;
        if(ppu.frame_fine_x_delay && n>ppu.frame_fine_x_delay) n=ppu.frame_fine_x_delay;
        unsigned event=fp_address_event(old,hw_cart.m.a12);
        if (old+n>event) n=event-old;
        /* The vertical increment observes a preceding address-copy event. */
        if(old<257 && old+n>257) n=257-old;
        if(old<258 && old+n>258) n=258-old;
        unsigned next = old + n;
        if (old < 1 && next >= 1) {
            ppu.frame_line_v = ppu.v; fp.valid = 0; fp.hit_x = -1;
            if (ppu.scanline < 240) fp_capture_line(ppu.scanline);
            if (ppu.scanline == 241) {
                if (!ppu.read2002) ppu.vblank = 1;
                hw_frame_done = hw_frame_end_hit = true;
            } else if (ppu.scanline == 261) {
                ppu.vblank = ppu.s0hit = ppu.s0hit_late = ppu.overflow = ppu.overflow_late = 0;
                ppu.can_s0hit = 1;
            }
        }
        fp_output(fp_x(old), fp_x(next));
        if(hw_zapper.port) hw_zapper.dots+=n;
        if (rendering() && (ppu.scanline < 240 || ppu.scanline == 261)) {
            if (old < 257 && next >= 257) { if(n>1) ppu.copy_v=0;increment_y(); }
            if (old < 258 && next >= 258) ppu.v = (uint16_t)((ppu.v & 0x7be0) | (ppu.t & 0x41f));
            if (ppu.scanline == 261 && old < 305 && next >= 281)
                ppu.v = (uint16_t)((ppu.v & 0x41f) | (ppu.t & 0x7be0));
        }
        if (ppu.scanline == 260 && old < 340 && next == 340) ppu.odd_frame ^= 1;
        ppu.dot = (uint16_t)next;
        fp_scroll_after(n);
        elapsed += n;
        if (next==event || !rendering() || (ppu.scanline>=240 && ppu.scanline!=261))
            fp_address(next,elapsed-1);
        count -= n;
        if (ppu.read2002) { ppu.vblank = 0; ppu.read2002 = 0; }
    }
    fp.half = half;
    fp_status();
}

void frame_ppu_ticks(unsigned first, unsigned end)
{
    if(!fp.enabled) {
        for(unsigned k=first;k<end;++k) {
            unsigned phase=(hw.align+k)&3;
            if(!phase) reference_ppu_dot();else if(phase==2) reference_ppu_half_dot();
        }
        return;
    }
    unsigned n = end - first;
    unsigned q = (hw.align + first) & 3;
    unsigned d = (4 - q) & 3;
    unsigned dots = d < n ? 1 + (n - 1 - d) / 4 : 0;
    unsigned last = (hw.align + end - 1) & 3;
    if ((last & 1) != 0) last = (last + 3) & 3;
    fp_cycle=hw.cycles; fp_dot_tick=first+d;
    fp_dots(dots, last == 2);
}
const char *cyc_ppu_implementation(void) { return "FRAME"; }
void ppu_dot(void) { if(!fp.enabled){reference_ppu_dot();return;} fp_cycle=hw.cycles;fp_dot_tick=hw.tick ? hw.tick-1:0;fp_dots(1, false); }
void ppu_half_dot(void) { if(!fp.enabled){reference_ppu_half_dot();return;}fp.half = 1; fp_status(); }
bool ppu_clock_cpu_tail(unsigned align)
{
    if(!fp.enabled) return reference_ppu_clock_cpu_tail(align);
    (void)align;
    frame_ppu_ticks(1, 4); cpu_nmi_input(ppu_nmi_output());
    frame_ppu_ticks(4, 7); apu_sample_irq();
    frame_ppu_ticks(7, 12); return true;
}
unsigned ppu_native_interval_limit(void)
{
    if(!fp.enabled) return reference_ppu_native_interval_limit();
    if ((ppu.scanline == 241 || ppu.scanline == 261) && ppu.dot < 2) return 0;
    unsigned limit=(340u-ppu.dot)/3u;
    if(hw_cart.mapper==4) {
        if(ppu.w2006_delay) {
            unsigned before=(ppu.w2006_delay-1)/3u;
            if(limit>before) limit=before;
        }
        if((!rendering() || (ppu.scanline>=240 && ppu.scanline!=261)) &&
           ((ppu.v^(hw_cart.m.a12<<12))&0x1000)) return 0;
    }
    /* Only a filtered edge that can assert IRQ is a CPU deadline. Other
     * pin transitions run inside the native device interval at their original
     * timestamps; register/PPUDATA/OAM writes invalidate this derived proof. */
    if(hw_cart.mapper==4 && rendering() && (ppu.scanline<240 || ppu.scanline==261)) {
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
void ppu_native_interval_run(unsigned cycles, unsigned align)
{ if(!fp.enabled){reference_ppu_native_interval_run(cycles,align);return;} frame_ppu_ticks(1, 1 + cycles * 12); }

static void fp_increment_data(void)
{
    /* During an active render line the CPU's data port clocks the scroll
     * vertical axis; horizontal progress belongs to the picture row.
     * The blanking increment selection does not replace that clock. */
    if(rendering() && (ppu.scanline<240 || ppu.scanline==261)) {
        increment_y();
    } else ppu.v=(uint16_t)((ppu.v+(ppu.inc32?32:1))&0x7fff);
}

uint8_t ppu_read(uint16_t address)
{
    if(!fp.enabled) return reference_ppu_read(address);
    uint8_t value = ppu.io_bus;
    switch (address & 7) {
    case 2:
        value = ppu.vblank ? 0x80 : 0; ppu.read2002 = 1;
        hw_clock_run_ticks(7);
        value |= (ppu.s0hit_late ? 0x40 : 0) | (ppu.overflow_late ? 0x20 : 0) | (ppu.io_bus & 31);
        ppu.addr_latch = 0; break;
    case 4: hw_clock_run_ticks(7); value = ppu.oam[ppu.oam_addr]; break;
    case 7: {
        unsigned v = ppu.v & 0x3fff;
        value = v >= 0x3f00 ? (uint8_t)((fp_read((uint16_t)v) & (ppu.greyscale ? 0x30 : 0x3f)) | (ppu.io_bus & 0xc0)) : ppu.read_buffer;
        ppu.read_buffer = fp_read((uint16_t)(v >= 0x3f00 ? v - 0x1000 : v));
        hw_clock_run_ticks(7); fp_increment_data(); break;
    }
    default: return value;
    }
    fp.irq_valid=false;
    ppu.io_bus = value; io_bus_refresh((address & 7) == 2 ? 0xe0 : 0xff); return value;
}
void ppu_write(uint16_t address, uint8_t value)
{
    if(!fp.enabled){reference_ppu_write(address,value);return;}
    ppu.io_bus = value; io_bus_refresh(0xff);
    switch (address & 7) {
    case 0:
        hw_clock_run_ticks(2);
        ppu.nmi_enable = value >> 7; ppu.inc32 = (value >> 2) & 1;
        ppu.sprite16 = (value >> 5) & 1; ppu.sprite_table = (value >> 3) & 1; ppu.bg_table = (value >> 4) & 1;
        ppu.t = (uint16_t)((ppu.t & 0x73ff) | ((value & 3) << 10)); break;
    case 1:
        fp_mask_prefix(value);
        ppu.greyscale = value & 1; ppu.emphasis = value >> 5;
        ppu.show_bg8 = (value >> 1) & 1; ppu.show_spr8 = (value >> 2) & 1;
        ppu.show_bg = (value >> 3) & 1; ppu.show_spr = (value >> 4) & 1; break;
    case 3: ppu.oam_addr = value; break;
    case 4:
        if (!rendering() || ppu.scanline >= 240) {
            if ((ppu.oam_addr & 3) == 2) value &= 0xe3;
            ppu.oam[ppu.oam_addr++] = value;
        } else ppu.oam_addr = (uint8_t)((ppu.oam_addr + 4) & 0xfc);
        break;
    case 5:
        ppu.w2005_delay=hw.align==2 ? 2:1;ppu.w2005_value=value;break;
    case 6:
        if (!ppu.addr_latch) ppu.t = (uint16_t)((ppu.t & 0xff) | ((value & 0x3f) << 8));
        else { ppu.t = (uint16_t)((ppu.t & 0x7f00) | value);
            if(content_write_hook && !hw_isolated) content_write_hook(6,ppu.t&0x3fff,value,ppu.inc32?32:1);
            ppu.w2006_old_v=ppu.v;ppu.w2006_value=ppu.t;ppu.w2006_delay=hw.align==2 ? 5:4; }
        ppu.addr_latch ^= 1; break;
    case 7:
        if (content_write_hook && !hw_isolated) content_write_hook(7, ppu.v & 0x3fff, value, ppu.inc32 ? 32 : 1);
        hw_clock_run_ticks(7); fp_store(ppu.v, value);
        fp_increment_data(); break;
    }
    fp.valid = 0;fp.irq_valid=fp.sprite_banks_valid=false;
}
void ppu_state_reloaded(void) {
    fp.enabled=fp_supported();
    if(!fp.enabled) reference_ppu_state_reloaded();
    fp.valid = 0; fp.hit_x = -1;fp.irq_valid=fp.sprite_banks_valid=false;
    fp.half = ((hw.align + hw.tick + 3u) & 3u) >= 2u;
}
void ppu_power_on(void) { reference_ppu_power_on(); memset(&fp, 0, sizeof(fp)); fp.line = -1; fp.hit_x = -1;fp.enabled=fp_supported(); }
