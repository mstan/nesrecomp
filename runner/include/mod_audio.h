/*
 * mod_audio.h -- small host-side PCM overlay mixer for trusted game mods.
 *
 * Clips are immutable mono signed-16 PCM at the NES runner's 44100 Hz output
 * rate. Registration copies the caller's samples, so a game may release its
 * loader buffer immediately. Playback is mixed into the same frame as the NES
 * APU; it never opens a second audio device and therefore stays synchronized
 * with the runner's existing volume and clock-domain bridge.
 *
 * All functions are called on the emulation thread. The audio callback only
 * consumes the already-mixed bridge output and never touches this state.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int NESModAudioClip;

#define NES_MOD_AUDIO_CLIP_INVALID 0
#define NES_MOD_AUDIO_SAMPLE_RATE 44100

/* Copies `frame_count` mono samples and returns a positive handle, or zero. */
NESModAudioClip nes_mod_audio_register_pcm_s16_mono(
    const int16_t *samples, uint32_t frame_count);

/* Stops voices using the clip and releases its copied sample data. */
void nes_mod_audio_unregister(NESModAudioClip clip);

/* Starts a one-shot. gain_percent is clamped to 0..200. Returns 1 on success. */
int nes_mod_audio_play(NESModAudioClip clip, int gain_percent);

/* Stops every overlay voice without unregistering clips. Save-state loads use
 * this to discard host delivery state instead of replaying a stale call. */
void nes_mod_audio_stop_all(void);

/* Runner-internal producer step: saturating-add active overlays into `dst`. */
void nes_mod_audio_mix(int16_t *dst, int frame_count);

#ifdef __cplusplus
}
#endif
