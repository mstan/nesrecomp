/* MM4's verified donor driver and overlay; no game RAM/physics live here.
 * Compile this component explicitly. Load the donor, configure and reset;
 * queue requests during simulation, tick once at frame end, then mix 48kHz
 * mono samples. Preserve the snapshot in the title's mod-state registry. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void cyc_mm4_audio_configure(bool enabled, bool own_native_channels);
void cyc_mm4_audio_reset(void);
void cyc_mm4_audio_play(int sfx);
void cyc_mm4_audio_stop(int sfx); /* retains the original driver's tail policy */
void cyc_mm4_audio_frame_end(void);
void cyc_mm4_audio_mix(int16_t *samples, size_t count);
uint8_t cyc_mm4_audio_owned_channels(void);
int cyc_mm4_audio_state_get(uint8_t *out, int capacity);
int cyc_mm4_audio_state_valid(const uint8_t *data, int size);
int cyc_mm4_audio_state_set(const uint8_t *data, int size);
#ifdef CYC_MM4_AUDIO_TEST
#include <stdio.h>
void cyc_mm4_audio_selftest_dump(FILE *out, int sfx, int frames);
void cyc_mm4_audio_selftest_render(int sfx, int16_t *out, size_t samples);
#endif
#ifdef __cplusplus
}
#endif
