#include "cyc_hdpack.h"
#include "cyc_presentation.h"
#include "cyc_core.h"
#include "cyc_render.h"
#include "hw_internal.h"
#include "hdpack.h"
#include "mod_runtime.h"
#include "mod_savestate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int32_t index;
    uint8_t bytes[16], row, x, valid;
} HdFetchedTile;
/* All tile keys are owned bytes, rather than pointers into mutable CHR RAM.
 * Four records keep each saved block below the mod registry's 1 MiB limit. */
typedef struct {
    int32_t bg_index,sp_index;
    uint8_t bg_tile[16],sp_tile[16],bg_pal[4],sp_pal[3];
    uint8_t bg_x,bg_y,sp_x,sp_y,flags,mask,bg_color,sp_color;
} HdRecordedPixel;
typedef struct {
    HdFetchedTile bg_fetch,bg[16],sprite[8];
    uint8_t sprite_x[8];
    HdRecordedPixel color[4];
} HdPipeline;
typedef struct {uint32_t version,active,scale,part;} HdStateHeader;
static HdPipeline pipeline;
static HdRecordedPixel recorded[256*240];
static uint32_t *upscaled;
static size_t upscaled_capacity;
static bool configured;

void cyc_hdpack_config(int enabled,const char *directory) {
    configured=true;g_nes_config.hdpack_enabled=enabled!=0;
    snprintf(g_nes_config.hdpack_dir,sizeof g_nes_config.hdpack_dir,"%s",directory?directory:"");
}
void cyc_hdpack_power_on(void) {
    memset(&pipeline,0,sizeof pipeline);memset(recorded,0,sizeof recorded);
    if(!configured)cyc_hdpack_config(1,"");
    hdpack_load_from_config(hw_cart.chr_ram!=0,256);
}
static void fetch_tile(HdFetchedTile *tile,uint16_t address,uint8_t value,bool high) {
    unsigned row=address&7;
    if(!high) {
        memset(tile,0,sizeof(*tile));tile->valid=1;tile->row=(uint8_t)row;
        uint16_t base=address&0x1ff0;
        tile->index=hw_cart.chr_ram?-1:(int32_t)(hw_cart_chr_index(base)/16);
        for(unsigned j=0;j<16;j++)tile->bytes[j]=cyc_render_chr((uint16_t)(base+j));
    }
    /* Preserve the two bytes actually latched even if CHR changed between
     * pattern fetches. Other tile rows are a content key, not guest reads. */
    tile->bytes[row+(high?8:0)]=value;
}
void cyc_hdpack_bg_fetch(uint16_t address,uint8_t value,bool high) {
    if(hdpack_recording())fetch_tile(&pipeline.bg_fetch,address,value,high);
}
void cyc_hdpack_bg_reload(void) {
    if(!hdpack_recording())return;
    for(unsigned i=0;i<8;i++){pipeline.bg[i]=pipeline.bg_fetch;pipeline.bg[i].x=(uint8_t)(7-i);}
}
void cyc_hdpack_bg_shift(void) {
    if(!hdpack_recording())return;
    for(unsigned i=15;i;i--)pipeline.bg[i]=pipeline.bg[i-1];
    memset(&pipeline.bg[0],0,sizeof pipeline.bg[0]);
}
void cyc_hdpack_sprite_fetch(unsigned slot,uint16_t address,uint8_t value,bool high) {
    if(!hdpack_recording())return;
    fetch_tile(&pipeline.sprite[slot],address,value,high);
    if(!high)pipeline.sprite_x[slot]=0;
}
void cyc_hdpack_sprite_shift(unsigned slot) {
    if(hdpack_recording() && pipeline.sprite_x[slot]<8)pipeline.sprite_x[slot]++;
}
void cyc_hdpack_clock(void) {
    if(!hdpack_recording())return;
    pipeline.color[3]=pipeline.color[2];pipeline.color[2]=pipeline.color[1];pipeline.color[1]=pipeline.color[0];
}
void cyc_hdpack_pixel(unsigned bg_color,unsigned bg_palette,int sprite,unsigned sprite_color) {
    if(!hdpack_recording())return;
    HdRecordedPixel *p=&pipeline.color[0];memset(p,0,sizeof(*p));
    p->bg_index=p->sp_index=-1;p->bg_pal[0]=ppu.palette[0];
    const HdFetchedTile *bg=&pipeline.bg[15-ppu.fine_x];
    if(ppu.show_bg && (ppu.dot>8 || ppu.show_bg8) && bg->valid) {
        p->flags=1;p->bg_index=bg->index;memcpy(p->bg_tile,bg->bytes,16);
        p->bg_x=bg->x;p->bg_y=bg->row;p->bg_color=(uint8_t)bg_color;
        for(unsigned j=1;j<4;j++)p->bg_pal[j]=ppu.palette[bg_palette*4+j];
    }
    if(sprite>=0 && pipeline.sprite[sprite].valid && pipeline.sprite_x[sprite]<8) {
        const HdFetchedTile *sp=&pipeline.sprite[sprite];unsigned attr=ppu.spr_attr[sprite];
        p->flags|=(uint8_t)(2|((attr&0x40)?4:0)|((attr&0x80)?8:0));
        p->sp_index=sp->index;memcpy(p->sp_tile,sp->bytes,16);
        /* The shared sampler expects the fetched row (already vertically
         * flipped by the PPU); vm flips only pixels within its HD block. */
        p->sp_x=pipeline.sprite_x[sprite];p->sp_y=sp->row;
        p->sp_color=(uint8_t)sprite_color;
        for(unsigned j=0;j<3;j++)p->sp_pal[j]=ppu.palette[16+(attr&3)*4+j+1];
    }
}
void cyc_hdpack_blank(uint8_t backdrop) {
    if(!hdpack_recording())return;
    memset(&pipeline.color[0],0,sizeof pipeline.color[0]);pipeline.color[0].bg_pal[0]=backdrop;
}
void cyc_hdpack_output(int x,int y) {
    if(!hdpack_recording())return;
    HdRecordedPixel *p=&recorded[y*256+x];*p=pipeline.color[3];
    unsigned emphasis=ppu.emphasis;
    if(hw_pal())emphasis=(emphasis&4)|((emphasis&1)<<1)|((emphasis&2)>>1);
    p->mask=(uint8_t)(ppu.greyscale|(emphasis<<5));
}
static uint32_t color(uint8_t value,uint8_t mask) {
    return hw_palette_argb[((mask>>5)*64)+((mask&1)?(value&0x30):(value&0x3f))];
}
const uint32_t *cyc_hdpack_present(const uint32_t *native,int *width,int *height) {
    *width=256;*height=240;if(!hdpack_active())return native;
    HdPixel *pixels=hdpack_pixels();
    for(unsigned i=0;i<256*240;i++) {
        const HdRecordedPixel *r=&recorded[i];HdPixel *p=&pixels[i];memset(p,0,sizeof(*p));
        p->bg_index=r->bg_index;p->sp_index=r->sp_index;
        p->bg_t16=r->bg_tile;p->sp_t16=r->sp_tile;
        p->bg_has=r->flags&1;p->sp_has=(r->flags>>1)&1;
        p->bg_p0=r->bg_pal[0];p->bg_p1=r->bg_pal[1];p->bg_p2=r->bg_pal[2];p->bg_p3=r->bg_pal[3];
        p->sp_p1=r->sp_pal[0];p->sp_p2=r->sp_pal[1];p->sp_p3=r->sp_pal[2];
        p->bg_ox=r->bg_x;p->bg_oy=r->bg_y;p->sp_ox=r->sp_x;p->sp_oy=r->sp_y;
        p->sp_hm=(r->flags>>2)&1;p->sp_vm=(r->flags>>3)&1;
        p->backdrop=color(r->bg_pal[0],r->mask);
        p->bg_argb=color(r->bg_pal[r->bg_color],r->mask);
        p->sp_argb=r->sp_color?color(r->sp_pal[r->sp_color-1],r->mask):native[i];
    }
    size_t count=(size_t)256*240*hdpack_scale()*hdpack_scale();
    if(count>upscaled_capacity) {
        uint32_t *next=realloc(upscaled,count*sizeof(*next));if(!next)return native;
        upscaled=next;upscaled_capacity=count;
    }
    hdpack_upscale(native,256,upscaled);*width=256*hdpack_scale();*height=240*hdpack_scale();return upscaled;
}

static int save_part(unsigned part,uint8_t *buf,int cap) {
    size_t size=part==4?sizeof pipeline:sizeof(recorded)/4;
    if(cap<(int)(sizeof(HdStateHeader)+size))return -1;
    HdStateHeader h={1,hdpack_active()!=0,(uint32_t)hdpack_scale(),part};memcpy(buf,&h,sizeof h);
    memcpy(buf+sizeof h,part==4?(const void *)&pipeline:(const void *)&recorded[part*256*60],size);
    return (int)(sizeof h+size);
}
static bool valid_pixel(const HdRecordedPixel *p) {
    if(p->flags>15 || p->bg_x>7 || p->bg_y>7 || p->sp_x>7 || p->sp_y>7 || p->bg_color>3 || p->sp_color>3)return false;
    for(unsigned j=0;j<4;j++)if(p->bg_pal[j]>63)return false;
    for(unsigned j=0;j<3;j++)if(p->sp_pal[j]>63)return false;
    return true;
}
static int validate_part(unsigned part,const uint8_t *buf,int len) {
    size_t size=part==4?sizeof pipeline:sizeof(recorded)/4;
    if(!buf || len!=(int)(sizeof(HdStateHeader)+size))return 0;
    HdStateHeader h;memcpy(&h,buf,sizeof h);
    if(h.version!=1 || h.part!=part || h.active!=(unsigned)(hdpack_active()!=0) || h.scale!=(unsigned)hdpack_scale())return 0;
    if(part==4) {
        HdPipeline v;memcpy(&v,buf+sizeof h,sizeof v);
        if(v.bg_fetch.row>7 || v.bg_fetch.x>7 || v.bg_fetch.valid>1)return 0;
        for(unsigned i=0;i<16;i++)if(v.bg[i].row>7 || v.bg[i].x>7 || v.bg[i].valid>1)return 0;
        for(unsigned i=0;i<8;i++)if(v.sprite[i].row>7 || v.sprite[i].valid>1 || v.sprite_x[i]>8)return 0;
        for(unsigned i=0;i<4;i++)if(!valid_pixel(&v.color[i]))return 0;
    } else {
        for(unsigned i=0;i<256*60;i++){HdRecordedPixel p;memcpy(&p,buf+sizeof h+i*sizeof p,sizeof p);if(!valid_pixel(&p))return 0;}
    }
    return 1;
}
static int load_part(unsigned part,const uint8_t *buf,int len) {
    if(!validate_part(part,buf,len))return 0;
    size_t size=part==4?sizeof pipeline:sizeof(recorded)/4;
    memcpy(part==4?(void *)&pipeline:(void *)&recorded[part*256*60],buf+sizeof(HdStateHeader),size);return 1;
}
#define HD_STATE_PART(n) \
static int get##n(uint8_t *b,int c){return save_part(n,b,c);} \
static int set##n(const uint8_t *b,int l){return load_part(n,b,l);} \
static int check##n(const uint8_t *b,int l){return validate_part(n,b,l);}
HD_STATE_PART(0) HD_STATE_PART(1) HD_STATE_PART(2) HD_STATE_PART(3) HD_STATE_PART(4)
NES_MOD_CONSTRUCTOR(register_cycle_hd_state) {
    NESModSavestateGet gets[]={get0,get1,get2,get3,get4};
    NESModSavestateSet sets[]={set0,set1,set2,set3,set4};
    NESModSavestateValidate checks[]={check0,check1,check2,check3,check4};
    for(unsigned i=0;i<5;i++) {
        char id[40];snprintf(id,sizeof id,"cyc.hdpack.%u",i);
        if(!nes_mod_register_savestate_hook(id,gets[i],sets[i]) || !nes_mod_register_savestate_validator(id,checks[i]))
            fprintf(stderr,"[HD pack] Cannot register cycle save record %u\n",i);
    }
}
