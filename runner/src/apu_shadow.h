/*
 * apu_shadow.h — NES APU "verified-enhancement" float shadow re-render.
 *
 * QoL-on-top-of-correctness per recomp-template/PRINCIPLES.md
 * ("Verified-Enhancement HLE Is Allowed; Load-Bearing HLE Is Not"):
 *
 *   The canon APU (apu.c) still runs and still produces the int16 sample
 *   stream that is queued to SDL — that stream stays the authoritative output
 *   AND the verify oracle. This shadow re-renders the SAME per-channel levels
 *   the canon mixer just produced, but:
 *     - in float, free of the canon path's int16 requantization, and
 *     - through the hardware's TRUE NONLINEAR DAC mix (the NESDev "accurate"
 *       formula) instead of apu.c's documented LINEAR approximation.
 *   It is policed every output sample by the engine-agnostic ShadowVerifier
 *   (audio_shadow.h): it substitutes only after a proven window and reverts
 *   loudly (logs a DEGRADED line) the instant it stops matching.
 *
 * Opt-in, present-time, OFF by default (env NESRECOMP_AUDIO_SHADOW=1). With it
 * off the canon int16 is returned verbatim => byte-identical output.
 *
 * Worst-case failure: "the user hears the authentic linear-mix APU output."
 * It cannot mask an APU bug, because the canon path it shadows is exactly the
 * stream it is diffed against.
 *
 * The verifier is ported from JRickey/gba-recomp via the gbarecomp/snesrecomp
 * ports (see audio_shadow.h). The NES nonlinear mixer formula is the public
 * NESDev-wiki APU mixer; the render harness is ours.
 */
#ifndef NESRECOMP_APU_SHADOW_H
#define NESRECOMP_APU_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-output-sample channel levels captured from the canon APU after it
 * computed this sample. pulse1/pulse2/noise are 0..15, triangle 0..15,
 * dmc 0..127 — exactly the inputs the canon mixer used. */
typedef struct {
  uint8_t pulse1;    /* 0..15 */
  uint8_t pulse2;    /* 0..15 */
  uint8_t triangle;  /* 0..15 */
  uint8_t noise;     /* 0..15 */
  uint8_t dmc;       /* 0..127 */
} ApuChannelLevels;

/* Initialize the shadow from the environment (NESRECOMP_AUDIO_SHADOW). Safe to
 * call at apu_init time. Default OFF. */
void apu_shadow_init(void);

/* True if the shadow was enabled via env. When false, every other entry point
 * is a no-op and the canon sample is always returned. */
bool apu_shadow_enabled(void);

/* Feed one output sample: the canon int16 the linear mixer produced, plus the
 * channel levels that fed it. Returns the int16 to actually emit:
 *   - canon verbatim when disabled, or while the verifier has not proven a
 *     window (PROBATION),
 *   - the float nonlinear re-render (gain-calibrated, clamped) once proven.
 * On a loud revert this logs a single DEGRADED line and falls back to canon. */
int16_t apu_shadow_sample(int16_t canon, const ApuChannelLevels* lv);

#ifdef __cplusplus
}
#endif

#endif /* NESRECOMP_APU_SHADOW_H */
