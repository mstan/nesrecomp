#include "ppu_hle_kernel.h"
#include <stdio.h>
#include <string.h>

static uint32_t random_state = 0x731ac419u;
static uint8_t next_byte(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return (uint8_t)random_state;
}

int main(void)
{
    /* Every planar byte combination, including transparent rows, both top
     * bits and every pixel position. */
    for (unsigned lo = 0; lo < 256; ++lo) for (unsigned hi = 0; hi < 256; ++hi) {
        uint32_t packed = ppu_hle_row((uint8_t)lo, (uint8_t)hi);
        if (ppu_hle_plane(packed, 0) != lo || ppu_hle_plane(packed, 1) != hi) return 1;
        for (unsigned x = 0; x < 8; ++x)
            if (((packed >> (2 * x)) & 3) != (((lo >> x) & 1) | (((hi >> x) & 1) << 1))) return 2;
    }
    /* Packed background/attribute histories across row replacement, arbitrary
     * fine-X, shifts and the high-plane serial fill. */
    uint16_t lo = 0, hi = 0, al = 0, ah = 0;
    uint32_t background = 0, attributes = 0;
    for (unsigned dot = 0; dot < 32768; ++dot) {
        if (!(dot & 7)) {
            uint8_t l = next_byte(), h = next_byte();
            lo = (uint16_t)((lo & 0xff00) | l); hi = (uint16_t)((hi & 0xff00) | h);
            background = (background & 0xffff0000u) | ppu_hle_row(l, h);
        }
        for (unsigned fx = 0; fx < 8; ++fx) {
            unsigned color = ((lo >> (15 - fx)) & 1) | (((hi >> (15 - fx)) & 1) << 1);
            unsigned palette = ((al >> (7 - fx)) & 1) | (((ah >> (7 - fx)) & 1) << 1);
            if (((background >> (30 - 2 * fx)) & 3) != color ||
                ((attributes >> (14 - 2 * fx)) & 3) != palette) return 3;
        }
        uint8_t latch = next_byte() & 3;
        lo = (uint16_t)(lo << 1); hi = (uint16_t)((hi << 1) | 1);
        al = (uint16_t)((al << 1) | (latch & 1)); ah = (uint16_t)((ah << 1) | (latch >> 1));
        background = (background << 2) | 2; attributes = (attributes << 2) | latch;
        if (ppu_hle_plane(background, 0) != lo || ppu_hle_plane(background, 1) != hi ||
            ppu_hle_plane(attributes, 0) != al || ppu_hle_plane(attributes, 1) != ah) return 4;
    }
    /* Eight independent sprite lanes; ensure counter 1 reaches zero without
     * shifting on that same dot, clipping/priority search skips transparent
     * earlier lanes, rendering-off still counts X down, odd skip shifts all. */
    for (unsigned trial = 0; trial < 8192; ++trial) for (unsigned mode = 0; mode < 4; ++mode) {
        uint8_t x[8], l[8], h[8], rx[8], rl[8], rh[8];
        bool render = (mode & 1) != 0, skipped = (mode & 2) != 0;
        unsigned expected_mask = 0, expected_shift = 0;
        for (unsigned i = 0; i < 8; ++i) {
            x[i] = (uint8_t)((trial & 7) ? next_byte() : i & 1);
            l[i] = next_byte(); h[i] = next_byte();
            if ((!x[i] || skipped) && ((l[i] | h[i]) & 0x80)) expected_mask |= 1u << i;
        }
        if (ppu_hle_sprite_mask(x, l, h, skipped) != expected_mask) return 5;
        if (expected_mask) {
            unsigned first = 0; while (!(expected_mask & (1u << first))) ++first;
            if (ppu_hle_first_sprite(expected_mask) != first) return 6;
        }
        memcpy(rx, x, 8); memcpy(rl, l, 8); memcpy(rh, h, 8);
        for (unsigned i = 0; i < 8; ++i) {
            if (rx[i] && !skipped) --rx[i];
            else if (render) { rl[i] <<= 1; rh[i] <<= 1; expected_shift |= 1u << i; }
        }
        if (ppu_hle_sprite_shift(x, l, h, render, skipped) != expected_shift ||
            memcmp(x, rx, 8) || memcmp(l, rl, 8) || memcmp(h, rh, 8)) return 7;
    }
    puts("ppu_hle_kernel_test: PASS");
    return 0;
}
