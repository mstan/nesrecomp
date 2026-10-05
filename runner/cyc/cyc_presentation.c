#include "cyc_presentation.h"
#include "cyc_core.h"
#include "cyc_render.h"
#include "cyc_video.h"
#include "hw_internal.h"
#include <string.h>
/* Activation plugins may inspect RAM before the game's power_on callback. */
uint8_t *cyc_presentation_ram=hw.ram;
uint8_t g_ppu_oam[256], g_ppu_nt[2048], g_ppu_pal[32], g_chr_ram[8192];
uint8_t g_ppuctrl, g_ppumask, g_controller1_buttons;
uint32_t g_nes_palette[64];
int g_render_width=256, g_widescreen_left, g_widescreen_right, g_ws_eff_left, g_ws_eff_right;
NesConfig g_nes_config;
static SDL_JoystickID s_event_instance;
static int s_event_player=-1;
void cyc_presentation_sync(void) {
    cyc_presentation_ram=hw.ram;
    memcpy(g_ppu_oam,cyc_render_oam(),sizeof g_ppu_oam);
    memcpy(g_ppu_nt,ppu.ciram,sizeof g_ppu_nt);
    memcpy(g_ppu_pal,cyc_ppu_palette(),sizeof g_ppu_pal);
    for(unsigned i=0;i<sizeof g_chr_ram;i++) g_chr_ram[i]=cyc_render_chr((uint16_t)i);
    g_ppuctrl=cyc_frame_lines()[239].ctrl; g_ppumask=cyc_render_line_mask(239);
    unsigned emphasis=(g_ppumask>>5)*64;
    for(unsigned i=0;i<64;i++) g_nes_palette[i]=hw_palette_argb[emphasis+((g_ppumask&1)?(i&0x30):i)];
    g_render_width=cyc_video_width();g_widescreen_left=cyc_video_native_x0();
    g_widescreen_right=g_render_width-256-g_widescreen_left;
}
void cyc_presentation_event(const SDL_Event *event,int player) {
    s_event_player=player;
    s_event_instance=event->type==SDL_CONTROLLERAXISMOTION?event->caxis.which:-1;
    g_nes_config.player_src[0]=player==0?2:1;
}
int controller_instance_is_player(SDL_JoystickID instance,int player) {
    return player>0 && s_event_player==player-1 && s_event_instance==instance;
}
void nes_video_set_aspect_mode(NesAspectMode mode) {cyc_video_set_mode((int)mode);}
NesAspectMode nes_video_aspect_mode(void) {return (NesAspectMode)cyc_video_mode();}
int nes_video_aspect_from_name(const char *s,NesAspectMode *out) {
    static const char *names[]={"stock","16:9","21:9","32:9","fit"};
    for(int i=0;i<5;i++)if(!strcmp(s,names[i])){*out=(NesAspectMode)i;return 1;}
    /* Declarative choices use hyphens. */
    if(!strcmp(s,"16-9")){*out=NES_ASPECT_16_9;return 1;}
    if(!strcmp(s,"21-9")){*out=NES_ASPECT_21_9;return 1;}
    if(!strcmp(s,"32-9")){*out=NES_ASPECT_32_9;return 1;}
    return 0;
}
const char *nes_video_aspect_name(NesAspectMode m) {
    static const char *names[]={"stock","16:9","21:9","32:9","fit"};
    return m>=0 && m<NES_ASPECT_COUNT?names[m]:"unknown";
}
void nes_video_request_width(int w) {cyc_video_request_width(w);}
void nes_video_request_margins(int left,int right) {cyc_video_request_width(256+left+right);}
