/* Rollback uses the complete cycle save domain plus the four logical seats.
 * Digests and snapshots share one cached image. No native stack is saved:
 * cyc_run_frame returns, so loading at the next admit is the continuation. */
#include "nes_rb_state.h"
#include "logical_input.h"
#include "cyc_state.h"
#include "cyc_session.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRAILER 12u
static uint8_t *s_image;
static size_t s_len;
static int s_valid;
static NesRbDigest s_digest;
static uint64_t s_count[3];
static float s_us[3][1024];

static void measured(int k, Uint64 begin) {
    s_us[k][s_count[k]++ % 1024] = (float)((SDL_GetPerformanceCounter()-begin)*1000000.0/SDL_GetPerformanceFrequency());
}
static uint32_t hash(const uint8_t *p,size_t n,uint32_t h) {
    for(size_t i=0;i<n;++i)h=(h^p[i])*16777619u;
    return h;
}
const char *nes_rb_part_name(int i) {
    static const char *names[]={"cpu_wram","ppu","apu_io_mods"};
    return i>=0&&i<3?names[i]:"unknown";
}
void nes_rb_digest_image(const uint8_t *img,size_t len,NesRbDigest *out) {
    Uint64 begin=SDL_GetPerformanceCounter();
    memset(out,0,sizeof *out);
    if(!img||len<32+TRAILER)return;
    out->master=hash(img,len,2166136261u);
    for(int i=0;i<3;++i)out->part[i]=2166136261u;
    out->part[0]=hash(img,32,out->part[0]);
    size_t at=32, end=len-TRAILER;
    while(end-at>=8) {
        uint32_t n;memcpy(&n,img+at+4,4);
        if(n>end-at-8)break;
        int part=!memcmp(img+at,"PPU ",4)||!memcmp(img+at,"PIC",3)||!memcmp(img+at,"CHRR",4)?1:
                 !memcmp(img+at,"CPU ",4)||!memcmp(img+at,"HW  ",4)||!memcmp(img+at,"CART",4)||!memcmp(img+at,"TIME",4)||!memcmp(img+at,"REGN",4)?0:2;
        out->part[part]=hash(img+at,8+n,out->part[part]);at+=8+n;
    }
    out->part[2]=hash(img+end,TRAILER,out->part[2]);
    measured(2,begin);
}
void nes_rb_state_invalidate(void){s_valid=0;}
const uint8_t *nes_rb_state_image(size_t *len) {
    if(!s_valid) {
        Uint64 begin=SDL_GetPerformanceCounter();
        uint8_t *raw=NULL;size_t n=0;char err[256];
        if(!cyc_state_save(&raw,&n,err,sizeof err)) {
            fprintf(stderr,"[cycle rollback] save: %s\n",err);if(len)*len=0;return NULL;
        }
        uint8_t *grown=(uint8_t *)realloc(raw,n+TRAILER);
        if(!grown){free(raw);if(len)*len=0;return NULL;}
        memcpy(grown+n,"CYCRBIN1",8);memcpy(grown+n+8,g_logical_input,4);
        free(s_image);s_image=grown;s_len=n+TRAILER;s_valid=1;
        measured(0,begin);nes_rb_digest_image(s_image,s_len,&s_digest);
    }
    if(len)*len=s_len;return s_image;
}
size_t nes_rb_state_save(uint8_t *buf,size_t cap,size_t *need) {
    size_t n;const uint8_t *p=nes_rb_state_image(&n);if(need)*need=n;
    if(!p||!buf||cap<n)return 0;memcpy(buf,p,n);return n;
}
int nes_rb_state_load(const uint8_t *buf,size_t len) {
    Uint64 begin=SDL_GetPerformanceCounter();char err[256];
    if(!buf||len<32+TRAILER||memcmp(buf+len-TRAILER,"CYCRBIN1",8))return 0;
    if(!cyc_state_load(buf,len-TRAILER,err,sizeof err)) {
        fprintf(stderr,"[cycle rollback] load: %s\n",err);return 0;
    }
    memcpy(g_logical_input,buf+len-4,4);cyc_session_state_loaded();
    nes_rb_state_invalidate();measured(1,begin);return 1;
}
void nes_rb_state_digest(NesRbDigest *out){size_t n;if(nes_rb_state_image(&n))*out=s_digest;else memset(out,0,sizeof *out);}
uint32_t nes_rb_state_digest_master(void){NesRbDigest d;nes_rb_state_digest(&d);return d.master;}
void nes_rb_state_describe(const uint8_t *img,size_t len,size_t off,char *what,size_t cap) {
    if(off>=len){snprintf(what,cap,"size");return;}
    if(off<32){snprintf(what,cap,"cycle.header+%zu",off);return;}
    if(len>=TRAILER&&off>=len-TRAILER){snprintf(what,cap,"logical-input+%zu",off-(len-TRAILER));return;}
    size_t at=32;
    while(len-at>=8) {
        uint32_t n;memcpy(&n,img+at+4,4);if(n>len-at-8)break;
        if(off<at+8+n){snprintf(what,cap,"cycle.%.4s+%zu",img+at,off-at);return;}
        at+=8+n;
    }
    snprintf(what,cap,"cycle+%zu",off);
}
long nes_rb_state_first_diff(const uint8_t *a,size_t alen,const uint8_t *b,size_t blen,char *what,size_t cap) {
    size_t n=alen<blen?alen:blen;
    for(size_t i=0;i<n;++i)if(a[i]!=b[i]){nes_rb_state_describe(a,alen,i,what,cap);return (long)i;}
    if(alen!=blen){snprintf(what,cap,"size");return (long)n;}
    if(cap)what[0]=0;return -1;
}
static int compare(const void *a,const void *b){float x=*(const float *)a,y=*(const float *)b;return (x>y)-(x<y);}
static void pct(int k,double *p50,double *p99) {
    float tmp[1024];size_t n=s_count[k]<1024?(size_t)s_count[k]:1024;*p50=*p99=0;
    if(!n)return;memcpy(tmp,s_us[k],n*sizeof(float));qsort(tmp,n,sizeof(float),compare);
    *p50=tmp[(n-1)/2];*p99=tmp[(size_t)((n-1)*0.99)];
}
void nes_rb_state_timing(NesRbStateTiming *out) {
    memset(out,0,sizeof *out);out->saves=s_count[0];out->loads=s_count[1];out->digests=s_count[2];out->image_bytes=s_len;
    pct(0,&out->save_us_p50,&out->save_us_p99);pct(1,&out->load_us_p50,&out->load_us_p99);pct(2,&out->digest_us_p50,&out->digest_us_p99);
}
void nes_rb_state_log_timing(const char *tag) {
    NesRbStateTiming t;nes_rb_state_timing(&t);
    fprintf(stderr,"RB_STATE_TIMING %s bytes=%zu saves=%llu loads=%llu save_us p50=%.1f p99=%.1f load_us p50=%.1f p99=%.1f\n",tag,t.image_bytes,(unsigned long long)t.saves,(unsigned long long)t.loads,t.save_us_p50,t.save_us_p99,t.load_us_p50,t.load_us_p99);
}
void nes_rb_log_bridge(void){fprintf(stderr,"RB_BRIDGE cycle_frame_returns=1 native_stack_snapshot=0\n");}
