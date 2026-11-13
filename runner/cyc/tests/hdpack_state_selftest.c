/* Assertions remain active in release test builds. */
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int active;
int hdpack_active(void){return active;}
int hdpack_scale(void){return active?2:1;}
#include "../cyc_hdpack_state.inc"

static int validate_checked(unsigned part,const uint8_t *buf,int len) {
    uint64_t before=presentation_epoch;
    bool valid=presentation_valid;
    int result=validate_part(part,buf,len);
    assert(presentation_epoch==before && presentation_valid==valid);
    return result;
}

static int load_checked(unsigned part,const uint8_t *buf,int len) {
    uint64_t before=presentation_epoch;
    presentation_valid=true; /* stand in for a cached picture from the discarded future */
    int result=load_part(part,buf,len);
    assert(presentation_epoch==before+(result?UINT64_C(1):UINT64_C(0)));
    assert(presentation_valid==(result?false:true));
    return result;
}

static void check_epoch_free_save(unsigned part,const uint8_t *bytes,int len,
                                  uint8_t *scratch,int cap) {
    uint64_t changed=presentation_epoch^UINT64_C(0x9e3779b97f4a7c15);
    presentation_epoch=changed;
    assert(save_part(part,scratch,cap)==len && !memcmp(bytes,scratch,(size_t)len));
    assert(presentation_epoch==changed);
}

int main(void) {
    uint8_t *buf=malloc(1024*1024),*again=malloc(1024*1024),*expected=malloc(1024*1024);
    assert(buf && again && expected);
    size_t full_total=0,compact_total=0;
    for(unsigned part=0;part<=HD_STATE_CHUNKS;part++) {
        uint8_t *data=part==HD_STATE_CHUNKS?(void *)&pipeline:(void *)&recorded[part*256*HD_STATE_ROWS];
        size_t size=part==HD_STATE_CHUNKS?sizeof pipeline:sizeof recorded/HD_STATE_CHUNKS;
        memset(data,0,size);active=0;
        int n=save_part(part,buf,1024*1024);assert(n==(int)sizeof(HdStateHeader));
        full_total+=size+sizeof(HdStateHeader);compact_total+=n;
        assert(validate_checked(part,buf,n));assert(save_part(part,again,n)==n && !memcmp(buf,again,n));
        assert(save_part(part,again,n-1)==-1);
        memset(data,0x7f,size);assert(load_checked(part,buf,n));
        memset(expected,0,size);assert(!memcmp(data,expected,size));
        assert(!presentation_valid);
        check_epoch_free_save(part,buf,n,again,1024*1024);
        for(int len=0;len<n;len++)assert(!validate_checked(part,buf,len));
        assert(!validate_checked(part,buf,n+1));assert(!validate_checked(part,NULL,0));
        assert(!load_checked(part,buf,n-1));
        HdStateHeader good;memcpy(&good,buf,sizeof good);
        for(unsigned field=0;field<5;field++) {
            HdStateHeader bad=good;
            if(field==0)bad.version=17;
            if(field==1)bad.active=1;
            if(field==2)bad.scale=2;
            if(field==3)bad.part=part+1;
            if(field==4)bad.fingerprint[19]^=1;
            memcpy(buf,&bad,sizeof bad);memset(data,1,size);memcpy(expected,data,size);
            assert(!load_checked(part,buf,n));assert(!memcmp(expected,data,size));
        }
        memcpy(buf,&good,sizeof good);active=1;assert(!load_checked(part,buf,n));active=0;
        /* Both ends, word boundaries, and scattered native-byte changes must
         * force the full representation, including bytes in struct padding. */
        const size_t fixed[]={0,1,7,8,9,15,16};
        for(unsigned trial=0;trial<42;trial++) {
            size_t pos=trial<7?fixed[trial]:trial==7?size-1:(trial*2654435761u)%size;
            memset(data,0,size);data[pos]=1;
            int full=save_part(part,buf,1024*1024);assert(full==(int)(sizeof(HdStateHeader)+size));
            assert(!memcmp(buf+sizeof(HdStateHeader),data,size));assert(validate_checked(part,buf,full));
            memcpy(expected,data,size);memset(data,0x7f,size);
            assert(load_checked(part,buf,full));assert(!memcmp(data,expected,size));
            if(!trial)check_epoch_free_save(part,buf,full,again,1024*1024);
            assert(save_part(part,again,full-1)==-1);
        }
        /* Active pixel records omit exactly zero sprites. The pipeline and
         * dense worst case retain the complete lossless representation. */
        active=1;memset(data,0,size);
        int full=save_part(part,buf,1024*1024);assert(full<=(int)(sizeof(HdStateHeader)+size));
        HdStateHeader h;memcpy(&h,buf,sizeof h);assert(h.version==(part<HD_STATE_CHUNKS?6u:8u) && h.active==1);
        assert(load_checked(part,buf,full));
        check_epoch_free_save(part,buf,full,again,1024*1024);
        if(part==HD_STATE_CHUNKS) {
            /* A frame-end phase alone can hide a missing cursor restore.
             * Preserve a partially shifted queue with distinct fetched keys. */
            pipeline.bg_head=5;
            for(unsigned k=0;k<16;k++) {
                pipeline.bg[k].valid=1;pipeline.bg[k].row=(uint8_t)(k&7);
                pipeline.bg[k].x=(uint8_t)((k+3)&7);pipeline.bg[k].index=(int32_t)(100+k);
                pipeline.bg[k].bytes[7]=(uint8_t)(k*7);
            }
            memcpy(expected,data,size);full=save_part(part,buf,1024*1024);
            memset(data,0,size);assert(load_checked(part,buf,full));assert(!memcmp(data,expected,size));
            assert(pipeline.bg_head==5);
            for(unsigned fine=0;fine<8;fine++)assert(pipeline.bg[(pipeline.bg_head+15-fine)&15].index==100+(int)((20-fine)&15));
            memcpy(again,buf,full);again[sizeof(HdStateHeader)+offsetof(HdPipeline,bg_head)]=16;
            assert(!load_checked(part,again,full));assert(!memcmp(data,expected,size));
            memcpy(again,buf,full);HdStateHeader old;memcpy(&old,again,sizeof old);old.version=4;memcpy(again,&old,sizeof old);
            assert(!load_checked(part,again,full));assert(!memcmp(data,expected,size));
        } else {
            HdRecordedPixel *pixels=(HdRecordedPixel *)data;
            for(unsigned k=0;k<4;k++){pixels[k*37].sp[k].flags=1;pixels[k*37].sp[k].tile[15]=(uint8_t)(k+1);}
            memcpy(expected,data,size);int sparse=save_part(part,buf,1024*1024);
            assert(sparse<(int)(sizeof(HdStateHeader)+size));
            memset(data,0x7f,size);assert(load_checked(part,buf,sparse));assert(!memcmp(data,expected,size));
            memcpy(again,buf,sparse);again[sizeof(HdStateHeader)+HD_PIXEL_PREFIX]=16;
            assert(!load_checked(part,again,sparse));assert(!memcmp(data,expected,size));
            assert(!load_checked(part,buf,sparse-1));assert(!memcmp(data,expected,size));
            assert(!load_checked(part,buf,sparse+1));assert(!memcmp(data,expected,size));
            for(unsigned k=0;k<256*HD_STATE_ROWS;k++)for(unsigned j=0;j<4;j++)pixels[k].sp[j].flags=1;
            memcpy(expected,data,size);full=save_part(part,buf,1024*1024);
            memcpy(&h,buf,sizeof h);assert(h.version==4 && full==(int)(sizeof(HdStateHeader)+size));
            memset(data,0,size);assert(load_checked(part,buf,full));assert(!memcmp(data,expected,size));
        }
        active=0;assert(!load_checked(part,buf,full));
    }
    printf("HD state exact_payload_roundtrips=210 full_bytes=%zu compact_bytes=%zu saved_bytes=%zu\n",full_total,compact_total,full_total-compact_total);
    free(buf);free(again);free(expected);return 0;
}
