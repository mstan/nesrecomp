# NES07 native OAM transfer candidate

Reject this prototype. All five games preserve exact outputs and current-build
continuation, but this package establishes no reliable gain and fails to qualify
Gumshoe as net-neutral. The selected runtime stays unchanged.

The implementation batches independent APU work during the interior of eligible
RAM-to-OAM transfers while retaining every PPU edge and actual $2004 write.
Initial arbitration, the final byte, DMC contention, held CPU I/O, trace, PAL,
extended timing, expansion boards and pre-render boundaries keep the original
executor. It is portable C; no assembly or host-specific fallback was added.

The existing device-interval probe proves actual untraced admission and held-I/O
fallback. Candidate/reference state, memory and PCM match for boards0/1/4/66 and
all four NTSC alignments under both HLE and LLE. Both registered tests pass.

The existing five-game routes then compare this candidate with the identical
selected combined package. All exact PCM/frame/hardware/RAM/picture comparisons
and600-frame own save/load continuations pass. The original compiler package is
preserved for each game; timing jobs run serially without overlapping builds.

| Game | Initial reference/candidate median CPU seconds | Disposition |
| --- | --- | --- |
| smb | 1.796875 / 1.796875 | Neutral/inconclusive |
| smb3 | 1.484375 / 1.437500 | Neutral/inconclusive |
| metroid | 1.437500 / 1.421875 | Neutral/inconclusive |
| gumshoe | 1.375000 / 1.437500 | Adverse, confirmed below |
| zelda | 1.890625 / 1.921875 | Neutral/inconclusive |

Gumshoe's initial pairs are two slower and one equal. A single targeted
five-pair confirmation retains the exact binaries, original affinity, route,
settings and fixed Zapper input: four slower and one faster. Its CPU times
vary substantially during the run, so neither the initial4.55% nor follow-up
1.60% median difference is an exact established regression size. Repeated
adverse direction and no established benefit are sufficient to reject this
package. Mixed timing in the other titles is not promoted to a performance win.

This resolves one implemented candidate, without claiming every possible DMA
architecture exhausted. No new profiler, harness or benchmark matrix was needed.
The owner waived reservation gating; actual headroom and the existing per-core
telemetry are retained, and no lease is claimed. No merge or release occurred.

Raw products, compile/link identities, settings, exact paired samples and current
continuation evidence remain at `G:/Temp/Codex/nes110-dma-candidate-20261031`;
test/build logs and prototype source are in the existing campaign workspace.
See the [machine-readable decision](2026-10-31-native-dma-candidate-results.json).
