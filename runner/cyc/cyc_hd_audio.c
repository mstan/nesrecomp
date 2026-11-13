/* Independent NEA register/player implementation. Vorbis decoding is provided
 * by Sean Barrett's stb_vorbis (MIT/public domain; original license retained).
 * Decoded immutable PCM never enters a state file or a host audio callback. */
#include "cyc_hd_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 2
#include "../external/stb_vorbis.c"

#define NEA_MAX_TRACKS 2048u
#define NEA_MAX_VOICES 4096u
#define NEA_MAX_PCM (UINT64_C(512)*1024*1024)
typedef struct {
    int16_t *pcm;
    uint32_t frames,rate,loop;
    uint16_t id;
    uint8_t channels,sfx;
} NeaTrack;
typedef struct {uint64_t start;uint32_t offset,phase,track,active;} NeaVoice;
typedef struct {
    uint32_t version,tracks,count;
    uint64_t identity,clock_num;
    uint32_t clock_den;
    uint8_t album,options,bgm_volume,sfx_volume,paused,error;
    NeaVoice bgm;
} NeaState;
static NeaTrack *tracks;
static unsigned track_count;
static NeaVoice *voices;
static size_t voice_capacity;
static NeaState state;
static bool available,alternate;
static unsigned trace_plays;
static bool trace_enabled;

void cyc_hd_audio_unload(void) {
    for(unsigned i=0;i<track_count;i++)free(tracks[i].pcm);
    free(tracks);tracks=NULL;track_count=0;free(voices);voices=NULL;voice_capacity=0;
    memset(&state,0,sizeof state);available=alternate=trace_enabled=false;trace_plays=0;
}
static uint64_t identity_word(uint64_t hash,uint32_t value) {
    for(unsigned i=0;i<4;i++)hash=(hash^(uint8_t)(value>>(i*8)))*UINT64_C(1099511628211);
    return hash;
}
void cyc_hd_audio_reset(bool pal) {
    uint64_t identity=state.identity;memset(&state,0,sizeof state);
    if(!available)return;
    state.version=1;state.tracks=track_count;state.identity=identity;
    state.clock_num=pal?UINT64_C(53203425):UINT64_C(21477272);
    state.clock_den=pal?32:12;state.bgm_volume=state.sfx_volume=128;trace_plays=0;
}
static bool number(const char *text,unsigned long limit,uint32_t *value) {
    while(isspace((unsigned char)*text))text++;
    if(!isdigit((unsigned char)*text))return false;
    char *end;unsigned long n=strtoul(text,&end,10);
    while(isspace((unsigned char)*end))end++;
    if(*end || n>limit)return false;
    *value=(uint32_t)n;return true;
}
static bool safe_filename(const char *text) {
    if(!*text || *text=='/' || *text=='\\' || strchr(text,':'))return false;
    const char *p=text;
    while(*p) {
        const char *end=p;while(*end && *end!='/' && *end!='\\')end++;
        if(end-p==2 && p[0]=='.' && p[1]=='.')return false;
        p=*end?end+1:end;
    }
    return true;
}
static bool decode_track(const char *directory,char *text,bool sfx,uint64_t *total) {
    char *fields[5];unsigned count=0;char *p=text;
    do {
        fields[count++]=p;char *comma=strchr(p,',');if(!comma)break;
        *comma=0;p=comma+1;
    } while(count<5);
    if(count<3 || count>4 || (sfx && count!=3) || track_count>=NEA_MAX_TRACKS)return false;
    uint32_t album,id,loop=0;
    if(!number(fields[0],255,&album) || !number(fields[1],255,&id) ||
       (count==4 && !number(fields[3],UINT32_MAX,&loop)))return false;
    char *name=fields[2];while(isspace((unsigned char)*name))name++;
    size_t n=strlen(name);while(n && isspace((unsigned char)name[n-1]))name[--n]=0;
    if(!safe_filename(name))return false;
    char path[2048];int size=snprintf(path,sizeof path,"%s/%s",directory,name);
    if(size<0 || size>=(int)sizeof path)return false;
    int error=0;stb_vorbis *reader=stb_vorbis_open_filename(path,&error,NULL);
    if(!reader){fprintf(stderr,"[HD audio] Cannot decode %s (Vorbis %d)\n",name,error);return false;}
    stb_vorbis_info info=stb_vorbis_get_info(reader);
    unsigned frames=stb_vorbis_stream_length_in_samples(reader);
    uint64_t bytes=(uint64_t)frames*(unsigned)info.channels*sizeof(int16_t);
    if(!frames || info.channels<1 || info.channels>2 || !info.sample_rate ||
       info.sample_rate>384000 || loop>=frames || bytes>NEA_MAX_PCM || *total>NEA_MAX_PCM-bytes) {
        stb_vorbis_close(reader);return false;
    }
    int16_t *pcm=malloc((size_t)bytes);
    if(!pcm){stb_vorbis_close(reader);return false;}
    unsigned done=0;
    while(done<frames) {
        unsigned chunk=frames-done;if(chunk>16384)chunk=16384;
        int got=stb_vorbis_get_samples_short_interleaved(reader,info.channels,
            pcm+(size_t)done*info.channels,(int)(chunk*(unsigned)info.channels));
        if(got<=0)break;
        done+=(unsigned)got;
    }
    stb_vorbis_close(reader);
    if(done!=frames){free(pcm);return false;}
    NeaTrack t={pcm,frames,info.sample_rate,loop,(uint16_t)(album*256+id),(uint8_t)info.channels,sfx};
    unsigned at=0;while(at<track_count && (tracks[at].id!=t.id || tracks[at].sfx!=t.sfx))at++;
    if(at==track_count) {
        NeaTrack *next=realloc(tracks,(track_count+1)*sizeof(*next));
        if(!next){free(pcm);return false;}tracks=next;track_count++;
    } else {
        *total-=(uint64_t)tracks[at].frames*tracks[at].channels*2;
        free(tracks[at].pcm);
    }
    tracks[at]=t;*total+=bytes;return true;
}
bool cyc_hd_audio_load(const char *directory,bool pal) {
    cyc_hd_audio_unload();
    char path[2048];int n=snprintf(path,sizeof path,"%s/hires.txt",directory);
    if(n<0 || n>=(int)sizeof path)return false;
    FILE *file=fopen(path,"rb");if(!file)return false;
    char text[4096];bool valid=true;uint64_t bytes=0;
    while(fgets(text,sizeof text,file)) {
        char *p=text;while(isspace((unsigned char)*p))p++;
        if(*p=='['){char *end=strchr(p,']');if(end)p=end+1;}
        if(!strncmp(p,"<bgm>",5))valid=decode_track(directory,p+5,false,&bytes);
        else if(!strncmp(p,"<sfx>",5))valid=decode_track(directory,p+5,true,&bytes);
        else if(!strncmp(p,"<options>",9) && strstr(p+9,"alternateRegisterRange"))alternate=true;
        if(!valid)break;
    }
    if(ferror(file))valid=false;fclose(file);
    if(!valid || !track_count) {
        if(!valid)fprintf(stderr,"[HD audio] NEA disabled: pack audio could not be qualified\n");
        cyc_hd_audio_unload();return false;
    }
    state.version=1;state.tracks=track_count;
    state.clock_num=pal?UINT64_C(53203425):UINT64_C(21477272);
    state.clock_den=pal?32:12;state.bgm_volume=state.sfx_volume=128;
    uint64_t hash=UINT64_C(1469598103934665603);
    for(unsigned i=0;i<track_count;i++) {
        hash=identity_word(hash,tracks[i].id|((uint32_t)tracks[i].sfx<<16));
        hash=identity_word(hash,tracks[i].frames);hash=identity_word(hash,tracks[i].rate);
        hash=identity_word(hash,tracks[i].loop);hash=identity_word(hash,tracks[i].channels);
    }
    state.identity=hash;available=true;trace_enabled=getenv("NESRECOMP_HD_AUDIO_TRACE")!=NULL;
    fprintf(stderr,"[HD audio] NEA tracks=%u decoded_pcm_bytes=%llu stereo enabled\n",track_count,(unsigned long long)bytes);
    return true;
}
static bool position(const NeaVoice *voice,uint64_t cycle,bool loop,uint32_t *frame) {
    if(!voice->active || voice->track>=track_count || cycle<voice->start)return false;
    const NeaTrack *t=&tracks[voice->track];uint64_t elapsed=cycle-voice->start;
    uint64_t multiplier=(uint64_t)t->rate*state.clock_den;
    if(elapsed>UINT64_MAX/multiplier)return false;
    if(elapsed*multiplier>UINT64_MAX-voice->phase)return false;
    uint64_t at=voice->offset+(elapsed*multiplier+voice->phase)/state.clock_num;
    if(at>=t->frames) {
        if(!loop)return false;
        at=t->loop+(at-t->loop)%(t->frames-t->loop);
    }
    *frame=(uint32_t)at;return true;
}
static void synchronize(uint64_t cycle) {
    uint32_t at;
    if(state.bgm.active && !state.paused) {
        if(!position(&state.bgm,cycle,(state.options&1)!=0,&at))state.bgm.active=0;
    }
    for(unsigned i=0;i<state.count;) {
        if(position(&voices[i],cycle,false,&at)){i++;continue;}
        memmove(voices+i,voices+i+1,(state.count-i-1)*sizeof(*voices));state.count--;
    }
}
static void freeze_bgm(uint64_t cycle) {
    uint32_t at;
    if(!state.bgm.active || state.paused)return;
    if(!position(&state.bgm,cycle,(state.options&1)!=0,&at)){state.bgm.active=0;return;}
    const NeaTrack *t=&tracks[state.bgm.track];
    uint64_t units=(cycle-state.bgm.start)*(uint64_t)t->rate*state.clock_den+state.bgm.phase;
    state.bgm.offset=at;state.bgm.phase=(uint32_t)(units%state.clock_num);state.bgm.start=cycle;
}
static int find_track(uint16_t id,bool sfx) {
    for(unsigned i=0;i<track_count;i++)if(tracks[i].id==id && tracks[i].sfx==sfx)return (int)i;
    return -1;
}
static int register_number(uint16_t address,bool write) {
    if(!available)return -1;
    if(!alternate)return address>=0x4100 && address<=0x4106?(int)(address-0x4100):-1;
    if(write)return address>=0x3002 && address<=0x3062 && (address&15)==2?(int)((address-0x3002)/16):-1;
    return address==0x4018?0:address==0x4019?1:-1;
}
bool cyc_hd_audio_read(uint16_t address,uint64_t cycle,uint8_t *value) {
    int reg=register_number(address,false);if(reg<0)return false;
    synchronize(cycle);*value=0;
    if(reg==0)*value=(uint8_t)((state.bgm.active && !state.paused?1:0)|(state.count?2:0)|(state.error?4:0));
    else if(reg==1)*value=1;
    else if(reg==2)*value='N';else if(reg==3)*value='E';else if(reg==4)*value='A';
    return true;
}
bool cyc_hd_audio_write(uint16_t address,uint8_t value,uint64_t cycle) {
    int reg=register_number(address,true);if(reg<0)return false;
    synchronize(cycle);
    switch(reg) {
        case 0:freeze_bgm(cycle);state.options=value;break;
        case 1:
            if(!state.paused && (value&1))freeze_bgm(cycle);
            if(state.paused && !(value&1))state.bgm.start=cycle;
            state.paused=value&1;
            if(value&2)state.bgm.active=0;
            if(value&4)state.count=0;
            break;
        case 2:state.bgm_volume=value;break;
        case 3:state.sfx_volume=value;break;
        case 4:state.album=value;break;
        case 5: {
            int track=find_track((uint16_t)(state.album*256+value),false);
            state.error=track<0;
            if(track>=0)state.bgm=(NeaVoice){cycle,0,0,(uint32_t)track,1};
            if(trace_enabled && trace_plays++<256)fprintf(stderr,"[HD audio] BGM album=%u id=%u cycle=%llu decoded=%d\n",state.album,value,(unsigned long long)cycle,track>=0);
            break;
        }
        case 6: {
            int track=find_track((uint16_t)(state.album*256+value),true);
            state.error=track<0 || state.count>=NEA_MAX_VOICES;
            if(state.error)break;
            if(state.count>=voice_capacity) {
                size_t capacity=voice_capacity?voice_capacity*2:16;
                NeaVoice *next=realloc(voices,capacity*sizeof(*next));
                if(!next){state.error=1;break;}voices=next;voice_capacity=capacity;
            }
            voices[state.count++]=(NeaVoice){cycle,0,0,(uint32_t)track,1};
            if(trace_enabled && trace_plays++<256)fprintf(stderr,"[HD audio] SFX album=%u id=%u cycle=%llu voices=%u\n",state.album,value,(unsigned long long)cycle,state.count);
            break;
        }
    }
    return true;
}
static int32_t voice_sample(const NeaVoice *voice,uint32_t frame,uint64_t cycle,unsigned channel,unsigned volume,bool loop) {
    const NeaTrack *t=&tracks[voice->track];
    unsigned c=t->channels==1?0:channel;
    uint64_t fraction=(voice->phase+(cycle-voice->start)*(uint64_t)t->rate*state.clock_den)%state.clock_num;
    uint32_t next=frame+1;if(next>=t->frames)next=loop?t->loop:frame;
    int64_t a=t->pcm[(size_t)frame*t->channels+c],b=t->pcm[(size_t)next*t->channels+c];
    int64_t value=(a*(int64_t)(state.clock_num-fraction)+b*(int64_t)fraction)/(int64_t)state.clock_num;
    return (int32_t)(value*volume/255);
}
static int16_t clipped(int64_t value){return (int16_t)(value>32767?32767:value<-32768?-32768:value);}
void cyc_hd_audio_sample(uint64_t cycle,int16_t *left,int16_t *right) {
    int64_t l=0,r=0;uint32_t at;
    if(available) {
        if(!state.paused && position(&state.bgm,cycle,(state.options&1)!=0,&at)) {
            l=voice_sample(&state.bgm,at,cycle,0,state.bgm_volume,(state.options&1)!=0);r=voice_sample(&state.bgm,at,cycle,1,state.bgm_volume,(state.options&1)!=0);
        }
        for(unsigned i=0;i<state.count;i++)if(position(&voices[i],cycle,false,&at)) {
            l+=voice_sample(&voices[i],at,cycle,0,state.sfx_volume,false);r+=voice_sample(&voices[i],at,cycle,1,state.sfx_volume,false);
        }
    }
    *left=clipped(l);*right=clipped(r);
}
size_t cyc_hd_audio_save(void *data,size_t capacity) {
    NeaState saved=state;if(!available)memset(&saved,0,sizeof saved);
    size_t bytes=sizeof saved+(size_t)saved.count*sizeof(NeaVoice);
    if(!data || capacity<bytes)return bytes;
    memcpy(data,&saved,sizeof saved);if(saved.count)memcpy((uint8_t *)data+sizeof saved,voices,saved.count*sizeof(NeaVoice));
    return bytes;
}
static bool valid_voice(const NeaVoice *voice,bool sfx) {
    return voice->active<=1 && voice->phase<state.clock_num && (!voice->active || (voice->track<track_count && tracks[voice->track].sfx==sfx && voice->offset<tracks[voice->track].frames));
}
bool cyc_hd_audio_load_state(const void *data,size_t bytes,bool apply) {
    if(!data || bytes<sizeof(NeaState))return false;
    NeaState next;memcpy(&next,data,sizeof next);
    if(!available) {
        NeaState zero={0};return bytes==sizeof zero && !memcmp(&next,&zero,sizeof zero);
    }
    if(next.version!=1 || next.tracks!=track_count || next.identity!=state.identity ||
       next.clock_num!=state.clock_num || next.clock_den!=state.clock_den || next.paused>1 || next.error>1 ||
       next.count>NEA_MAX_VOICES || bytes!=sizeof next+(size_t)next.count*sizeof(NeaVoice) || !valid_voice(&next.bgm,false))return false;
    const uint8_t *p=(const uint8_t *)data+sizeof next;
    for(unsigned i=0;i<next.count;i++){NeaVoice voice;memcpy(&voice,p+i*sizeof voice,sizeof voice);if(!voice.active || !valid_voice(&voice,true))return false;}
    if(!apply)return true;
    if(next.count>voice_capacity) {
        NeaVoice *new_voices=realloc(voices,(size_t)next.count*sizeof(*voices));if(!new_voices)return false;
        voices=new_voices;voice_capacity=next.count;
    }
    state=next;if(next.count)memcpy(voices,p,next.count*sizeof(*voices));return true;
}
