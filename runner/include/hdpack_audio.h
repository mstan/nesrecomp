/* Mesen enhanced audio, shared by trusted native game hooks and HD hosts.
 * Ogg streams are decoded on the simulation thread into the existing mono
 * output clock. No extra device, fully decoded music copy, or guest CPU runs.
 * The imported pack owns the files. No source audio is included in releases. */
#pragma once
#include <stddef.h>
#include <stdint.h>

void hdpack_audio_reset(void);
int hdpack_audio_define(int music, unsigned album, unsigned track,
                        const char *directory, const char *filename);
int hdpack_audio_available(int music, unsigned album, unsigned track);
int hdpack_audio_current_music(void); /* album*256+track, or -1 */
int hdpack_audio_validate_sources(void);
void hdpack_audio_write(unsigned reg, uint8_t value);
uint8_t hdpack_audio_read(unsigned reg);
void hdpack_audio_mix(int16_t *samples, size_t count, unsigned sample_rate);
/* Pointer-free, current-build state. Load opens/seeks every staged voice before
 * replacing the current set; an unavailable source leaves playback intact. */
size_t hdpack_audio_state_size(void);
int hdpack_audio_state_save(uint8_t *out, size_t size);
int hdpack_audio_state_validate(const uint8_t *data, size_t size);
int hdpack_audio_state_load(const uint8_t *data, size_t size);
