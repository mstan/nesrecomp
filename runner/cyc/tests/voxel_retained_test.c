/* Differential final pictures, not a mirror of the cache implementation.
 * Include the renderer to inject allocation failure without a production hook. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static int fail_alloc;
static void *test_realloc(void *p, size_t size) {
    return fail_alloc ? NULL : realloc(p,size);
}
#define realloc test_realloc
#include "../../src/voxel_renderer.c"
#undef realloc

uint8_t g_ppu_oam[256],g_ppu_nt[2048],g_ppu_pal[32],g_chr_ram[8192];
uint8_t g_ppuctrl,g_ppumask,g_controller1_buttons;
uint32_t g_nes_palette[64];
static uint8_t tiles[32*22];
static uint32_t original[512*240],reference[512*240],candidate[512*240];
static unsigned animation;

static float height(uint8_t tile,int x,int y,void *user) {
    (void)x;(void)y;(void)user;
    return tile==2?18.0f:tile==3?-2.0f:0.0f;
}
static void pixels(uint32_t *out,int stride,uint8_t tile,int tx,int ty,void *user) {
    (void)user;
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)
        out[y*stride+x]=g_nes_palette[(tile+tx+ty+x+y+animation+g_chr_ram[0])&63];
}
static int billboard(uint8_t tile,int x,int y,int *w,int *h,void *user) {
    (void)user;
    if(tile!=4)return 0;
    if((x&1)||(y&1))return -1;
    *w=*h=2;return 1;
}
static int compare(const NesVoxelScene *scene,int frame) {
    NesVoxelScene a=*scene,b=*scene;
    size_t count=(size_t)a.output_width*a.output_height;
    memcpy(reference,original,count*sizeof(uint32_t));
    memcpy(candidate,original,count*sizeof(uint32_t));
    a.framebuffer=reference;b.framebuffer=candidate;
    if(!nes_voxel_render(&a)||!nes_voxel_render_retained(&b))return 0;
    if(memcmp(reference,candidate,count*sizeof(uint32_t))) {
        for(size_t i=0;i<count;i++)if(reference[i]!=candidate[i]) {
            fprintf(stderr,"frame%d pixel%zu: %08x != %08x\n",frame,i,reference[i],candidate[i]);break;
        }
        return 0;
    }
    return 1;
}
int main(void) {
    NesVoxelScene s;
    uint64_t builds,hits;
    memset(&s,0,sizeof(s));
    for(unsigned i=0;i<64;i++)g_nes_palette[i]=0xff000000u|i*0x030509u;
    for(unsigned i=0;i<32;i++)g_ppu_pal[i]=(uint8_t)i;
    memset(g_chr_ram,0xaa,sizeof(g_chr_ram));
    for(int y=0;y<22;y++)for(int x=0;x<32;x++)
        tiles[y*32+x]=(x==0||x==31||y==0||y==21)?2:1;
    tiles[6*32+12]=tiles[6*32+13]=tiles[7*32+12]=tiles[7*32+13]=4;
    s.output_width=426;s.output_height=240;
    s.source_x=85;s.source_y=64;s.source_width=256;s.source_height=176;
    s.tile_columns=32;s.tile_rows=22;s.tile_stride=32;s.tile_size=8;
    s.tiles=tiles;s.tile_height=height;s.tile_pixels=pixels;s.tile_billboard=billboard;
    s.tile_billboard_scale=1.2f;s.tile_billboard_shadow_scale=.7f;s.tile_billboard_shadow_opacity=.3f;
    s.elevation_degrees=35;s.yaw_degrees=-20;s.camera_distance=285;
    s.draw_oam_sprites=1;s.sprite_scale=1.35f;s.sprite_shadow_scale=.6f;s.sprite_shadow_opacity=.3f;
    s.preserve_top_rows=64;s.extend_preserved_rows=1;s.preserved_rows_fill=0xff000000;
    for(int frame=0;frame<160;frame++) {
        memset(g_ppu_oam,0xff,sizeof(g_ppu_oam));
        g_ppu_oam[0]=120;g_ppu_oam[1]=1;g_ppu_oam[2]=0;g_ppu_oam[3]=(uint8_t)(80+frame%30);
        g_ppu_oam[4]=120;g_ppu_oam[5]=2;g_ppu_oam[6]=0x40;g_ppu_oam[7]=g_ppu_oam[3]+8;
        if(frame==12)animation++;
        if(frame==24)tiles[10*32+10]=2;
        if(frame==36)g_nes_palette[9]^=0x00550f00;
        if(frame==48)g_chr_ram[0]^=1;
        if(frame>=60&&frame<80){s.yaw_degrees+=2;s.roll_degrees+=.25f;}
        if(frame==80){s.output_width=512;s.source_x=128;}
        if(frame==90)s.tile_pixels=NULL; /* live source and OAM contamination */
        if(frame==105)s.tile_pixels=pixels;
        if(frame>=110&&frame<140){s.use_camera_pose=1;s.camera_eye_x=128+(frame-110)*.2f;s.camera_eye_y=8;s.camera_eye_z=160;s.camera_look_at_x=128;s.camera_look_at_y=8;s.camera_look_at_z=0;}
        if(frame==140){nes_voxel_retained_forget();s.use_camera_pose=0;}
        for(int y=0;y<s.output_height;y++)for(int x=0;x<s.output_width;x++)
            original[y*s.output_width+x]=g_nes_palette[(x/8+y/8+(y<64?frame:0))&63];
        if(!compare(&s,frame))return 1;
    }
    nes_voxel_retained_stats(&builds,&hits);
#if defined(NESRECOMP_VOXEL_RETAINED_TERRAIN) && NESRECOMP_VOXEL_RETAINED_TERRAIN
    if(!builds||!hits){fprintf(stderr,"cache not exercised: %llu/%llu\n",(unsigned long long)builds,(unsigned long long)hits);return 1;}
#else
    if(builds||hits)return 1;
#endif
    nes_voxel_retained_forget();fail_alloc=1;
    if(!compare(&s,160))return 1;
    fail_alloc=0;nes_voxel_retained_forget();
    puts("PASS:160 full pictures, animated materials/CHR/palette, geometry, camera/pose, resize, OAM cards/shadows, HUD, cache reuse and allocation-failure fallback");
    return 0;
}
