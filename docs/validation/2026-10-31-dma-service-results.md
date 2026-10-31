# NES07 DMA service attribution

Defer a standalone DMA rewrite after the smallest post-HLE gameplay check.
No runtime candidate, adoption or PR. The next larger selection is NES08's
polling/blank CPU and device execution; future DMA reuse remains explicit.

| Stock workload | Bursts | Stolen cycles | OAM starts | DMC starts | Full burst wall ms | Observed interval wall fraction |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| smb3 | 14451 | 663143 | 1188 | 13265 | 43.1460 | 1.081% |
| smb | 1197 | 614671 | 1197 | 0 | 37.9573 | 0.927% |

These are two diagnostic baselines on this PC, not A/B gains. Actual hidden
software-SDL playback uses the existing gameplay states/routes, native HLE,
audio emulated/mixed/queued to dummy output and current rewind100/4. The1199
instrumented labels2402..3600/1802..3000 sit within1200resumedframes plus shutdown.
Source identities/settings/binaries/raw interval CPU+wall/capacity telemetry are
retained. Do not pool different workloads or present an exact recoverable share.

The probe times complete stolen bursts: first successful wants-cycle at tick0
through the final stolen-cycle tail. It includes PPU/APU advancement and excludes
the preceding normal CPU tail. Counter/QPC/preemption overhead is included.
Required device advancement is not removable work; a memcpy is not a DMA service.
No numerical retention floor or demonstrated optimization gain was rejected.

Counts validate the scope: halt/OAMget/OAMput/DMCget sum exactly to stolen cycles;
OAM puts equal256times OAM starts. SMB3 has13,460DMC reads and783cycles with both
requests pending, not783distinct collision episodes. Its coarse blank-RAM/noDMC
starting cohort is1018bursts; SMB1's is1197. These are eligibility clues, not
qualification of a complete transfer across IRQ, PPU, held-bus or clock events.

Frozen selected-object compile recipes are recovered from their actual original
Ninja/relink origin. This avoids mixing newer link-build flags/headers with old
selected core objects. Each diagnostic replaces only hardware-machine and APU
objects; all device bodies remain intact apart from probe/counter additions.
Independent source/recipe/binary/raw-data review passes.

The initial SMB3 process completed normally but the QA parser searched the wrong
report marker and did not retain returned interval CPU/wall metadata. That attempt
is archived/excluded. The corrected runner persists window.json before analysis;
it reuses the same binaries and reproduces the exact SMB3 event counts.

Current native PPU intervals cover stable rendering interior. A coherent native
blank/polling clock service can be considered next; existing loops and private
representation are not immutable constraints. Any future DMA admission still
needs actual fast-executor/collision/bus/interrupt/OAM/Zapper/PCM/current-restore
and fallback evidence. No candidate exists here, so no broad fixture reruns.

Both temporary staging roots are removed after verified archive retention.
Other three titles and feature variants remain unchanged/unmeasured. See the
[machine-readable record](2026-10-31-dma-service-results.json). Final combined
supported five-game builds/features/performance and owner approval remain open.
No main/master merge or release; FDS stays parked.
