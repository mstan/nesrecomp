# Shared NES engine HLE experiment

Central issue: `beads-o777`. This experiment starts at the current cycle-engine
pin plus the reviewed attribution tooling (`53448f9`). It does not change a
guest routine or enable a title-specific shortcut. The existing LLE remains
the default. Available-host qualification is Windows x64; no mobile, ARM,
32-bit Windows, or original-Xbox performance claim follows from it.

## Shared engine opportunities

Evidence is the existing 731-sample SMB3 level-entry/death/return capture in
[SMB3_HLE_ATTRIBUTION.md](SMB3_HLE_ATTRIBUTION.md). Samples are perturbed,
main-thread self-address bins including startup; they rank opportunities and
are not uninstrumented cost or a speedup prediction. The current native and
interpreter runs match all 3,200 per-frame diagnostic records on that route.

| Engine class | Measured evidence | Estimate or unknown | Disposition |
| --- | --- | --- | --- |
| Graphics / PPU | `ppu_dot`, `ppu_half_dot`, `bg_fetch`: 402/731 samples, 54.993%. `shift_sprites`: 18/731, 2.462%; pixel composition may be inlined into `ppu_dot`. | Packed row decoding and one eight-lane sprite batch remove repeated scalar work. Its recoverable share and whole-title gain are unknown; the entire PPU bucket is not recoverable. | Implement this bounded native raster-pipeline replacement first, preserving the bus/status event adapter. Larger scanline/event rendering remains a follow-up if measured useful. |
| Audio / APU | `apu_cycle`: 42/731, 5.746%, for cycle-engine synthesis. | Audio-device pacing and worker/callback costs were excluded. Legacy-engine APU period-cache evidence is not a cycle-engine gain. Buffer synthesis/resampling or expansion-audio replacement needs a representative audio workload and completion/sample contract. | Retain the current APU. Profile audio-on host work after the PPU result; do not infer a whole-title gain from legacy tests. |
| DMA / memory | `hw_read`: 10/731, 1.368%, is one bus symbol, not all memory/DMA work. | OAM/DMC DMA stolen-cycle, memory-routing and dispatch costs are not independently measured. Bulk OAM copy must retain CPU stalls, DMC interaction and peripheral reads. | No DMA/memory candidate selected from this capture. Preserve those effects; attribute before replacing a transfer service. |
| Scheduling / polling | `hw_cycle_start` + `hw_cycle_finish`: 47/731, 6.429%, self samples. | Polling instructions and shared device costs are not inclusively assigned. Event batching across CPU cycles risks reads, NMI/IRQ sample points and DMA readiness. | Keep the clock adapter in this experiment. A later event scheduler can own a broader boundary, but requires measured cost and observer contracts. |
| Mapper / firmware services | Watched PPU address, PPU read and cartridge IRQ symbols: 36/731, 4.925%, an observed subset. SMB3 uses MMC3. | FDS BIOS/media services and other boards' expansion devices are outside the capture. Mapper work can be game-visible even when no pixel changes. | Preserve mapper callbacks and their ordering. Existing FDS behavioral services are a separate candidate family requiring disk/progression evidence, not a speed claim here. |
| Coprocessor / expansion work | No independent coprocessor bucket was measured on this cartridge. | VRC7/Namco/FDS audio and other special boards may have different costs. | Defer to a matching supported board workload; no generic replacement claim. |

## Selected boundary and build choice

Configure `NESRECOMP_PPU_IMPL=LLE` (default) or `HLE` in distinct build trees.
`runner/cyc/ppu_implementation.cmake` resolves the definition for all consumers.
Standalone builds default to LLE; `NESRECOMP_PPU_HLE=1` selects the experiment
and requires SSE2. Unsupported architectures fail the HLE build explicitly.
`cyc_ppu_implementation()` and host startup output identify the fixed choice.
There is no runtime selector, dual execution or live state handoff.

This is a native replacement of the raster output pipeline inside the shared
cycle PPU, not a whole-machine timing shortcut. At a fetched background row,
HLE decodes eight two-bit pixels once into packed private state. It replaces
four scalar background/attribute shift planes and repeated per-pixel plane
extraction. All eight sprite output units are selected and advanced together
with SSE2, retaining countdown-before-shift, transparency and earlier-object
priority. The immutable row decode table is 512 bytes and keys on fetched
bytes, so changed CHR banks/RAM need no cache invalidation.

Caller-visible contract:

- Pixel indices, opacity sidecar, fine-X, clipping, palette, emphasis, sprite
  priority, sprite-zero visibility and the three-dot output pipeline.
- CPU register accesses, delayed writes, VBlank/NMI and sprite-status latches.
- OAM evaluation/overflow and palette/OAM corruption remain in the adapter.
- Every VRAM address/read and MMC3 A12 IRQ observation keeps its existing
  first-half-dot boundary. No fetch, IRQ edge or CPU polling completion is
  claimed early. DMA and APU clocks are unchanged.
- Existing HD-pack and background presentation hooks retain their event count
  and values; separate enhancement qualification remains necessary.

No numerical or timing difference is deliberately introduced. The HLE policy
permits minute practical differences, but this candidate can satisfy its
selected boundary exactly without recreating a second executing model.
Diagnostic hash/dump calls unpack private rows into the established trace
schema only when observed. Ordinary HLE execution does not maintain a duplicate
LLE pipeline. This normalization is a representation adapter, not evidence
that the low-level machine is independently correct.

Same-build snapshots store the selected private representation. The existing
layout signature includes an HLE schema marker and rejects cross-build or
legacy LLE states before loading. LLE's signature remains unchanged. Native
game saves are unaffected; cross-build savestate conversion is not supported.

## Validation and disposition

The focused kernel test covers all 65,536 planar row pairs, all fine-X values,
row replacement/history/fill, and 32,768 sprite lane batches including zero/one
countdowns, transparent candidates, rendering-off and odd-frame skip behavior.

```text
cmake -S tests/ppu_hle -B build/ppu-kernel -DCMAKE_BUILD_TYPE=Release
cmake --build build/ppu-kernel
ctest --test-dir build/ppu-kernel --output-on-failure
```

Both title builds, differential hardware fixtures, same-build snapshot
continuation and the existing 3,200-frame SMB3 route are integration gates.
Performance must use uninstrumented same-compiler builds, identical useful
game progress and paired order, in the parent's serial measurement window.
Report raw results and code size. A neutral/negative or inconclusive experiment
remains a draft with its evidence; HLE is not promoted simply because its
kernel is correct or it replaces fewer scalar operations.

Qualification performed on 2026-10-06 (Windows x86-64, GCC 15.2 Release):

- Both cycle title builds completed; exhaustive kernel cases passed.
- Sixteen paired game-neutral hardware routes passed: NROM CHR-RAM/nametable
  access and mapper 4/118/119 IRQ fixtures at four CPU/PPU alignments. Full
  trace records matched and each IRQ fixture reached its documented A=42.
- The existing SMB3 level-entry/death/map route matched every one of 3,200
  frame records (95,295,224 console cycles), including the prior LLE floor.
  Record SHA-256: `7d5087791668f036f3f7e1b23ef1908eae58caa87814ab35959a5f2ee3b7b777`.
- Both same-build snapshots resumed at frame 2,001 and matched all 1,199
  remaining records. The HLE host rejected an LLE snapshot with exit code 2
  and the existing another-build diagnostic before loading state.

This route demonstrates continued useful play and the same recorded death/map
transition, not a completed game or all-board qualification. Desktop HD-pack,
ARM/mobile and other consoles remain unqualified. The selected HLE is an exact
native replacement of the raster output service; scheduling, bus protocol and
other PPU behavior remain in the maintained cycle adapter.

## Whole-host timing: inconclusive, keep draft

One serial set used the same GCC 15.2 Release binaries and the same 3,200-frame
route, without hashes, trace capture, screenshots, saves, samples or pacing.
`cyc_trace_enabled` remained false. The existing production event-ring
bookkeeping has no host disable switch and remained active identically; ring
output was disabled. One warmup per arm preceded six alternating AB/BA pairs,
then two LLE/LLE controls. Processor affinity was mask `0x4`, assigned immediately
following launch. The elapsed measurement includes process startup/teardown;
startup before affinity assignment is a small uncontrolled interval. Each run
was bounded to 60 seconds and checked for exit 0 and 95,295,224 console cycles.

| Pair | Order | LLE wall seconds | HLE wall seconds | LLE CPU seconds | HLE CPU seconds |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | LLE, HLE | 10.667636 | 9.326575 | 10.062500 | 9.156250 |
| 2 | HLE, LLE | 9.811421 | 11.160737 | 9.515625 | 10.359375 |
| 3 | LLE, HLE | 10.008441 | 10.600545 | 9.703125 | 9.843750 |
| 4 | HLE, LLE | 13.598112 | 11.389057 | 12.328125 | 10.703125 |
| 5 | LLE, HLE | 10.735148 | 10.136077 | 10.437500 | 9.750000 |
| 6 | HLE, LLE | 10.086174 | 10.077879 | 10.000000 | 9.875000 |

Warmups: LLE 9.350766 seconds, HLE 8.640065 seconds. LLE/LLE control wall times:
10.086466 / 9.477078 and 10.532460 / 10.412991 seconds. Mean wall times were
10.817822 / 10.448478 seconds (an apparent 3.41% reduction), but individual
paired changes ranged from a 13.75% regression to a 16.25% reduction, and the
first unchanged-binary control varied by 6.04%. These data do not establish a
material whole-title improvement. No default promotion is justified.

Per-run foreign process CPU deltas were recorded without stopping external
work. Contention included SuperMarioWorldSNESRecomp throughout, a busy
Vigilante82PSXRecomp in later pairs, Tsumu_Light_Recompiled during a warmup and
controls, Discord, Docker and desktop processes. No quiet-host claim follows
from serializing our own team. Sampling at run boundaries cannot detect every
short-lived compiler; it is a contamination indicator rather than a complete
scheduler trace.

Executable sizes: LLE 4,112,063 bytes, HLE 4,112,609 bytes (+546).
PE `.text`: LLE `0x30e790`, HLE `0x30e750` (-64 bytes); `.rdata`:
LLE `0x48320`, HLE `0x48540` (+544 bytes). The immutable decode table is included.

Local raw evidence and reproduction launcher are preserved under
`F:\Projects\nesrecomp\_hle-o777-20261006`: `timing-results.json`,
`run-timing.ps1`, per-run `timing-*.log`, build logs, `contract-evidence`, frame
hashes, snapshots and screenshots. They include local ROM paths, not ROM data.
Retain this experiment as a draft. A repeat on an available quiet host can
resolve its performance disposition; broader scanline/event service work is
still an estimated opportunity, not a measured success of this replacement.

## Representative-workload discovery (2026-10-06)

This is a three-title coverage pool, not an automatic benchmark matrix:
SMB3 (MMC3 IRQ/banked graphics), SMB1 (NROM control), and Otocky (FDS disk/BIOS,
mutable RAM code and expansion audio). Reuse evidence first; take one production
configuration per selected workload only when it can change an HLE theory.
The entire cross-system discovery pass permits at most six new captures total,
not six per system or per configuration. These titles do not exhaust board,
audio, peripheral or game behavior.

The reused SMB3 evidence is
`F:/Projects/nesrecomp/_hle-wtuu-20261006/cycle-route-functions-reviewed.json`
and the corresponding `cycle-route-samples.csv`, documented in
[SMB3_HLE_ATTRIBUTION.md](SMB3_HLE_ATTRIBUTION.md). The October 6 cycle pin
`0c061070` capture has 731 main-thread self samples: the top three PPU symbols
account for 54.993%, APU 5.746%, and clock start/finish 6.429%. Its 3,200-frame
route is validated, but sampling includes startup and perturbs execution;
periodic aliasing, inlining and absent worker/callback coverage prevent treating
these percentages as recoverable savings. The broad PPU service remains the
leading measured theory, while this packed-raster experiment's inconclusive
whole-host result above remains unchanged.

`SuperMarioBrosRecomp/game.toml` establishes NROM and optional presentation
hooks; `OtockyRecomp/game.toml` establishes the cycle-only FDS/BIOS and captured
RAM-code route. No current subsystem cost capture for either is used here.
Their addition to the pool proposes diversity, not measured cost or qualified
HLE behavior. Use stock presentation for discovery and separately record the
actual board, audio and execution settings before interpreting a new capture.

## Execution strategy and completion gate

The next NES work is an active-game cost screen of the maintained cycle LLE,
not another revision of the packed leaf kernel. Reuse the SMB3 route and its
frame 2000 gameplay checkpoint, separate its active-play interval from boot,
and collect the missing stock SMB1 and Otocky screens within the new bounded
implementation-round discovery budget. Record all-thread process CPU, active frame work,
pacing/tail behavior and executed native/interpreted coverage; keep diagnostic
attribution separate from uninstrumented timings. Ready the production floor
first: exact compiler/build/ROM/board identities, enhancements disabled, no
hash/trace/dump work in timing, and the real FDS BIOS available. Current SMB3
has a functioning floor; SMB1 and Otocky still need current production cost
and bounded route evidence before they can be qualified companions.

| Workload | Bounded useful route and readiness milestone | Role |
| --- | --- | --- |
| SMB3 | Reuse `_cycle-migration-20261004/SuperMarioBros3Recomp/cycle-evidence/level-route.txt`: world 1-1 entry, movement/jump, death and return to map, 3,200 frames. Use actual active event/frame intervals and retained checkpoints, not the entire boot as the active workload. | Primary MMC3 PPU candidate; existing exact hashes and snapshots are reusable correctness evidence for the current experiment, not future code. |
| SMB1 | Establish a stock cold-boot/START/world 1-1 movement and jump route through a death/respawn or area transition; record its observed event frames once and stop. Current optional presentation hooks must be disabled for this floor. | NROM companion: ordinary fetches and sprite-zero/status behavior without MMC3 IRQ cost. |
| Otocky | Reuse `routes/routes.toml`: `swap.txt` reaches side-B selection in 3,000 frames; `play.txt` contains name entry, GAME MODE, stage 1 movement/shots and game over. Select the observed stage-play-to-transition interval from that existing 12,000-frame route rather than adding campaigns. | FDS companion: BIOS/disk completion, mutable RAM code and expansion audio. Do not claim new disk-service qualification from a PPU result. |

The first proposed implementation owns a broader native PPU rendering service:
decode and emit tile/sprite spans from fetched data, while an adapter publishes
CPU-visible status, NMI and mapper A12 observations at the required boundaries.
It must cover enough measured active raster work to matter; the inconclusive
packed-raster experiment is evidence against merely repeating that leaf change.
If the active screens move cost elsewhere, revise the boundary before coding.
Unobserved internal work may be removed or batched; there is no requirement for
shadow execution or live switching between two machines.

Before implementation, declare the affected route interval and an explicit
useful gain target. A preliminary planning target is 10% lower whole active
workload CPU/frame work at the same guest progress, with no pacing-tail
regression. It is not a universal acceptance threshold or an asserted gain;
acceptance must exceed the observed host noise and be useful on the tested
platform. Count eligible rendering work and native/interpreted coverage so a
change in execution mix cannot masquerade as a renderer gain.

The LLE/HLE contract compares framebuffer/palette/opacity output, register and
sprite-status reads, NMI/IRQ and MMC3 A12 order, VRAM/OAM effects, DMA/APU
completion and same-build save continuation. Compare both builds on each
bounded route, retaining gameplay checkpoints and short audio evidence where
relevant. Tiny differences permitted by policy require an explicit practical
assessment; trace equality alone is neither required for every internal detail
nor sufficient to demonstrate feel or continued gameplay. A softlock or lost
required event fails the route.

Then run only two balanced primary LLE/HLE timing pairs (ABBA), plus one A/B
for each companion, using one production configuration per title. Correctness
outputs can be reused when they belong to the exact builds; performance intervals
must exclude diagnostic capture; reuse route executions where practical. Record CPU, wall/frame
work and tail behavior, code size, coverage and contention. No automatic extra
repeats or unchanged-binary matrix follows an inconclusive result: retain a
draft with its evidence or choose a separately justified next boundary.

Only a promising, contract-checked and materially faster build reaches the
owner's final playtest. The concrete primary handoff is a playable Windows x64
`SuperMarioBros3Recomp.exe` built against the selected framework branch, with
ROM/config/build identity and the same world 1-1 route; the owner plays normal
movement, jumps, death/re-entry and controls/audio feel. SMB1 and Otocky are
companion evidence, not a requirement to finish three campaigns. After the
owner accepts the final build, enable HLE by default only for the explicitly
qualified platform/board scope, retain build-time LLE opt-out, merge and close
the owning issue. If owner feel, behavior or useful gain fails, leave LLE
normal and the candidate draft. That approval is the final completion gate,
not another profiling project.

## Measurement, decision and delivery protocol

Owner completion rule: establish a material game-workload gain and automated
compatibility, then deliver the final playable build for the owner's feel check.
After that check passes, integrate the prepared default change and close the
scoped work. Exhaustive game coverage and completed campaigns are not additional
completion requirements.

1. **Pin the workload and floor.** Use the three games and concrete routes above.
   Build LLE and HLE from the same title/framework revisions, compiler/options,
   ROM/firmware identities, presentation/audio settings and initial game state;
   only the selected implementation differs. Keep the replaced LLE service
   runnable. An old executable is discovery evidence, not a mismatched control.
   Use native game saves or replayed inputs when private savestates cannot cross
   builds. First resolve the named route/build gaps; do not perfect unrelated
   hardware before replacing a functioning operation.
   Verify that companion routes actually exercise the replacement; an unaffected
   title is a regression control, not evidence for that HLE service. If the
   chosen service changes, replace an unsuitable companion in the three-title
   set instead of accumulating extra games or claiming unexercised coverage.
2. **Attribute only what is missing.** Reuse suitable profiles and collect at
   most one new active-workload attribution capture per selected game in this
   implementation round. Identify the intended service's eligible dynamic work.
   Include worker threads and external modules or report them unresolved; a
   main-thread symbol histogram cannot supply a whole-process cost percentage.
   Capture diagnostics separately from performance. End discovery when there
   is enough evidence to select a useful service, not when every subsystem has
   a profile. The earlier six-launch discovery cap applied to that completed
   pass, not to the whole implementation/qualification program.
3. **Choose one replacement.** Record its caller ABI, inputs, outputs, observable
   side effects, supported operation scope, permitted tiny differences, expected
   cost removed, and candidate-specific useful gain before coding. Implement a
   shared service with build-time LLE/HLE selection and explicit build identity.
   Do not stack several speculative replacements into the same comparison.
4. **Measure equivalent active play.** Delimit a fixed gameplay window by guest
   frames and meaningful game events, excluding boot, warmup and teardown.
   Choose enough active work to dominate measurement granularity once, then keep
   it fixed. Report total process CPU milliseconds per guest frame (all threads),
   critical-path frame work, median/p95 frame time and missed presentation/audio
   deadlines where available. Record peak memory and code size, since constrained
   targets matter. Preserve normal renderer and audio production; a benchmark
   that omits presentation/audio is a core-only diagnostic, not end-to-end proof.
   Normal capped play can show reduced CPU/frame even when FPS stays unchanged.
   Uncapped throughput is optional corroboration only when it performs equivalent
   rendering/audio work. Measure GPU completion/queue cost when work moves there;
   a shorter submission call alone is not a win. Check actual movement/progress
   and audio duration so changed guest timing cannot inflate the result.
5. **Use a fixed comparison budget.** The primary game gets LLE/HLE/HLE/LLE:
   two order-balanced pairs, four measured executions. Each of the two companion
   games gets one LLE/HLE pair, two executions each. That is eight measured runs
   per candidate on one declared host/configuration, not a Cartesian matrix.
   Reuse their progression telemetry and final outputs; take expensive milestone
   captures outside timing, and use isolated LLE/HLE fixtures for detailed
   contracts. Do not automatically add separate full campaigns or trace runs.
   Keep team builds/profiling out of the timed window, record host load/power/
   thermal conditions, and preserve every result. A noisy or contradictory result
   stops that screen; fix an identified condition before a bounded replacement
   measurement. Never repeat until a passing subset appears.
6. **Decide from useful gain and compatibility.** Report both paired percentage
   and absolute savings, with the observed pair spread. About 10% lower whole
   active-workload CPU time is a planning aim, not a universal acceptance rule.
   A candidate may instead solve a declared frame-budget or stutter problem.
   Both primary pairs must show a clear consistent useful improvement beyond
   observed noise; two pairs are not a formal confidence interval. Companion
   single pairs screen for large regressions, not proof of zero performance
   change. Explain any apparent regression before broadening defaults. Exact
   promises require exact outputs; permitted approximations use a declared
   practical image/audio/result comparison. Check input, audio, progression,
   affected completion/IRQ consumers, transitions and relevant pause/reset/save
   behavior. No crash, softlock, stale buffer, lost completion or save corruption
   passes. A huge isolated kernel ratio cannot substitute for this decision.
7. **Hand off the actual finished candidate.** Provide the named primary game as
   a ready-to-launch normal-paced HLE package, an LLE comparison build, isolated
   save/checkpoint setup, launch instructions and checksums/build identity. Include
   a short before/after report, companion results and any tiny known differences.
   Prepare the intended default-selection/integration change in the draft PR so
   the owner tests the package intended to ship. Ask the owner to play normally
   and assess response, motion/collision, camera/scrolling, stereo where relevant,
   audio rhythm and continued progression. There is no prescribed full-campaign
   completion or multi-game human test matrix. Owner rejection reopens the
   affected behavior; fix and recheck that change before another handoff.
8. **Finish the scoped delivery.** After owner acceptance, integrate the reviewed
   candidate, make HLE the default for the supported titles/platform/service,
   retain a documented build-time LLE opt-out, and record the measured and manual
   evidence before closing the issue. Do not add unrelated qualification gates
   after the agreed playtest. If the replacement cannot deliver material gain,
   preserve its branch and draft PR with results, explain why, and choose a new
   boundary deliberately; an unsuccessful experiment is not a completed system.

Initial measurements can use Windows x64 already available here. Choose the
first constrained target with the owner, then carry only the winning candidate
and the relevant route to that target. Measure there before claiming mobile or
original-Xbox savings; desktop results do not establish a port's performance.
A target-specific build/default is qualified separately rather than multiplying
all hosts into the discovery matrix.
