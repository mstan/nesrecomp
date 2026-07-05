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
#include "apu_shadow.h"
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

/* NTSC DMC period table — CPU cycles per output-bit clock */
static const uint16_t DMC_RATE[16] = {
    428, 380, 340, 320, 286, 254, 226, 214,
    190, 160, 142, 128, 106,  84,  72,  54
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

/* DMC: DPCM sample playback ($4010/$4012/$4013) + direct load ($4011). */
typedef struct {
    /* Config (from $4010/$4012/$4013) */
    bool     loop;
    bool     irq_en;
    bool     irq_flag;     /* DMC interrupt flag; set on sample-end if irq_en (NESdev $4015 bit 7) */
    uint8_t  rate_idx;
    uint16_t start_addr;   /* $4012: $C000 + (val<<6)  */
    uint16_t start_len;    /* $4013: (val<<4) + 1      */

    /* Memory reader / sample fetch */
    uint16_t cur_addr;
    uint16_t bytes_left;
    uint8_t  sample_buf;
    bool     buf_empty;

    /* Output unit */
    uint8_t  shift;        /* 8-bit shift register   */
    uint8_t  bits_left;    /* bits remaining in cycle */
    bool     silence;
    uint8_t  output;       /* 7-bit DAC level, 0-127  */
    float    timer_acc;    /* fractional CPU-cycle accumulator */
    bool     enabled;      /* $4015 bit 4             */
} DMC;

static Pulse    s_p1, s_p2;
static Triangle s_tri;
static Noise    s_noise;
static DMC      s_dmc;

/* DMC DMA cycle-steal accumulator: each sample-byte fetch halts the CPU ~4
 * cycles while the APU's DMA unit reads from CPU memory. Accumulated here and
 * drained by apu_take_dmc_stall() into the CPU frame budget. */
static int s_dmc_stall = 0;
int apu_take_dmc_stall(void) { int s = s_dmc_stall; s_dmc_stall = 0; return s; }

static bool s_fc_mode;    /* 0=4-step  1=5-step */
static bool s_fc_irq_inh;
static bool s_fc_irq_flag; /* frame-counter interrupt flag ($4015 bit 6); set
                            * each 4-step sequence unless inhibited, cleared by
                            * a $4015 read or by setting the inhibit bit. */
static int  s_fc_cycle_acc; /* CPU-cycle accumulator for the cycle-driven frame
                             * sequencer (apu_clock_cycles); NMI-independent. */

/* Sample-accurate engine (Option B): output ring + per-sample integration state.
 * Declared here (before apu_init) — defined/used by the engine below. */
#define APU_RING_SIZE 8192               /* power of two; ~185 ms at 44.1 kHz */
#define CYC_PER_SAMPLE ((float)CPU_FREQ / (float)SAMPLE_RATE)
static int16_t s_ring[APU_RING_SIZE];
static int     s_ring_head;              /* write index */
static int     s_ring_tail;              /* read index  */
static float   s_out_acc;                /* fractional CPU cycles toward next sample */
static float   s_mix_acc;                /* running sum of the mix over this window  */
static int     s_mix_n;
static ApuChannelLevels s_lv_last;
static int     s_shadow_cached = -1;

/* ---- T0 per-channel debug taps (recomp_audio_debug five-tap model) ----
 * Side rings written in lockstep with s_ring at the same head slot, drained by
 * apu_generate into per-frame staging buffers that main_runner pushes as the
 * t0_* taps. Each entry is the sample-window average of the channel's raw DAC
 * input (the same per-cycle levels the canon mixer consumed), linearly rescaled
 * to int16 so per-channel waveform shape survives in the dumped WAVs:
 * pulse/triangle/noise 0..15 -> x2184, dmc 0..127 -> x258. Env-gated on
 * RECOMP_AUDIO_DEBUG via recomp_audio_debug_enabled(): zero extra work per
 * cycle when capture is off. */
#define APU_T0_CHANNELS  5
#define APU_T0_FRAME_MAX 1024
static int     s_t0_cached = -1;      /* -1 unqueried; 0 off; 1 capturing */
static float   s_t0_acc[APU_T0_CHANNELS];
static int16_t s_t0_ring[APU_T0_CHANNELS][APU_RING_SIZE];
static int16_t s_t0_frame[APU_T0_CHANNELS][APU_T0_FRAME_MAX];
int recomp_audio_debug_enabled(void); /* impl lives in main_runner.c (recomp_audio_debug.h) */

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

/* ---- DMC DPCM sample playback ---- */

/* Side-effect-free PRG/SRAM read (implemented in runtime.c). */
extern uint8_t apu_dmc_read(uint16_t addr);

/* Fetch the next sample byte from memory into the sample buffer. */
static void dmc_refill(void) {
    if (!s_dmc.buf_empty || s_dmc.bytes_left == 0) return;
    s_dmc.sample_buf = apu_dmc_read(s_dmc.cur_addr);
    s_dmc.buf_empty  = false;
    s_dmc_stall += 4;   /* DMC DMA halts the CPU ~4 cycles for the sample fetch */
    /* Address wraps from $FFFF back to $8000. */
    s_dmc.cur_addr   = (uint16_t)((s_dmc.cur_addr + 1) | 0x8000);
    if (--s_dmc.bytes_left == 0) {
        if (s_dmc.loop) {
            s_dmc.cur_addr   = s_dmc.start_addr;
            s_dmc.bytes_left = s_dmc.start_len;
        } else if (s_dmc.irq_en) {
            /* Sample finished, no loop: assert the DMC interrupt flag (NESdev
             * "APU DMC"). Observable via $4015 bit 7 — polling games work now.
             * NOTE: actual IRQ-VECTOR delivery is not yet wired — the static
             * recomp has no general pending-IRQ path (only the MMC3 scanline
             * IRQ is delivered, via the renderer). Vector-driven DMC IRQ needs
             * the pending-IRQ delivery hook (separate, deferred). */
            s_dmc.irq_flag = true;
        }
    }
}

/* Clock the DMC output unit once (one bit). */
static void dmc_clock(void) {
    if (s_dmc.bits_left == 0) {
        s_dmc.bits_left = 8;
        if (s_dmc.buf_empty) {
            s_dmc.silence = true;
        } else {
            s_dmc.silence   = false;
            s_dmc.shift     = s_dmc.sample_buf;
            s_dmc.buf_empty = true;
            dmc_refill();
        }
    }
    if (!s_dmc.silence) {
        if (s_dmc.shift & 1) { if (s_dmc.output <= 125) s_dmc.output += 2; }
        else                 { if (s_dmc.output >=   2) s_dmc.output -= 2; }
    }
    s_dmc.shift >>= 1;
    s_dmc.bits_left--;
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

/* ---- NES mixer (linear approximation) ----
 * This is the CANON, authoritative mixer. It also captures the per-channel
 * levels it used into *lv (when non-NULL) so the verified-enhancement audio
 * shadow (apu_shadow.c) can re-render the SAME state in float through the
 * hardware's nonlinear DAC curve and diff itself against this stream. The
 * canon int16 returned here is unchanged and remains the verify oracle. */
static float mix_sample_f(ApuChannelLevels *lv) {
    uint8_t l_p1  = pulse_out(&s_p1);
    uint8_t l_p2  = pulse_out(&s_p2);
    uint8_t l_tri = triangle_out(&s_tri);
    uint8_t l_nse = noise_out(&s_noise);
    uint8_t l_dmc = s_dmc.output;
    if (lv) {
        lv->pulse1   = l_p1;
        lv->pulse2   = l_p2;
        lv->triangle = l_tri;
        lv->noise    = l_nse;
        lv->dmc      = l_dmc;
    }

    /* Hardware nonlinear DAC (NESDev "accurate" APU mixer). Root-caused by the
     * accuracy slice: the APU register-write inputs are bit-identical to the Mesen2
     * oracle, so the audible difference was entirely the mixer — the prior linear
     * approximation over-weighted low/mid channels and ran ~14x hot. The two NES
     * DAC groups (pulse pair; triangle+noise+dmc) each saturate nonlinearly:
     *   pulse = 95.88 / (8128/(p1+p2) + 100)
     *   tnd   = 159.79 / (1/(tri/8227 + noise/12241 + dmc/22638) + 100)   */
    float pulse = 0.0f;
    unsigned psum = (unsigned)l_p1 + (unsigned)l_p2;
    if (psum != 0u) pulse = 95.88f / (8128.0f / (float)psum + 100.0f);
    float tnd_acc = (float)l_tri / 8227.0f + (float)l_nse / 12241.0f + (float)l_dmc / 22638.0f;
    float tnd = (tnd_acc != 0.0f) ? 159.79f / (1.0f / tnd_acc + 100.0f) : 0.0f;

    /* (pulse+tnd) spans ~[0,1] with the all-channels-max ceiling at ~1.0, so map
     * full-scale 1:1 to int16 — no extra gain (the prior linear path's 2x is what
     * made it run hot and clip). Returned pre-clamp so the oversampler can average. */
    return (pulse + tnd) * 32767.0f;
}

static int16_t mix_sample(ApuChannelLevels *lv) {
    float out = mix_sample_f(lv);
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
    s_p1.which     = 1;
    s_p2.which     = 2;
    s_noise.lfsr   = 1;  /* LFSR must not be all zeros */
    s_dmc.buf_empty = true;
    s_dmc.silence   = true;
    s_fc_mode    = false;
    s_fc_irq_inh = false;
    s_fc_irq_flag = false;
    s_fc_cycle_acc = 0;

    /* sample-accurate engine state (ring + integration accumulators) */
    s_ring_head = s_ring_tail = 0;
    s_out_acc = 0.0f; s_mix_acc = 0.0f; s_mix_n = 0; s_shadow_cached = -1;
    s_t0_cached = -1;
    memset(s_t0_acc, 0, sizeof(s_t0_acc));

    /* Arm the (default-OFF) verified-enhancement audio shadow. */
    apu_shadow_init();
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
        s_dmc.irq_en   = (val >> 7) & 1;
        s_dmc.loop     = (val >> 6) & 1;
        s_dmc.rate_idx =  val & 0x0F;
        if (!s_dmc.irq_en) s_dmc.irq_flag = false;  /* clearing IRQ enable clears the flag */
        break;
    case 0x4011:
        s_dmc.output = val & 0x7F;   /* direct DAC load */
        break;
    case 0x4012:
        s_dmc.start_addr = (uint16_t)(0xC000 + ((uint16_t)val << 6));
        break;
    case 0x4013:
        s_dmc.start_len  = (uint16_t)(((uint16_t)val << 4) + 1);
        break;

    /* ---- Status ($4015) ---- */
    case 0x4015:
        s_dmc.irq_flag = false;   /* writing $4015 clears the DMC interrupt flag (NESdev) */
        s_p1.enabled    = (val >> 0) & 1;
        s_p2.enabled    = (val >> 1) & 1;
        s_tri.enabled   = (val >> 2) & 1;
        s_noise.enabled = (val >> 3) & 1;
        s_dmc.enabled   = (val >> 4) & 1;
        if (!s_p1.enabled)    s_p1.length    = 0;
        if (!s_p2.enabled)    s_p2.length    = 0;
        if (!s_tri.enabled)   s_tri.length   = 0;
        if (!s_noise.enabled) s_noise.length = 0;
        /* DMC: clearing bit 4 stops playback; setting it (re)starts the
         * sample only when none is currently playing. */
        if (!s_dmc.enabled) {
            s_dmc.bytes_left = 0;
        } else if (s_dmc.bytes_left == 0) {
            s_dmc.cur_addr   = s_dmc.start_addr;
            s_dmc.bytes_left = s_dmc.start_len;
            s_dmc.buf_empty  = true;
            dmc_refill();
        }
        break;

    /* ---- Frame counter ($4017) ---- */
    case 0x4017:
        s_fc_mode    = (val >> 7) & 1;
        s_fc_irq_inh = (val >> 6) & 1;
        /* Writing $4017 resets the frame sequencer phase (after a few cycles on
         * hardware; modelled here as an immediate restart of the cycle clock). */
        s_fc_cycle_acc = 0;
        /* Setting the inhibit bit clears the frame interrupt flag (NESdev). */
        if (s_fc_irq_inh) s_fc_irq_flag = false;
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
    if (s_dmc.bytes_left > 0) s |= 0x10;   /* DMC active = sample bytes remain */
    if (s_fc_irq_flag)        s |= 0x40;   /* frame-counter interrupt flag ($4015 bit 6) */
    if (s_dmc.irq_flag)       s |= 0x80;   /* DMC interrupt flag (NESdev $4015 bit 7) */
    /* Reading $4015 clears the frame-counter interrupt flag (bit 6) but NOT the
     * DMC flag (bit 7).  The DMC flag is cleared only by a $4015 write or by
     * disabling DMC IRQ via $4010. */
    s_fc_irq_flag = false;
    return s;
}

/* ---- Sample-accurate APU engine (Option B) -------------------------------
 * apu_clock_cycles() is THE APU engine, driven on the CPU-cycle stream (from
 * maybe_trigger_vblank, per instruction). Per CPU cycle it advances the frame
 * sequencer + all channel timers + DMC at their true offsets AND integrates the
 * band-limited nonlinear-DAC mix into one output sample per CPU_FREQ/SAMPLE_RATE
 * cycles, pushed to a ring buffer. apu_generate() now DRAINS that ring
 * (main_runner unchanged). Intra-frame envelope/sweep/length/DMC therefore land
 * at their real sub-frame times (vs the old per-frame batched apu_generate), and
 * the frame + DMC IRQs are cycle-accurate and NMI-independent.
 * (ring + integration state declared above apu_init.) */

static void ring_push(int16_t s) {
    int nh = (s_ring_head + 1) & (APU_RING_SIZE - 1);
    if (nh == s_ring_tail)               /* full: drop oldest (smoke/turbo never drain) */
        s_ring_tail = (s_ring_tail + 1) & (APU_RING_SIZE - 1);
    s_ring[s_ring_head] = s;
    s_ring_head = nh;
}

/* Advance all five channel timers by `dc` CPU cycles (the same stepping the old
 * apu_generate oversample loop used, now driven per CPU cycle). */
static void apu_step_channels(float dc) {
    s_p1.timer_acc += dc;
    { float period = (float)((s_p1.timer + 1) * 2);
      while (s_p1.timer_acc >= period) { s_p1.timer_acc -= period; s_p1.seq = (s_p1.seq + 1) & 7; } }
    s_p2.timer_acc += dc;
    { float period = (float)((s_p2.timer + 1) * 2);
      while (s_p2.timer_acc >= period) { s_p2.timer_acc -= period; s_p2.seq = (s_p2.seq + 1) & 7; } }
    s_tri.timer_acc += dc;
    { float period = (float)(s_tri.timer + 1);
      while (s_tri.timer_acc >= period) { s_tri.timer_acc -= period; s_tri.seq = (s_tri.seq + 1) & 31; } }
    s_noise.timer_acc += dc;
    { float period = (float)NOISE_PERIOD[s_noise.period_idx];
      while (s_noise.timer_acc >= period) { s_noise.timer_acc -= period; clock_noise(); } }
    s_dmc.timer_acc += dc;
    { float period = (float)DMC_RATE[s_dmc.rate_idx];
      while (s_dmc.timer_acc >= period) { s_dmc.timer_acc -= period; dmc_clock(); } }
}

/* Cycle-accurate frame sequencer: fire quarter/half-frame clocks + the frame IRQ
 * at their exact NTSC CPU-cycle offsets. 4-step: 7457/14913/22371/29829
 * (qf; qf+hf; qf; qf+hf+IRQ), wrap 29830. 5-step: +37281 (qf+hf), wrap 37282, no
 * IRQ. Phase anchored to the last $4017 write (which zeroes s_fc_cycle_acc). */
static void advance_frame_seq(void) {
    s_fc_cycle_acc++;
    if (!s_fc_mode) {
        switch (s_fc_cycle_acc) {
            case 7457:  quarter_frame(); break;
            case 14913: quarter_frame(); half_frame(); break;
            case 22371: quarter_frame(); break;
            case 29829: quarter_frame(); half_frame();
                        if (!s_fc_irq_inh) s_fc_irq_flag = true; break;
        }
        if (s_fc_cycle_acc >= 29830) s_fc_cycle_acc = 0;
    } else {
        switch (s_fc_cycle_acc) {
            case 7457:  quarter_frame(); break;
            case 14913: quarter_frame(); half_frame(); break;
            case 22371: quarter_frame(); break;
            case 37281: quarter_frame(); half_frame(); break;
        }
        if (s_fc_cycle_acc >= 37282) s_fc_cycle_acc = 0;
    }
}

void apu_clock_cycles(int cpu_cycles) {
    if (s_shadow_cached < 0) s_shadow_cached = apu_shadow_enabled();
    if (s_t0_cached < 0)     s_t0_cached = recomp_audio_debug_enabled() ? 1 : 0;
    for (int c = 0; c < cpu_cycles; c++) {
        advance_frame_seq();
        apu_step_channels(1.0f);
        s_mix_acc += mix_sample_f((s_shadow_cached || s_t0_cached) ? &s_lv_last : NULL);
        if (s_t0_cached) {
            s_t0_acc[0] += (float)s_lv_last.pulse1;
            s_t0_acc[1] += (float)s_lv_last.pulse2;
            s_t0_acc[2] += (float)s_lv_last.triangle;
            s_t0_acc[3] += (float)s_lv_last.noise;
            s_t0_acc[4] += (float)s_lv_last.dmc;
        }
        s_mix_n++;
        s_out_acc += 1.0f;
        if (s_out_acc >= CYC_PER_SAMPLE) {
            s_out_acc -= CYC_PER_SAMPLE;
            float avg = (s_mix_n > 0) ? s_mix_acc / (float)s_mix_n : 0.0f;
            if (avg >  32767.0f) avg =  32767.0f;
            if (avg < -32767.0f) avg = -32767.0f;
            int16_t canon = (int16_t)avg;
            if (s_t0_cached) {
                /* Same averaging window as the canon mix; write the side rings
                 * at the slot ring_push is about to fill so the streams stay
                 * sample-aligned. */
                float inv = (s_mix_n > 0) ? 1.0f / (float)s_mix_n : 0.0f;
                int h = s_ring_head;
                s_t0_ring[0][h] = (int16_t)(s_t0_acc[0] * inv * 2184.0f);
                s_t0_ring[1][h] = (int16_t)(s_t0_acc[1] * inv * 2184.0f);
                s_t0_ring[2][h] = (int16_t)(s_t0_acc[2] * inv * 2184.0f);
                s_t0_ring[3][h] = (int16_t)(s_t0_acc[3] * inv * 2184.0f);
                s_t0_ring[4][h] = (int16_t)(s_t0_acc[4] * inv * 258.0f);
                memset(s_t0_acc, 0, sizeof(s_t0_acc));
            }
            ring_push(s_shadow_cached ? apu_shadow_sample(canon, &s_lv_last) : canon);
            s_mix_acc = 0.0f; s_mix_n = 0;
        }
    }
}

/* CPU IRQ line state contributed by the APU: DMC sample-end or frame counter. */
bool apu_irq_asserted(void) {
    return s_dmc.irq_flag || s_fc_irq_flag;
}

/* Drain n samples from the cycle-driven ring (filled by apu_clock_cycles).
 * The APU is now run by the CPU-cycle stream, so this no longer synthesizes —
 * it pulls finished samples. Underrun (ring empty on a slow/odd frame) holds the
 * last sample; the DRC bridge's servo + stall-conceal absorb the slight jitter. */
void apu_generate(int16_t *buf, int n_samples) {
    int16_t last = 0;
    int16_t lastc[APU_T0_CHANNELS] = {0, 0, 0, 0, 0};
    int cap = (s_t0_cached == 1);
    for (int i = 0; i < n_samples; i++) {
        if (s_ring_tail != s_ring_head) {
            last = s_ring[s_ring_tail];
            if (cap)
                for (int k = 0; k < APU_T0_CHANNELS; k++)
                    lastc[k] = s_t0_ring[k][s_ring_tail];
            s_ring_tail = (s_ring_tail + 1) & (APU_RING_SIZE - 1);
        }
        buf[i] = last;
        if (cap && i < APU_T0_FRAME_MAX)
            for (int k = 0; k < APU_T0_CHANNELS; k++)
                s_t0_frame[k][i] = lastc[k];
    }
}

/* Per-channel T0 staging buffer for the frame most recently drained by
 * apu_generate. ch: 0=pulse1 1=pulse2 2=triangle 3=noise 4=dmc. NULL when
 * RECOMP_AUDIO_DEBUG capture is off. Valid for the same sample count passed to
 * apu_generate (<= APU_T0_FRAME_MAX). */
const int16_t *apu_debug_t0(int ch) {
    if (s_t0_cached != 1 || ch < 0 || ch >= APU_T0_CHANNELS) return NULL;
    return s_t0_frame[ch];
}

/* ---- Co-sim full-APU-state serializer (see apu.h) ----
 * Explicit little-endian, fixed field order, no struct padding — so the blob is
 * deterministic across runs and (unlike a raw struct memcpy) comparable across
 * implementations later. Excludes the sample output ring (s_ring/head/tail) and
 * the mix accumulators: those are host-side audio-delivery bookkeeping, not
 * guest-architectural state (the guest cannot observe them), exactly the kind of
 * host-only field the co-sim doc says to exclude. Includes everything the guest
 * CAN observe or that decides future APU output: channel timers/sequencers,
 * envelopes, sweep, length, the DMC memory reader + output unit, and the frame
 * counter's mode/inhibit/flag/cycle-phase. */
static int cosim_put_u8 (uint8_t *b, int i, uint8_t v)  { b[i] = v; return i + 1; }
static int cosim_put_u16(uint8_t *b, int i, uint16_t v) { b[i]=(uint8_t)v; b[i+1]=(uint8_t)(v>>8); return i+2; }
static int cosim_put_u32(uint8_t *b, int i, uint32_t v) {
    b[i]=(uint8_t)v; b[i+1]=(uint8_t)(v>>8); b[i+2]=(uint8_t)(v>>16); b[i+3]=(uint8_t)(v>>24); return i+4;
}
static int cosim_put_f32(uint8_t *b, int i, float f) {
    uint32_t u; memcpy(&u, &f, sizeof(u)); return cosim_put_u32(b, i, u);
}

static int cosim_put_pulse(uint8_t *b, int i, const Pulse *p) {
    i = cosim_put_u8 (b, i, p->duty);
    i = cosim_put_u8 (b, i, (uint8_t)((p->halt?1:0) | (p->const_vol?2:0) | (p->sweep_en?4:0)
                                    | (p->sweep_neg?8:0) | (p->sweep_reload?16:0) | (p->enabled?32:0)));
    i = cosim_put_u8 (b, i, p->vol);
    i = cosim_put_u8 (b, i, p->sweep_period);
    i = cosim_put_u8 (b, i, p->sweep_shift);
    i = cosim_put_u8 (b, i, p->sweep_div);
    i = cosim_put_u16(b, i, p->timer);
    i = cosim_put_f32(b, i, p->timer_acc);
    i = cosim_put_u8 (b, i, p->seq);
    i = cosim_put_u8 (b, i, p->env_div);
    i = cosim_put_u8 (b, i, p->env_vol);
    i = cosim_put_u8 (b, i, (uint8_t)(p->env_start?1:0));
    i = cosim_put_u8 (b, i, p->length);
    i = cosim_put_u8 (b, i, (uint8_t)p->which);
    return i;
}

int apu_get_state_blob(uint8_t *buf, int cap) {
    if (cap < 256) return 0;   /* current layout is ~120 bytes; guard generously */
    int i = 0;
    i = cosim_put_pulse(buf, i, &s_p1);
    i = cosim_put_pulse(buf, i, &s_p2);
    /* Triangle */
    i = cosim_put_u8 (buf, i, (uint8_t)((s_tri.halt?1:0)|(s_tri.linear_reload?2:0)|(s_tri.enabled?4:0)));
    i = cosim_put_u8 (buf, i, s_tri.linear_load);
    i = cosim_put_u8 (buf, i, s_tri.linear);
    i = cosim_put_u16(buf, i, s_tri.timer);
    i = cosim_put_f32(buf, i, s_tri.timer_acc);
    i = cosim_put_u8 (buf, i, s_tri.seq);
    i = cosim_put_u8 (buf, i, s_tri.length);
    /* Noise */
    i = cosim_put_u8 (buf, i, (uint8_t)((s_noise.halt?1:0)|(s_noise.const_vol?2:0)
                                      |(s_noise.mode?4:0)|(s_noise.env_start?8:0)|(s_noise.enabled?16:0)));
    i = cosim_put_u8 (buf, i, s_noise.vol);
    i = cosim_put_u8 (buf, i, s_noise.period_idx);
    i = cosim_put_f32(buf, i, s_noise.timer_acc);
    i = cosim_put_u16(buf, i, s_noise.lfsr);
    i = cosim_put_u8 (buf, i, s_noise.env_div);
    i = cosim_put_u8 (buf, i, s_noise.env_vol);
    i = cosim_put_u8 (buf, i, s_noise.length);
    /* DMC */
    i = cosim_put_u8 (buf, i, (uint8_t)((s_dmc.loop?1:0)|(s_dmc.irq_en?2:0)|(s_dmc.irq_flag?4:0)
                                      |(s_dmc.buf_empty?8:0)|(s_dmc.silence?16:0)|(s_dmc.enabled?32:0)));
    i = cosim_put_u8 (buf, i, s_dmc.rate_idx);
    i = cosim_put_u16(buf, i, s_dmc.start_addr);
    i = cosim_put_u16(buf, i, s_dmc.start_len);
    i = cosim_put_u16(buf, i, s_dmc.cur_addr);
    i = cosim_put_u16(buf, i, s_dmc.bytes_left);
    i = cosim_put_u8 (buf, i, s_dmc.sample_buf);
    i = cosim_put_u8 (buf, i, s_dmc.shift);
    i = cosim_put_u8 (buf, i, s_dmc.bits_left);
    i = cosim_put_u8 (buf, i, s_dmc.output);
    i = cosim_put_f32(buf, i, s_dmc.timer_acc);
    /* Frame counter / sequencer phase */
    i = cosim_put_u8 (buf, i, (uint8_t)((s_fc_mode?1:0)|(s_fc_irq_inh?2:0)|(s_fc_irq_flag?4:0)));
    i = cosim_put_u32(buf, i, (uint32_t)s_fc_cycle_acc);
    i = cosim_put_u32(buf, i, (uint32_t)s_dmc_stall);
    return i;
}
