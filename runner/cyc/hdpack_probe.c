/* Standalone whole-machine host for HD pipeline fixtures, with no generated
 * ROM identity or game-specific code. Also usable for inspecting a local pack. */
#include "cyc_host_extras.h"
#include "cyc_presentation.h"
#include "cyc_hdpack.h"
#include "cyc_core.h"
#include <string.h>
#include "ppu_implementation.h"
#if NESRECOMP_PPU_FRAME
#include "hw_internal.h"
#include "hw.h"
#include "cyc_state.h"
#include "cyc_render.h"
#include <stdio.h>
#include <stdlib.h>
static bool prefix_check;
static void require(bool ok,const char *what) {
    if(!ok){fprintf(stderr,"HD native prefix FAIL: %s\n",what);exit(1);}
}
/* Test-only provider probe. Both banks contain identical original pixels but
 * different HD art. A late-resumed prefix must retain bank0's owned tile key,
 * while the remainder of the row uses the newly selected bank1. */
static void check_native_prefix(void) {
    CycSnapshot *boot=cyc_snapshot_new(),*mid=cyc_snapshot_new();
    require(boot && mid,"allocate snapshots");cyc_snapshot_take(boot);
    for(unsigned phase=0;phase<3;++phase) for(unsigned fine_x=0;fine_x<8;++fine_x) {
    uint32_t samples[2][2];
    for(unsigned bank=0;bank<2;++bank) {
        cyc_snapshot_restore(boot);hw_cart_cpu_write(0xf000,0);frame_ppu_mapper_changed();
        memset(ppu.ciram,0,sizeof ppu.ciram);memset(ppu.oam,255,sizeof ppu.oam);
        ppu.palette[0]=0x0f;ppu.palette[1]=1;ppu.palette[2]=2;ppu.palette[3]=3;
        ppu.v=ppu.t=ppu.frame_line_v=0;ppu.fine_x=(uint8_t)fine_x;
        ppu.bg_table=0;ppu.show_bg=ppu.show_bg8=1;ppu.show_spr=0;
        ppu.scanline=193;ppu.dot=phase==2?16:10;ppu_state_reloaded();
        ppu_write(0x2001,0);require(ppu.frame_hold_status==1,"capture prefix");
        hw_cart_cpu_write(0xf000,(uint8_t)bank);frame_ppu_mapper_changed();
        if(phase==1)ppu.ciram[0]=1; /* a later NT write cannot replace a latched tile */
        ppu.dot=phase==1?330:phase==2?340:334;ppu_write(0x2001,0x0a);
        require(ppu.frame_hold_status==(phase==1?6:2),"resume retained prefix, including across row end");
        ppu.scanline=194;ppu.dot=4;ppu.frame_line_v=0;ppu_state_reloaded();
        cyc_snapshot_take(mid);
        for(unsigned i=0;i<40;++i)ppu_dot();
        cyc_render_frame_done();
        int w,h;const uint32_t *picture=cyc_hdpack_present(cyc_frame_argb(),&w,&h);
        require(w==512 && h==480,"HD product dimensions");
        samples[bank][0]=picture[(194*2)*w];
        samples[bank][1]=picture[(194*2)*w+24*2];
        if(phase==1)require(((picture[(194*2)*w+(10-fine_x)*2]>>16)&255)==80+80*bank,
                         "partial prefetch retains selected tile through new CHR bank");
        size_t bytes=(size_t)w*h*sizeof(*picture);uint32_t *expected=malloc(bytes);
        require(expected!=NULL,"copy enhanced picture");memcpy(expected,picture,bytes);
        cyc_snapshot_restore(mid);
        for(unsigned i=0;i<40;++i)ppu_dot();
        cyc_render_frame_done();
        picture=cyc_hdpack_present(cyc_frame_argb(),&w,&h);
        require(!memcmp(expected,picture,bytes),"enhanced retained-prefix replay");free(expected);
    }
    require(samples[0][0]==samples[1][0],"prefix preserves original tile bank");
    require(samples[0][1]!=samples[1][1],"new row observes changed tile bank");
    }
    cyc_snapshot_restore(boot);cyc_snapshot_free(mid);cyc_snapshot_free(boot);
    fprintf(stderr,"PASS: native HD retained tile identity and current-build replay\n");
}
#endif
static void power_on(void *ctx){(void)ctx;cyc_presentation_sync();cyc_hdpack_power_on();
#if NESRECOMP_PPU_FRAME
    if(prefix_check)check_native_prefix();
#endif
}
static const uint32_t *present(void *ctx,int *w,int *h){(void)ctx;cyc_presentation_sync();return cyc_hdpack_present(cyc_frame_argb(),w,h);}
static bool option(void *ctx,const char *name,const char *value){(void)ctx;
#if NESRECOMP_PPU_FRAME
    if(!strcmp(name,"--hdpack-prefix-check")){prefix_check=true;return true;}
#endif
    cyc_hdpack_config(strcmp(value,"off")!=0,strcmp(value,"off")?value:"");return true;}
const CycHostExtras *cyc_host_extras(void){
    static const CycHostOption options[]={{"--hdpack",true,"Texture pack folder, or off"}
#if NESRECOMP_PPU_FRAME
        ,{"--hdpack-prefix-check",false,"Check retained native HD tile inputs"}
#endif
    };
    static const CycHostExtras extras={.power_on=power_on,.present=present,.options=options,.option_count=sizeof(options)/sizeof(options[0]),.option=option};return &extras;
}
