# Native pipeline implementation progress

Current checkpoint: ten of 31 opportunities have qualified evaluation
decisions, including complete stable HLE raster jobs under NES01 and the
qualified isolated native room service under NES18. NES20 snapshot replacement
is deferred after measured attribution. See the
[current register](2026-10-27-remaining-opportunities.md) and
[raster results](2026-10-28-raster-job-results.md) and
[isolated service results](2026-10-28-isolated-native-results.md). The initial attribution
and plan below are historical; final combined builds and owner approval
remain open. FDS stays parked.

Owner goal: execute all NES01–NES31 in the
[remaining-opportunity register](2026-10-27-remaining-opportunities.md), evaluate
every item, implement and measure useful coherent packages, and leave affected
builds for owner visual approval before merging or releasing. The complete
scope remains active. FDS stays parked under the earlier owner exclusion.

Tracking: `beads-2dw.1.110`, related to audit `beads-2dw.1.109` and campaign
`beads-2wy8.1`. Isolated branch `perf/pipeline-burndown-20261028`, based on
`fc0bf3fa46a710464960c0a1f2d3b83b4d345600`. No implementation or performance
win is qualified by this checkpoint. The audit's closure did not close the
execution opportunities.

## Frozen inputs and first attribution

Evidence root: `G:/Temp/Codex/nesrecomp/pipeline-burndown-20261028`.
`source-heads.json` freezes current remote title heads: SMB1 `45f9575`, SMB3
`99e5d3b`, Metroid `caccee6` (main), Zelda `5d90eb7` and Gumshoe `867a009`.
The reference engine is a separate detached worktree at the frozen engine
base; candidate edits cannot silently rebuild the baseline. Recomp-ui
`c11a4b6` and existing net dependencies remain read-only. New source worktrees
preserve the owner's modified original checkouts and native saves/settings.

A new compiler from the frozen reference and an optimized current SMB3 title
were built through the supported generation/CMake pipeline with GCC15.2
`-O2 -g1 -DNDEBUG`, HLE and actual SDL/software presentation. The baseline
binary is identified in `checkpoint.json`; build/configuration logs, generated
source inputs and compile commands are retained.

One diagnostic main-thread IP capture covers current-route frames2401–3000,
audio emulated/mixed/queued through dummy output, hidden SDL/software1280x720,
pristine mods and no native-save writes. `initial.png` confirms World1-1.
A separate bounded continuation's `endpoint.png` shows that Mario dies and
returns to the world map by frame3000. Thus this is a gameplay/death interval,
**not a level-only interval**. No stage completion or full-route correctness
claim follows from these pictures. This distinction is preserved rather than
adding a player-survival prerequisite to the optimization campaign.

The corrected712-sample capture assigns370 samples to `hw_ppu.c` (51.97%),
63 to `hw_apu.c` (8.85%),45 to `hw_machine.c` (6.32%) and35 to `hw_mapper.c`
(4.92%). Another112 (15.73%) are outside the executable, including runtime,
device and wait work. These are perturbed diagnostic source-ownership bins,
not process-CPU attribution, inclusive call-stack cost, recoverable fractions,
or before/after performance results. Inlined work follows its DWARF owner.
They select the PPU/machine interval pipeline over CPU-coverage-only tuning;
they do not forecast a percentage saving.

Diagnostic corrections are explicit:

- `--input` selects the headless path; actual SDL input requires
  `--window-input`. Failed setup launches yielded no admitted samples.
- The initial older route began at frame1801 on the map and was excluded.
- The reused sampler's `-(20000 + rng % 20000)` negated an unsigned32-bit
  expression, creating an expired absolute Windows timer rather than a
  negative relative delay. The task-owned copy now casts to `LONGLONG` before
  negation and checks timer waits. Earlier rapid/perturbed captures are
  retained under `excluded-timer` and are not current cost evidence.
- Final sampling used a verified new process, actual frame/load state and
  cooperative host file locks; no foreign jobs/leases were stopped or removed.

Previous campaign speedup measurements used uninstrumented games, not this
sampler; this diagnostic correction does not revise their CPU-time results.

## First coherent service: native stable intervals

The next implementation owns a proven stable interval of ordinary CPU memory
work and PPU/APU advancement together. Existing per-cycle clock calls are not
the external contract. A faithful cycle executor remains the universal path
for unsupported or observable edges. This is the first package within the full
burndown, not a declaration that whole-row rendering or other families are
finished.

Implementation outline and obligations:

1. Keep the frozen reference unchanged. Begin with scalar portable native
   intervals on NROM/MMC1 ordinary mirrored RAM and folded immutable PRG reads.
   Board/region/mod/observer capabilities determine eligibility. Unsupported
   mappers, PAL, timing enhancements, active tracing/hooks/read substitutions,
   writable code, DMA and uncertain interrupt conditions retain the old path.
   Prefer compile-time board specialization for unsupported generated titles
   so they do not pay a failed eligibility call on every CPU access.
2. A derived interval cursor owns deferred CPU-cycle count and final bus state.
   Native CPU helpers may use ordinary memory directly only while the cursor
   proves no external observer can distinguish the interleaving. RAM code
   write watches and data/cheat overlays remain binding; writes to watched
   code or mapper/device registers flush before the real access.
3. First prove conservative visible stable spans: NTSC tick1, settled raster
   classification, no scanline/frame/host stop crossing, NMI input settled and
   no pending NMI/reset/IRQ. APU capability/deadline queries must cover OAM and
   DMC requests, delayed enables/aborts, frame IRQ and port state. A temporary
   unsupported condition may reduce coverage; it must never change behavior.
4. At a flush, independently advance eligible PPU and APU spans only after
   proving they cannot exchange data or raise a CPU event within that span.
   Initial NROM/MMC1 services have no expansion audio/CPU-clocked IRQ source;
   MMC1 PPU-A12 RAM gating must still advance and must not permit a speculative
   cartridge-RAM access. Keep mapper address events and final bus/private
   machine state correct. Extend coverage through supported deadlines, not
   unchecked elapsed-time jumps.
5. The existing tick1 sequence gives three PPU dots and three half-dots per
   deferred CPU cycle. Alignments0/1 have half-dot then dot order; alignments2/3
   have dot then half-dot order. A native PPU interval owns that whole run and
   can later own wider raster jobs. APU spans preserve timer, mixer, sample and
   filter ordering. Merely multiplying floating-point accumulation would not
   prove PCM equality.
6. Generic cycle/I/O entry, native-service callbacks, snapshots/restores,
   observation boundaries, isolation and power-on must close/discard the cursor
   appropriately. The cycle-count API must report correct virtual elapsed
   time. Never serialize a stale derived eligibility decision. Current-build
   continuation remains correct; older states need no migration.
7. Test the native interval primitive directly against the coupled reference
   for all four NTSC alignments and relevant eligibility transitions, then
   exercise actual optimized current-build gameplay. Traced runs that disable
   the candidate are not evidence that its active fast path is correct.
   Untraced endpoint hardware/memory, PCM/pictures and current-build replay
   checks need counters proving the native service actually ran. Retain the
   existing hardware fixtures for the fallback and affected bus edges.
8. Measure ordinary built games without diagnostic sampling/observers. Label
   which titles use native intervals and which retain the common fallback;
   neutral games are acceptable. Reuse the campaign's balanced controls and
   real feature profiles proportionately. After two failed improvement
   experiments, reassess the architecture before more tuning.

At the initial design checkpoint no implementation was present. The opportunity JSON
records selected design work for NES01/02/03/06, the FDS parked status and every
other item still awaiting its implementation/evaluation disposition. No release,
title pin update, merge to main/master or owner visual handoff has occurred.

## Prototype now implemented (not yet performance-qualified)

The first runtime prototype owns a stable visible NTSC interval on NROM/MMC1:
ordinary mirrored CPU RAM and folded immutable PRG accesses consume a derived
cursor, then PPU and APU services advance independently in their original
internal order. CPU I/O and mapper accesses, watched code writes, native dispatch
returns, operand-policy callbacks and state capture close the interval. Restore
and power-on discard derived credit. Every DMA, fetch, pixel, sprite, controller
and PCM obligation remains; unsupported deadlines use the coupled executor.
The fast bus also resets the per-cycle DMA-stall result, which undocumented
store instructions inspect immediately.

Paired machine fixtures pass all four NTSC alignments for NROM/MMC1 in both LLE
and HLE builds, with active-interval assertions, hardware/bus/memory/raster and
PCM comparisons, mixed delayed-register/DMA/IRQ traffic, explicit eligibility
refusals, operand observers and current-build snapshot replay. Existing return
observer and RAM-policy tests pass. These are correctness results, not runtime
speedup evidence. The source remains portable C11, without a new host ISA path.

Actual SMB1 integration exposed two overly broad initial guards. Instruction
hooks are armed during ordinary play, but run after dispatch closes the cursor;
they need not disable it. SMB1 also permanently registers a Smash64 RAM operand
policy, including when that enhancement is inactive. The policy must observe
completed physical hardware time, so its call now flushes the cursor rather
than excluding the entire game. Captures made with either blanket guard had
zero native-interval cycles and are excluded from fast-path validation. Inline
RTS observers still retain the conservative fallback until an actual callback
boundary is implemented and qualified.

New SMB1 reference/candidate builds use frozen title `45f9575`, reference engine
`fc0bf3f`, HLE, GCC15.2 `-O2 -g1 -DNDEBUG` and the supported generation pipeline.
Raw input/settings, commands, logs and binary SHA-256 identities are retained.
The local campaign reservation is held by another process and is preserved.
The authorized NAS SERVER (i5-4670) had an available cooperative reservation and
idle disks; its bounded task directory is
`G:/Temp/Codex/nes110-interval-20261028`. Matched muted untraced gameplay/output
checks and ordinary SDL process-CPU samples are in progress there. NAS samples
must remain labeled as that host, never compared directly with earlier local
campaign percentages. The copied private ROM is temporary and must be removed
with the disposable remote inputs after retaining evidence.

This prototype directly advances NES02/NES03. NES01 still needs a replacement
for the per-pixel raster algorithm, and NES06 still needs substantive APU event
or channel spans; independent service ownership alone does not close either.
Every other register entry remains open for evaluation, with NES29 parked.
No owner handoff, main/master merge, title pin update or release has occurred.

## First active game scout

SMB1 now exercises29,376,956 native-interval cycles in1,438,160 runs over the
finite1801-frame route. Untraced frame360..1000 memory/raster records, endpoint
frame1800 hardware/memory/picture and full PCM are byte-identical to the frozen
reference. The endpoint is a World1-1 death/retry screen; no level-only or
stage-completion claim is made.

Five order-balanced ordinary SDL gameplay/death pairs on SERVER use less CPU
in every candidate leg. Medians are8.578125s reference and7.5625s candidate,
an observed11.84% reduction. This is a **busy-host intermediate scout**, not
final package qualification: preflight per-core load was41..51%, and the
candidate preceded the per-fast-cycle DMA-stall-result correction. The result
selects continued implementation rather than an immediate merge or release.
Raw leg times, binary identities, settings, output hashes and caveats are in
[the machine-readable scout](2026-10-28-interval-scout.json); the complete
4MB evidence archive is retained in the task root.

Current-build state loading emits warnings for inactive Sonic/Samus/Link mod
records and a Smash64 reseed in **both** reference and candidate. These shared
baseline warnings do not disqualify the optimization. They remain recorded
separately from new-path output equality and are not silently called passes.
The remaining gates include the corrected final binary, other affected titles
and real feature profiles, per-game non-regression, and final owner builds.

## Unsupported-board cost

Generated title projects now classify the immutable full NES2 mapper number
once during preparation. NROM/MMC1 retain runtime deadline qualification;
unsupported boards compile the per-access cursor checks, generic-cycle flush
and dispatch setup out. Standalone multi-board hosts keep runtime qualification.
This avoids charging SMB3/MMC3 for an optimization package it cannot yet use.
Header fixtures cover NROM/MMC1/MMC3 and extended NES2 mapper numbers whose low
byte resembles a supported board. Actual built-game neutrality remains a gate.
## Registered return observers

The registered RTS observer now closes native time in the mod adapter before
calling the game. Runtime qualification accepts that known wrapper and keeps
fallback for arbitrary external RTS pointers. This removes another whole-game
exclusion while preserving real callback timing; generated CPU sources need no
observer-call rewrite. Active cursor fixtures include operand and return
observers, and the existing real-RTS/isolated-permission contract test passes.

## Corrected stock suite measured on both hosts

The owner explicitly directed measuring despite concurrent work, including on
the workstation. That supersedes earlier quiet-slot scheduling. Foreign jobs,
reservations and shared services were preserved. All five matched game pairs
are built from frozen reference `fc0bf3f` and runtime candidate `520a958`.

The actual SDL/audio workload shows CPU reductions on SERVER i5-4670 of
11.95% for SMB1, 14.22% for Metroid and 9.70% for Zelda. Each has five faster
order-balanced pairs and exactly matching untraced checked output. Zelda also
improves all five Ryzen 9800X3D workstation pairs, with a 12.90% median CPU
reduction. Metroid's local observed median improves16.62%, but four pairs are
faster and one is2.9% slower under changing load; that caveat stays attached.
Earlier mixed SMB1 workstation batches are also retained, not cherry-picked.

SMB3/MMC3 and Gumshoe/GxROM compile this interval service out. Their mixed
timings remain neutral/inconclusive fallback controls; neither raw median
swing is claimed as an optimization gain. Neutral titles do not disqualify
the positive ones. All five games match their baseline raster, memory,
hardware and PCM; the four games also checked on the NAS match across hosts.
Identical baseline mod-state warnings remain separate shared defects.

Raw pairs, source/binary identities, participation, host load and evidence
archives are in [the measured results](2026-10-28-interval-results.json), with
a readable [results table](2026-10-28-interval-results.md). Stock qualification
is complete for this first package. Real enhancement profiles and the rest of
NES01-NES31 still need their dispositions. No owner handoff, merge or release
has occurred. The next work should address a demonstrated complete remaining
path, without reinstating quiet-host waits as an admission requirement.

## Extending the service to the remaining stock boards

The next prototype admits GxROM (CPU bank writes already close the cursor)
and MMC3 stable background intervals. MMC3 requires both an unsampled mapper
IRQ refusal and a proof that no qualified A12 rise can occur: background table
zero remains low, while table one is admitted only after A12 is already high.
Its subsequent four-dot low gaps stay below the three-CPU-cycle IRQ filter.
The first rise after a long low remains coupled. Sprite-fetch/scanline edges,
partial accesses, PAL, DMA and other device deadlines still use the general
executor. No mapper address notification is removed.

A dedicated MMC3 PPU operator publishes the original CPU cycle at each dot
and half-dot event for all four alignments, preserving A12-low timestamps.
The machine still owns the final counter increment. The simpler NROM/MMC1/
GxROM dot loop does not pay a mapper-time branch for every dot.

Paired primitive and CPU-cursor fixtures pass in LLE and HLE for all four
boards and alignments, including MMC3 bank/latch/reload/enable/ack traffic,
GxROM bank writes, unsampled IRQ and first-rise refusals, committed observers,
device/bus/memory/picture/PCM and current-build replay. Full NES2 header checks
qualify only the actual boards, not extended mapper numbers sharing a low byte.
The first-package binaries/results remain immutable; do not relabel their
zero-interval fallback measurements as participation in this extension.

All five rebuilt candidates from frozen `021c540` now exercise active intervals
and pass untraced frame memory/raster, endpoint hardware/memory/picture and PCM
comparison on both hosts. Checked artifacts also match across hosts. SERVER
CPU median reductions versus `fc0bf3f`: SMB1 12.24%, SMB3 11.04%, Metroid14.45%,
Zelda10.14% and Gumshoe15.61%, with five faster alternating pairs for every
game. On the workstation Metroid14.69%, Zelda14.56% and Gumshoe28.27% have
five faster pairs. Local SMB1 and SMB3 have mixed pairs and remain inconclusive;
their raw medians and contention are retained, not hidden or pooled.

The [current package results](2026-10-28-board-deadline-results.md) and
[raw numerical records](2026-10-28-board-deadline-results.json) identify the
same package for all five games, immutable source/binary identities, all ten
batches and compressed evidence locations. These are CPU-time reductions for
the bounded stock workloads, not guaranteed FPS gains or all-feature claims.
NES02/NES03/NES05 now record stock qualification; enhancement profiles and the
other opportunities remain open. No owner visual handoff, merge or release
has occurred.

## Selected sprite-output interval operator (NES01)

Runtime `935033d` prepares current-line sprite pixels and final unit counters
once per stable interval, replacing per-pixel eight-unit selection/shifting.
Other fetch, OAM, status, palette, mapper and Zapper events remain scheduled.
Short spans and active HD recording keep the ordinary raster operator.

Measured incrementally against `021c540`, NAS CPU reductions are SMB1 11.69%,
SMB3 6.98%, Metroid6.29% and Zelda11.88%, each five faster alternating pairs.
Gumshoe is mixed/inconclusive on both hosts, including a negative workstation
median amid very large contention. It is not a proven win or reproducible
regression; its full negative samples remain. All five games match untraced
machine/memory/raster/PCM output and cross-host artifacts. Controlled sprite
overlap/priority/transparency/phase cases pass on all four boards/alignments
in both HLE and LLE, with current-build replay.

See [sprite measurements](2026-10-28-sprite-span-results.md) and
[raw records](2026-10-28-sprite-span-results.json), with recorded object/link
identities. These are incremental results, not percentages to add to v2. The
same native CPU-generated units are retained in these isolated runtime relinks;
final production regeneration remains before the human handoff. NES01 still
has background/fetch/OAM work, and feature profiles remain open. Next selected
experiment is NES06 constant-output APU event spans. No main/master merge,
owner handoff or release has occurred.
