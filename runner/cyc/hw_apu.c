/*
 * hw_apu.c - NESRecomp's 2A03 outside the CPU: the APU (frame counter, length
 * counters, DMC, the four tone channels and the mixer), the two DMAs that
 * pause the CPU (sprite DMA from $4014, sample DMA for the DMC) and the
 * controller ports.
 *
 * What the CPU can observe here - $4015, the IRQ line, when DMAs take cycles
 * and what they read, the controller shift registers - is cycle accurate and
 * checked against the oracle. The APU is clocked once per CPU cycle, and
 * alternates between "get" and "put" cycles (the two halves of an APU
 * cycle); the delays of $4015 and $4017 writes and of DMA starts depend on
 * which half the write lands in (AccuracyCoin "APU Register Activation",
 * "DMA" pages; timings as measured with AccuracyCoin/TriCNES).
 *
 * The tone generators (pulse duty and sweep, envelopes, triangle linear
 * counter, noise LFSR) are not visible to the CPU and are synthesized from
 * the nesdev wiki descriptions only when audio output is enabled.
 */
#include "hw_internal.h"

#include "cyc_trace.h"
#include "hw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum { CH_PULSE1, CH_PULSE2, CH_TRIANGLE, CH_NOISE };

typedef struct {
    uint8_t count;
    uint8_t halt;          /* halt bit as of the previous APU clock */
    uint8_t reload;        /* a length write waiting for this cycle's clock */
    uint8_t reload_value;
    uint8_t enabled;       /* $4015 */
} LengthCounter;

typedef struct {
    uint8_t  start, divider, decay;
} Envelope;

typedef struct {
    uint16_t period, timer;
    uint8_t  step;
    Envelope env;
    uint8_t  sweep_divider, sweep_reload;
} Pulse;

typedef struct {
    /* Frame counter. */
    uint8_t  put;                  /* this CPU cycle is an APU put cycle */
    uint8_t  five_step, irq_inhibit;
    uint8_t  reset_delay;          /* cycles until the $4017 write resets it; bit 7 = idle */
    uint16_t counter;
    uint8_t  quarter, half;
    uint8_t  frame_irq, frame_irq_clear;
    uint8_t  frame_irq_out;        /* the frame interrupt on the IRQ output (apu_sample_irq) */

    LengthCounter len[4];
    uint8_t  reg[16];              /* $4000-$400F as last written */

    /* DMC. */
    uint8_t  dmc_irq_enable, dmc_loop, dmc_irq;
    uint16_t dmc_rate, dmc_timer;
    uint8_t  dmc_level;
    uint16_t dmc_sample_addr, dmc_sample_len, dmc_bytes, dmc_addr;
    uint8_t  dmc_buffer, dmc_bits, dmc_silent;
    uint8_t  dmc_have_buffer, dmc_shifter, dmc_out_silent; /* the output unit (audio) */
    uint8_t  dmc_playing;          /* $4015 bit 4 as the DMC acts on it */
    uint8_t  dmc_enable;           /* $4015 bit 4 as written */
    uint8_t  dmc_enable_delay;     /* cycles until dmc_playing follows dmc_enable */
    uint8_t  dmc_start_delay;      /* cycles until a $4015 write's DMA */
    uint8_t  dmc_dma_cooldown;
    uint8_t  implicit_abort, implicit_abort_armed;

    /* DMAs. */
    uint8_t  dmc_dma, dmc_dma_halt;
    uint8_t  oam_dma, oam_dma_first, oam_dma_halt, oam_dma_aligned;
    uint8_t  oam_dma_page, oam_dma_index, oam_dma_value;

    /* Controller ports. */
    uint8_t  strobe, strobed;
    uint8_t  shift[2], shift_delay[2], buttons[2];

    /* Tone generators (audio only). */
    Pulse    pulse[2];
    uint16_t tri_period, tri_timer;
    uint8_t  tri_step, tri_linear, tri_linear_reload, tri_out;
    uint16_t noise_period, noise_timer, noise_lfsr;
    Envelope noise_env;
} HwApu;

static HwApu apu;

static const uint8_t length_table[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};
static const uint16_t dmc_rate_table[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54,
};
static const uint16_t noise_period_table[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};

/* ------------------------------------------------------------------------- */
/* Audio                                                                     */
/* ------------------------------------------------------------------------- */

#define AUDIO_RING 32768u
static struct {
    bool     on;
    double   cycles_per_sample, phase;
    double   acc;
    uint32_t count;
    double   hp90_prev_in, hp90_prev_out, hp440_prev_in, hp440_prev_out, lp_out;
    double   hp90_a, hp440_a, lp_a;
    int16_t  ring[AUDIO_RING];
    uint32_t head, tail;
    float    pulse_mix[31], tnd_mix[203];
} audio;

void apu_audio_enable(bool on, int sample_rate)
{
    audio.on = on && sample_rate > 0;
    if (!audio.on) return;
    const double cpu_rate = 21477272.0 / 12.0;
    audio.cycles_per_sample = cpu_rate / sample_rate;
    audio.phase = audio.acc = 0;
    audio.count = 0;
    /* The console's output stage: two high-pass filters (90 Hz, 440 Hz) and a
     * 14 kHz low-pass (nesdev wiki, APU Mixer). */
    double dt = 1.0 / sample_rate;
    double rc90 = 1.0 / (2 * 3.14159265358979 * 90), rc440 = 1.0 / (2 * 3.14159265358979 * 440),
           rc14k = 1.0 / (2 * 3.14159265358979 * 14000);
    audio.hp90_a = rc90 / (rc90 + dt);
    audio.hp440_a = rc440 / (rc440 + dt);
    audio.lp_a = dt / (rc14k + dt);
    for (int i = 0; i < 31; i++) audio.pulse_mix[i] = i ? (float)(95.52 / (8128.0 / i + 100)) : 0;
    for (int i = 0; i < 203; i++) audio.tnd_mix[i] = i ? (float)(163.67 / (24329.0 / i + 100)) : 0;
}

size_t apu_audio_read(int16_t *out, size_t max)
{
    size_t n = 0;
    while (n < max && audio.tail != audio.head) {
        out[n++] = audio.ring[audio.tail];
        audio.tail = (audio.tail + 1) % AUDIO_RING;
    }
    return n;
}

static void audio_emit(double level)
{
    double hp = audio.hp90_a * (audio.hp90_prev_out + level - audio.hp90_prev_in);
    audio.hp90_prev_in = level;
    audio.hp90_prev_out = hp;
    double hp2 = audio.hp440_a * (audio.hp440_prev_out + hp - audio.hp440_prev_in);
    audio.hp440_prev_in = hp;
    audio.hp440_prev_out = hp2;
    audio.lp_out += audio.lp_a * (hp2 - audio.lp_out);
    double s = audio.lp_out * 30000.0;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    uint32_t next = (audio.head + 1) % AUDIO_RING;
    if (next == audio.tail) audio.tail = (audio.tail + 1) % AUDIO_RING; /* overrun: drop the oldest */
    audio.ring[audio.head] = (int16_t)s;
    audio.head = next;
}

/* The sweep's target period. Pulse 1 negates by ones' complement, pulse 2 by
 * two's complement; a negated target below 0 never mutes the channel. */
static int sweep_target(int ch)
{
    const Pulse *p = &apu.pulse[ch];
    uint8_t r = apu.reg[1 + 4 * ch];
    int change = p->period >> (r & 7);
    if (r & 0x08) {
        int target = p->period - change - (ch == 0 ? 1 : 0);
        return target < 0 ? 0 : target;
    }
    return p->period + change;
}

static inline uint8_t envelope_volume(const Envelope *e, uint8_t reg)
{
    return (reg & 0x10) ? (reg & 15) : e->decay;
}

static void clock_envelope(Envelope *e, uint8_t reg)
{
    if (e->start) {
        e->start = 0;
        e->decay = 15;
        e->divider = reg & 15;
    } else if (e->divider == 0) {
        e->divider = reg & 15;
        if (e->decay) e->decay--;
        else if (reg & 0x20) e->decay = 15;
    } else {
        e->divider--;
    }
}

static void audio_quarter_frame(void)
{
    clock_envelope(&apu.pulse[0].env, apu.reg[0]);
    clock_envelope(&apu.pulse[1].env, apu.reg[4]);
    clock_envelope(&apu.noise_env, apu.reg[12]);
    if (apu.tri_linear_reload) apu.tri_linear = apu.reg[8] & 0x7F;
    else if (apu.tri_linear) apu.tri_linear--;
    if (!(apu.reg[8] & 0x80)) apu.tri_linear_reload = 0;
}

static void audio_half_frame(void)
{
    for (int ch = 0; ch < 2; ch++) {
        Pulse *p = &apu.pulse[ch];
        uint8_t r = apu.reg[1 + 4 * ch];
        int target = sweep_target(ch);
        if (p->sweep_divider == 0 && (r & 0x80) && (r & 7) && p->period >= 8 && target <= 0x7FF)
            p->period = (uint16_t)target;
        if (p->sweep_divider == 0 || p->sweep_reload) {
            p->sweep_divider = (r >> 4) & 7;
            p->sweep_reload = 0;
        } else {
            p->sweep_divider--;
        }
    }
}

void apu_channel_levels(uint8_t out[5])
{
    /* The duty waveforms in output order from the step a $4003/$4007 write
     * resets to; bit 7 - step set = high (nesdev wiki, APU Pulse). */
    static const uint8_t duty[4] = {0x40, 0x60, 0x78, 0x9F};
    for (int ch = 0; ch < 2; ch++) {
        const Pulse *p = &apu.pulse[ch];
        uint8_t r = apu.reg[4 * ch];
        bool high = (duty[r >> 6] >> (7 - p->step)) & 1;
        out[ch] = (uint8_t)((high && apu.len[ch].count && p->period >= 8 && sweep_target(ch) <= 0x7FF)
                                ? envelope_volume(&p->env, r) : 0);
    }
    out[2] = apu.tri_out;
    out[3] = (uint8_t)((apu.len[CH_NOISE].count && !(apu.noise_lfsr & 1)) ? envelope_volume(&apu.noise_env, apu.reg[12]) : 0);
    out[4] = apu.dmc_level;
}

bool apu_irq_output(void) { return (apu.frame_irq_out | apu.dmc_irq) != 0; }

uint16_t apu_noise_lfsr(void) { return apu.noise_lfsr; }

void apu_debug_set_dmc_timer(uint16_t cycles) { apu.dmc_timer = cycles; }

void apu_debug_set_noise(uint16_t lfsr, uint16_t timer)
{
    apu.noise_lfsr = lfsr;
    apu.noise_timer = timer;
}

void apu_debug_dmc(ApuDmcView *v)
{
    v->addr = apu.dmc_addr;
    v->bytes = apu.dmc_bytes;
    v->timer = apu.dmc_timer;
    v->buffer = apu.dmc_buffer;
    v->have_buffer = apu.dmc_have_buffer;
    v->shifter = apu.dmc_shifter;
    v->bits = apu.dmc_bits;
    v->out_silent = apu.dmc_out_silent;
    v->playing = apu.dmc_playing;
    v->enable = apu.dmc_enable;
    v->dma = apu.dmc_dma;
}

static void audio_cycle(void)
{
    /* Timers: the pulse channels' count APU cycles, the others CPU cycles. */
    if (!apu.put) {
        for (int ch = 0; ch < 2; ch++) {
            Pulse *p = &apu.pulse[ch];
            if (p->timer == 0) {
                p->timer = p->period;
                p->step = (p->step + 1) & 7;
            } else {
                p->timer--;
            }
        }
    }
    if (apu.noise_timer == 0) {
        apu.noise_timer = (uint16_t)(apu.noise_period - 1);
        unsigned tap = (apu.reg[14] & 0x80) ? 6 : 1;
        unsigned fb = (apu.noise_lfsr ^ (apu.noise_lfsr >> tap)) & 1;
        apu.noise_lfsr = (uint16_t)((apu.noise_lfsr >> 1) | (fb << 14));
    } else {
        apu.noise_timer--;
    }
    if (apu.tri_timer == 0) {
        apu.tri_timer = apu.tri_period;
        if (apu.len[CH_TRIANGLE].count && apu.tri_linear) apu.tri_step = (apu.tri_step + 1) & 31;
    } else {
        apu.tri_timer--;
    }
    /* Periods 0 and 1 step at ultrasonic rates that the output stages filter
     * to a constant; hold the last level rather than alias it. */
    if (apu.tri_period >= 2) apu.tri_out = (uint8_t)(apu.tri_step < 16 ? 15 - apu.tri_step : apu.tri_step - 16);

    uint8_t out[5];
    apu_channel_levels(out);
    double level = audio.pulse_mix[out[0] + out[1]] + audio.tnd_mix[3 * out[2] + 2 * out[3] + out[4]];

    level += hw_cart_audio_level();
    audio.acc += level;
    audio.count++;
    audio.phase += 1.0;
    if (audio.phase >= audio.cycles_per_sample) {
        audio.phase -= audio.cycles_per_sample;
        audio_emit(audio.acc / audio.count);
        audio.acc = 0;
        audio.count = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Registers                                                                 */
/* ------------------------------------------------------------------------- */

static void dmc_restart(void)
{
    apu.dmc_addr = apu.dmc_sample_addr;
    apu.dmc_bytes = apu.dmc_sample_len;
}

static void length_write(int ch, uint8_t value)
{
    if (apu.len[ch].enabled) {
        apu.len[ch].reload_value = length_table[value >> 3];
        apu.len[ch].reload = 1;
    }
}

void apu_write(uint16_t addr, uint8_t value)
{
    if (addr <= 0x400F) apu.reg[addr & 15] = value;
    switch (addr) {
    case 0x4002:
    case 0x4006: {
        Pulse *p = &apu.pulse[(addr - 0x4002) / 4];
        p->period = (uint16_t)((p->period & 0x700) | value);
        break;
    }
    case 0x4001:
    case 0x4005:
        apu.pulse[(addr - 0x4001) / 4].sweep_reload = 1;
        break;
    case 0x4003:
    case 0x4007: {
        int ch = (addr - 0x4003) / 4;
        Pulse *p = &apu.pulse[ch];
        length_write(ch, value);
        p->period = (uint16_t)((p->period & 0xFF) | ((value & 7) << 8));
        p->step = 0;
        p->env.start = 1;
        break;
    }
    case 0x4008:
        break;
    case 0x400A:
        apu.tri_period = (uint16_t)((apu.tri_period & 0x700) | value);
        break;
    case 0x400B:
        length_write(CH_TRIANGLE, value);
        apu.tri_period = (uint16_t)((apu.tri_period & 0xFF) | ((value & 7) << 8));
        apu.tri_linear_reload = 1;
        break;
    case 0x400E:
        apu.noise_period = noise_period_table[value & 15];
        break;
    case 0x400F:
        length_write(CH_NOISE, value);
        apu.noise_env.start = 1;
        break;
    case 0x4010:
        apu.dmc_irq_enable = (value & 0x80) != 0;
        apu.dmc_loop = (value & 0x40) != 0;
        apu.dmc_rate = dmc_rate_table[value & 15];
        if (!apu.dmc_irq_enable) apu.dmc_irq = 0;
        break;
    case 0x4011:
        apu.dmc_level = value & 0x7F;
        break;
    case 0x4012:
        apu.dmc_sample_addr = (uint16_t)(0xC000 | (value << 6));
        break;
    case 0x4013:
        apu.dmc_sample_len = (uint16_t)((value << 4) | 1);
        break;
    case 0x4014:
        apu.oam_dma = 1;
        apu.oam_dma_first = 1;
        apu.oam_dma_index = 0;
        apu.oam_dma_page = value;
        break;
    case 0x4015:
        apu.dmc_enable = (value & 0x10) != 0;
        for (int ch = 0; ch < 4; ch++) {
            apu.len[ch].enabled = (value >> ch) & 1;
            if (!apu.len[ch].enabled) apu.len[ch].count = 0;
        }
        /* The DMC acts on bit 4 1.5 or 2 APU cycles later. */
        apu.dmc_enable_delay = apu.put ? 3 : 4;
        if (apu.dmc_enable && apu.dmc_bytes == 0) {
            dmc_restart();
            if (apu.dmc_silent) apu.dmc_start_delay = 2;
        }
        apu.dmc_irq = 0;
        /* Explicit abort: disabling on the cycle a sample DMA is about to
         * start lets that DMA run, and the disable lands later. */
        if (!apu.dmc_enable &&
            ((apu.dmc_timer == 2 && !apu.put) || (apu.dmc_timer == apu.dmc_rate && apu.put)))
            apu.dmc_enable_delay = apu.put ? 5 : 6;
        /* Implicit abort: enabling just before the output unit needs a new
         * byte schedules a one-cycle DMA that is cancelled again. */
        if (apu.dmc_enable && ((apu.dmc_timer == 10 && !apu.put) || (apu.dmc_timer == 8 && apu.put)))
            apu.implicit_abort_armed = 1;
        break;
    case 0x4016:
        apu.strobe = value & 1;
        if (!apu.strobe) apu.strobed = 0;
        break;
    case 0x4017:
        apu.five_step = (value & 0x80) != 0;
        apu.irq_inhibit = (value & 0x40) != 0;
        if (apu.five_step) apu.half = apu.quarter = 1;
        if (apu.irq_inhibit) apu.frame_irq = apu.frame_irq_out = 0;
        apu.reset_delay = apu.put ? 3 : 4;
        break;
    default:
        break;
    }
}

uint8_t apu_read_status(void)
{
    uint8_t v = hw.internal_bus & 0x20;
    v |= apu.dmc_irq ? 0x80 : 0;
    v |= apu.frame_irq ? 0x40 : 0;
    /* Bit 4 follows the write immediately even though the DMC stops later. */
    v |= (apu.dmc_bytes != 0 && apu.dmc_enable) ? 0x10 : 0;
    for (int ch = 0; ch < 4; ch++) v |= apu.len[ch].count ? (1 << ch) : 0;
    hw.internal_bus = v;
    apu.frame_irq_clear = 1;
    return v;
}

uint8_t apu_read_controller(int port)
{
    uint8_t bit = (apu.shift[port] & 0x80) ? 1 : 0;
    /* The shift happens on the next APU cycles; a second consecutive read
     * postpones it. */
    apu.shift_delay[port] = 2;
    apu.strobed = 0;
    return bit;
}

bool hw_oam_dma_active(void) { return apu.oam_dma != 0; }

void hw_set_controller(int port, uint8_t buttons)
{
    apu.buttons[port & 1] = buttons;
}

/* ------------------------------------------------------------------------- */
/* Clock                                                                     */
/* ------------------------------------------------------------------------- */

static void clock_length_counters(void)
{
    if (apu.half) {
        for (int ch = 0; ch < 4; ch++) {
            LengthCounter *l = &apu.len[ch];
            /* A length write on a half-frame clock only takes if the counter
             * was already 0. */
            if (l->reload && l->count == 0) l->count = l->reload_value;
            else l->reload = 0;
            if (!l->enabled) l->count = 0;
            if (l->count && !l->halt && !l->reload) l->count--;
        }
    } else {
        for (int ch = 0; ch < 4; ch++) {
            LengthCounter *l = &apu.len[ch];
            if (l->reload) l->count = l->reload_value;
            l->reload = 0;
        }
    }
}

void apu_cycle(void)
{
    if (apu.strobe | apu.shift_delay[0] | apu.shift_delay[1]) {
        for (int port = 0; port < 2; port++) {
            if (apu.strobe) apu.shift_delay[port] = 0;
            else if (apu.shift_delay[port] && --apu.shift_delay[port] == 0)
                apu.shift[port] = (uint8_t)(apu.shift[port] << 1 | 1);
        }
    }

    if (!apu.put) {
        if (apu.strobe) {
            if (!apu.strobed) {
                apu.strobed = 1;
                apu.shift[0] = apu.buttons[0];
                apu.shift[1] = apu.buttons[1];
            }
        } else {
            apu.strobed = 0;
        }

        apu.dmc_timer -= 2;
        if (apu.dmc_timer == 0) {
            apu.dmc_timer = apu.dmc_rate;
            if (!apu.dmc_out_silent) {
                if (apu.dmc_shifter & 1) {
                    if (apu.dmc_level <= 125) apu.dmc_level += 2;
                } else if (apu.dmc_level >= 2) {
                    apu.dmc_level -= 2;
                }
            }
            apu.dmc_shifter >>= 1;
            if (--apu.dmc_bits == 0) {
                /* Output cycle ends. The output unit takes the sample buffer
                 * if a DMA has filled it, and is silent otherwise. */
                apu.dmc_bits = 8;
                apu.dmc_out_silent = !apu.dmc_have_buffer;
                if (apu.dmc_have_buffer) {
                    apu.dmc_shifter = apu.dmc_buffer;
                    apu.dmc_have_buffer = 0;
                }
                /* When the next DMA is scheduled (measured timing; dmc_silent
                 * is this model's view of an empty buffer for $4015). */
                if (apu.dmc_bytes > 0 || apu.implicit_abort_armed) {
                    if (!apu.dmc_dma && apu.dmc_dma_cooldown != 2) {
                        apu.dmc_dma = 1;
                        apu.dmc_dma_halt = 1;
                    }
                    if (apu.implicit_abort_armed) {
                        apu.implicit_abort = 1;
                        apu.implicit_abort_armed = 0;
                    }
                    apu.dmc_silent = 0;
                } else {
                    apu.dmc_silent = 1;
                }
            }
        }
        if (apu.dmc_dma_cooldown > 0) apu.dmc_dma_cooldown -= 2;
    } else {
        if (apu.frame_irq_clear) apu.frame_irq_clear = apu.frame_irq = apu.frame_irq_out = 0;
        if (apu.dmc_start_delay > 0 && --apu.dmc_start_delay == 0 && !apu.dmc_dma) {
            apu.dmc_dma = 1;
            apu.dmc_dma_halt = 1;
            apu.dmc_silent = 0;
        }
    }
    if (apu.dmc_enable_delay > 0 && --apu.dmc_enable_delay == 0) {
        apu.dmc_playing = apu.dmc_enable;
        if (!apu.dmc_playing) apu.dmc_bytes = 0;
    }

    /* Frame counter. A $4017 write resets it 3-4 cycles later. */
    if (!(apu.reset_delay & 0x80)) {
        apu.reset_delay--;
        if (apu.reset_delay & 0x80) apu.counter = 0;
    }
    apu.counter++;
    switch (apu.counter) {
    case 7457: apu.quarter = 1; break;
    case 14913: apu.quarter = apu.half = 1; break;
    case 22371: apu.quarter = 1; break;
    default:
        if (apu.five_step) {
            if (apu.counter == 37281) apu.quarter = apu.half = 1;
            else if (apu.counter == 37282) apu.counter = 0;
        } else if (apu.counter == 29828) {
            apu.frame_irq = 1;
        } else if (apu.counter == 29829) {
            apu.quarter = apu.half = 1;
            apu.frame_irq = 1;
            if (!apu.irq_inhibit) apu.frame_irq_out = 1;
        } else if (apu.counter == 29830) {
            /* The flag is visible for these cycles even with IRQs inhibited. */
            apu.frame_irq = !apu.irq_inhibit;
            if (!apu.irq_inhibit) apu.frame_irq_out = 1;
            apu.counter = 0;
        }
        break;
    }

    if (apu.quarter) {
        apu.quarter = 0;
        if (audio.on) audio_quarter_frame();
    }
    if (apu.half && audio.on) audio_half_frame();
    if (apu.half | apu.len[0].reload | apu.len[1].reload | apu.len[2].reload | apu.len[3].reload)
        clock_length_counters();
    apu.half = 0;

    apu.len[CH_PULSE1].halt = (apu.reg[0] & 0x20) != 0;
    apu.len[CH_PULSE2].halt = (apu.reg[4] & 0x20) != 0;
    apu.len[CH_TRIANGLE].halt = (apu.reg[8] & 0x80) != 0;
    apu.len[CH_NOISE].halt = (apu.reg[12] & 0x20) != 0;

    if (audio.on) audio_cycle();
    apu.put = !apu.put;
}

/* The IRQ output is the frame interrupt OR the DMC interrupt, so acknowledging
 * one leaves the other asserted (nesdev wiki, APU). The frame interrupt reaches
 * it a sample after its $4015 flag is set (flag at 29828, IRQ from 29829 as
 * measured) and leaves it with the flag; the DMC flag drives it directly.
 *
 * /IRQ is one open-drain line shared with the cartridge, so a mapper's
 * interrupt (MMC3's scanline counter) is wired in parallel: a handler that
 * acknowledges the APU still sees the line held by the cartridge. */
void apu_sample_irq(void)
{
    hw.irq_line = apu.frame_irq_out | apu.dmc_irq | (uint8_t)hw_cart_irq();
    if (apu.frame_irq && !apu.irq_inhibit) apu.frame_irq_out = 1;
}

/* ------------------------------------------------------------------------- */
/* DMAs                                                                      */
/* ------------------------------------------------------------------------- */

bool dma_wants_cycle(void)
{
    return hw.cpu_reading && ((apu.dmc_dma && (apu.dmc_playing || apu.implicit_abort)) || apu.oam_dma);
}

/* A cycle in which a DMA holds the CPU without a transfer of its own: the
 * CPU's pending read stays on the bus and is repeated. */
static void dma_halt(void)
{
    if (cyc_trace_enabled) cyc_trace_dma(true);
    hw_bus_read(hw.cpu_addr);
}

static void oam_dma_get(void)
{
    apu.oam_dma_aligned = 1;
    apu.oam_dma_value = hw_bus_read((uint16_t)(apu.oam_dma_page << 8 | apu.oam_dma_index));
}

static void oam_dma_put(void)
{
    if (!apu.oam_dma_aligned) {
        dma_halt();
        return;
    }
    hw_bus_write(0x2004, apu.oam_dma_value);
    if (++apu.oam_dma_index == 0) {
        apu.oam_dma = 0;
        apu.oam_dma_aligned = 0;
    }
}

static void dmc_dma_get(void)
{
    apu.dmc_buffer = hw_bus_read(apu.dmc_addr);
    apu.dmc_have_buffer = 1;
    if (++apu.dmc_addr == 0) apu.dmc_addr = 0x8000;
    if (apu.dmc_bytes > 0) apu.dmc_bytes--;
    if (apu.dmc_bytes == 0) {
        if (!apu.dmc_loop) {
            apu.dmc_playing = 0;
            if (apu.dmc_irq_enable) apu.dmc_irq = 1;
        } else {
            dmc_restart();
        }
    }
    apu.dmc_dma = 0;
    apu.oam_dma_aligned = 0;
    apu.dmc_dma_cooldown = 2;
}

/* A DMA cycle. Put cycles write (the sprite DMA's $2004 store) or wait; get
 * cycles read. With both DMAs pending, the sprite DMA has the put cycles
 * and the sample DMA the get cycles; a halted DMA yields to the other. */
void dma_cycle(void)
{
    if (cyc_trace_enabled) cyc_trace_dma(false);
    if (apu.oam_dma && apu.oam_dma_first) {
        apu.oam_dma_first = 0;
        if (!apu.put) apu.oam_dma_halt = 1;
    }
    bool dmc = apu.dmc_dma, oam = apu.oam_dma;
    if (apu.put) {
        if (dmc && oam) {
            if (apu.dmc_dma_halt && apu.oam_dma_halt) dma_halt();
            else if (apu.oam_dma_halt) dma_halt(); /* the sample DMA's put cycle */
            else oam_dma_put();
        } else if (dmc) {
            dma_halt();
        } else if (apu.oam_dma_halt) {
            dma_halt();
        } else {
            oam_dma_put();
        }
    } else {
        if (dmc && oam) {
            if (apu.dmc_dma_halt && apu.oam_dma_halt) dma_halt();
            else if (apu.dmc_dma_halt) oam_dma_get();
            else dmc_dma_get();
        } else if (dmc) {
            if (apu.dmc_dma_halt) dma_halt();
            else dmc_dma_get();
        } else if (apu.oam_dma_halt) {
            dma_halt();
        } else {
            oam_dma_get();
        }
        apu.dmc_dma_halt = 0;
        apu.oam_dma_halt = 0;
    }
}

void dma_end_of_cycle(void)
{
    /* The implicit abort's DMA only runs on the cycle right after it is
     * scheduled; a write cycle there cancels it. */
    if (apu.dmc_dma && apu.implicit_abort) apu.implicit_abort = 0;
}

/* ------------------------------------------------------------------------- */
/* Power-on, state                                                           */
/* ------------------------------------------------------------------------- */

void apu_power_on(void)
{
    memset(&apu, 0, sizeof(apu));
    apu.put = 1;
    apu.reset_delay = 0xFF;
    apu.dmc_rate = 428;
    apu.dmc_timer = 1022;
    apu.dmc_sample_addr = apu.dmc_addr = 0xC000;
    apu.dmc_sample_len = 1;
    apu.dmc_bits = 8;
    apu.dmc_silent = 1;
    apu.dmc_out_silent = 1;
    apu.noise_lfsr = 1;
    apu.noise_period = noise_period_table[0];
    apu.tri_out = 15;
}

#define APU_FIELD(f) {#f, offsetof(HwApu, f), sizeof(((HwApu *)0)->f)}
static const struct {
    const char *name;
    size_t      offset, size;
} apu_fields[] = {
    APU_FIELD(put), APU_FIELD(five_step), APU_FIELD(irq_inhibit), APU_FIELD(reset_delay), APU_FIELD(counter),
    APU_FIELD(quarter), APU_FIELD(half), APU_FIELD(frame_irq), APU_FIELD(frame_irq_clear), APU_FIELD(frame_irq_out),
    APU_FIELD(len), APU_FIELD(reg), APU_FIELD(dmc_irq_enable), APU_FIELD(dmc_loop), APU_FIELD(dmc_irq),
    APU_FIELD(dmc_rate), APU_FIELD(dmc_timer), APU_FIELD(dmc_level), APU_FIELD(dmc_sample_addr),
    APU_FIELD(dmc_sample_len), APU_FIELD(dmc_bytes), APU_FIELD(dmc_addr), APU_FIELD(dmc_buffer),
    APU_FIELD(dmc_shifter), APU_FIELD(dmc_have_buffer), APU_FIELD(dmc_out_silent), APU_FIELD(dmc_bits), APU_FIELD(dmc_silent), APU_FIELD(dmc_playing),
    APU_FIELD(dmc_enable), APU_FIELD(dmc_enable_delay), APU_FIELD(dmc_start_delay), APU_FIELD(dmc_dma_cooldown),
    APU_FIELD(implicit_abort), APU_FIELD(implicit_abort_armed), APU_FIELD(dmc_dma), APU_FIELD(dmc_dma_halt),
    APU_FIELD(oam_dma), APU_FIELD(oam_dma_first), APU_FIELD(oam_dma_halt), APU_FIELD(oam_dma_aligned),
    APU_FIELD(oam_dma_page), APU_FIELD(oam_dma_index), APU_FIELD(oam_dma_value), APU_FIELD(strobe),
    APU_FIELD(strobed), APU_FIELD(shift), APU_FIELD(shift_delay), APU_FIELD(buttons),
};

uint64_t apu_state_hash(uint64_t h)
{
    uint64_t acc = 0;
    for (size_t k = 0; k < sizeof(apu_fields) / sizeof(apu_fields[0]); k++) {
        const uint8_t *b = (const uint8_t *)&apu + apu_fields[k].offset;
        for (size_t i = 0; i < apu_fields[k].size; i++) acc = acc * 131 + b[i];
    }
    return h ^ (acc + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
}

void apu_state_dump(void *file)
{
    FILE *f = (FILE *)file;
    for (size_t k = 0; k < sizeof(apu_fields) / sizeof(apu_fields[0]); k++) {
        const uint8_t *b = (const uint8_t *)&apu + apu_fields[k].offset;
        fprintf(f, "apu.%s", apu_fields[k].name);
        for (size_t i = 0; i < apu_fields[k].size; i++) fprintf(f, "%s%02X", i % 32 ? "" : " ", b[i]);
        fputc('\n', f);
    }
}
