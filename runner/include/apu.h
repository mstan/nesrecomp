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

/* True while any APU interrupt source is asserting the CPU IRQ line:
 * the DMC sample-end flag ($4010 bit 7 path) or the frame-counter flag.
 * Polled by the general pending-IRQ delivery hook (runtime.c). Level — stays
 * true until the handler acknowledges the source. */
bool    apu_irq_asserted(void);
