// audio_shadow.h — engine-agnostic HLE-shadow differential verifier (C).
//
// The gate that makes a "verified-enhancement shadow" SAFE: a higher-fidelity
// re-render of a guest audio engine (here the NES APU's five channels, mixed
// in float with the hardware's true nonlinear DAC response instead of the
// runner's linear approximation, and free of the int16 requantization) runs
// ALONGSIDE the canon APU mix, never replacing it. This verifier decides,
// from the running canon stream, whether the shadow is proven enough to
// substitute, and reverts loudly the instant it stops matching:
//
//   - envelope correlation + level-ratio self-check vs the canon stream
//   - probation auto-gain calibration (constant mixer-scale differences)
//   - prove-then-substitute, strike-then-pause, escalating re-prove
//
// The canon (emulated APU) output stays the verify oracle; the shadow is
// never ground truth. If this verifier is wrong, the worst case is "we keep
// playing the authentic APU mix" — it cannot corrupt output.
//
// Ported (C) from JRickey/gba-recomp crates/gba-core/src/shadow.rs via the
// gbarecomp C++ port (src/gba/audio_shadow.*) and the snesrecomp C port
// (runner/src/snes/audio_shadow.*), © Jrickey, MIT OR Apache-2.0, used with
// permission. Engine-agnostic; reused verbatim across the recomp ecosystem.

#ifndef NESRECOMP_AUDIO_SHADOW_H
#define NESRECOMP_AUDIO_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  SHADOW_JUDGE_NONE = 0,  // mid-window, or canon silent (no verdict)
  SHADOW_JUDGE_PASS = 1,  // window passed all gates
  SHADOW_JUDGE_FAIL = 2,  // window failed (see fail_structural)
} ShadowJudgement;

// Window/decimation geometry (see the .c for the rationale).
#define SHADOW_DECIM 64u
#define SHADOW_WINDOW 1024u
#define SHADOW_MAX_LAG 56u

// Differential self-check: shadow mix vs the canon DAC stream. Polices
// STRUCTURE (same notes, same loudness, same time) via rectified-lowpassed
// envelopes, decimated and Pearson-correlated over a coarse lag range.
typedef struct {
  float lp_x[2];                 // canon envelope follower (L,R)
  float lp_y[2];                 // shadow envelope follower (L,R)
  float ex[SHADOW_WINDOW][2];    // decimated canon envelope
  float ey[SHADOW_WINDOW][2];    // decimated shadow envelope
  uint32_t count;                // entries filled in ex/ey this window
  uint32_t phase;                // decimation phase
  uint32_t strikes;
} ShadowSelfCheck;

// The engine-agnostic verification state machine.
typedef struct {
  ShadowSelfCheck check;
  float gain;          // calibrated output gain (1.0 = stock scale)
  bool calibrated;
  float ratio_hist[3];
  uint32_t ratio_n;
  bool proven;         // frontend substitutes only when true
  uint32_t prove_need;
  uint32_t consec_pass;
  uint64_t pauses;
  char reverted[160];  // reason on each pause (caller logs + clears); "" = none
  float last_r;
  float last_ratio;
  bool fail_structural;  // last Fail broke correlation at an in-band level
} ShadowVerifier;

// Zero-init then call this (sets gain=1, prove_need=1).
void shadow_verifier_init(ShadowVerifier* v);

// Feed one grid sample of (canon, shadow-check-copy) stereo pairs, both in the
// canon's normalized domain. Drives calibration, proving, strikes, pauses.
ShadowJudgement shadow_verifier_judge(ShadowVerifier* v,
                                      float canon_l, float canon_r,
                                      float chk_l, float chk_r);

static inline float shadow_verifier_gain(const ShadowVerifier* v) {
  return v->gain;
}
static inline bool shadow_verifier_proven(const ShadowVerifier* v) {
  return v->proven;
}

#ifdef __cplusplus
}
#endif

#endif  // NESRECOMP_AUDIO_SHADOW_H
