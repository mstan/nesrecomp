#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "apu_shadow.h"

#define DECL(prefix) \
void prefix##_apu_init(void); \
void prefix##_apu_write(uint16_t addr, uint8_t val); \
uint8_t prefix##_apu_read_status(void); \
void prefix##_apu_generate(int16_t *buf, int n_samples); \
void prefix##_apu_clock_cycles(int cpu_cycles); \
int prefix##_apu_take_dmc_stall(void); \
bool prefix##_apu_irq_asserted(void);

DECL(base)
DECL(cand)

uint8_t apu_dmc_read(uint16_t addr) { return (uint8_t)(((addr * 13u) ^ (addr >> 3) ^ 0xA5u) & 0xFFu); }
int recomp_audio_debug_enabled(void) { return 0; }
void apu_shadow_init(void) {}
bool apu_shadow_enabled(void) { return false; }
int16_t apu_shadow_sample(int16_t canon, const ApuChannelLevels* lv) { (void)lv; return canon; }

static void write_base(uint16_t a, uint8_t v) { base_apu_write(a, v); }
static void write_cand(uint16_t a, uint8_t v) { cand_apu_write(a, v); }

static void setup_base(void) {
    base_apu_init();
    write_base(0x4000, 0x3F); write_base(0x4001, 0x8F); write_base(0x4002, 0x20); write_base(0x4003, 0x08);
    write_base(0x4004, 0x7F); write_base(0x4005, 0x88); write_base(0x4006, 0x40); write_base(0x4007, 0x10);
    write_base(0x4008, 0x80); write_base(0x400A, 0x02); write_base(0x400B, 0x08);
    write_base(0x400C, 0x1F); write_base(0x400E, 0x8F); write_base(0x400F, 0x08);
    write_base(0x4010, 0x8F); write_base(0x4011, 0x40); write_base(0x4012, 0x00); write_base(0x4013, 0x02); write_base(0x4015, 0x1F);
}
static void setup_cand(void) {
    cand_apu_init();
    write_cand(0x4000, 0x3F); write_cand(0x4001, 0x8F); write_cand(0x4002, 0x20); write_cand(0x4003, 0x08);
    write_cand(0x4004, 0x7F); write_cand(0x4005, 0x88); write_cand(0x4006, 0x40); write_cand(0x4007, 0x10);
    write_cand(0x4008, 0x80); write_cand(0x400A, 0x02); write_cand(0x400B, 0x08);
    write_cand(0x400C, 0x1F); write_cand(0x400E, 0x8F); write_cand(0x400F, 0x08);
    write_cand(0x4010, 0x8F); write_cand(0x4011, 0x40); write_cand(0x4012, 0x00); write_cand(0x4013, 0x02); write_cand(0x4015, 0x1F);
}

static double now_ms(void) { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart; }
static uint32_t checksum16(const int16_t *p, int n) { uint32_t h = 2166136261u; for (int i = 0; i < n; i++) { uint16_t v = (uint16_t)p[i]; h = (h ^ (v & 0xffu)) * 16777619u; h = (h ^ (v >> 8)) * 16777619u; } return h; }

static double run_base(int frames, uint32_t *out) {
    int16_t pcm[735]; *out = 2166136261u; setup_base(); double t0 = now_ms();
    for (int f = 0; f < frames; f++) { base_apu_clock_cycles(29830); int stall = base_apu_take_dmc_stall(); if (stall) base_apu_clock_cycles(stall); base_apu_generate(pcm, 735); *out = (*out ^ checksum16(pcm, 735)) * 16777619u + (uint32_t)base_apu_read_status(); }
    return now_ms() - t0;
}
static double run_cand(int frames, uint32_t *out) {
    int16_t pcm[735]; *out = 2166136261u; setup_cand(); double t0 = now_ms();
    for (int f = 0; f < frames; f++) { cand_apu_clock_cycles(29830); int stall = cand_apu_take_dmc_stall(); if (stall) cand_apu_clock_cycles(stall); cand_apu_generate(pcm, 735); *out = (*out ^ checksum16(pcm, 735)) * 16777619u + (uint32_t)cand_apu_read_status(); }
    return now_ms() - t0;
}

int main(void) {
    const int frames = 900;
    for (int i = 0; i < 2; i++) { uint32_t hb, hc; (void)run_base(120, &hb); (void)run_cand(120, &hc); }
    for (int i = 0; i < 7; i++) {
        uint32_t hb, hc; double tb, tc;
        if (i & 1) { tc = run_cand(frames, &hc); tb = run_base(frames, &hb); }
        else { tb = run_base(frames, &hb); tc = run_cand(frames, &hc); }
        printf("run=%d base_ms=%.3f cand_ms=%.3f elapsed_reduction=%.2f%% base_hash=%08x cand_hash=%08x\n", i, tb, tc, (tb - tc) * 100.0 / tb, hb, hc);
    }
    return 0;
}
