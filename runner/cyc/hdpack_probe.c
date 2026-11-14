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
/* Both banks contain identical original pixels but different HD art. Clock
 * actual tile reads, then change the bank while later reads are in flight.
 * Already fetched input must keep its owned HD key through state restore. */
static void check_native_prefix(void) {
    uint8_t *boot=NULL,*mid=NULL;size_t boot_len=0,mid_len=0;char why[256];
    require(cyc_state_save(&boot,&boot_len,why,sizeof why),"save initial complete picture state");
    for(unsigned phase=0;phase<3;++phase) for(unsigned fine_x=0;fine_x<8;++fine_x) {
    uint32_t samples[2][2];
    for(unsigned bank=0;bank<2;++bank) {
        require(cyc_state_load(boot,boot_len,why,sizeof why),"restore initial complete picture state");
        hw_cart_cpu_write(0xf000,0);frame_ppu_mapper_changed();
        memset(ppu.ciram,0,sizeof ppu.ciram);memset(ppu.oam,255,sizeof ppu.oam);
        ppu.palette[0]=0x0f;ppu.palette[1]=1;ppu.palette[2]=2;ppu.palette[3]=3;
        ppu.v=ppu.t=0;ppu.fine_x=(uint8_t)fine_x;
        ppu.bg_table=0;ppu.show_bg=ppu.show_bg8=ppu.instant_bg=ppu.eval_bg=1;
        ppu.show_spr=ppu.instant_spr=ppu.eval_spr=0;ppu.render_count=5;
        ppu.scanline=193;ppu.dot=0;ppu_state_reloaded();
        while(ppu.dot<8+phase*2){ppu_dot();ppu_half_dot();}
        hw_cart_cpu_write(0xf000,(uint8_t)bank);frame_ppu_mapper_changed();
        if(phase==1)ppu.ciram[0]=1; /* current memory cannot replace a fetched tile */
        require(cyc_state_save(&mid,&mid_len,why,sizeof why),"save pending fetched input and HD observer");
        while(ppu.dot<64){ppu_dot();ppu_half_dot();}
        cyc_render_frame_done();
        int w,h;const uint32_t *picture=cyc_hdpack_present(cyc_frame_argb(),&w,&h);
        require(w==512 && h==480,"HD product dimensions");
        samples[bank][0]=picture[(193*2)*w+(16-fine_x)*2];
        samples[bank][1]=picture[(193*2)*w+40*2];
        require(((samples[bank][0]>>16)&255)==80,"fetched tile keeps original HD bank");
        size_t bytes=(size_t)w*h*sizeof(*picture);uint32_t *expected=malloc(bytes);
        require(expected!=NULL,"copy enhanced picture");memcpy(expected,picture,bytes);
        require(cyc_state_load(mid,mid_len,why,sizeof why),"restore pending fetched input and HD observer");
        free(mid);mid=NULL;
        while(ppu.dot<64){ppu_dot();ppu_half_dot();}
        cyc_render_frame_done();
        picture=cyc_hdpack_present(cyc_frame_argb(),&w,&h);
        require(!memcmp(expected,picture,bytes),"enhanced retained-prefix replay");free(expected);
    }
    require(samples[0][0]==samples[1][0],"prefix preserves original tile bank");
    require(samples[0][1]!=samples[1][1],"new row observes changed tile bank");
    }
    require(cyc_state_load(boot,boot_len,why,sizeof why),"restore initial picture state after probe");free(boot);
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
