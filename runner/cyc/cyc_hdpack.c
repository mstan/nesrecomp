#include "cyc_hdpack.h"
#ifndef NESRECOMP_CYCLE_HDPACK_MODS
#include "cyc_presentation.h"
#endif
#include "cyc_core.h"
#include "cyc_render.h"
#include "cyc_mod.h"
#include "hw_internal.h"
#include "hdpack.h"
#include "cyc_hd_audio.h"
#include "mod_runtime.h"
#include "mod_savestate.h"
#if CYC_NATIVE_PROGRAMS
#include "cyc_program.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "cyc_hdpack_state.inc"
static uint32_t *upscaled;
static size_t upscaled_capacity;
static uint64_t presented_generation;
static const HdJobFrame *prepared_jobs;
static uint64_t jobs_generation,jobs_epoch,thumbnail_serial;
static uint32_t thumbnail[256*240];
static void release_jobs(void) {
    hdpack_jobs_release(prepared_jobs);prepared_jobs=NULL;
    thumbnail_serial=UINT64_MAX;
}
#ifndef NESRECOMP_CYCLE_HDPACK_MODS
static bool configured;

void cyc_hdpack_config(int enabled,const char *directory) {
    configured=true;g_nes_config.hdpack_enabled=enabled!=0;
    snprintf(g_nes_config.hdpack_dir,sizeof g_nes_config.hdpack_dir,"%s",directory?directory:"");
}
void cyc_hdpack_power_on(void) {
    presentation_valid=false;++presentation_epoch;release_jobs();
    memset(&pipeline,0,sizeof pipeline);memset(recorded,0,sizeof recorded);
    if(!configured)cyc_hdpack_config(1,"");
    hdpack_load_from_config(hw_cart.chr_ram!=0,256);
    if(hdpack_active())cyc_hd_audio_load(hdpack_directory(),hw_pal());
    else cyc_hd_audio_unload();
}
#else
static void activate_hd_pack(void) { nes_mod_set_local_only("HD pack",1); }
#if CYC_NATIVE_PROGRAMS
static int compiled_hd_image(const uint8_t *image,uint32_t size) {
    return cyc_program_match_image(image,size)!=NULL;
}
#endif
NES_MOD_CONSTRUCTOR(register_hd_pack_plugin) {
    nes_mod_register_activation_plugin("nesrecomp.hd-pack",activate_hd_pack);
#if CYC_NATIVE_PROGRAMS
    nes_mod_register_hd_image_validator(compiled_hd_image);
#endif
}
bool cyc_hdpack_mod_image(uint8_t **image,size_t *size) {
    const NESModHdPack *pack=nes_mod_hd_pack();
    if(!pack || !pack->patched_image)return true;
#if CYC_NATIVE_PROGRAMS
    if(!*image || !compiled_hd_image(pack->patched_image,pack->image_size))return false;
    uint8_t *copy=malloc(pack->image_size);
    if(!copy)return false;
    memcpy(copy,pack->patched_image,pack->image_size);
    free(*image);*image=copy;*size=pack->image_size;
    return true;
#else
    return false;
#endif
}
bool cyc_hdpack_mod_prepare(void) {
    presentation_valid=false;++presentation_epoch;release_jobs();
    const NESModHdPack *pack=nes_mod_hd_pack();
    memset(&pipeline,0,sizeof pipeline);memset(recorded,0,sizeof recorded);
    memset(fingerprint,0,sizeof fingerprint);hdpack_unload();cyc_hd_audio_unload();
    if(pack) {
        if(pack->patched_payload) {
            if(!cyc_mod_apply_cart_payload(pack->patched_payload,pack->payload_size)) {
                fprintf(stderr,"[Mods] HD patch does not match this cartridge's geometry\n");return false;
            }
        }
        if(hdpack_load(pack->directory,hw_cart.chr_ram!=0,256)) {
            fprintf(stderr,"[Mods] Selected HD pack could not be loaded\n");return false;
        }
        cyc_hd_audio_load(pack->directory,hw_pal());
        memcpy(fingerprint,pack->fingerprint,sizeof fingerprint);
    }
    nes_mod_lock_hd_pack();
    return true;
}
const uint32_t *cyc_hdpack_mod_present(int *width,int *height) {
    if(!hdpack_active())return NULL;
    return cyc_hdpack_present(cyc_frame_argb(),width,height);
}
#endif
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
/* Build only display metadata for sprites beyond the hardware's eight units.
 * Snapshot once at the end of sprite fetch, stamp at most four OAM-ordered
 * entries per screen pixel, and never issue bus accesses or change overflow. */
static void prepare_visual_sprites(void) {
    pipeline.visual_valid=0;
    if(!hdpack_unlimited_sprites())return;
    unsigned next=ppu.scanline>=240?0:ppu.scanline+1;
    if(next>=240)return;
    memset(pipeline.visual_pixels,255,sizeof pipeline.visual_pixels);
    memset(pipeline.visual,0,sizeof pipeline.visual);
    unsigned height=ppu.sprite16?16:8,used=0;
    for(unsigned i=0;i<64;i++) {
        HdVisualSprite *sp=&pipeline.visual[i];const uint8_t *oam=ppu.oam+i*4;
        sp->x=oam[3];sp->attr=oam[2];sp->slot=255;
        int row=(int)next-(int)oam[0]-1;
        if(row<0 || (unsigned)row>=height)continue;
        /* Reuse actually fetched keys for the hardware sprites, including
         * any bank change between their low/high pattern fetches. */
        for(unsigned slot=0;slot<8;slot++) {
            if(!(used&(1u<<slot)) && pipeline.sprite[slot].valid && !memcmp(ppu.oam2+slot*4,oam,4)) {
                sp->tile=pipeline.sprite[slot];sp->slot=(uint8_t)slot;used|=1u<<slot;break;
            }
        }
        if(sp->slot==255) {
            unsigned source_row=(sp->attr&0x80)?height-1-(unsigned)row:(unsigned)row;
            unsigned tile=oam[1],base=ppu.sprite_table?0x1000:0;
            if(height==16){base=(tile&1)?0x1000:0;tile=(tile&0xfe)+(source_row>>3);}
            uint16_t address=(uint16_t)(base+tile*16);
            sp->tile.valid=1;sp->tile.row=(uint8_t)(source_row&7);
            sp->tile.index=hw_cart.chr_ram?-1:(int32_t)(hw_cart_chr_index(address)/16);
            for(unsigned b=0;b<16;b++)sp->tile.bytes[b]=cyc_render_chr((uint16_t)(address+b));
        }
        for(unsigned ox=0;ox<8 && (unsigned)sp->x+ox<256;ox++) {
            uint8_t *at=pipeline.visual_pixels[(unsigned)sp->x+ox];
            unsigned k=0;while(k<4 && at[k]!=255)k++;
            if(k<4)at[k]=(uint8_t)i;
        }
    }
    pipeline.visual_line=(uint8_t)next;pipeline.visual_valid=1;
}
void cyc_hdpack_sprite_fetch(unsigned slot,uint16_t address,uint8_t value,bool high) {
    if(!hdpack_recording())return;
    fetch_tile(&pipeline.sprite[slot],address,value,high);
    if(!high){pipeline.sprite_x[slot]=0;pipeline.sprite[slot].valid=ppu.sprite_row<(ppu.sprite16?16:8);}
    if(high && slot==7)prepare_visual_sprites();
}
void cyc_hdpack_sprite_shift(unsigned slot) {
    if(hdpack_recording() && pipeline.sprite_x[slot]<8)pipeline.sprite_x[slot]++;
}
void cyc_hdpack_clock(void) {
    if(!hdpack_recording())return;
    /* Advancing display delay does not copy four sprite keys per dot. Each
     * new pixel takes the next free slot; repeated colors can share a slot. */
    pipeline.color_slot[3]=pipeline.color_slot[2];
    pipeline.color_slot[2]=pipeline.color_slot[1];
    pipeline.color_slot[1]=pipeline.color_slot[0];
}
void cyc_hdpack_pixel(unsigned bg_color,unsigned bg_palette,int sprite,unsigned sprite_color) {
    if(!hdpack_recording())return;
    pipeline.color_next=(uint8_t)((pipeline.color_next+1)&3);
    pipeline.color_slot[0]=pipeline.color_next;
    HdRecordedPixel *p=&pipeline.color[pipeline.color_next];memset(p,0,sizeof(*p));
    p->bg_index=-1;p->bg_pal[0]=ppu.palette[0];
    const HdFetchedTile *bg=&pipeline.bg[15-ppu.fine_x];
    if(ppu.show_bg && (ppu.dot>8 || ppu.show_bg8) && bg->valid) {
        p->flags=1;p->bg_index=bg->index;memcpy(p->bg_tile,bg->bytes,16);
        p->bg_x=bg->x;p->bg_y=bg->row;p->bg_color=(uint8_t)bg_color;
        for(unsigned j=1;j<4;j++)p->bg_pal[j]=ppu.palette[bg_palette*4+j];
    }
    (void)sprite;(void)sprite_color;
    if(ppu.show_spr && (ppu.dot>8 || ppu.show_spr8)) {
        if(hdpack_unlimited_sprites() && pipeline.visual_valid && pipeline.visual_line==ppu.scanline && ppu.dot>=1 && ppu.dot<=256) {
            const uint8_t *at=pipeline.visual_pixels[ppu.dot-1];
            for(unsigned k=0;k<4 && at[k]!=255;k++) {
                const HdVisualSprite *visual=&pipeline.visual[at[k]];
                const HdFetchedTile *tile=&visual->tile;unsigned attr=visual->attr;
                if(!tile->valid)continue;
                HdRecordedSprite *sp=&p->sp[k];
                sp->index=tile->index;memcpy(sp->tile,tile->bytes,16);
                sp->x=(uint8_t)(ppu.dot-1-visual->x);sp->y=tile->row;
                unsigned bit=(attr&0x40)?sp->x:7-sp->x;
                sp->color=(uint8_t)(((tile->bytes[sp->y]>>bit)&1)|(((tile->bytes[sp->y+8]>>bit)&1)<<1));
                sp->flags=(uint8_t)(1|((attr&0x40)?2:0)|((attr&0x80)?4:0)|((attr&0x20)?8:0));
                for(unsigned j=0;j<3;j++)sp->pal[j]=ppu.palette[16+(attr&3)*4+j+1];
            }
            return;
        }
        unsigned count=0;
        for(unsigned i=0;i<8 && count<HD_MAX_SPRITES;i++) {
            const HdFetchedTile *tile=&pipeline.sprite[i];
            if(!tile->valid || pipeline.sprite_x[i]>=8 || (ppu.spr_x[i] && !ppu.skipped_dot))continue;
            HdRecordedSprite *sp=&p->sp[count++];unsigned attr=ppu.spr_attr[i];
            sp->flags=(uint8_t)(1|((attr&0x40)?2:0)|((attr&0x80)?4:0)|((attr&0x20)?8:0));
            sp->index=tile->index;memcpy(sp->tile,tile->bytes,16);
            sp->x=pipeline.sprite_x[i];sp->y=tile->row;
            sp->color=(uint8_t)(((ppu.spr_lo[i]>>7)&1)|((ppu.spr_hi[i]>>6)&2));
            for(unsigned j=0;j<3;j++)sp->pal[j]=ppu.palette[16+(attr&3)*4+j+1];
        }
    }
}
void cyc_hdpack_blank(uint8_t backdrop) {
    if(!hdpack_recording())return;
    HdRecordedPixel *p=&pipeline.color[pipeline.color_slot[0]];
    memset(p,0,sizeof(*p));p->bg_pal[0]=backdrop;
}
void cyc_hdpack_output(int x,int y) {
    if(!hdpack_recording())return;
    HdRecordedPixel *p=&recorded[y*256+x];*p=pipeline.color[pipeline.color_slot[3]];
    unsigned emphasis=ppu.emphasis;
    if(hw_pal())emphasis=(emphasis&4)|((emphasis&1)<<1)|((emphasis&2)>>1);
    p->mask=(uint8_t)(ppu.greyscale|(emphasis<<5));
}
static uint32_t color(uint8_t value,uint8_t mask) {
    return hw_palette_argb[((mask>>5)*64)+((mask&1)?(value&0x30):(value&0x3f))];
}
static void prepare_pixels(const uint32_t *native) {
    hdpack_invalidate_conditions();
    HdPixel *pixels=hdpack_pixels();
    for(unsigned i=0;i<256*240;i++) {
        const HdRecordedPixel *r=&recorded[i];HdPixel *p=&pixels[i];
        p->bg_index=r->bg_index;p->bg_t16=r->bg_tile;p->bg_has=r->flags&1;
        p->bg_transparent=r->bg_color==0;
        p->bg_p0=r->bg_pal[0];p->bg_p1=r->bg_pal[1];p->bg_p2=r->bg_pal[2];p->bg_p3=r->bg_pal[3];
        p->bg_ox=r->bg_x;p->bg_oy=r->bg_y;
        p->backdrop=color(r->bg_pal[0],r->mask);
        p->bg_argb=color(r->bg_pal[r->bg_color],r->mask);
        for(unsigned j=0;j<HD_MAX_SPRITES;j++)p->sp[j].has=0;
        for(unsigned j=0;j<HD_MAX_SPRITES && (r->sp[j].flags&1);j++) {
            const HdRecordedSprite *rs=&r->sp[j];HdSprite *sp=&p->sp[j];
            sp->index=rs->index;sp->t16=rs->tile;sp->has=rs->flags&1;
            sp->p0=0xff;sp->p1=rs->pal[0];sp->p2=rs->pal[1];sp->p3=rs->pal[2];
            sp->ox=rs->x;sp->oy=rs->y;sp->hm=(rs->flags>>1)&1;sp->vm=(rs->flags>>2)&1;sp->behind=(rs->flags>>3)&1;
            sp->transparent=rs->color==0;
            sp->argb=rs->color?color(rs->pal[rs->color-1],r->mask):native[i];
        }
    }
}
const HdJobFrame *cyc_hdpack_jobs(const uint32_t *native) {
    if(!hdpack_active()){release_jobs();return NULL;}
    uint64_t generation=cyc_render_generation();
    if(prepared_jobs && generation==jobs_generation && jobs_epoch==presentation_epoch)return prepared_jobs;
    prepare_pixels(native);
    const HdJobFrame *next=hdpack_prepare_jobs(native,256);
    release_jobs();prepared_jobs=next;jobs_generation=generation;jobs_epoch=presentation_epoch;
    return prepared_jobs;
}
const uint32_t *cyc_hdpack_thumbnail(const uint32_t *native) {
    const HdJobFrame *frame=cyc_hdpack_jobs(native);
    if(!frame)return native;
    if(thumbnail_serial!=frame->serial){
        if(hdpack_render_jobs(frame,1,thumbnail,256))return native;
        thumbnail_serial=frame->serial;
    }
    return thumbnail;
}
const uint32_t *cyc_hdpack_present(const uint32_t *native,int *width,int *height) {
    *width=256;*height=240;if(!hdpack_active())return native;
    uint64_t generation=cyc_render_generation();
    if(presentation_valid && generation==presented_generation) {
        *width=256*hdpack_scale();*height=240*hdpack_scale();return upscaled;
    }
    const HdJobFrame *frame=cyc_hdpack_jobs(native);
    size_t count=(size_t)256*240*hdpack_scale()*hdpack_scale();
    if(count>upscaled_capacity) {
        uint32_t *next=realloc(upscaled,count*sizeof(*next));if(!next)return native;
        upscaled=next;upscaled_capacity=count;
    }
    if(!frame || hdpack_render_jobs(frame,(unsigned)hdpack_scale(),upscaled,256*hdpack_scale()))
        hdpack_upscale(native,256,upscaled);
    presented_generation=generation;presentation_valid=true;
    *width=256*hdpack_scale();*height=240*hdpack_scale();return upscaled;
}

#define HD_STATE_PART(n) \
static int get##n(uint8_t *b,int c){return save_part(n,b,c);} \
static int set##n(const uint8_t *b,int l){return load_part(n,b,l);} \
static int check##n(const uint8_t *b,int l){return validate_part(n,b,l);}
HD_STATE_PART(0) HD_STATE_PART(1) HD_STATE_PART(2) HD_STATE_PART(3) HD_STATE_PART(4) HD_STATE_PART(5) HD_STATE_PART(6) HD_STATE_PART(7) HD_STATE_PART(8) HD_STATE_PART(9) HD_STATE_PART(10) HD_STATE_PART(11) HD_STATE_PART(12) HD_STATE_PART(13) HD_STATE_PART(14) HD_STATE_PART(15) HD_STATE_PART(16)
static int get_audio(uint8_t *b,int c){size_t n=cyc_hd_audio_save(NULL,0);if(n>(size_t)c)return -1;return (int)cyc_hd_audio_save(b,(size_t)c);}
static int set_audio(const uint8_t *b,int n){return n>=0 && cyc_hd_audio_load_state(b,(size_t)n,true);}
static int check_audio(const uint8_t *b,int n){return n>=0 && cyc_hd_audio_load_state(b,(size_t)n,false);}
NES_MOD_CONSTRUCTOR(register_cycle_hd_state) {
    if(!nes_mod_register_savestate_hook("cyc.hd.audio",get_audio,set_audio) ||
       !nes_mod_register_savestate_validator("cyc.hd.audio",check_audio))
        fprintf(stderr,"[HD audio] Cannot register playback state\n");
    NESModSavestateGet gets[]={get0,get1,get2,get3,get4,get5,get6,get7,get8,get9,get10,get11,get12,get13,get14,get15,get16};
    NESModSavestateSet sets[]={set0,set1,set2,set3,set4,set5,set6,set7,set8,set9,set10,set11,set12,set13,set14,set15,set16};
    NESModSavestateValidate checks[]={check0,check1,check2,check3,check4,check5,check6,check7,check8,check9,check10,check11,check12,check13,check14,check15,check16};
    for(unsigned i=0;i<=HD_STATE_CHUNKS;i++) {
        char id[40];snprintf(id,sizeof id,"cyc.hdpack.%u",i);
        if(!nes_mod_register_savestate_hook(id,gets[i],sets[i]) || !nes_mod_register_savestate_validator(id,checks[i]))
            fprintf(stderr,"[HD pack] Cannot register cycle save record %u\n",i);
    }
}
