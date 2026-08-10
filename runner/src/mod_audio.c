/* See runner/include/mod_audio.h. */
#include "mod_audio.h"

#include <stdlib.h>
#include <string.h>

#define MOD_AUDIO_MAX_CLIPS 64
#define MOD_AUDIO_MAX_VOICES 8

typedef struct ModAudioClipSlot {
    int16_t *samples;
    uint32_t frame_count;
} ModAudioClipSlot;

typedef struct ModAudioVoice {
    NESModAudioClip clip;
    uint32_t position;
    int gain_percent;
    int looping;
} ModAudioVoice;

static ModAudioClipSlot s_clips[MOD_AUDIO_MAX_CLIPS];
static ModAudioVoice s_voices[MOD_AUDIO_MAX_VOICES];
static unsigned s_replace_cursor;

static ModAudioClipSlot *clip_slot(NESModAudioClip clip)
{
    if (clip <= 0 || clip > MOD_AUDIO_MAX_CLIPS) return NULL;
    if (!s_clips[clip - 1].samples) return NULL;
    return &s_clips[clip - 1];
}

NESModAudioClip nes_mod_audio_register_pcm_s16_mono(
    const int16_t *samples, uint32_t frame_count)
{
    int16_t *copy;
    int i;

    if (!samples || frame_count == 0 ||
        frame_count > (uint32_t)(SIZE_MAX / sizeof(*samples)))
        return NES_MOD_AUDIO_CLIP_INVALID;

    for (i = 0; i < MOD_AUDIO_MAX_CLIPS; ++i) {
        if (!s_clips[i].samples) break;
    }
    if (i == MOD_AUDIO_MAX_CLIPS) return NES_MOD_AUDIO_CLIP_INVALID;

    copy = (int16_t *)malloc((size_t)frame_count * sizeof(*copy));
    if (!copy) return NES_MOD_AUDIO_CLIP_INVALID;
    memcpy(copy, samples, (size_t)frame_count * sizeof(*copy));
    s_clips[i].samples = copy;
    s_clips[i].frame_count = frame_count;
    return i + 1;
}

void nes_mod_audio_unregister(NESModAudioClip clip)
{
    ModAudioClipSlot *slot = clip_slot(clip);
    int i;
    if (!slot) return;

    for (i = 0; i < MOD_AUDIO_MAX_VOICES; ++i) {
        if (s_voices[i].clip == clip)
            memset(&s_voices[i], 0, sizeof(s_voices[i]));
    }
    free(slot->samples);
    memset(slot, 0, sizeof(*slot));
}

int nes_mod_audio_play(NESModAudioClip clip, int gain_percent)
{
    int i;
    if (!clip_slot(clip)) return 0;
    if (gain_percent < 0) gain_percent = 0;
    if (gain_percent > 200) gain_percent = 200;

    for (i = 0; i < MOD_AUDIO_MAX_VOICES; ++i) {
        if (s_voices[i].clip == NES_MOD_AUDIO_CLIP_INVALID) break;
    }
    /* A bounded mixer must have deterministic overload behavior. Replace in
     * round-robin order when all eight voices are occupied. */
    if (i == MOD_AUDIO_MAX_VOICES) {
        i = (int)s_replace_cursor;
        s_replace_cursor = (s_replace_cursor + 1u) % MOD_AUDIO_MAX_VOICES;
    }
    s_voices[i].clip = clip;
    s_voices[i].position = 0;
    s_voices[i].gain_percent = gain_percent;
    s_voices[i].looping = 0;
    return 1;
}

int nes_mod_audio_play_loop(NESModAudioClip clip, int gain_percent)
{
    int i;
    if (!clip_slot(clip)) return 0;
    if (gain_percent < 0) gain_percent = 0;
    if (gain_percent > 200) gain_percent = 200;

    /* There is exactly one persistent voice per clip. A state-reconciliation
     * caller can therefore invoke this every emulation frame, including the
     * first frame after a savestate reload, without audibly restarting it. */
    for (i = 0; i < MOD_AUDIO_MAX_VOICES; ++i) {
        if (s_voices[i].clip == clip && s_voices[i].looping) {
            s_voices[i].gain_percent = gain_percent;
            return 1;
        }
    }
    for (i = 0; i < MOD_AUDIO_MAX_VOICES; ++i) {
        if (s_voices[i].clip == NES_MOD_AUDIO_CLIP_INVALID) break;
    }
    if (i == MOD_AUDIO_MAX_VOICES) {
        i = (int)s_replace_cursor;
        s_replace_cursor = (s_replace_cursor + 1u) % MOD_AUDIO_MAX_VOICES;
    }
    s_voices[i].clip = clip;
    s_voices[i].position = 0;
    s_voices[i].gain_percent = gain_percent;
    s_voices[i].looping = 1;
    return 1;
}

void nes_mod_audio_stop_loop(NESModAudioClip clip)
{
    int i;
    for (i = 0; i < MOD_AUDIO_MAX_VOICES; ++i) {
        if (s_voices[i].clip == clip && s_voices[i].looping)
            memset(&s_voices[i], 0, sizeof(s_voices[i]));
    }
}

void nes_mod_audio_stop_all(void)
{
    memset(s_voices, 0, sizeof(s_voices));
    s_replace_cursor = 0;
}

void nes_mod_audio_mix(int16_t *dst, int frame_count)
{
    int i, v;
    if (!dst || frame_count <= 0) return;

    for (i = 0; i < frame_count; ++i) {
        int mixed = dst[i];
        for (v = 0; v < MOD_AUDIO_MAX_VOICES; ++v) {
            ModAudioVoice *voice = &s_voices[v];
            ModAudioClipSlot *slot;
            if (voice->clip == NES_MOD_AUDIO_CLIP_INVALID) continue;
            slot = clip_slot(voice->clip);
            if (!slot || voice->position >= slot->frame_count) {
                memset(voice, 0, sizeof(*voice));
                continue;
            }
            mixed += ((int)slot->samples[voice->position++] *
                      voice->gain_percent) / 100;
            if (voice->position >= slot->frame_count) {
                if (voice->looping)
                    voice->position = 0;
                else
                    memset(voice, 0, sizeof(*voice));
            }
        }
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        dst[i] = (int16_t)mixed;
    }
}
