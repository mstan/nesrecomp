#include "hdpack_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) {fprintf(stderr,"line%d: %s\n",__LINE__,#c);return 1;} } while(0)
static void word(uint8_t *p,unsigned v) {for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(i*8));}
static int audible(const int16_t *p,size_t n) {for(size_t i=0;i<n;++i)if(p[i])return 1;return 0;}
int main(int argc,char **argv) {
    CHECK(argc==2);
    CHECK(hdpack_audio_state_size()==224);
    CHECK(hdpack_audio_read(0x4101)==1);
    CHECK(hdpack_audio_read(0x4102)=='N' && hdpack_audio_read(0x4103)=='E' && hdpack_audio_read(0x4104)=='A');
    CHECK(!hdpack_audio_define(1,256,0,argv[1],"mono.ogg"));
    CHECK(!hdpack_audio_define(1,3,1,argv[1],"../mono.ogg"));
    CHECK(!hdpack_audio_define(1,3,1,argv[1],"sub\\..\\mono.ogg"));
    CHECK(!hdpack_audio_define(1,3,1,argv[1],"C:/mono.ogg"));
    CHECK(hdpack_audio_define(1,3,1,argv[1],"mono.ogg"));
    CHECK(hdpack_audio_validate_sources());
    CHECK(hdpack_audio_define(1,3,2,argv[1],"missing.ogg"));
    CHECK(!hdpack_audio_validate_sources());
    CHECK(!hdpack_audio_define(1,3,1,argv[1],"mono.ogg"));
    CHECK(hdpack_audio_define(0,3,1,argv[1],"stereo.ogg"));
    int16_t a[8000]={0},b[8000]={0}; uint8_t state[224],bad[224],after[224];
    hdpack_audio_write(0x4104,3);
    hdpack_audio_write(0x4105,1);
    CHECK(hdpack_audio_read(0x4100)==1);
    hdpack_audio_mix(a,8000,44100);
    CHECK(audible(a,8000) && !(hdpack_audio_read(0x4100)&1));
    /* Decode a differently sampled stereo SFX on the same clock. */
    hdpack_audio_write(0x4106,1); memset(a,0,sizeof a);
    hdpack_audio_mix(a,8000,44100);
    CHECK(audible(a,8000) && !(hdpack_audio_read(0x4100)&2));
    hdpack_audio_write(0x4100,1); hdpack_audio_write(0x4105,1);
    memset(a,0,sizeof a);hdpack_audio_mix(a,713,48000);
    CHECK(audible(a,713) && hdpack_audio_state_save(state,sizeof state));
    memset(a,0,sizeof a);hdpack_audio_mix(a,8000,48000);
    CHECK(hdpack_audio_read(0x4100)&1);
    CHECK(hdpack_audio_state_load(state,sizeof state));
    /* A state restores the decoder's exact source frame and interpolation
     * fraction, independent of delivery chunk boundaries and loop wrap. */
    for(unsigned i=0;i<8000;i+=125)hdpack_audio_mix(b+i,125,48000);
    CHECK(memcmp(a,b,sizeof a)==0);
    hdpack_audio_write(0x4101,1);
    CHECK(hdpack_audio_state_save(state,sizeof state));
    hdpack_audio_write(0x4101,1); /* level, not toggle */
    memset(a,0,sizeof a);hdpack_audio_mix(a,8000,44100);
    CHECK(!audible(a,8000));
    CHECK(hdpack_audio_state_save(after,sizeof after) && !memcmp(state,after,sizeof state));
    hdpack_audio_write(0x4101,0);hdpack_audio_write(0x4102,0);
    memset(a,0,sizeof a);hdpack_audio_mix(a,100,44100);CHECK(!audible(a,100));
    hdpack_audio_write(0x4102,255);
    CHECK(hdpack_audio_state_save(state,sizeof state));
    memcpy(bad,state,sizeof bad);bad[0]^=1;
    CHECK(!hdpack_audio_state_load(bad,sizeof bad));
    CHECK(hdpack_audio_state_save(after,sizeof after) && !memcmp(state,after,sizeof state));
    memcpy(bad,state,sizeof bad);word(bad+36,0x10302); /* declared source absent */
    CHECK(hdpack_audio_state_validate(bad,sizeof bad));
    CHECK(!hdpack_audio_state_load(bad,sizeof bad));
    CHECK(hdpack_audio_state_save(after,sizeof after) && !memcmp(state,after,sizeof state));
    hdpack_audio_write(0x4106,77);CHECK(hdpack_audio_read(0x4100)&4);
    hdpack_audio_write(0x4106,1);CHECK(!(hdpack_audio_read(0x4100)&4));
    hdpack_audio_write(0x4101,6);CHECK(hdpack_audio_read(0x4100)==0);
    hdpack_audio_reset();CHECK(!hdpack_audio_available(1,3,1));
    puts("HD audio: mono/stereo resampling, EOF/loops, pause/gain, exact restore, transactional failure, identity/path and playback controls pass");
    return 0;
}
