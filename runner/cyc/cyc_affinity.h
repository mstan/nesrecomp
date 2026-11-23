/*
 * cyc_affinity.h - keep the window's two busy threads on separate cores.
 *
 * With the emulation thread (a game's present_from_snapshot), the main thread
 * draws in-between pictures while the next NES frame runs on the emulation
 * thread. Left to the scheduler, both can end up taking turns on one core
 * (Contra at 240: bursts of frames where the emulation only ran after the
 * pictures were drawn, ~18-22 ms instead of ~9, and the picture rate fell to
 * 120). cyc_affinity_pin() gives the emulation thread a core of its own and
 * keeps the main thread on the other cores that share its L3 cache.
 *
 * Linux only for now (elsewhere it does nothing). CYC_PIN=0 turns it off.
 */
#ifndef CYC_AFFINITY_H
#define CYC_AFFINITY_H

enum { CYC_PIN_MAIN = 0, CYC_PIN_EMU = 1 };

/* Call from the thread itself: CYC_PIN_MAIN on the main thread when the
 * emulation thread starts, CYC_PIN_EMU at the top of the emulation thread. */
void cyc_affinity_pin(int role);

#endif
