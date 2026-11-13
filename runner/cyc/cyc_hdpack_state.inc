/* Internal HD snapshot representation shared with its contract test.
 * Included once by cyc_hdpack.c after standard headers and hdpack.h.
 * Presentation storage is exact: active/nonzero payloads keep every byte. */
typedef struct {
    int32_t index;
    uint8_t bytes[16], row, x, valid;
} HdFetchedTile;
/* Owned keys survive bank changes and current-build save/load. Sixteen
 * native row chunks stay below the registry's per-record 1 MiB limit. */
typedef struct {
    int32_t index;
    uint8_t tile[16],pal[3],x,y,color,flags;
} HdRecordedSprite;
typedef struct {
    int32_t bg_index;
    uint8_t bg_tile[16],bg_pal[4],bg_x,bg_y,flags,mask,bg_color;
    HdRecordedSprite sp[4];
} HdRecordedPixel;
#define HD_STATE_CHUNKS 16
#define HD_STATE_ROWS (240/HD_STATE_CHUNKS)
typedef struct {
    HdFetchedTile tile;
    uint8_t x,attr,slot;
} HdVisualSprite;
typedef struct {
    HdFetchedTile bg_fetch,bg[16],sprite[8];
    uint8_t sprite_x[8];
    HdRecordedPixel color[4];
    HdVisualSprite visual[64];
    uint8_t visual_pixels[256][4],visual_line,visual_valid;
} HdPipeline;
typedef struct {uint32_t version,active,scale,part;uint8_t fingerprint[20];} HdStateHeader;
static uint8_t fingerprint[20];
static HdPipeline pipeline;
static HdRecordedPixel recorded[256*240];
static bool presentation_valid;
/* Display-only invalidation, deliberately absent from every serialized part. */
static uint64_t presentation_epoch;

/* Version 5 represents an exactly zero inactive payload. Version 4 active
 * records keep all four sprite keys; neither format stores invalidation keys. */
static bool zero_payload(const void *data,size_t n) {
    const uint8_t *p=data;
    while(n>=sizeof(uint64_t)) {
        uint64_t value;memcpy(&value,p,sizeof value);
        if(value)return false;
        p+=sizeof value;n-=sizeof value;
    }
    while(n--)if(*p++)return false;
    return true;
}

static int save_part(unsigned part,uint8_t *buf,int cap) {
    size_t size=part==HD_STATE_CHUNKS?sizeof pipeline:sizeof(recorded)/HD_STATE_CHUNKS;
    const void *payload=part==HD_STATE_CHUNKS?(const void *)&pipeline:(const void *)&recorded[part*256*HD_STATE_ROWS];
    bool compact=!hdpack_active() && zero_payload(payload,size);
    if(compact)size=0;
    if(cap<(int)(sizeof(HdStateHeader)+size))return -1;
    HdStateHeader h={compact?5u:4u,hdpack_active()!=0,(uint32_t)hdpack_scale(),part,{0}};
    memcpy(h.fingerprint,fingerprint,sizeof fingerprint);memcpy(buf,&h,sizeof h);
    if(size)memcpy(buf+sizeof h,payload,size);
    return (int)(sizeof h+size);
}
_Static_assert(sizeof(recorded)/HD_STATE_CHUNKS+sizeof(HdStateHeader)<=1024*1024,"HD pixel state chunk");
static bool valid_pixel(const HdRecordedPixel *p) {
    if(p->flags>1 || p->bg_x>7 || p->bg_y>7 || p->bg_color>3)return false;
    for(unsigned j=0;j<4;j++)if(p->bg_pal[j]>63)return false;
    for(unsigned i=0;i<4;i++) {
        const HdRecordedSprite *sp=&p->sp[i];
        if(sp->x>7 || sp->y>7 || sp->color>3 || sp->flags>15)return false;
        for(unsigned j=0;j<3;j++)if(sp->pal[j]>63)return false;
    }
    return true;
}
static int validate_part(unsigned part,const uint8_t *buf,int len) {
    size_t size=part==HD_STATE_CHUNKS?sizeof pipeline:sizeof(recorded)/HD_STATE_CHUNKS;
    if(part>HD_STATE_CHUNKS || !buf || len<(int)sizeof(HdStateHeader))return 0;
    HdStateHeader h;memcpy(&h,buf,sizeof h);
    bool compact=h.version==5;
    if((h.version!=4 && !compact) || h.part!=part || h.active!=(unsigned)(hdpack_active()!=0) || h.scale!=(unsigned)hdpack_scale() || memcmp(h.fingerprint,fingerprint,sizeof fingerprint))return 0;
    if(compact)return !h.active && len==(int)sizeof h;
    if(len!=(int)(sizeof h+size))return 0;
    if(part==HD_STATE_CHUNKS) {
        HdPipeline v;memcpy(&v,buf+sizeof h,sizeof v);
        if(v.bg_fetch.row>7 || v.bg_fetch.x>7 || v.bg_fetch.valid>1)return 0;
        for(unsigned i=0;i<16;i++)if(v.bg[i].row>7 || v.bg[i].x>7 || v.bg[i].valid>1)return 0;
        for(unsigned i=0;i<8;i++)if(v.sprite[i].row>7 || v.sprite[i].valid>1 || v.sprite_x[i]>8)return 0;
        for(unsigned i=0;i<4;i++)if(!valid_pixel(&v.color[i]))return 0;
        if(v.visual_valid>1 || v.visual_line>=240)return 0;
        for(unsigned i=0;i<64;i++) {
            const HdVisualSprite *sp=&v.visual[i];
            if(sp->tile.valid>1 || sp->tile.row>7 || sp->tile.x>7 || (sp->slot>7 && sp->slot!=255))return 0;
        }
        for(unsigned x=0;x<256;x++)for(unsigned i=0;i<4;i++)if(v.visual_pixels[x][i]>63 && v.visual_pixels[x][i]!=255)return 0;
    } else {
        for(unsigned i=0;i<256*HD_STATE_ROWS;i++){HdRecordedPixel p;memcpy(&p,buf+sizeof h+i*sizeof p,sizeof p);if(!valid_pixel(&p))return 0;}
    }
    return 1;
}
static int load_part(unsigned part,const uint8_t *buf,int len) {
    if(!validate_part(part,buf,len))return 0;
    size_t size=part==HD_STATE_CHUNKS?sizeof pipeline:sizeof(recorded)/HD_STATE_CHUNKS;
    void *payload=part==HD_STATE_CHUNKS?(void *)&pipeline:(void *)&recorded[part*256*HD_STATE_ROWS];
    HdStateHeader h;memcpy(&h,buf,sizeof h);
    if(h.version==5)memset(payload,0,size);else memcpy(payload,buf+sizeof h,size);
    presentation_valid=false;++presentation_epoch;return 1;
}
