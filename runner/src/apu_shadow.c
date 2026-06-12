/* apu_shadow.c — see apu_shadow.h.
 *
 * The verifier is ported from JRickey/gba-recomp via the gbarecomp/snesrecomp
 * ports (audio_shadow.{c,h}), © Jrickey, MIT OR Apache-2.0, used with
 * permission. The NES nonlinear-mixer formula below is the public NESDev-wiki
 * APU mixer. The render/substitute harness is ours.
 */
#include "apu_shadow.h"

#include "audio_shadow.h"

#include <stdio.h>
#include <stdlib.h>

/* Match the canon path's int16 scale so level ratios are ~1.0 to begin with.
 * Canon (apu.c mix_sample): out = linear_mix * 2.0 * 32767, clamped. We render
 * the nonlinear mix and scale it to the SAME nominal full-scale; any residual
 * constant scale difference (linear vs nonlinear loudness) is absorbed by the
 * verifier's probation auto-gain, not guessed here. */
#define SHADOW_AMP   (2.0f * 32767.0f)

static bool           s_enabled = false;
static ShadowVerifier s_vf;
static uint64_t       s_degraded_logs = 0;

void apu_shadow_init(void) {
  const char* env = getenv("NESRECOMP_AUDIO_SHADOW");
  s_enabled = (env && env[0] && env[0] != '0');
  shadow_verifier_init(&s_vf);
  s_degraded_logs = 0;
  if (s_enabled) {
    fprintf(stderr, "[apu_shadow] enabled (float nonlinear re-render, "
                    "verified vs canon; reverts loudly on divergence)\n");
  }
}

bool apu_shadow_enabled(void) { return s_enabled; }

/* NESDev "accurate" nonlinear APU mixer. Returns a normalized level ~[0,1].
 *   pulse_out    = 95.88 / (8128/(p1+p2) + 100)
 *   tnd_out      = 159.79 / (1/(tri/8227 + noise/12241 + dmc/22638) + 100)
 *   output       = pulse_out + tnd_out
 * (the canon apu.c uses the *linear* approximation of exactly this curve.) */
static float nonlinear_mix(const ApuChannelLevels* lv) {
  float pulse_out = 0.0f;
  unsigned psum = (unsigned)lv->pulse1 + (unsigned)lv->pulse2;
  if (psum != 0u) {
    pulse_out = 95.88f / (8128.0f / (float)psum + 100.0f);
  }
  float tnd_out = 0.0f;
  float tnd_acc = (float)lv->triangle / 8227.0f
                + (float)lv->noise    / 12241.0f
                + (float)lv->dmc      / 22638.0f;
  if (tnd_acc != 0.0f) {
    tnd_out = 159.79f / (1.0f / tnd_acc + 100.0f);
  }
  return pulse_out + tnd_out;  /* ~0..1 */
}

int16_t apu_shadow_sample(int16_t canon, const ApuChannelLevels* lv) {
  if (!s_enabled) return canon;

  /* Canon stream in the verifier's normalized domain. */
  float canon_n = (float)canon / 32768.0f;

  /* Shadow nonlinear re-render, scaled to the same nominal full-scale. */
  float shadow_lvl = nonlinear_mix(lv) * SHADOW_AMP;
  float shadow_n   = shadow_lvl / 32768.0f;

  /* Differential self-check (mono: feed the same value to both stereo sides).
   * The check-copy is the shadow in the CANON domain (no gain) so the verifier
   * measures structure + the raw level ratio; gain is its own calibration. */
  ShadowJudgement j =
      shadow_verifier_judge(&s_vf, canon_n, canon_n, shadow_n, shadow_n);

  if (j == SHADOW_JUDGE_FAIL && s_vf.reverted[0]) {
    /* Loud, rate-limited DEGRADED revert log; then fall back to canon. */
    if (s_degraded_logs < 64) {
      fprintf(stderr, "[apu_shadow] DEGRADED revert: %s — falling back to "
                      "canon APU mix\n", s_vf.reverted);
      ++s_degraded_logs;
    }
    s_vf.reverted[0] = '\0';
  }

  if (!shadow_verifier_proven(&s_vf)) {
    return canon;  /* probation / not proven => authentic canon output */
  }

  /* Proven: substitute the gain-calibrated float re-render. */
  float out = shadow_lvl * shadow_verifier_gain(&s_vf);
  if (out >  32767.0f) out =  32767.0f;
  if (out < -32768.0f) out = -32768.0f;
  return (int16_t)out;
}
