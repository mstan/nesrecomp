/*
 * cyc_mm4_audio.c - Mega Man 4's sound effects, run from the player's own image.
 *
 * A title's sound engine cannot play the donor effects, so the backports play
 * Mega Man 4's own: the donor's sound driver (PRG bank $1E, $8000 tick /
 * $8003 request) runs on a small 6502 core here, exactly as on the console.
 * Its APU register writes, one frame's worth per tick, drive a compact
 * pulse/triangle/noise synthesiser whose output is mixed over the game's audio
 * (CycHostExtras.audio_mix).  No MM4 data is stored anywhere: the driver code
 * and the effect scripts are read from the image at start.
 *
 * Banks the driver sees (MMC3 $8000-$FFFF): $8000 = bank $1E, $A000 = R7
 * (the driver switches it; $1F at start), $C000 = $3E, $E000 = $3F.
 * The driver is ticked once per emulated video frame (frame_end) and the
 * synthesiser renders the frame's writes during the audio chunks that follow.
 */
#include "cyc_mm4_audio.h"
#include "cyc_mm4_donor.h"
#include "cyc_mod.h"
static int s_enabled = 1, s_own_native_channels = 1;

#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------- 6502 -- */

typedef struct {
    uint8_t  a, x, y, sp, p;
    uint16_t pc;
    uint8_t  ram[0x800];
    uint8_t  sel, r7;
    int      writes;                  /* APU writes captured this tick */
    struct { uint16_t addr; uint8_t value; } log[256];
} Drv;
static Drv D;
static int s_ready;

static uint8_t bank_of(uint16_t a) {
    if (a < 0xA000) return 0x1E;
    if (a < 0xC000) return D.r7;
    if (a < 0xE000) return 0x3E;
    return 0x3F;
}
static uint8_t rd8(uint16_t a) {
    if (a < 0x2000) return D.ram[a & 0x7FF];
    if (a >= 0x8000) return cyc_mm4_donor.prg[bank_of(a) * 0x2000u + (a & 0x1FFFu)];
    return 0;
}
static void wr8(uint16_t a, uint8_t v) {
    if (a < 0x2000) D.ram[a & 0x7FF] = v;
    else if (a >= 0x4000 && a <= 0x4017) {
        if (D.writes < 256) { D.log[D.writes].addr = a; D.log[D.writes].value = v; D.writes++; }
    } else if (a >= 0x8000 && a <= 0x9FFF) {
        if (!(a & 1)) D.sel = v & 7;
        else if (D.sel == 7) D.r7 = v & 0x3F;
    }
}

static uint8_t fetch(void) { return rd8(D.pc++); }
static uint16_t fetch16(void) { uint8_t lo = fetch(); return (uint16_t)(lo | fetch() << 8); }
static void push(uint8_t v) { wr8(0x100 + D.sp, v); D.sp--; }
static uint8_t pull(void) { D.sp++; return rd8(0x100 + D.sp); }
static void setzn(uint8_t v) { D.p = (uint8_t)((D.p & 0x7D) | (v == 0 ? 2 : 0) | (v & 0x80)); }
static int  getc_(void) { return D.p & 1; }
static void setc(int c) { D.p = (uint8_t)((D.p & 0xFE) | (c ? 1 : 0)); }
static void setv(int c) { D.p = (uint8_t)((D.p & 0xBF) | (c ? 0x40 : 0)); }

enum { M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY, M_IZX, M_IZY };
static uint16_t ea(int m) {
    switch (m) {
    case M_IMM: return D.pc++;
    case M_ZP:  return fetch();
    case M_ZPX: return (uint8_t)(fetch() + D.x);
    case M_ZPY: return (uint8_t)(fetch() + D.y);
    case M_ABS: return fetch16();
    case M_ABX: return (uint16_t)(fetch16() + D.x);
    case M_ABY: return (uint16_t)(fetch16() + D.y);
    case M_IZX: { uint8_t z = (uint8_t)(fetch() + D.x); return (uint16_t)(rd8(z) | rd8((uint8_t)(z + 1)) << 8); }
    default:    { uint8_t z = fetch(); return (uint16_t)((rd8(z) | rd8((uint8_t)(z + 1)) << 8) + D.y); }
    }
}
static void adc(uint8_t v) {
    unsigned r = (unsigned)D.a + v + (unsigned)getc_();
    setv((~(D.a ^ v)) & (D.a ^ r) & 0x80);
    setc(r > 0xFF);
    D.a = (uint8_t)r;
    setzn(D.a);
}
static void cmp(uint8_t r, uint8_t v) { setc(r >= v); setzn((uint8_t)(r - v)); }
static void branch(int cond) { int8_t o = (int8_t)fetch(); if (cond) D.pc = (uint16_t)(D.pc + o); }

static uint8_t rmw(int op, uint8_t v) {
    unsigned c;
    switch (op) {
    case 0: c = v >> 7; v = (uint8_t)(v << 1); setc((int)c); break;                          /* ASL */
    case 1: c = v >> 7; v = (uint8_t)((v << 1) | getc_()); setc((int)c); break;              /* ROL */
    case 2: c = v & 1; v >>= 1; setc((int)c); break;                                         /* LSR */
    case 3: c = v & 1; v = (uint8_t)((v >> 1) | (getc_() << 7)); setc((int)c); break;        /* ROR */
    case 4: v++; break;                                                                       /* INC */
    default: v--; break;                                                                      /* DEC */
    }
    setzn(v);
    return v;
}

/* returns 0, or 1 for an opcode this driver should never reach */
static int step(void) {
    uint8_t op = fetch();
    int grp = op & 3, aaa = op >> 5, bbb = (op >> 2) & 7;
    static const int m1[8] = { M_IZX, M_ZP, M_IMM, M_ABS, M_IZY, M_ZPX, M_ABY, M_ABX };   /* cc=01 */
    static const int m2[8] = { M_IMM, M_ZP, -1, M_ABS, -1, M_ZPX, -1, M_ABX };           /* cc=10 */
    static const int m0[8] = { M_IMM, M_ZP, -1, M_ABS, -1, M_ZPX, -1, M_ABX };           /* cc=00 */
    switch (op) {
    case 0x4C: D.pc = fetch16(); return 0;
    case 0x6C: { uint16_t p = fetch16(); D.pc = (uint16_t)(rd8(p) | rd8((uint16_t)((p & 0xFF00) | ((p + 1) & 0xFF))) << 8); return 0; }
    case 0x20: { uint16_t t = fetch16(), r = (uint16_t)(D.pc - 1); push((uint8_t)(r >> 8)); push((uint8_t)r); D.pc = t; return 0; }
    case 0x60: { uint8_t lo = pull(), hi = pull(); D.pc = (uint16_t)(((hi << 8 | lo) + 1) & 0xFFFF); return 0; }
    case 0x40: D.p = (uint8_t)((pull() & 0xCF) | 0x20); { uint8_t lo = pull(), hi = pull(); D.pc = (uint16_t)(hi << 8 | lo); } return 0;
    case 0x10: branch(!(D.p & 0x80)); return 0;  case 0x30: branch(D.p & 0x80); return 0;
    case 0x50: branch(!(D.p & 0x40)); return 0;  case 0x70: branch(D.p & 0x40); return 0;
    case 0x90: branch(!(D.p & 1)); return 0;     case 0xB0: branch(D.p & 1); return 0;
    case 0xD0: branch(!(D.p & 2)); return 0;     case 0xF0: branch(D.p & 2); return 0;
    case 0xAA: D.x = D.a; setzn(D.x); return 0;  case 0xA8: D.y = D.a; setzn(D.y); return 0;
    case 0x8A: D.a = D.x; setzn(D.a); return 0;  case 0x98: D.a = D.y; setzn(D.a); return 0;
    case 0xBA: D.x = D.sp; setzn(D.x); return 0; case 0x9A: D.sp = D.x; return 0;
    case 0xE8: D.x++; setzn(D.x); return 0;      case 0xC8: D.y++; setzn(D.y); return 0;
    case 0xCA: D.x--; setzn(D.x); return 0;      case 0x88: D.y--; setzn(D.y); return 0;
    case 0x48: push(D.a); return 0;              case 0x08: push((uint8_t)(D.p | 0x30)); return 0;
    case 0x68: D.a = pull(); setzn(D.a); return 0; case 0x28: D.p = (uint8_t)((pull() & 0xCF) | 0x20); return 0;
    case 0x18: setc(0); return 0; case 0x38: setc(1); return 0; case 0x58: D.p &= (uint8_t)~4; return 0;
    case 0x78: D.p |= 4; return 0; case 0xB8: setv(0); return 0; case 0xD8: D.p &= (uint8_t)~8; return 0;
    case 0xF8: D.p |= 8; return 0; case 0xEA: return 0;
    case 0x0A: D.a = rmw(0, D.a); return 0; case 0x2A: D.a = rmw(1, D.a); return 0;
    case 0x4A: D.a = rmw(2, D.a); return 0; case 0x6A: D.a = rmw(3, D.a); return 0;
    case 0x24: case 0x2C: { uint8_t v = rd8(ea(op == 0x24 ? M_ZP : M_ABS)); D.p = (uint8_t)((D.p & 0x3D) | (v & 0xC0) | ((v & D.a) ? 0 : 2)); return 0; }
    default: break;
    }
    if (grp == 1) {
        int m = m1[bbb];
        if (aaa == 4) {                                   /* STA */
            if (m == M_IMM) return 1;
            wr8(ea(m), D.a);
            return 0;
        }
        uint8_t v = rd8(ea(m));
        switch (aaa) {
        case 0: D.a |= v; setzn(D.a); break;
        case 1: D.a &= v; setzn(D.a); break;
        case 2: D.a ^= v; setzn(D.a); break;
        case 3: adc(v); break;
        case 5: D.a = v; setzn(v); break;
        case 6: cmp(D.a, v); break;
        default: adc((uint8_t)(v ^ 0xFF)); break;
        }
        return 0;
    }
    if (grp == 2) {
        int m = m2[bbb];
        if (aaa == 4 || aaa == 5) {                       /* STX / LDX */
            if (bbb == 5) m = M_ZPY;
            if (aaa == 5 && bbb == 7) m = M_ABY;
            if (m < 0) return 1;
            if (aaa == 4) { wr8(ea(m), D.x); return 0; }
            D.x = rd8(ea(m)); setzn(D.x); return 0;
        }
        if (aaa >= 6 && (bbb == 0 || m < 0)) return 1;
        if (m < 0 || m == M_IMM) return 1;
        uint16_t a = ea(m);
        int kind = aaa <= 3 ? aaa : aaa == 6 ? 5 : 4;     /* 0 ASL 1 ROL 2 LSR 3 ROR 6 DEC 7 INC */
        wr8(a, rmw(kind, rd8(a)));
        return 0;
    }
    if (grp == 0) {
        int m = m0[bbb];
        if (m < 0) return 1;
        if (aaa == 4) { if (bbb == 5) m = M_ZPX; wr8(ea(m), D.y); return 0; }    /* STY */
        if (aaa == 5) { D.y = rd8(ea(m)); setzn(D.y); return 0; }                 /* LDY */
        if (aaa == 6) { cmp(D.y, rd8(ea(m))); return 0; }                         /* CPY */
        if (aaa == 7) { cmp(D.x, rd8(ea(m))); return 0; }                         /* CPX */
    }
    return 1;
}

/* JSR addr with A, Y; false when the driver did not return within a sane count. */
static int call(uint16_t addr, uint8_t a, uint8_t y) {
    D.a = a; D.y = y; D.x = 0; D.sp = 0xEE; D.p = 0x24;
    D.ram[0x1F0] = 0; D.ram[0x1EF] = 1;                  /* return to $0002 (RTS adds one) */
    D.pc = addr;
    for (long n = 0; D.pc != 0x0002; n++) {
        if (n > 2000000 || step()) return 0;
    }
    D.r7 = 0x1F;
    return 1;
}

/* -------------------------------------------------------------- synth -- */

typedef struct {
    int      enabled, len, halt, const_vol, vol, duty, step;
    int      period, timer, env, env_div, env_start, env_loop;
    int      sweep_en, sweep_div, sweep_neg, sweep_shift, sweep_reload, sweep_cnt;
} Pulse;
typedef struct { int enabled, len, halt, lin_reload, lin_cnt, lin_load, control, period, timer, step; } Tri;
typedef struct { int enabled, len, halt, const_vol, vol, mode, period, timer, env, env_div, env_start, env_loop; unsigned lfsr; } Noise;

static Pulse   P2;
static Tri     T;
static Noise   N;
static int     s_have_pulse1;
static Pulse   P1;
static const uint8_t LEN[32] = { 10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
                                 12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30 };
static const uint8_t DUTY[4][8] = { { 0, 1, 0, 0, 0, 0, 0, 0 }, { 0, 1, 1, 0, 0, 0, 0, 0 }, { 0, 1, 1, 1, 1, 0, 0, 0 }, { 1, 0, 0, 1, 1, 1, 1, 1 } };
static const int NOISE_PER[16] = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };

static void pulse_write(Pulse *p, int reg, uint8_t v, int is_p2) {
    switch (reg) {
    case 0: p->duty = v >> 6; p->halt = (v >> 5) & 1; p->env_loop = p->halt; p->const_vol = (v >> 4) & 1; p->vol = v & 15; break;
    case 1: p->sweep_en = v >> 7; p->sweep_div = (v >> 4) & 7; p->sweep_neg = (v >> 3) & 1; p->sweep_shift = v & 7; p->sweep_reload = 1; break;
    case 2: p->period = (p->period & 0x700) | v; break;
    default:
        p->period = (p->period & 0xFF) | ((v & 7) << 8);
        if (p->enabled) p->len = LEN[v >> 3];
        p->step = 0; p->env_start = 1;
        break;
    }
    (void)is_p2;
}
static void apu_reg(uint16_t a, uint8_t v) {
    if (a >= 0x4000 && a <= 0x4003) pulse_write(&P1, (int)(a - 0x4000), v, 0);
    else if (a >= 0x4004 && a <= 0x4007) pulse_write(&P2, (int)(a - 0x4004), v, 1);
    else switch (a) {
    case 0x4008: T.control = v >> 7; T.halt = T.control; T.lin_load = v & 0x7F; break;
    case 0x400A: T.period = (T.period & 0x700) | v; break;
    case 0x400B: T.period = (T.period & 0xFF) | ((v & 7) << 8); if (T.enabled) T.len = LEN[v >> 3]; T.lin_reload = 1; break;
    case 0x400C: N.halt = (v >> 5) & 1; N.env_loop = N.halt; N.const_vol = (v >> 4) & 1; N.vol = v & 15; break;
    case 0x400E: N.mode = v >> 7; N.period = NOISE_PER[v & 15]; break;
    case 0x400F: if (N.enabled) N.len = LEN[v >> 3]; N.env_start = 1; break;
    case 0x4015:
        P1.enabled = v & 1; P2.enabled = (v >> 1) & 1; T.enabled = (v >> 2) & 1; N.enabled = (v >> 3) & 1;
        if (!P1.enabled) P1.len = 0;
        if (!P2.enabled) P2.len = 0;
        if (!T.enabled) T.len = 0;
        if (!N.enabled) N.len = 0;
        break;
    default: break;
    }
}

/* quarter / half frame clocks (the driver sets the 4-step sequence: 60 Hz) */
static void env_clock(int *env, int *div, int *start, int vol, int loop) {
    if (*start) { *start = 0; *env = 15; *div = vol; return; }
    if (*div == 0) { *div = vol; if (*env) (*env)--; else if (loop) *env = 15; } else (*div)--;
}
static void sweep_target(Pulse *p, int is_p2, int *target) {
    int d = p->period >> p->sweep_shift;
    *target = p->sweep_neg ? p->period - d - (is_p2 ? 0 : 1) : p->period + d;
}
static void pulse_half(Pulse *p, int is_p2) {
    if (!p->halt && p->len) p->len--;
    int tgt;
    sweep_target(p, is_p2, &tgt);
    if (p->sweep_cnt == 0 && p->sweep_en && p->sweep_shift && p->period >= 8 && tgt <= 0x7FF) p->period = tgt;
    if (p->sweep_cnt == 0 || p->sweep_reload) { p->sweep_cnt = p->sweep_div; p->sweep_reload = 0; } else p->sweep_cnt--;
}
static void frame_clocks(int half) {           /* called 4 times per 60 Hz tick: quarter frames; halves on 2 and 4 */
    env_clock(&P1.env, &P1.env_div, &P1.env_start, P1.vol, P1.env_loop);
    env_clock(&P2.env, &P2.env_div, &P2.env_start, P2.vol, P2.env_loop);
    env_clock(&N.env, &N.env_div, &N.env_start, N.vol, N.env_loop);
    if (T.lin_reload) T.lin_cnt = T.lin_load; else if (T.lin_cnt) T.lin_cnt--;
    if (!T.control) T.lin_reload = 0;
    if (half) {
        pulse_half(&P1, 0); pulse_half(&P2, 1);
        if (!T.halt && T.len) T.len--;
        if (!N.halt && N.len) N.len--;
    }
}

/* ----- the player -----
 * One driver tick per emulated frame, made at the frame-end callback (before the host mixes that frame's audio):
 * the tick's APU writes set the synthesiser's state, which the mix then renders for the frame, and the channels the
 * driver says its effect owns ($CF: bit 1 pulse 2, bit 2 triangle, bit 3 noise, bit 0 pulse 1) are muted in the
 * host APU for the NEXT frame's emulation (cyc_mod_set_apu_audio_mute), exactly as a console running both sound
 * drivers' priority rules plays: the effect owns the channel, the music on it is silent for the effect's duration,
 * and the music's own register writes (still made by the title's driver every frame) are heard again the frame the
 * effect releases the channel. */

enum { RATE = 48000 };
static double s_cpu_per_sample = 1789773.0 / RATE;     /* NTSC; the effects are NTSC */
static int    s_pending[16], s_npending;               /* requests the next tick passes to the driver */
static int    s_active;                                /* an effect is playing (or was requested): keep ticking */
static int    s_quiet_ticks;
static uint8_t s_owned;                                /* channels the driver's effect owns right now */
static double s_hp, s_prev;

static void synth_reset(void) {
    memset(&P1, 0, sizeof P1); memset(&P2, 0, sizeof P2); memset(&T, 0, sizeof T); memset(&N, 0, sizeof N);
    N.lfsr = 1; N.period = 4;
    s_hp = s_prev = 0;
}

static double mix_sample(double cycles) {
    /* run the timers for `cycles` CPU cycles, averaging nothing: one point sample per output sample is enough
     * for these short, band-limited-by-the-game effects */
    int c = (int)cycles;
    for (int i = 0; i < c; i++) {
        if (!(i & 1)) {   /* pulses clock at half the CPU rate */
            if (P2.timer == 0) { P2.timer = P2.period; P2.step = (P2.step + 1) & 7; } else P2.timer--;
            if (P1.timer == 0) { P1.timer = P1.period; P1.step = (P1.step + 1) & 7; } else P1.timer--;
        }
        if (N.timer == 0) {
            N.timer = N.period - 1;
            unsigned tap = N.mode ? 6 : 1, fb = (N.lfsr ^ (N.lfsr >> tap)) & 1;
            N.lfsr = (N.lfsr >> 1) | (fb << 14);
        } else N.timer--;
        if (T.timer == 0) { T.timer = T.period; if (T.len && T.lin_cnt) T.step = (T.step + 1) & 31; } else T.timer--;
    }
    int pulse = 0;
    Pulse *ps[2] = { &P1, &P2 };
    for (int k = 0; k < 2; k++) {
        Pulse *p = ps[k];
        int tgt;
        sweep_target(p, k, &tgt);
        if (p->len && p->period >= 8 && tgt <= 0x7FF && DUTY[p->duty][p->step])
            pulse += p->const_vol ? p->vol : p->env;
    }
    int tri = T.period >= 2 ? (T.step < 16 ? 15 - T.step : T.step - 16) : 0;
    int noise = (N.len && !(N.lfsr & 1)) ? (N.const_vol ? N.vol : N.env) : 0;
    double pl = pulse ? 95.88 / (8128.0 / pulse + 100) : 0;
    double tnd_in = tri / 8227.0 + noise / 12241.0;
    double tl = tnd_in > 0 ? 159.79 / (1.0 / tnd_in + 100) : 0;
    return pl + tl;
}

/* ---- API ---- */

static int s_incoming[16], s_nincoming, s_staged[16], s_nstaged;
void cyc_mm4_audio_configure(bool enabled, bool own_native_channels) {
    s_enabled = enabled;
    s_own_native_channels = own_native_channels;
    cyc_mod_set_apu_audio_mute(s_enabled && s_own_native_channels ? s_owned : 0);
}
uint8_t cyc_mm4_audio_owned_channels(void) { return s_owned; }
void cyc_mm4_audio_reset(void) {
    s_npending = s_nincoming = s_nstaged = 0;
    memset(s_pending, 0, sizeof s_pending);
    memset(s_incoming, 0, sizeof s_incoming);
    memset(s_staged, 0, sizeof s_staged);
    s_active = 0; s_quiet_ticks = 0; s_owned = 0;
    cyc_mod_set_apu_audio_mute(0);
    memset(&D, 0, sizeof D);
    synth_reset();
    if (!s_enabled || !cyc_mm4_donor.prg) { s_ready = 0; return; }
    D.r7 = 0x1F;
    s_ready = call(0x8003, 0xF0, 0) && call(0x8000, 0, 0);       /* the driver's own init request, one tick */
    D.writes = 0;
}

/* A request made during game frame N reaches Mega Man 4's driver in the NMI that ends frame N (its own sound queue
 * is drained there: that tick, "frame 0", only resets the channel) and the first tone is heard one NMI later.  The
 * request is held one frame-end call (staged) and made at the next, so the tone sounds in frame N+2. */
void cyc_mm4_audio_play(int sfx) {
    if (!s_ready || !s_enabled) return;
    if (s_nincoming < 16) s_incoming[s_nincoming++] = sfx;
}
void cyc_mm4_audio_stop(int sfx) { (void)sfx; }

/* One driver tick: pass the requests, tick, apply the APU writes it made. */
static void driver_tick(void) {
    D.writes = 0;
    for (int i = 0; i < s_npending; i++) call(0x8003, (uint8_t)s_pending[i], 0);
    s_npending = 0;
    call(0x8000, 0, 0);
    for (int i = 0; i < D.writes; i++) apu_reg(D.log[i].addr, D.log[i].value);
    s_owned = (uint8_t)(D.ram[0xCF] & 0x0F);
    if (!(P1.len || P2.len || T.len || N.len) && !s_owned) s_quiet_ticks++; else s_quiet_ticks = 0;
}

void cyc_mm4_audio_frame_end(void) {
    if (!cyc_mm4_donor.prg) { cyc_mm4_audio_reset(); return; }
    if (!s_ready) return;
    for (int i = 0; i < s_nstaged && s_npending < 16; i++) s_pending[s_npending++] = s_staged[i];
    memcpy(s_staged, s_incoming, sizeof s_staged);
    s_nstaged = s_nincoming;
    s_nincoming = 0;
    if (s_npending) { s_active = 1; s_quiet_ticks = 0; }
    if (!s_active) return;
    driver_tick();
    if (s_quiet_ticks >= 2 && !s_npending && !s_nstaged) {      /* finished: hand every channel back */
        s_active = 0;
        s_owned = 0;
    }
    cyc_mod_set_apu_audio_mute(s_own_native_channels ? s_owned : 0);  /* applies to the next frame's emulation */
}

/* Mix the effects over the game's audio for this frame (mono, 48 kHz).  The sequencer's four quarter-frame clocks
 * (halves on the 2nd and 4th) are spread over the frame's samples. */
void cyc_mm4_audio_mix(int16_t *samples, size_t count) {
    if (!s_ready || !s_active) return;
    size_t next_clock = 0, clock = 0;
    for (size_t i = 0; i < count; i++) {
        if (i == next_clock && clock < 4) {
            frame_clocks((int)(clock & 1));
            clock++;
            next_clock = count * clock / 4;
        }
        double level = mix_sample(s_cpu_per_sample);
        /* DC block (the console's output stage high-passes too) */
        double hp = 0.9975 * (s_hp + level - s_prev);
        s_prev = level; s_hp = hp;
        int v = samples[i] + (int)(hp * 30000.0);
        samples[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

/* The driver, staged NMI requests and synthesizer phase are one continuation.
 * Donor bytes remain runtime-only and are validated by the caller's profile. */
typedef struct {
    char magic[4];
    int enabled, own_native_channels, ready, have_pulse1;
    Drv driver;
    Pulse pulse1, pulse2;
    Tri triangle;
    Noise noise;
    int pending[16], incoming[16], staged[16];
    int npending, nincoming, nstaged, active, quiet_ticks;
    uint8_t owned;
    double hp, previous;
} AudioSnapshot;

int cyc_mm4_audio_state_get(uint8_t *out, int capacity) {
    if (!out || capacity < (int)sizeof(AudioSnapshot)) return -1;
    AudioSnapshot next;
    memset(&next, 0, sizeof next);
    memcpy(next.magic, "M4A1", 4);
    next.enabled = s_enabled; next.own_native_channels = s_own_native_channels;
    next.ready = s_ready; next.have_pulse1 = s_have_pulse1;
    next.driver = D; next.pulse1 = P1; next.pulse2 = P2;
    next.triangle = T; next.noise = N;
    memcpy(next.pending, s_pending, sizeof s_pending);
    memcpy(next.incoming, s_incoming, sizeof s_incoming);
    memcpy(next.staged, s_staged, sizeof s_staged);
    next.npending = s_npending; next.nincoming = s_nincoming; next.nstaged = s_nstaged;
    next.active = s_active; next.quiet_ticks = s_quiet_ticks; next.owned = s_owned;
    next.hp = s_hp; next.previous = s_prev;
    memcpy(out, &next, sizeof next);
    return sizeof next;
}

static int pulse_valid(const Pulse *p) {
    return p->duty >= 0 && p->duty < 4 && p->step >= 0 && p->step < 8 &&
        p->period >= 0 && p->period <= 0x7FF && p->timer >= 0 && p->timer <= 0x7FF &&
        p->vol >= 0 && p->vol <= 15 && p->env >= 0 && p->env <= 15 &&
        p->sweep_shift >= 0 && p->sweep_shift <= 7 && p->len >= 0 && p->len <= 254 &&
        (unsigned)p->enabled <= 1 && (unsigned)p->halt <= 1 && (unsigned)p->const_vol <= 1 &&
        (unsigned)p->env_start <= 1 && (unsigned)p->env_loop <= 1 &&
        (unsigned)p->sweep_en <= 1 && (unsigned)p->sweep_neg <= 1 && (unsigned)p->sweep_reload <= 1 &&
        (unsigned)p->env_div <= 15 && (unsigned)p->sweep_div <= 7 && (unsigned)p->sweep_cnt <= 7;
}
int cyc_mm4_audio_state_valid(const uint8_t *data, int size) {
    if (!size) return !s_ready;
    if (!data || size != (int)sizeof(AudioSnapshot)) return 0;
    AudioSnapshot next;
    memcpy(&next, data, sizeof next);
    return !memcmp(next.magic, "M4A1", 4) && next.enabled == s_enabled &&
        next.own_native_channels == s_own_native_channels &&
        next.ready >= 0 && next.ready <= 1 && (!next.ready || cyc_mm4_donor.prg) &&
        (unsigned)next.have_pulse1 <= 1 && next.driver.sel <= 7 &&
        next.driver.r7 < 64 && next.driver.writes >= 0 && next.driver.writes <= 256 &&
        next.npending >= 0 && next.npending <= 16 && next.nincoming >= 0 && next.nincoming <= 16 &&
        next.nstaged >= 0 && next.nstaged <= 16 && next.active >= 0 && next.active <= 1 &&
        next.quiet_ticks >= 0 && next.quiet_ticks <= 2 && !(next.owned & 0xF0) &&
        pulse_valid(&next.pulse1) && pulse_valid(&next.pulse2) &&
        next.triangle.step >= 0 && next.triangle.step < 32 &&
        next.triangle.period >= 0 && next.triangle.period <= 0x7FF &&
        next.triangle.timer >= 0 && next.triangle.timer <= 0x7FF &&
        (unsigned)next.triangle.enabled <= 1 && (unsigned)next.triangle.halt <= 1 &&
        (unsigned)next.triangle.lin_reload <= 1 && (unsigned)next.triangle.control <= 1 &&
        (unsigned)next.triangle.lin_cnt <= 127 && (unsigned)next.triangle.lin_load <= 127 &&
        (unsigned)next.triangle.len <= 254 &&
        next.noise.mode >= 0 && next.noise.mode <= 1 && next.noise.lfsr <= 0x7FFF &&
        next.noise.period >= 0 && next.noise.period <= 4068 &&
        next.noise.timer >= 0 && next.noise.timer <= 4068 &&
        (unsigned)next.noise.enabled <= 1 && (unsigned)next.noise.halt <= 1 &&
        (unsigned)next.noise.const_vol <= 1 && (unsigned)next.noise.env_start <= 1 &&
        (unsigned)next.noise.env_loop <= 1 && (unsigned)next.noise.vol <= 15 &&
        (unsigned)next.noise.env <= 15 && (unsigned)next.noise.env_div <= 15 &&
        (unsigned)next.noise.len <= 254 &&
        isfinite(next.hp) && isfinite(next.previous) && fabs(next.hp) <= 16 && fabs(next.previous) <= 16;
}
int cyc_mm4_audio_state_set(const uint8_t *data, int size) {
    if (!cyc_mm4_audio_state_valid(data, size)) return 0;
    if (!size) { cyc_mm4_audio_reset(); return 1; }
    AudioSnapshot next;
    memcpy(&next, data, sizeof next);
    s_ready = next.ready; s_have_pulse1 = next.have_pulse1;
    D = next.driver; P1 = next.pulse1; P2 = next.pulse2;
    T = next.triangle; N = next.noise;
    memcpy(s_pending, next.pending, sizeof s_pending);
    memcpy(s_incoming, next.incoming, sizeof s_incoming);
    memcpy(s_staged, next.staged, sizeof s_staged);
    s_npending = next.npending; s_nincoming = next.nincoming; s_nstaged = next.nstaged;
    s_active = next.active; s_quiet_ticks = next.quiet_ticks; s_owned = next.owned;
    s_hp = next.hp; s_prev = next.previous;
    cyc_mod_set_apu_audio_mute(s_own_native_channels ? s_owned : 0);
    return 1;
}

#ifdef CYC_MM4_AUDIO_TEST
/* tools/test_mm4_audio.c: print each driver tick's APU writes, to compare with a reference run of the driver */
#include <stdio.h>
#include <stdlib.h>
void cyc_mm4_audio_selftest_dump(FILE *f, int sfx, int frames) {
    memset(&D, 0, sizeof D);
    D.r7 = 0x1F;
    call(0x8003, 0xF0, 0); call(0x8000, 0, 0);
    for (int fr = 0; fr < frames; fr++) {
        D.writes = 0;
        if (fr == 0) call(0x8003, (uint8_t)sfx, 0);
        call(0x8000, 0, 0);
        fprintf(f, "f%d:", fr);
        for (int i = 0; i < D.writes; i++) fprintf(f, " %04X=%02X", D.log[i].addr, D.log[i].value);
        fprintf(f, " | cf=%02X ce=%02X\n", D.ram[0xCF], D.ram[0xCE]);
    }
}
/* render n samples of an effect to 16-bit mono PCM for listening/inspection */
void cyc_mm4_audio_selftest_render(int sfx, int16_t *out, size_t n) {
    memset(out, 0, n * 2);
    cyc_mm4_audio_reset();
    cyc_mm4_audio_play(sfx);
    for (size_t at = 0; at < n; at += 800) {          /* one emulated frame at a time, as the host does */
        cyc_mm4_audio_frame_end();
        cyc_mm4_audio_mix(out + at, n - at < 800 ? n - at : 800);
    }
}
#endif
