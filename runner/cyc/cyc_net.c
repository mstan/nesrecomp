#include "cyc_net.h"
#include "cyc_core.h"
#include "cyc_ring.h"
#include "cyc_session.h"
#include "cyc_trace.h"
#include "cyc_video.h"
#include "nes_netplay_identity.h"
#include "nes_netplay_rb.h"
#include "nes_rb_state.h"
#include "logical_input.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t s_rows[4];
static bool s_replay,s_guest;
static int s_width=256,s_offline_mode=-1,s_offline_width;
static Uint64 s_tick_start;
static uint8_t *s_storage;
static int width_get(char *out,int cap){return snprintf(out,(size_t)cap,"%d",cyc_video_width());}
static int width_apply(const char *v) {
    char *end;long n=strtol(v,&end,10);
    if(end==v||*end||n<256||n>864||(n&1))return 0;
    if(s_offline_mode<0){s_offline_mode=cyc_video_mode();s_offline_width=cyc_video_width();}
    s_width=(int)n;cyc_video_request_width(s_width);cyc_video_apply_pending();return 1;
}
static void width_restore(void) {
    if(s_offline_mode<0)return;
    if(s_offline_mode==NES_VIDEO_STOCK)cyc_video_request_width(s_offline_width);else cyc_video_set_mode(s_offline_mode);
    cyc_video_apply_pending();s_offline_mode=-1;
}
void nes_runner_register_session_keys(void){nes_netplay_session_register("vw",width_get,width_apply,width_restore);}
static void publish(const uint8_t *rows,int slots,int replay) {
    memset(s_rows,0,sizeof s_rows);if(slots>4)slots=4;if(slots>0)memcpy(s_rows,rows,(size_t)slots);
    s_replay=replay!=0;
}
static void resim(int begin){(void)begin;/* each admitted frame carries its replay flag */}
void cyc_net_prepare(NesNetplayConfig *cfg) {
    if(!nes_netplay_take_pending_config(cfg))nes_netplay_config_defaults(cfg);
    nes_netplay_config_apply_env(cfg);s_guest=cfg->enabled&&(cfg->spectator||cfg->local_slot!=0);
}
int cyc_net_start(const NesNetplayConfig *cfg,const char *rom) {
    s_replay=false;memset(s_rows,0,sizeof s_rows);
    if(!cfg->enabled)return 0;
    nes_runner_register_session_keys();
    if(!nes_netplay_identity_set_rom_file(rom))return -1;
    nes_rb_state_invalidate();
    nes_netplay_set_publish_hook(publish);nes_netplay_set_resim_hook(resim);
    return nes_netplay_start(cfg);
}
int cyc_net_boot(void) {
    if(!nes_netplay_active())return 0;
    /* Trace and APU clocks are simulation settings, independent of whether
     * this peer writes diagnostics or has a working sound device. */
    cyc_trace_enabled=true;
    cyc_audio_enable(cyc_session_audio_rate(48000));
    cyc_video_request_width(s_width);cyc_video_apply_pending();
    memset(g_logical_input,0,4);nes_rb_state_invalidate();
    return nes_netplay_boot_barrier();
}
static uint8_t test_pad(uint8_t local) {
    const char *v=getenv("NES_NET_TEST_PAD");if(!v||!*v)return local;
    char *end;unsigned seed=(unsigned)strtoul(v,&end,10),start=90;
    if(*end==':')start=(unsigned)strtoul(end+1,NULL,10);
    unsigned tick=nes_netplay_sim_tick();if(tick<start)return 0;
    unsigned t=tick-start;if(seed==0&&t<6)return 0x10;if(t<60)return 0;
    uint8_t b=0x01;if((t+seed*17)%180>150)b=0x02;
    if((t+seed*11)%51<13)b|=0x80;if((t+seed*7)%80<65)b|=0x40;return b;
}
int cyc_net_admit(uint8_t local) {
    const char *ticks=getenv("NES_NET_MATCH_TICKS");
    if(ticks&&atoi(ticks)>0&&nes_netplay_sim_tick()>=(uint32_t)atoi(ticks))nes_netplay_request_quiesce();
    if(cyc_net_leaving())return NES_NETPLAY_ADMIT_STALL;
    nes_netplay_stage_local(test_pad(local));
    int a=nes_netplay_poll_admit();
    if(a){nes_rb_state_invalidate();s_tick_start=SDL_GetPerformanceCounter();}
    return a;
}
void cyc_net_input(uint8_t buttons[2]) {
    cyc_session_logical_input(s_rows,4);buttons[0]=s_rows[0];buttons[1]=s_rows[1];cyc_session_input(buttons);
    cyc_set_controller(0,buttons[0]);cyc_set_controller(1,buttons[1]);
}
void cyc_net_finish(void) {
    nes_rb_state_invalidate();nes_netplay_rb_note_tick_cost(s_replay,(SDL_GetPerformanceCounter()-s_tick_start)*1000000.0/SDL_GetPerformanceFrequency());
    nes_netplay_frame_end();
}
bool cyc_net_replaying(void){return s_replay;}
bool cyc_net_leaving(void){return nes_netplay_quiesced()||nes_netplay_return_to_lobby_requested(NULL);}
bool cyc_net_guest(void){return s_guest;}
void cyc_net_shutdown(void) {
    if(nes_netplay_active()) {
        const char *path=getenv("NES_NET_FINAL_STATE");
        if(path&&*path) {
            size_t n=0;const uint8_t *image=nes_rb_state_image(&n);FILE *f=fopen(path,"wb");
            if(!f||!image||fwrite(image,1,n,f)!=n)fprintf(stderr,"[cycle rollback] cannot write final state %s\n",path);
            if(f)fclose(f);
        }
        nes_netplay_log_summary();
    }
    nes_netplay_shutdown();s_replay=false;
}
const uint8_t *cyc_net_sram(size_t *len) {
    uint32_t sizes[2]={(uint32_t)cyc_nvram_size(0),(uint32_t)cyc_nvram_size(1)};
    *len=8+(size_t)sizes[0]+sizes[1];uint8_t *p=(uint8_t *)realloc(s_storage,*len);
    if(!p)return NULL;s_storage=p;memcpy(p,sizes,8);
    if((sizes[0]&&!cyc_nvram_export(0,p+8,sizes[0]))||(sizes[1]&&!cyc_nvram_export(1,p+8+sizes[0],sizes[1])))return NULL;
    return p;
}
bool cyc_net_sram_load(const void *data,size_t len) {
    uint32_t sizes[2];if(!data||len<8)return false;memcpy(sizes,data,8);
    if(sizes[0]!=cyc_nvram_size(0)||sizes[1]!=cyc_nvram_size(1)||len!=8+(size_t)sizes[0]+sizes[1])return false;
    const uint8_t *p=(const uint8_t *)data;
    return (!sizes[0]||cyc_nvram_import(0,p+8,sizes[0]))&&(!sizes[1]||cyc_nvram_import(1,p+8+sizes[0],sizes[1]));
}
