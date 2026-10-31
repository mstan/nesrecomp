/* Internal HD snapshot representation shared with its contract test.
 * Included once by cyc_hdpack.c after standard headers and hdpack.h.
 * Presentation storage is exact: active/nonzero payloads keep every byte. */
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
typedef struct {uint32_t version,active,scale,part;uint8_t fingerprint[20];} HdStateHeader;
static uint8_t fingerprint[20];
static HdPipeline pipeline;
static HdRecordedPixel recorded[256*240];
static bool presentation_valid;
/* Display-only invalidation, deliberately absent from every serialized part. */
static uint64_t presentation_epoch;

/* Version 3 represents an exactly zero inactive payload. Nonzero and
 * active records retain their complete bytes; no generation/invalidation key. */
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
    size_t size=part==4?sizeof pipeline:sizeof(recorded)/4;
    const void *payload=part==4?(const void *)&pipeline:(const void *)&recorded[part*256*60];
    bool compact=!hdpack_active() && zero_payload(payload,size);
    if(compact)size=0;
    if(cap<(int)(sizeof(HdStateHeader)+size))return -1;
    HdStateHeader h={compact?3u:2u,hdpack_active()!=0,(uint32_t)hdpack_scale(),part,{0}};
    memcpy(h.fingerprint,fingerprint,sizeof fingerprint);memcpy(buf,&h,sizeof h);
    if(size)memcpy(buf+sizeof h,payload,size);
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
    if(!buf || len<(int)sizeof(HdStateHeader))return 0;
    HdStateHeader h;memcpy(&h,buf,sizeof h);
    bool compact=h.version==3;
    if((h.version!=2 && !compact) || h.part!=part || h.active!=(unsigned)(hdpack_active()!=0) || h.scale!=(unsigned)hdpack_scale() || memcmp(h.fingerprint,fingerprint,sizeof fingerprint))return 0;
    if(compact)return !h.active && len==(int)sizeof h;
    if(len!=(int)(sizeof h+size))return 0;
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
    void *payload=part==4?(void *)&pipeline:(void *)&recorded[part*256*60];
    HdStateHeader h;memcpy(&h,buf,sizeof h);
    if(h.version==3)memset(payload,0,size);else memcpy(payload,buf+sizeof h,size);
    presentation_valid=false;++presentation_epoch;return 1;
}
