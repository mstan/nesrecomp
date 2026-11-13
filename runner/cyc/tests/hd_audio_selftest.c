#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "../cyc_hd_audio.c"

static uint8_t status(uint64_t now) {uint8_t value=255;assert(cyc_hd_audio_read(0x4100,now,&value));return value;}
static void write_reg(unsigned reg,uint8_t value,uint64_t now){assert(cyc_hd_audio_write((uint16_t)(0x4100+reg),value,now));}
static void sample(uint64_t now,int left,int right) {
    int16_t l,r;cyc_hd_audio_sample(now,&l,&r);assert(l==left && r==right);
}
static void controlled(void) {
    uint8_t value=0;assert(!cyc_hd_audio_read(0x4102,0,&value));
    tracks=calloc(2,sizeof(*tracks));assert(tracks);track_count=2;
    for(unsigned i=0;i<2;i++) {
        tracks[i]=(NeaTrack){calloc(16,sizeof(int16_t)),8,1000,2,1,2,(uint8_t)i};assert(tracks[i].pcm);
        for(unsigned k=0;k<8;k++){tracks[i].pcm[k*2]=(int16_t)((k+1)*1000);tracks[i].pcm[k*2+1]=(int16_t)(-((int)k+1)*1000);}
    }
    available=true;state.version=1;state.tracks=2;state.identity=123;state.clock_num=1000;state.clock_den=1;
    state.bgm_volume=state.sfx_volume=255;
    assert(cyc_hd_audio_read(0x4102,0,&value) && value=='N');
    assert(cyc_hd_audio_read(0x4103,0,&value) && value=='E');
    assert(cyc_hd_audio_read(0x4104,0,&value) && value=='A');
    write_reg(5,1,0);
    for(unsigned k=0;k<8;k++){assert(status(k)==1);sample(k,(k+1)*1000,-((int)k+1)*1000);}
    assert(!status(8));sample(8,0,0); // polling must not reset elapsed fractions
    write_reg(0,1,10);write_reg(5,1,10);sample(18,3000,-3000);assert(status(18)==1);
    write_reg(1,1,19);assert(status(19)==0);sample(100,0,0);
    write_reg(1,0,100);sample(100,4000,-4000);sample(101,5000,-5000);
    write_reg(5,255,101);assert(status(101)==5); // invalid selection preserves music
    write_reg(5,1,102);assert(status(102)==1);
    write_reg(1,2,102);write_reg(6,1,102);write_reg(6,1,104);
    assert(status(105)==2);sample(105,6000,-6000);
    write_reg(3,0,105);sample(105,0,0);assert(status(105)==2);
    write_reg(3,255,105);write_reg(5,1,105);
    size_t size=cyc_hd_audio_save(NULL,0);void *saved=malloc(size),*same=malloc(size);assert(saved && same);
    assert(cyc_hd_audio_save(saved,size)==size && cyc_hd_audio_load_state(saved,size,false));
    int16_t expected[20];for(unsigned k=0;k<10;k++)cyc_hd_audio_sample(105+k,&expected[k*2],&expected[k*2+1]);
    write_reg(1,6,200);assert(!status(200));assert(cyc_hd_audio_load_state(saved,size,true));
    for(unsigned k=0;k<10;k++)sample(105+k,expected[k*2],expected[k*2+1]);
    assert(cyc_hd_audio_save(same,size)==size && !memcmp(saved,same,size));
    ((NeaState *)saved)->count=NEA_MAX_VOICES+1;assert(!cyc_hd_audio_load_state(saved,size,true));
    assert(cyc_hd_audio_save(saved,size)==size && !memcmp(saved,same,size));
    free(saved);free(same);
    write_reg(1,6,300);state.clock_num=3000;write_reg(0,0,300);write_reg(5,1,300);
    assert(status(301)==1);sample(301,1333,-1333);
    write_reg(1,1,301);write_reg(1,1,450);sample(450,0,0);
    write_reg(1,0,500);sample(500,1333,-1333);sample(502,2000,-2000);
    for(unsigned now=501;now<523;now++)assert(status(now)==1);
    assert(!status(523)); // sub-sample phase survives pause and repeated polling
    write_reg(5,1,530);cyc_hd_audio_reset(true);
    assert(!status(0) && state.clock_num==53203425 && state.clock_den==32);
    assert(state.bgm_volume==128 && state.sfx_volume==128 && !state.options && !state.paused);
    cyc_hd_audio_unload();
    puts("NEA control: stereo, polling, completion, loop, pause, volumes, polyphony, errors and state passed");
}
int main(int argc,char **argv) {
    controlled();
    if(argc<2)return 0;
    assert(cyc_hd_audio_load(argv[1],false));
    uint64_t nonzero=0,stereo=0,hash=UINT64_C(1469598103934665603);
    for(unsigned i=0;i<track_count;i++) {
        NeaTrack *t=&tracks[i];
        for(size_t k=0;k<(size_t)t->frames*t->channels;k++) {
            int16_t v=t->pcm[k];nonzero+=v!=0;
            hash=(hash^(uint8_t)v)*UINT64_C(1099511628211);hash=(hash^(uint8_t)((uint16_t)v>>8))*UINT64_C(1099511628211);
        }
        if(t->channels==2)for(unsigned k=0;k<t->frames;k++)stereo+=t->pcm[k*2]!=t->pcm[k*2+1];
        printf("track %u %s id=%u frames=%u rate=%u channels=%u loop=%u\n",i,t->sfx?"sfx":"bgm",t->id,t->frames,t->rate,t->channels,t->loop);
    }
    assert(nonzero && stereo);printf("decoded tracks=%u nonzero=%llu stereo=%llu hash=%016llx\n",track_count,(unsigned long long)nonzero,(unsigned long long)stereo,(unsigned long long)hash);
    cyc_hd_audio_unload();
    if(argc>2) {
        assert(!cyc_hd_audio_load(argv[2],false));uint8_t value=0;
        assert(!cyc_hd_audio_read(0x4102,0,&value));
        puts("Invalid asset: NEA signature and replacement playback remain disabled");
    }
    return 0;
}
