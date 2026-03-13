/*
 * apu.h — NES APU emulation interface
 */
#pragma once
#include <stdint.h>

void    apu_init(void);
void    apu_write(uint16_t addr, uint8_t val);
uint8_t apu_read_status(void);

/* Generate n_samples mono int16 samples into buf.
 * Call once per VBlank (n_samples = 735 at 44100 Hz / 60 fps). */
void    apu_generate(int16_t *buf, int n_samples);
