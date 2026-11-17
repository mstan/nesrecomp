/* Audio-only 2A03 voices. Reuse the cycle engine's channels, DMC and output
 * stage without advancing CPU/PPU/mapper time, DMA stalls or controller input.
 * Calls belong to the emulation thread, outside isolated scopes. Expansion
 * audio is excluded. The caller supplies a pure sample-ROM reader and a
 * timestamped register stream, normally captured from its native driver.
 * Voice state is a local presentation continuation, never gameplay state. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CycApuVoice CycApuVoice;
typedef uint8_t (*CycApuSampleRead)(void *ctx,uint16_t addr);
typedef struct {uint32_t cycle;uint16_t addr;uint8_t value;} CycApuWrite;
CycApuVoice *cyc_apu_voice_new(unsigned rate,CycApuSampleRead read,void *ctx);
void cyc_apu_voice_free(CycApuVoice *voice);
bool cyc_apu_voice_run(CycApuVoice *voice,const CycApuWrite *writes,size_t count,
                       unsigned cycles,uint8_t audible_channels);
size_t cyc_apu_voice_read(CycApuVoice *voice,int16_t *samples,size_t count);
size_t cyc_apu_voice_state_size(void);
bool cyc_apu_voice_state_get(CycApuVoice *voice,void *out,size_t size);
bool cyc_apu_voice_state_valid(CycApuVoice *voice,const void *data,size_t size);
bool cyc_apu_voice_state_set(CycApuVoice *voice,const void *data,size_t size);
#ifdef __cplusplus
}
#endif
