/*
 * apu.c — NES APU emulation (NTSC)
 *
 * Channels: pulse1, pulse2, triangle, noise, DMC (output-level only).
 * Frame counter: 4-step mode, ticking quarter/half-frame clocks at
 * approximately 240 Hz by distributing 4 ticks across each audio frame.
 *
 * Audio synthesis uses a fractional timer accumulator: each sample
 * advances the timer by (CPU_FREQ / SAMPLE_RATE) ~= 40.58 CPU cycles.
 * Pulse timers clock every 2 CPU cycles; triangle every 1 CPU cycle.
 */
#include "apu.h"
#include <string.h>
#include <stdbool.h>

#define CPU_FREQ        1789773.0f
#define SAMPLE_RATE     44100

/* ---- Look-up tables ---- */

static const uint8_t LENGTH_TABLE[32] = {
    10,254, 20,  2, 40,  4, 80,  6,160,  8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22,192, 24, 72, 26, 16, 28, 32, 30
};

static const uint8_t DUTY_TABLE[4][8] = {
    {0,1,0,0,0,0,0,0},   /* 12.5% */
    {0,1,1,0,0,0,0,0},   /* 25%   */
    {0,1,1,1,1,0,0,0},   /* 50%   */
    {1,0,0,1,1,1,1,1},   /* 75%   */
};

static const uint8_t TRI_TABLE[32] = {
    15,14,13,12,11,10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
     0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15
};

/* NTSC noise timer periods in CPU cycles */
static const uint16_t NOISE_PERIOD[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

/* ---- Channel state ---- */

typedef struct {
    uint8_t  duty;
    bool     halt;        /* length-counter halt + envelope loop */
    bool     const_vol;
    uint8_t  vol;         /* 4-bit: volume (const_vol=1) or envelope period */

    bool     sweep_en;
    uint8_t  sweep_period;
    bool     sweep_neg;
    uint8_t  sweep_shift;
    bool     sweep_reload;
    uint8_t  sweep_div;

    uint16_t timer;       /* 11-bit period register */
    float    timer_acc;   /* fractional CPU-cycle accumulator */
    uint8_t  seq;         /* duty sequencer pos 0-7 */

    uint8_t  env_div;
    uint8_t  env_vol;     /* decay level 0-15 */
    bool     env_start;

    uint8_t  length;
    bool     enabled;
    int      which;       /* 1 or 2, affects sweep negation */
} Pulse;

typedef struct {
    bool     halt;        /* also linear-counter control flag */
    uint8_t  linear_load;
    uint8_t  linear;
    bool     linear_reload;

    uint16_t timer;       /* 11-bit */
    float    timer_acc;
    uint8_t  seq;         /* 0-31 */

    uint8_t  length;
    bool     enabled;
} Triangle;

typedef struct {
    bool     halt;
    bool     const_vol;
    uint8_t  vol;
    bool     mode;
    uint8_t  period_idx;
    float    timer_acc;
    uint16_t lfsr;        /* 15-bit, initialised to 1 */

    uint8_t  env_div;
    uint8_t  env_vol;
    bool     env_start;

    uint8_t  length;
    bool     enabled;
} Noise;

/* DMC: simplified — only the direct-load output level ($4011). */
typedef struct {
    uint8_t  output;      /* 7-bit */
    bool     enabled;
} DMC;

static Pulse    s_p1, s_p2;
static Triangle s_tri;
static Noise    s_noise;
static DMC      s_dmc;

static bool s_fc_mode;    /* 0=4-step  1=5-step */
static bool s_fc_irq_inh;

/* ---- Envelope ---- */
static void tick_envelope(uint8_t *div, uint8_t *vol, bool *start,
                           bool loop, uint8_t period) {
    if (*start) {
        *start = false;
        *vol   = 15;
        *div   = period;
    } else {
        if (*div > 0) {
            (*div)--;
        } else {
            *div = period;
            if (*vol > 0)    (*vol)--;
            else if (loop)    *vol = 15;
        }
    }
}

/* ---- Quarter-frame: envelopes + triangle linear counter ---- */
static void quarter_frame(void) {
    tick_envelope(&s_p1.env_div,    &s_p1.env_vol,    &s_p1.env_start,    s_p1.halt,    s_p1.vol);
    tick_envelope(&s_p2.env_div,    &s_p2.env_vol,    &s_p2.env_start,    s_p2.halt,    s_p2.vol);
    tick_envelope(&s_noise.env_div, &s_noise.env_vol, &s_noise.env_start, s_noise.halt, s_noise.vol);

    if (s_tri.linear_reload) {
        s_tri.linear = s_tri.linear_load;
    } else if (s_tri.linear > 0) {
        s_tri.linear--;
    }
    if (!s_tri.halt) s_tri.linear_reload = false;
}

/* ---- Half-frame: length counters + sweep ---- */
static void tick_sweep(Pulse *p) {
    /* Compute sweep target */
    uint16_t shift = p->timer >> p->sweep_shift;
    uint16_t tgt;
    if (p->sweep_neg) {
        tgt = p->timer - shift;
        if (p->which == 1) tgt--;  /* pulse 1 uses 1's complement */
    } else {
        tgt = p->timer + shift;
    }

    /* Apply if conditions met */
    if (p->sweep_div == 0 && p->sweep_en && p->sweep_shift > 0
            && tgt <= 0x7FF && p->timer >= 8) {
        p->timer = tgt;
    }

    if (p->sweep_div == 0 || p->sweep_reload) {
        p->sweep_div    = p->sweep_period;
        p->sweep_reload = false;
    } else {
        p->sweep_div--;
    }
}

static void half_frame(void) {
    if (!s_p1.halt    && s_p1.length    > 0) s_p1.length--;
    if (!s_p2.halt    && s_p2.length    > 0) s_p2.length--;
    if (!s_tri.halt   && s_tri.length   > 0) s_tri.length--;
    if (!s_noise.halt && s_noise.length > 0) s_noise.length--;
    tick_sweep(&s_p1);
    tick_sweep(&s_p2);
}

/* ---- Noise LFSR ---- */
static void clock_noise(void) {
    uint16_t fb = ((s_noise.lfsr >> 0) ^ (s_noise.lfsr >> (s_noise.mode ? 6 : 1))) & 1;
    s_noise.lfsr = (s_noise.lfsr >> 1) | (uint16_t)(fb << 14);
}

/* ---- Channel outputs (0-15 each) ---- */
static uint8_t pulse_out(const Pulse *p) {
    if (!p->enabled || p->length == 0 || p->timer < 8) return 0;
    /* Sweep mute: check if target would overflow */
    uint16_t tgt = p->timer;
    if (!p->sweep_neg) tgt += (p->timer >> p->sweep_shift);
    if (tgt > 0x7FF)   return 0;
    if (!DUTY_TABLE[p->duty][p->seq]) return 0;
    return p->const_vol ? p->vol : p->env_vol;
}

static uint8_t triangle_out(const Triangle *t) {
    if (!t->enabled || t->length == 0 || t->linear == 0) return 0;
    return TRI_TABLE[t->seq];
}

static uint8_t noise_out(const Noise *n) {
    if (!n->enabled || n->length == 0 || (n->lfsr & 1)) return 0;
    return n->const_vol ? n->vol : n->env_vol;
}

/* ---- NES mixer (linear approximation) ---- */
static int16_t mix_sample(void) {
    float p1  = (float)pulse_out(&s_p1);
    float p2  = (float)pulse_out(&s_p2);
    float tri = (float)triangle_out(&s_tri);
    float nse = (float)noise_out(&s_noise);
    float dmc = (float)s_dmc.output;

    /* Linear approximation coefficients from NESDev wiki */
    float out = 0.00752f * (p1 + p2)
              + 0.00851f * tri
              + 0.00494f * nse
              + 0.00335f * dmc;

    /* Scale to int16 range with ~2x amplification for comfortable volume */
    out *= 2.0f * 32767.0f;
    if (out >  32767.0f) out =  32767.0f;
    if (out < -32767.0f) out = -32767.0f;
    return (int16_t)out;
}

/* ---- Public API ---- */

void apu_init(void) {
    memset(&s_p1,    0, sizeof(s_p1));
    memset(&s_p2,    0, sizeof(s_p2));
    memset(&s_tri,   0, sizeof(s_tri));
    memset(&s_noise, 0, sizeof(s_noise));
    memset(&s_dmc,   0, sizeof(s_dmc));
    s_p1.which   = 1;
    s_p2.which   = 2;
    s_noise.lfsr = 1;  /* LFSR must not be all zeros */
    s_fc_mode    = false;
    s_fc_irq_inh = false;
}

void apu_write(uint16_t addr, uint8_t val) {
    Pulse *p = (addr < 0x4004) ? &s_p1 : &s_p2;

    switch (addr) {
    /* ---- Pulse 1 ($4000-$4003) and Pulse 2 ($4004-$4007) ---- */
    case 0x4000: case 0x4004:
        p->duty      = (val >> 6) & 3;
        p->halt      = (val >> 5) & 1;
        p->const_vol = (val >> 4) & 1;
        p->vol       =  val & 0x0F;
        break;
    case 0x4001: case 0x4005:
        p->sweep_en     = (val >> 7) & 1;
        p->sweep_period = (val >> 4) & 7;
        p->sweep_neg    = (val >> 3) & 1;
        p->sweep_shift  =  val & 7;
        p->sweep_reload = true;
        break;
    case 0x4002: case 0x4006:
        p->timer = (p->timer & 0x700) | val;
        break;
    case 0x4003: case 0x4007:
        p->timer      = (p->timer & 0x00FF) | ((uint16_t)(val & 7) << 8);
        if (p->enabled) p->length = LENGTH_TABLE[val >> 3];
        p->env_start  = true;
        p->seq        = 0;
        break;

    /* ---- Triangle ($4008-$400B) ---- */
    case 0x4008:
        s_tri.halt        = (val >> 7) & 1;
        s_tri.linear_load =  val & 0x7F;
        break;
    case 0x400A:
        s_tri.timer = (s_tri.timer & 0x700) | val;
        break;
    case 0x400B:
        s_tri.timer         = (s_tri.timer & 0x00FF) | ((uint16_t)(val & 7) << 8);
        if (s_tri.enabled)  s_tri.length = LENGTH_TABLE[val >> 3];
        s_tri.linear_reload = true;
        break;

    /* ---- Noise ($400C-$400F) ---- */
    case 0x400C:
        s_noise.halt      = (val >> 5) & 1;
        s_noise.const_vol = (val >> 4) & 1;
        s_noise.vol       =  val & 0x0F;
        break;
    case 0x400E:
        s_noise.mode       = (val >> 7) & 1;
        s_noise.period_idx =  val & 0x0F;
        break;
    case 0x400F:
        if (s_noise.enabled) s_noise.length = LENGTH_TABLE[val >> 3];
        s_noise.env_start = true;
        break;

    /* ---- DMC ($4010-$4013) ---- */
    case 0x4010:
        /* Rate/IRQ — not emulated beyond silencing */
        s_dmc.enabled = (val >> 4) & 1;
        break;
    case 0x4011:
        s_dmc.output = val & 0x7F;
        break;
    case 0x4012:
    case 0x4013:
        /* Sample address/length — ROM DMA not implemented */
        break;

    /* ---- Status ($4015) ---- */
    case 0x4015:
        s_p1.enabled    = (val >> 0) & 1;
        s_p2.enabled    = (val >> 1) & 1;
        s_tri.enabled   = (val >> 2) & 1;
        s_noise.enabled = (val >> 3) & 1;
        s_dmc.enabled   = (val >> 4) & 1;
        if (!s_p1.enabled)    s_p1.length    = 0;
        if (!s_p2.enabled)    s_p2.length    = 0;
        if (!s_tri.enabled)   s_tri.length   = 0;
        if (!s_noise.enabled) s_noise.length = 0;
        break;

    /* ---- Frame counter ($4017) ---- */
    case 0x4017:
        s_fc_mode    = (val >> 7) & 1;
        s_fc_irq_inh = (val >> 6) & 1;
        /* 5-step mode immediately fires a half-frame clock */
        if (s_fc_mode) { quarter_frame(); half_frame(); }
        break;

    default:
        break;
    }
}

uint8_t apu_read_status(void) {
    uint8_t s = 0;
    if (s_p1.length    > 0) s |= 0x01;
    if (s_p2.length    > 0) s |= 0x02;
    if (s_tri.length   > 0) s |= 0x04;
    if (s_noise.length > 0) s |= 0x08;
    if (s_dmc.enabled)      s |= 0x10;
    return s;
}

void apu_generate(int16_t *buf, int n_samples) {
    /* Distribute 4 quarter-frame clocks evenly across the audio frame.
     * Steps 2 and 4 also fire a half-frame clock (length + sweep). */
    const int qf[4] = {
        0,
        n_samples / 4,
        n_samples / 2,
        3 * n_samples / 4
    };
    int qf_idx = 0;

    const float cpu_per_sample = CPU_FREQ / (float)SAMPLE_RATE;

    for (int i = 0; i < n_samples; i++) {

        /* Frame counter ticks */
        if (qf_idx < 4 && i == qf[qf_idx]) {
            quarter_frame();
            if (qf_idx == 1 || qf_idx == 3) half_frame();
            qf_idx++;
        }

        /* Pulse 1: timer clocks every 2 CPU cycles */
        {
            s_p1.timer_acc += cpu_per_sample;
            float period = (float)((s_p1.timer + 1) * 2);
            while (s_p1.timer_acc >= period) {
                s_p1.timer_acc -= period;
                s_p1.seq = (s_p1.seq + 1) & 7;
            }
        }

        /* Pulse 2 */
        {
            s_p2.timer_acc += cpu_per_sample;
            float period = (float)((s_p2.timer + 1) * 2);
            while (s_p2.timer_acc >= period) {
                s_p2.timer_acc -= period;
                s_p2.seq = (s_p2.seq + 1) & 7;
            }
        }

        /* Triangle: timer clocks every CPU cycle */
        {
            s_tri.timer_acc += cpu_per_sample;
            float period = (float)(s_tri.timer + 1);
            while (s_tri.timer_acc >= period) {
                s_tri.timer_acc -= period;
                s_tri.seq = (s_tri.seq + 1) & 31;
            }
        }

        /* Noise */
        {
            s_noise.timer_acc += cpu_per_sample;
            float period = (float)NOISE_PERIOD[s_noise.period_idx];
            while (s_noise.timer_acc >= period) {
                s_noise.timer_acc -= period;
                clock_noise();
            }
        }

        buf[i] = mix_sample();
    }
}
