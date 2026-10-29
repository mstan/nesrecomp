/* Whole stable raster jobs against the independent coupled-clock route. */
#include "cpu6502.h"
#include "cyc_core.h"
#include "cyc_mod.h"
#include "cyc_render.h"
#include "cyc_state.h"
#include "hw_internal.h"
#include "mod_savestate.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(bool ok, const char *why)
{ if (!ok) { fprintf(stderr, "FAIL: %s\n", why); exit(1); } }
static void bus_access(uint16_t a, uint8_t value, bool write)
{
    hw_cycle_start(a, write ? HW_WRITE : HW_READ);
    if (write) hw_write(a, value); else (void)hw_read(a);
    hw_cycle_finish(false);
}
static void serial(uint16_t a, unsigned value)
{
    for (unsigned i=0;i<5;++i) {
        bus_access(a,(uint8_t)(value>>i&1),true); bus_access(0,0,false);
    }
}
static uint64_t mix(uint64_t h, const void *data, size_t size)
{
    const uint8_t *p=data;
    for (size_t i=0;i<size;++i) h=(h^p[i])*UINT64_C(1099511628211);
    return h;
}
static uint64_t background_hash(void)
{
    static uint8_t buffer[NES_MOD_SAVESTATE_BLOB_CAP];
    for (int i=0;i<nes_mod_savestate_hook_count();++i) {
        if (strcmp(nes_mod_savestate_hook_id_at(i),"cyc.background")) continue;
        int n=nes_mod_savestate_hook_get_at(i)(buffer,sizeof(buffer));
        check(n>=0,"background record");
        return mix(UINT64_C(1469598103934665603),buffer,(size_t)n);
    }
    return 0;
}
static void replay_route(bool reference)
{
    for (unsigned i=0;i<257;++i) {
        bus_access(0,0,false);
        unsigned n=hw_native_interval_limit();
        if (n>12) n=12;
        if (reference) for (unsigned j=0;j<n;++j) bus_access(0,0,false);
        else if (n) {
            hw.cpu_addr=0;hw.cpu_reading=hw.data_driven=1;
            hw.data_bus=hw.internal_bus=hw.ram[0];hw_native_interval_run(n);
        }
    }
}
int main(int argc, char **argv)
{
    bool reference=argc>1 && !strcmp(argv[1],"--reference");
    static uint8_t image[16+65536+16384];
    static const unsigned boards[]={0,1,4,66};
    memcpy(image,"NES\032",4);
    for (size_t i=16;i<sizeof(image);++i) image[i]=(uint8_t)(i*37+(i>>3));
    for (unsigned ram=0;ram<2;++ram) for (unsigned board=0;board<4;++board) {
        unsigned mapper=boards[board];
        image[4]=mapper>=4?4:2; image[5]=ram?0:(mapper>=4?2:1);
        image[6]=(uint8_t)(mapper<<4); image[7]=(uint8_t)(mapper&0xf0);
        for (unsigned align=0;align<4;++align) {
            check(cyc_load_ines(image,16+image[4]*16384u+image[5]*8192u),"load synthetic cart");
            cyc_power_on((uint8_t)align); cpu.do_reset=cpu.power_on=0;
            check(cyc_audio_enable(align&1?48000:44100),"audio");
            check(cyc_zapper_attach(2),"Zapper"); cyc_set_zapper(120,100,true);
            bus_access(0x4017,0xc0,true); bus_access(0x2001,0x1e,true);
            if (mapper==1) serial(0x8000,0x1c);
            uint64_t pcm=UINT64_C(1469598103934665603), samples=0;
            printf("case %u %u %u\n",mapper,ram,align);
            for (unsigned q=0;q<128;++q) {
                bus_access(0x2000,(q&1)?0x10:0,true);
                bus_access(0x2001,(uint8_t)((q%3==0?0x0a:q%3==1?0x14:0x1e)|((q&3)<<5)|(q&1)),true);
                if (mapper==4) {
                    bus_access(0xe000,0,true); bus_access(0xc000,(uint8_t)(q&7),true);
                    bus_access(0x8000,(uint8_t)((q&7)|((q&1)<<7)),true);
                    bus_access(0x8001,(uint8_t)(q*3),true);
                } else if (mapper==1) { serial(0xa000,q&1); serial(0xc000,q>>1&1); }
                else if (mapper==66) bus_access(0x8000,(uint8_t)(q&0x11),true);
                unsigned limit=0,wait;
                for (wait=0;wait<100000;++wait) {
                    limit=hw_native_interval_limit(); if (limit>=12) break;
                    bus_access(0,0,false);
                }
                check(wait<100000,"reach stable raster interior");
                hw_cart.mirroring=(HwMirroring)((q>>3)&3);
                hw_cart.info.four_screen=(q%11)==0;
                for (unsigned i=0;i<sizeof(ppu.ciram);++i) ppu.ciram[i]=(uint8_t)(i*11+q*17+(i>>4));
                for (unsigned i=0;i<32;++i) ppu.palette[i]=(uint8_t)((q*7+i*13)&63);
                if (ram) for (unsigned i=0;i<hw_cart.chr_len;++i) hw_cart.chr[i]=(uint8_t)(i*17+q*29+(i>>5));
                for (unsigned i=0;i<256;i+=4) {
                    ppu.oam[i]=(q&3)?(uint8_t)(q+i*13):(uint8_t)ppu.scanline;
                    ppu.oam[i+1]=(uint8_t)(i+q); ppu.oam[i+2]=(uint8_t)(q*29+i); ppu.oam[i+3]=(uint8_t)(q*31+i);
                }
                for (unsigned i=0;i<8;++i) {
                    ppu.spr_x[i]=(uint8_t)((q&7)?(q+i*13)%256:0);
                    ppu.spr_lo[i]=(uint8_t)(q*11+i*31); ppu.spr_hi[i]=(uint8_t)(q*23+i*7);
                    ppu.spr_attr[i]=(uint8_t)(q*19+i*5);
                }
                ppu.v=(uint16_t)((q*257+31)&0x7fff); ppu.fine_x=(uint8_t)(q&7);
#if NESRECOMP_PPU_HLE
                ppu.bg_pixels=0x924936c9u^(q*0x01020408u); ppu.attr_pixels=0xe4e4e4e4u^q;
#else
                ppu.bg_lo=0xa57c; ppu.bg_hi=0x65a3; ppu.attr_lo=0x5a; ppu.attr_hi=0xa5;
#endif
                for (unsigned i=0;i<4;++i) ppu.color[i]=(uint8_t)((q+i*17)&63);
                ppu.can_s0hit=ppu.cur_has_s0=1;
                ppu.s0hit=ppu.s0hit_late=0;
                ppu.s0hit_pending1=(q%7)==0; ppu.s0hit_pending2=(q%11)==0;
                ppu.io_bus=0xf3; ppu.io_decay_clock=q*37;
                ppu.io_decay_next=UINT64_MAX;
                for (unsigned i=0;i<8;++i) {
                    ppu.io_decay_deadline[i]=ppu.io_decay_clock+1+(q+i)%12;
                    if (ppu.io_decay_deadline[i]<ppu.io_decay_next) ppu.io_decay_next=ppu.io_decay_deadline[i];
                }
                cyc_render_capture_background((q&1)!=0);
                for (unsigned i=0;i<4;++i) cyc_background_pipe[i]=(uint8_t)((q*13+i*17)&63);
                unsigned n=q%4==0?limit:1+(q%limit);
                if (reference) for (unsigned i=0;i<n;++i) bus_access(0,0,false);
                else {
                    hw.cpu_addr=0; hw.cpu_reading=1; hw.data_driven=1;
                    hw.data_bus=hw.internal_bus=hw.ram[0]; hw_native_interval_run(n);
                }
                int16_t audio[2048]; size_t count;
                while ((count=cyc_audio_read(audio,2048))!=0) {
                    pcm=mix(pcm,audio,count*sizeof(*audio)); samples+=count;
                }
                printf("%u %u %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %" PRIu64 "\n",
                       q,n,cyc_hw_state_hash(),cyc_mem_state_hash(),background_hash(),pcm,samples);
                const char *dump=getenv("NESRECOMP_RASTER_DUMP_ROW");
                if (dump && strtoul(dump,NULL,10)==((ram*4+board)*4+align)*128+q)
                    cyc_hw_state_dump(stderr);
            }
            uint8_t *state=NULL;size_t size=0;char error[256];
            check(cyc_state_save(&state,&size,error,sizeof(error)),"save raster current build");
            replay_route(reference);
            uint64_t machine=cyc_hw_state_hash(), memory=cyc_mem_state_hash(), background=background_hash();
            check(cyc_state_load(state,size,error,sizeof(error)),"restore raster current build");free(state);
            replay_route(reference);
            check(machine==cyc_hw_state_hash() && memory==cyc_mem_state_hash() && background==background_hash(),
                  "current-build raster/frame/background replay");
        }
    }
#if NESRECOMP_PPU_HLE
    if (!reference) check(ppu_native_raster_tiles>100,"whole raster job actually used");
#endif
    return 0;
}
