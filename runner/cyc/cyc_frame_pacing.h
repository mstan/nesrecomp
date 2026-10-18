/* Presentation debt never changes the number or timing of guest frames.
 * Skip a picture while catching up after a blocking present on a slower
 * display. Still refresh at least every six guest periods on an overloaded
 * host, so a machine that cannot catch up does not leave a frozen picture. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

static inline bool cyc_frame_present_due(uint64_t now, uint64_t next,
                                        uint64_t period, uint64_t shown,
                                        bool synced, bool unpaced)
{
    return unpaced || !synced || now < next || now - next < period ||
           now - shown >= 6 * period;
}
