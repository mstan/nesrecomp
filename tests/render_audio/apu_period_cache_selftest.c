#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "apu.h"
#include "apu_shadow.h"

uint8_t apu_dmc_read(uint16_t addr) {
    return (uint8_t)(((addr * 13u) ^ (addr >> 3) ^ 0xA5u) & 0xFFu);
}

int recomp_audio_debug_enabled(void) {
    return 0;
}

void apu_shadow_init(void) {
}

bool apu_shadow_enabled(void) {
    return false;
}

int16_t apu_shadow_sample(int16_t canon, const ApuChannelLevels *lv) {
    (void)lv;
    return canon;
}

static uint32_t fnv1a_update(uint32_t h, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t hash_step(uint32_t h, const char *label, int chunk, int nsamp) {
    int16_t pcm[1024];
    uint8_t state[256];

    if (nsamp > (int)(sizeof(pcm) / sizeof(pcm[0])))
        return 0;

    apu_clock_cycles(chunk);
    int stall = apu_take_dmc_stall();
    if (stall)
        apu_clock_cycles(stall);
    apu_generate(pcm, nsamp);

    int state_len = apu_get_state_blob(state, sizeof(state));
    if (state_len <= 0)
        return 0;

    uint8_t status = apu_read_status();
    uint8_t irq = apu_irq_asserted() ? 1u : 0u;

    h = fnv1a_update(h, label, strlen(label));
    h = fnv1a_update(h, &stall, sizeof(stall));
    h = fnv1a_update(h, pcm, (size_t)nsamp * sizeof(pcm[0]));
    h = fnv1a_update(h, &state_len, sizeof(state_len));
    h = fnv1a_update(h, state, (size_t)state_len);
    h = fnv1a_update(h, &status, sizeof(status));
    h = fnv1a_update(h, &irq, sizeof(irq));
    return h;
}

static void write_reg(uint16_t addr, uint8_t val) {
    apu_write(addr, val);
}

static void mutate_timer_period_sources(void) {
    write_reg(0x4002, 0xFF);
    write_reg(0x4003, 0x07);
    write_reg(0x4006, 0x7F);
    write_reg(0x4007, 0x07);
    write_reg(0x400A, 0xFF);
    write_reg(0x400B, 0x07);
    write_reg(0x400E, 0x00);
    write_reg(0x4010, 0x00);
}

static uint32_t truncated_restore_checks(uint32_t h,
                                         const uint8_t *saved,
                                         int saved_len) {
    /* Prefixes are long enough to decode, then truncate after, selected live
     * period sources: pulse 1 timer, pulse 2 timer, triangle timer, noise
     * period index, and DMC rate index. Rejected restores intentionally keep
     * the legacy partial-write behavior; cached derived periods must not remain
     * stale after those partial writes. */
    const int prefixes[] = { 8, 26, 41, 46, 57 };
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (prefixes[i] >= saved_len)
            return 0;
        mutate_timer_period_sources();
        if (apu_set_state_blob(saved, prefixes[i]) != 0) {
            fprintf(stderr, "truncated restore unexpectedly succeeded at %d\n",
                    prefixes[i]);
            return 0;
        }
        h = hash_step(h, "truncated-restore", 157 + (int)i * 17, 40);
        if (!h)
            return 0;
    }
    return h;
}

int main(void) {
    /* This catches uninitialized cached periods after a caller mutates a timer
     * register before the formal reset path has run. */
    write_reg(0x4002, 0x20);
    write_reg(0x400A, 0x02);
    apu_clock_cycles(16);

    apu_init();
    uint32_t h = 2166136261u;

    const uint16_t regs[] = {
        0x4000, 0x4001, 0x4002, 0x4003, 0x4004, 0x4005, 0x4006, 0x4007,
        0x4008, 0x400A, 0x400B, 0x400C, 0x400E, 0x400F, 0x4010, 0x4011,
        0x4012, 0x4013, 0x4015, 0x4017
    };
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        write_reg(regs[i], (uint8_t)(0x11u + i * 17u));
        h = hash_step(h, "direct-write", 37 + (int)i * 19, 32);
        if (!h)
            return 1;
    }

    write_reg(0x4000, 0x3F);
    write_reg(0x4001, 0x8F);
    write_reg(0x4002, 0x20);
    write_reg(0x4003, 0x08);
    write_reg(0x4004, 0x7F);
    write_reg(0x4005, 0x88);
    write_reg(0x4006, 0x40);
    write_reg(0x4007, 0x10);
    write_reg(0x4008, 0x80);
    write_reg(0x400A, 0x02);
    write_reg(0x400B, 0x08);
    write_reg(0x400C, 0x1F);
    write_reg(0x400E, 0x8F);
    write_reg(0x400F, 0x08);
    write_reg(0x4010, 0x8F);
    write_reg(0x4011, 0x40);
    write_reg(0x4012, 0x00);
    write_reg(0x4013, 0x02);
    write_reg(0x4015, 0x1F);

    for (int i = 0; i < 220; i++) {
        h = hash_step(h, "sweep-dmc", 113 + (i % 11), 48);
        if (!h)
            return 1;
    }

    uint8_t saved[256];
    int saved_len = apu_get_state_blob(saved, sizeof(saved));
    if (saved_len <= 0)
        return 1;

    h = truncated_restore_checks(h, saved, saved_len);
    if (!h)
        return 1;

    write_reg(0x4002, 0xFF);
    write_reg(0x4003, 0x07);
    write_reg(0x400A, 0xFF);
    write_reg(0x400B, 0x07);
    write_reg(0x400E, 0x00);
    write_reg(0x4010, 0x00);
    if (!apu_set_state_blob(saved, saved_len)) {
        fprintf(stderr, "restore failed\n");
        return 1;
    }

    for (int i = 0; i < 120; i++) {
        h = hash_step(h, "post-restore", 211 + (i % 7), 64);
        if (!h)
            return 1;
    }

    const uint32_t expected = 0xd1b9fb00u;
    if (h != expected) {
        fprintf(stderr, "APU regression hash mismatch: got %08x expected %08x\n",
                h, expected);
        return 1;
    }
    return 0;
}
