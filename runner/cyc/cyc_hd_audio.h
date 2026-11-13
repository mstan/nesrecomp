#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* HD-pack NEA is available only after every declared Ogg asset decodes.
 * Logical playback uses CPU cycles; host mute and audio polling do not clock it. */
bool cyc_hd_audio_load(const char *directory,bool pal);
void cyc_hd_audio_unload(void);
void cyc_hd_audio_reset(bool pal); /* keep decoded assets, reset the device */
bool cyc_hd_audio_read(uint16_t address,uint64_t cycle,uint8_t *value);
bool cyc_hd_audio_write(uint16_t address,uint8_t value,uint64_t cycle);
void cyc_hd_audio_sample(uint64_t cycle,int16_t *left,int16_t *right);
size_t cyc_hd_audio_save(void *data,size_t capacity);
bool cyc_hd_audio_load_state(const void *data,size_t size,bool apply);
