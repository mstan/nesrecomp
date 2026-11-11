/* Pose crops and anchors follow the corrected MM2 v0.3.14 implementation.
 * Artwork stays in each game's credited package; this file contains no art.
 * Immutable pixels work with both CHR-RAM and CHR-ROM title compositors. */
#include "cyc_protoman_atlas.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#include "stb_image.h"

typedef struct { int x, y, w, h, origin_x, y_adjust; } Rect;
static const Rect RECTS[CYC_PROTO_POSE_COUNT] = {
    {60,270,24,32,0,1}, {85,270,24,32,0,1}, {110,270,24,32,0,1}, {135,270,24,32,0,1},
    {160,270,32,32}, {51,336,32,32,0,6},
    {193,270,32,32,0,1}, {226,270,24,32,0,1}, {251,278,32,24,0,1}, {317,270,32,32,20},
    {350,270,32,32,0,1}, {383,278,32,24,0,1}, {416,270,32,32,0,1},
    {284,270,32,32,21,1}, {284,270,32,32,21,1}, {0,0,26,29,13,6},
    {67,303,24,32}, {67,303,24,32}, {99,303,26,32,19}, {132,303,26,32,19},
    {1,311,32,24,0,3}, {2,270,6,32}, {35,270,24,32}, {9,270,25,32},
    {165,303,24,32}, {165,303,24,32}, {193,303,30,32,15,1}, {223,303,32,29,17},
    {256,303,32,32,15}, {289,303,32,32,15}, {322,303,32,32,15},
    {110,270,24,32,0,1}, {135,270,24,32,0,1},
};

static bool fail(CycProtoAtlas *out, char *why, size_t cap, const char *message) {
    if(out) memset(out, 0, sizeof *out);
    if(why && cap) snprintf(why, cap, "%s", message);
    return false;
}
static uint32_t pixel(const uint8_t *rgba, int width, int x, int y) {
    const uint8_t *p = rgba + ((size_t)y * width + x) * 4;
    return 0xff000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}
static uint32_t crc32(const uint8_t *bytes, size_t count) {
    uint32_t crc=0xffffffffu;
    for(size_t i=0;i<count;i++) {
        crc^=bytes[i];
        for(int b=0;b<8;b++) crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return crc^0xffffffffu;
}
static int color_index(CycProtoAtlas *out, int *count, uint32_t color) {
    for(int i=0;i<*count;i++) if(out->colors[i]==color) return i;
    if(*count==6) return -1;
    out->colors[*count]=color;
    return (*count)++;
}

bool cyc_protoman_atlas_load(CycProtoAtlas *out,
    const uint8_t *sheet, size_t sheet_size,
    const uint8_t *open_jump, size_t jump_size,
    char *why, size_t why_size) {
    if(!out || !sheet || !open_jump || sheet_size!=44372 || jump_size>INT_MAX)
        return fail(out,why,why_size,"Missing or unsupported character art");
    if(crc32(sheet,sheet_size)!=0x49caffc7u)
        return fail(out,why,why_size,"Unsupported character sheet revision");
    int w,h,channels,jw,jh,jc;
    uint8_t *rgba=stbi_load_from_memory(sheet,(int)sheet_size,&w,&h,&channels,4);
    uint8_t *jump=stbi_load_from_memory(open_jump,(int)jump_size,&jw,&jh,&jc,4);
    if(!rgba || w!=466 || h!=602 || !jump || jw!=26 || jh!=29) {
        stbi_image_free(rgba);stbi_image_free(jump);
        return fail(out,why,why_size,"Invalid character art dimensions");
    }
    memset(out,0,sizeof *out);
    uint32_t bg0=pixel(rgba,w,465,269),bg1=pixel(rgba,w,60,270);
    for(int y=0;y<16;y++) for(int x=0;x<16;x++) {
        uint32_t c=pixel(rgba,w,274+x,197+y);
        out->portrait[y*16+x]=(c==bg0 || c==bg1)?0:c;
    }
    int colors=0;
    bool ok=true;
    for(int p=0;p<CYC_PROTO_POSE_COUNT && ok;p++) {
        const Rect *r=&RECTS[p];
        CycProtoPose *pose=&out->poses[p];
        pose->width=(uint8_t)r->w;pose->height=(uint8_t)r->h;
        pose->origin_x=(int8_t)(r->origin_x?r->origin_x:r->w/2);
        pose->top_y=(int8_t)(-r->h+11+r->y_adjust);
        /* Match MM2's tile/layer traversal to preserve its palette identity. */
        for(int layer=0;layer<2 && ok;layer++)
            for(int ty=0;ty<r->h && ok;ty+=8)
                for(int tx=0;tx<r->w && ok;tx+=8)
                    for(int y=0;y<8 && y+ty<r->h && ok;y++)
                        for(int x=0;x<8 && x+tx<r->w;x++) {
                            uint32_t c;
                            if(p==CYC_PROTO_JUMP_OPEN) {
                                const uint8_t *q=jump+((ty+y)*jw+tx+x)*4;
                                if(!q[3]) continue;
                                c=0xff000000u|(uint32_t)q[0]<<16|(uint32_t)q[1]<<8|q[2];
                                if(c!=bg0 && c!=bg1) {
                                    int nearest=-1,distance=13;
                                    for(int i=0;i<colors;i++) {
                                        int dr=(int)q[0]-(int)((out->colors[i]>>16)&255);
                                        int dg=(int)q[1]-(int)((out->colors[i]>>8)&255);
                                        int db=(int)q[2]-(int)(out->colors[i]&255);
                                        int d=dr*dr+dg*dg+db*db;
                                        if(d<distance){distance=d;nearest=i;}
                                    }
                                    if(nearest<0){ok=false;break;}
                                    c=out->colors[nearest];
                                }
                            } else {
                                const Rect *sample=p<=CYC_PROTO_IDLE3 && ty+y>=20?&RECTS[CYC_PROTO_IDLE0]:r;
                                c=pixel(rgba,w,sample->x+tx+x,sample->y+ty+y);
                            }
                            if(c==bg0 || c==bg1) continue;
                            int ci=color_index(out,&colors,c);
                            if(ci<0){ok=false;break;}
                            if(ci/3==layer) pose->pixels[(ty+y)*32+tx+x]=(uint8_t)(ci+1);
                        }
    }
    stbi_image_free(rgba);stbi_image_free(jump);
    if(!ok || colors!=6) return fail(out,why,why_size,"Unsupported character art palette");
    if(why && why_size) why[0]=0;
    return true;
}
