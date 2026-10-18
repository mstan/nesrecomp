/* Drive the production present decision with a virtual blocking display.
 * Guest work costs 1 ms; scan-out costs a refresh interval. Verify elapsed
 * guest time rather than merely testing individual branch conditions. */
#include "cyc_frame_pacing.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void display(unsigned hz, double guest_hz, bool unpaced)
{
    const uint64_t second = 1000000, period = (uint64_t)(second / guest_hz);
    const uint64_t refresh = second / hz, duration = 10 * second;
    uint64_t now = 0, next = 0, shown = 0;
    unsigned frames = 0, pictures = 0;
    while (now < duration) {
        now += 1000;
        ++frames;
        /* The renderer is unsynced during fast-forward / uncapped mode. */
        if (cyc_frame_present_due(now, next, period, shown, !unpaced, unpaced)) {
            if (!unpaced) now = (now / refresh + 1) * refresh;
            shown = now;
            ++pictures;
        }
        if (unpaced) next = now;
        else {
            next += period;
            if (next > now) now = next;
            else if (now - next > second / 4) next = now;
        }
    }
    if (unpaced) CHECK(frames == 10000 && pictures == frames);
    else {
        long difference = (long)frames - (long)(guest_hz * 10);
        CHECK(difference >= -2 && difference <= 2);
        CHECK(pictures <= frames && pictures > 0);
        if (hz + 1 < guest_hz) CHECK(pictures < frames);
    }
    printf("display=%u guest=%.4f unpaced=%u frames=%u pictures=%u\n",
           hz, guest_hz, unpaced, frames, pictures);
}

int main(void)
{
    const unsigned rates[] = {30, 50, 60, 120, 144};
    for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        display(rates[i], 60.0988, false);
        display(rates[i], 50.0069779683, false);
        display(rates[i], 60.0988, true);
    }
    /* Presentation remains responsive even when guest work cannot catch up. */
    CHECK(!cyc_frame_present_due(90000, 0, 16639, 85000, true, false));
    CHECK(cyc_frame_present_due(190000, 0, 16639, 85000, true, false));
    printf("frame_pacing_test: %u failures\n", failures);
    return failures != 0;
}
