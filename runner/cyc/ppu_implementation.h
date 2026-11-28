#ifndef NESRECOMP_PPU_IMPLEMENTATION_H
#define NESRECOMP_PPU_IMPLEMENTATION_H

/* Fixed by the build, never by an environment variable or player setting. */
#ifndef NESRECOMP_PPU_HLE
#define NESRECOMP_PPU_HLE 1
#endif
#if NESRECOMP_PPU_HLE != 0 && NESRECOMP_PPU_HLE != 1
#error "NESRECOMP_PPU_HLE must be 0 (LLE) or 1 (HLE)"
#endif
/* FRAME owns picture production and mapper deadlines. It is a separate,
 * experimental build profile, never a player setting or per-match switch. */
#ifndef NESRECOMP_PPU_FRAME
#define NESRECOMP_PPU_FRAME 0
#endif
#if NESRECOMP_PPU_FRAME != 0 && NESRECOMP_PPU_FRAME != 1
#error "NESRECOMP_PPU_FRAME must be 0 or 1"
#endif
/* ppu_hle_kernel.h selects SSE2 only for the x86-64 ABI and supplies the
 * same scalar interface elsewhere. HLE must not reject a portable host. */
#endif
