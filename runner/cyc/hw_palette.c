/*
 * hw_palette.c - ARGB colors for the PPU's 9-bit color indices.
 *
 * The 2C02 does not output RGB. For each color it produces a square wave
 * between two voltage levels, whose phase is the hue, and the emphasis bits
 * attenuate part of each color cycle. This decodes that signal the way an
 * NTSC receiver would: sample 12 points of one color cycle, take luma as the
 * average and chroma as the fundamental relative to color burst (which has
 * the phase of hue 8), and convert YUV to RGB. Voltage levels are the
 * measured 2C02 levels from the nesdev wiki ("NTSC video").
 *
 * The emulated machine never sees these values: comparisons and hashes use
 * the color indices.
 */
#include "hw_internal.h"

#include <math.h>

uint32_t hw_palette_argb[512];

static const double level_low[4] = {0.350, 0.518, 0.962, 1.550};
static const double level_high[4] = {1.094, 1.506, 1.962, 1.962};
static const double black = 0.518, white = 1.962, emphasis_attenuation = 0.746;

static bool in_color_phase(int hue, int phase) { return (hue + phase) % 12 < 6; }

static uint8_t to_byte(double c)
{
    if (c <= 0) return 0;
    if (c >= 1) return 255;
    return (uint8_t)(c * 255.0 + 0.5);
}

void hw_palette_init(void)
{
    const double pi = 3.14159265358979323846;
    for (int index = 0; index < 512; index++) {
        int hue = index & 0x0F, level = (index >> 4) & 3, emphasis = index >> 6;
        if (hue > 13) level = 1;
        double low = level_low[level], high = level_high[level];
        if (hue == 0) low = high;
        if (hue > 12) high = low;

        double y = 0, u = 0, v = 0;
        for (int p = 0; p < 12; p++) {
            double s = in_color_phase(hue, p) ? high : low;
            if (((emphasis & 1) && in_color_phase(0, p)) || ((emphasis & 2) && in_color_phase(4, p)) ||
                ((emphasis & 4) && in_color_phase(8, p)))
                s *= emphasis_attenuation;
            s = (s - black) / (white - black);
            /* Reference phase chosen so hue 8 decodes at burst phase (180
             * degrees) and hues advance by 30 degrees. */
            double angle = pi * (p - 0.5) / 6.0;
            y += s;
            u += s * cos(angle);
            v -= s * sin(angle);
        }
        y /= 12.0;
        u *= 2.0 / 12.0;
        v *= 2.0 / 12.0;

        double r = y + 1.140 * v;
        double g = y - 0.395 * u - 0.581 * v;
        double b = y + 2.032 * u;
        hw_palette_argb[index] = 0xFF000000u | (uint32_t)to_byte(r) << 16 | (uint32_t)to_byte(g) << 8 | to_byte(b);
    }
}
