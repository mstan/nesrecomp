#ifndef NESRECOMP_PPU_IMPLEMENTATION_H
#define NESRECOMP_PPU_IMPLEMENTATION_H

/* Fixed by the build, never by an environment variable or player setting. */
#ifndef NESRECOMP_PPU_HLE
#define NESRECOMP_PPU_HLE 0
#endif
#if NESRECOMP_PPU_HLE != 0 && NESRECOMP_PPU_HLE != 1
#error "NESRECOMP_PPU_HLE must be 0 (LLE) or 1 (HLE)"
#endif
#if NESRECOMP_PPU_HLE && !defined(__SSE2__) && !defined(_M_X64) && (!defined(_M_IX86_FP) || _M_IX86_FP < 2)
#error "The packed PPU HLE experiment requires an SSE2 build; select LLE on other hosts"
#endif
#endif
