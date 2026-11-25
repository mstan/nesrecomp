#include "hdpack_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Upstream nothings/stb, commit6b5799bae8a1fe3792691ef4b72ae26434c3e799.
 * The decoder's MIT/public-domain notices are retained in stb_vorbis.c. */
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "stb_vorbis.c"

enum { MAX_TRACKS = 512, MAX_VOICES = 8, STATE_WORDS = 8 + MAX_VOICES * 6 };
typedef struct { unsigned key; char *path; } Track;
typedef struct {
    stb_vorbis *decoder;
    unsigned key, frame, fraction, rate, length, channels;
    int16_t a, b;
    unsigned looping;
} Voice;
static Track tracks[MAX_TRACKS];
static unsigned track_count;
static Voice voices[MAX_VOICES]; /* zero is the single BGM owner */
static unsigned album, loop_music, paused, bgm_volume = 128, sfx_volume = 128;
static unsigned track_error, replacement;

static unsigned key_of(int music, unsigned a, unsigned t) {
    return (music ? 0x10000u : 0) | (a << 8) | t;
}
static const char *path_of(unsigned key) {
    for (unsigned i = 0; i < track_count; ++i)
        if (tracks[i].key == key) return tracks[i].path;
    return NULL;
}
static void close_voice(Voice *v) {
    if (v->decoder) stb_vorbis_close(v->decoder);
    memset(v, 0, sizeof(*v));
}
void hdpack_audio_reset(void) {
    for (unsigned i = 0; i < MAX_VOICES; ++i) close_voice(&voices[i]);
    for (unsigned i = 0; i < track_count; ++i) free(tracks[i].path);
    memset(tracks, 0, sizeof(tracks)); track_count = 0;
    album = loop_music = paused = track_error = replacement = 0;
    bgm_volume = sfx_volume = 128;
}
int hdpack_audio_define(int music, unsigned a, unsigned t,
                        const char *directory, const char *filename) {
    if (a > 255 || t > 255 || !directory || !filename || !*filename ||
        filename[0] == '/' || filename[0] == '\\' || strchr(filename, ':')) return 0;
    /* Imported paths use either separator. Reject complete parent components,
     * rather than rejecting harmless names such as music..ogg. */
    for (const char *p = filename; *p;) {
        const char *end = p;
        while (*end && *end != '/' && *end != '\\') ++end;
        if ((end - p == 2 && p[0] == '.' && p[1] == '.') || end == p) return 0;
        p = *end ? end + 1 : end;
    }
    unsigned key = key_of(music, a, t);
    if (path_of(key) || track_count == MAX_TRACKS) return 0;
    size_t n = strlen(directory), m = strlen(filename);
    if (n > SIZE_MAX - m - 2) return 0;
    char *path = malloc(n + m + 2);
    if (!path) return 0;
    memcpy(path, directory, n); path[n] = '/'; memcpy(path+n+1, filename, m+1);
    for(size_t i=n+1;i<n+m+1;++i) if(path[i]=='\\') path[i]='/';
    tracks[track_count++] = (Track){key, path};
    return 1;
}
int hdpack_audio_available(int music, unsigned a, unsigned t) {
    return a <= 255 && t <= 255 && path_of(key_of(music, a, t)) != NULL;
}
int hdpack_audio_current_music(void) {
    return voices[0].decoder ? (int)(voices[0].key & 0xffff) : -1;
}
static int read_sample(Voice *v, int16_t *out) {
    /* Pull one frame through stb's interleaved float API. Its internal packet
     * buffer amortizes decode; stereo is downmixed into the runner's mono bus. */
    float sample[2];
    int n = stb_vorbis_get_samples_float_interleaved(v->decoder, 2, sample, 2);
    if (!n && v->looping && stb_vorbis_seek_start(v->decoder))
        n = stb_vorbis_get_samples_float_interleaved(v->decoder, 2, sample, 2);
    if (!n) return 0;
    float value = v->channels == 1 ? sample[0] * 32768.0f :
                                    (sample[0] + sample[1]) * 16384.0f;
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    *out = (int16_t)value;
    return 1;
}
static int open_voice(Voice *v, unsigned key, unsigned looping,
                      unsigned frame, unsigned fraction) {
    const char *path = path_of(key);
    if (!path) return 0;
    int e;
    memset(v, 0, sizeof(*v));
    v->decoder = stb_vorbis_open_filename(path, &e, NULL);
    if (!v->decoder) return 0;
    stb_vorbis_info info = stb_vorbis_get_info(v->decoder);
    v->length = stb_vorbis_stream_length_in_samples(v->decoder);
    if (info.sample_rate < 8000 || info.sample_rate > 192000 ||
        info.channels < 1 || info.channels > 2 ||
        !v->length || frame >= v->length ||
        !stb_vorbis_seek(v->decoder, frame)) { close_voice(v); return 0; }
    v->rate = info.sample_rate; v->channels = (unsigned)info.channels;
    v->key = key; v->looping = looping;
    v->frame = frame; v->fraction = fraction;
    if (!read_sample(v, &v->a)) { close_voice(v); return 0; }
    if (!read_sample(v, &v->b)) v->b = v->a;
    return 1;
}
int hdpack_audio_validate_sources(void) {
    for(unsigned i=0;i<track_count;++i) {
        Voice v;
        if(!open_voice(&v,tracks[i].key,0,0,0)) {
            fprintf(stderr,"[HD audio] Invalid or unavailable source: %s\n",tracks[i].path);
            return 0;
        }
        close_voice(&v);
    }
    return 1;
}
static void play(unsigned slot, int music, uint8_t track) {
    Voice next;
    if (!open_voice(&next, key_of(music, album, track), music && loop_music, 0, 0)) {
        track_error = 1;
        fprintf(stderr, "[HD audio] Cannot play %s album%u track%u\n",
                music ? "BGM" : "SFX", album, track);
        return;
    }
    close_voice(&voices[slot]); voices[slot] = next; track_error = 0;
    if (music) paused = 0;
}
void hdpack_audio_write(unsigned reg, uint8_t value) {
    switch (reg & 0xffffu) {
    case 0x4100: loop_music = value & 1; voices[0].looping = loop_music; break;
    case 0x4101:
        paused = value & 1;
        if (value & 2) { close_voice(&voices[0]); paused = 0; }
        if (value & 4) for (unsigned i = 1; i < MAX_VOICES; ++i) close_voice(&voices[i]);
        break;
    case 0x4102: bgm_volume = value; break;
    case 0x4103: sfx_volume = value; break;
    case 0x4104: album = value; break;
    case 0x4105: play(0, 1, value); break;
    case 0x4106: {
        unsigned i;
        for (i = 1; i < MAX_VOICES; ++i) if (!voices[i].decoder) break;
        if (i == MAX_VOICES) { i = 1 + replacement; replacement = (replacement+1) % (MAX_VOICES-1); }
        play(i, 0, value); break;
    }
    }
}
uint8_t hdpack_audio_read(unsigned reg) {
    switch (reg & 0xffffu) {
    case 0x4100: {
        unsigned status = (voices[0].decoder && !paused ? 1 : 0) | (track_error ? 4 : 0);
        for (unsigned i = 1; i < MAX_VOICES; ++i) if (voices[i].decoder) status |= 2;
        return (uint8_t)status;
    }
    case 0x4101: return 1;
    case 0x4102: return 'N';
    case 0x4103: return 'E';
    case 0x4104: return 'A';
    default: return 0;
    }
}
void hdpack_audio_mix(int16_t *samples, size_t count, unsigned rate) {
    if (!samples || !rate) return;
    int playing=0;
    for(unsigned i=0;i<MAX_VOICES;++i)
        if(voices[i].decoder && (i || !paused)) {playing=1;break;}
    if(!playing) return;
    for (size_t n = 0; n < count; ++n) {
        int mixed = samples[n];
        for (unsigned i = 0; i < MAX_VOICES; ++i) {
            Voice *v = &voices[i];
            if (!v->decoder || (!i && paused)) continue;
            /* Fraction is a source-frame fraction in Q32, independent of the
             * host sample rate and therefore valid across device changes. */
            int value = v->a + (int)(((int64_t)v->b-v->a) * v->fraction / 4294967296LL);
            mixed += value * (int)(i ? sfx_volume : bgm_volume) / 255;
            uint64_t phase = (uint64_t)v->fraction + ((uint64_t)v->rate << 32) / rate;
            while (phase >= 4294967296ULL && v->decoder) {
                phase -= 4294967296ULL;
                if (++v->frame == v->length) {
                    if (!v->looping) { close_voice(v); break; }
                    v->frame = 0;
                }
                v->a = v->b;
                if (!read_sample(v, &v->b)) v->b = v->a;
            }
            if (v->decoder) v->fraction = (unsigned)phase;
        }
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        samples[n] = (int16_t)mixed;
    }
}
static void state_put32(uint8_t *p, unsigned value) {
    for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(value>>(i*8));
}
static unsigned state_get32(const uint8_t *p) {
    unsigned value=0;
    for (unsigned i=0;i<4;++i) value|=(unsigned)p[i]<<(i*8);
    return value;
}
size_t hdpack_audio_state_size(void) { return STATE_WORDS*4; }
int hdpack_audio_state_save(uint8_t *out, size_t size) {
    if (!out || size != hdpack_audio_state_size()) return 0;
    unsigned words[STATE_WORDS] = {0x31414448, album, loop_music, paused,
                                 bgm_volume, sfx_volume, track_error, replacement};
    for (unsigned i=0;i<MAX_VOICES;++i) {
        Voice *v=&voices[i]; unsigned *w=words+8+i*6;
        if (v->decoder) { w[0]=1;w[1]=v->key;w[2]=v->frame;w[3]=v->fraction;w[4]=v->looping;w[5]=v->length; }
    }
    for(unsigned i=0;i<STATE_WORDS;++i) state_put32(out+i*4,words[i]);
    return 1;
}
int hdpack_audio_state_validate(const uint8_t *data, size_t size) {
    if (!data || size != hdpack_audio_state_size() || state_get32(data)!=0x31414448 ||
        state_get32(data+4)>255 || state_get32(data+8)>1 || state_get32(data+12)>1 ||
        state_get32(data+16)>255 || state_get32(data+20)>255 || state_get32(data+24)>1 ||
        state_get32(data+28)>=MAX_VOICES-1) return 0;
    for(unsigned i=0;i<MAX_VOICES;++i) {
        const uint8_t *p=data+(8+i*6)*4;
        if (state_get32(p)>1) return 0;
        if (state_get32(p)) {
            unsigned key=state_get32(p+4);
            if (key>0x1ffff || !path_of(key) || !!(key&0x10000)!=!i ||
                state_get32(p+8)>=state_get32(p+20) || state_get32(p+16)>1 || (i&&state_get32(p+16))) return 0;
        } else for(unsigned j=1;j<6;++j) if(state_get32(p+j*4)) return 0;
    }
    return 1;
}
int hdpack_audio_state_load(const uint8_t *data, size_t size) {
    Voice next[MAX_VOICES] = {{0}};
    if (!hdpack_audio_state_validate(data,size)) return 0;
    for(unsigned i=0;i<MAX_VOICES;++i) {
        const uint8_t *p=data+(8+i*6)*4;
        if(state_get32(p) && (!open_voice(&next[i],state_get32(p+4),state_get32(p+16),state_get32(p+8),state_get32(p+12)) ||
                       next[i].length!=state_get32(p+20))) {
            for(unsigned j=0;j<MAX_VOICES;++j) close_voice(&next[j]);
            return 0;
        }
    }
    for(unsigned i=0;i<MAX_VOICES;++i) {close_voice(&voices[i]);voices[i]=next[i];}
    album=state_get32(data+4);loop_music=state_get32(data+8);paused=state_get32(data+12);
    bgm_volume=state_get32(data+16);sfx_volume=state_get32(data+20);track_error=state_get32(data+24);replacement=state_get32(data+28);
    return 1;
}
