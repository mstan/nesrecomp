/*
 * apu.h — NES APU emulation interface
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

void    apu_init(void);
void    apu_write(uint16_t addr, uint8_t val);
uint8_t apu_read_status(void);

/* Generate n_samples mono int16 samples into buf.
 * Call once per VBlank (n_samples = 735 at 44100 Hz / 60 fps). */
void    apu_generate(int16_t *buf, int n_samples);

/* Advance the APU frame sequencer's IRQ flag by `cpu_cycles` CPU cycles.
 * NMI-independent: code that runs with NMI disabled (blargg APU tests, any
 * main-thread $4015 poll) still sees the frame IRQ assert.  Call from the
 * per-instruction cycle hook (maybe_trigger_vblank).  4-step mode asserts the
 * frame IRQ once per 29830-cycle sequence unless inhibited; cleared by a $4015
 * read or by setting the inhibit bit. */
void    apu_clock_cycles(int cpu_cycles);
/* Drain the accumulated DMC DMA cycle-steal (4 CPU cycles per sample-byte
 * fetch), to be added to the CPU frame budget. Returns 0 when DMC is idle. */
int     apu_take_dmc_stall(void);

/* True while any APU interrupt source is asserting the CPU IRQ line:
 * the DMC sample-end flag ($4010 bit 7 path) or the frame-counter flag.
 * Polled by the general pending-IRQ delivery hook (runtime.c). Level — stays
 * true until the handler acknowledges the source. */
bool    apu_irq_asserted(void);

/* Audio-fidelity T0 tap: per-channel staging buffer for the frame most
 * recently drained by apu_generate. ch: 0=pulse1 1=pulse2 2=triangle 3=noise
 * 4=dmc. Entries are the sample-window-averaged raw DAC channel inputs,
 * linearly rescaled to int16 (pulse/tri/noise 0..15 -> x2184, dmc 0..127 ->
 * x258). Returns NULL unless RECOMP_AUDIO_DEBUG capture is armed — zero cost
 * otherwise. main_runner pushes these as the t0_* recomp_audio_debug taps. */
const int16_t *apu_debug_t0(int ch);

/* Co-sim: serialize the FULL APU architectural state (all 5 channels + the
 * frame-counter/sequencer phase + DMC-stall accumulator) into buf as a
 * deterministic little-endian byte blob, for the differential co-sim state
 * hash. This is the save-state blind spot (savestate.c omits the APU). Returns
 * the number of bytes written, or 0 if cap is too small. Pure read; no state
 * mutation, no host-only pointers included. */
int     apu_get_state_blob(uint8_t *buf, int cap);
