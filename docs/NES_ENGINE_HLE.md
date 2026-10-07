# Shared NES engine PPU service and Windows qualification

Central issue: `beads-o777`. This experiment starts at the current cycle-engine
pin plus the reviewed attribution tooling (`53448f9`). It does not change a
guest routine or enable a title-specific shortcut. The existing LLE remains
the default. Available-host qualification is Windows x64; no mobile, ARM,
32-bit Windows, or original-Xbox performance claim follows from it.

## Current qualification

The broader steady-render PPU event service passed 20 focused hardware and
register-boundary cases. The corrected SMB3 route reached World 1-1 and gained
8.50% full-runtime FPS with 7.95% less process CPU. The owner accepted normal
paced SMB3 with adaptive widescreen: "Looks, sounds, and plays right."
SMB3's Windows default is integrated in its title PR 1, using framework PR 63.
SMB1 improved in both run orders, but the owner reported stale widescreen
margins at one-way scrolling transitions. Its title-level camera fix is tracked
in `beads-2dw.2.14`; a targeted recheck and owner acceptance remain outstanding.
The owner explicitly excluded FDS games from further validation. Otocky's
earlier measurement is retained as exploratory evidence only; it is not a
pending playtest or a default-promotion candidate. Foreign compiler activity
limits the precision of these single-pair percentages.

The shared framework retains `NESRECOMP_PPU_IMPL=LLE`. Qualified Windows x64
titles can select `HLE` by default while preserving an explicit
`-DNESRECOMP_PPU_IMPL=LLE` build opt-out. Integration preserves the current
master Zapper/crosshair mapping fix alongside the measured renderer code.

## Shared engine opportunities

Evidence is the existing 731-sample SMB3 level-entry/death/return capture in
[SMB3_HLE_ATTRIBUTION.md](https://github.com/mstan/nesrecomp/blob/investigate/smb3-hle-attribution-wtuu/docs/SMB3_HLE_ATTRIBUTION.md). Samples are perturbed,
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

Windows is the first delivery scope. Keep the three-game pool: SMB3 (MMC3),
SMB1 (NROM), and Otocky (FDS). Preserve a functioning LLE implementation and
caller ABI, with fixed build-time LLE/HLE selection. Small practical visual or
timing differences are allowed; internal-state and pixel identity are not
universal acceptance requirements. Historical exact comparisons above remain
evidence of the completed experiment, not a mandatory future test matrix.

### First work and replacement boundary

Reuse the SMB3 active-play evidence and establish the missing current stock
SMB1/FDS production floors. Profile only an unanswered cost question needed to
select the replacement; do not fill a quota. Record the actual title/framework,
compiler, ROM/firmware, board and presentation/audio settings. Old executable
presence is discovery evidence, not a ready new candidate.

The proposed first implementation owns a broader native PPU rendering service:
tile/sprite span decoding and output, with an adapter preserving CPU-visible
status, NMI and mapper A12 observations. The packed leaf experiment above did
not show a robust gain; do not simply repeat that kernel as a system success.
Choose a boundary that removes useful measured work, and revise the theory
before coding if active-game costs point elsewhere. No shadow execution or
live switching is required.

| Game | Ready route / remaining gap | Delivery role |
| --- | --- | --- |
| SMB3 | Reuse `_cycle-migration-20261004/SuperMarioBros3Recomp/cycle-evidence/level-route.txt`: world 1-1 entry, movement/jump and death/map return, 3,200 frames. Existing cycle LLE and gameplay evidence are available. | Primary rendering implementation and playable `SuperMarioBros3Recomp.exe`. |
| SMB1 | Establish a stock world 1-1 start/movement/jump route on a current production pin; disable optional presentation enhancements. Current cost and route readiness are gaps. | NROM companion and normal owner play. |
| Otocky | Reuse `routes/routes.toml` with `swap.txt` and `play.txt` for side B, name entry and stage play. Resolve the current production floor and real BIOS before comparing. | FDS/expansion-audio companion and normal owner play. |

### Focused objective checks and gain

Use the existing boundary tests for packed rows/sprites, register/status,
NMI/IRQ/A12 ordering and snapshot behavior where relevant to the new change.
Add a test only for a concrete missing correctness case. Preserve required
memory effects, DMA/APU consumers and caller completion; a crash, softlock,
lost event or corrupted save is a defect. Do not mandate screenshot sweeps,
all-state/audio equality, automated transition campaigns or full game completion.

Start with one same-route LLE/HLE performance pair per selected game on matched
Windows builds/configuration. Uncapped measurement is allowed; preserve useful
guest work and report FPS plus percentage gain, route length and host contention.
Keep expensive diagnostics out of timing. A reduced-work headless test is a
component result, not a normal-game gain. Declare what material improvement the
candidate is intended to deliver; 10% is only a planning aim, not a universal
threshold. Repeat in reverse order only if noise or contradictory results
prevent a decision. Do not automatically expand games, settings or run counts.

Look once at a representative current image from the new implementation:
does it look right, without garbled graphics? No old/new pixel matching is
required. Investigate more deeply only if that glance finds a defect or the
owner reports one. Keep unsuccessful or inconclusive work as a draft with
its measurements; do not present a kernel ratio as a shipped game gain.

### Human validation and Windows completion

After focused checks and a material measured gain, prepare ready-to-launch
normal-paced HLE builds for all three selected games, their fixed LLE build
alternatives, launch instructions and build identities. Launch each ready game
for the owner, one at a time or as a per-system batch, and ask whether it looks
and plays right. Existing authorization covers launching; do not ask for launch
permission again. Do not launch old/unqualified binaries as if the new candidate
were ready. The owner assesses ordinary controls, motion, graphics, sound and
continued play; human validation carries the final practical decision.

Positive owner feedback plus measured gain completes the Windows scope:
merge the reviewed candidate, default HLE only for the supported service/title
scope, retain documented build-time LLE opt-out, and close with the evidence.
Owner-reported defects reopen that specific behavior, not an automatic campaign
or comparison matrix. Mobile/Xbox claims need their own later measurements;
they are not additional gates after this Windows handoff.

## Implementation and measurement history: steady-render event service

The entries below retain the experiment's progression; the current decision
and owner feedback are summarized above.

The new unmeasured HLE owns stable visible scanline spans (dots 9-253),
removing repeated delayed-register, scroll-edge, blank/data-latch and VBlank
control checks. It retains real fetches and mapper /RD/A12 observations, OAM
processing, pixel publication and sprite-zero status stages. Register access
invalidates the span classification; pending writes/data-port work and scanline
edges use the maintained event adapter. This replaces broader render/event
work rather than only the packed arithmetic leaf. LLE remains the default.

Focused machine fixtures pass 20 cases: the existing 16 and four alignments
of a missing mid-render register-interruption case (mask disable/re-enable,
scroll/address writes, status read and VRAM data-port transaction). These are
boundary tests, not new game screenshot sweeps. Production game gain and owner
acceptance have not yet been measured for this change.

Matched SDL builds are being prepared in isolated SMB3/SMB1/Otocky title
checkouts. `--window-input FILE` reuses the existing NES-pad/disk replay through
normal rendering/audio; `--uncapped --exit-after N` removes pacing while keeping
that work and reporting frames, FPS, console/native cycles, audio-device state
and produced/queued samples. Queued playback remains latency-bounded, so an
uncapped run is not real-time listening evidence. Normal-paced owner builds
use neither flag. Headless-only timing will not be presented as a normal-game
win. No gameplay, performance or visual run occurred during build preparation.

### Initial steady-render production screen (2026-10-06)

Six production Release runs used the ordinary SDL host with rendering/upload/
present and audio generation/queueing enabled. `--uncapped` removed pacing;
`--window-input` replayed the same logical controller/disk route within each
pair. One LLE run then one HLE run per title, affinity mask 15, no automatic
repeats. Audio device and audio enabled both reported 1 in every run.

| Title | LLE FPS | HLE FPS | Frame-work reduction | Process CPU reduction |
|---|---:|---:|---:|---:|
| SMB3 | 178.315 | 214.546 | 16.89% | 15.34% |
| SMB1 | 162.668 | 173.310 | 6.14% | 5.54% |
| Otocky | 186.251 | 241.136 | 22.76% | 17.01% |

Every pair exited 0 with equal completed frames, guest cycles, native cycles,
and produced audio samples: SMB3 3,200 frames / 95,295,224 cycles / 2,555,727
samples; SMB1 1,200 / 35,734,225 / 880,491; Otocky 12,000 / 357,363,740 /
9,584,155. Otocky's disk eject/select/insert events were replayed. Queued audio
samples differ because the ordinary uncapped host caps queued latency; this
screen measures real audio production/drain work, not real-time listening.

Foreign compiler activity was present. Before/after compiler-process counts
were SMB3 LLE 10/10, HLE 9/9; SMB1 10/12 and 12/12; Otocky 12/9 and 4/4.
These are initial favorable observations, not clean-host causal estimates;
Otocky's wall improvement especially includes different contention. No
statistical confidence or universal performance threshold follows from one
pair. Process CPU includes all game threads, while FPS is elapsed host loop
work including presentation; startup/teardown are excluded from that loop.

The single basic HLE image inspection at SMB3 frame 2,451 was coherent and
not garbled. It showed the World 1 map, exposing a useful-work gap in the
inherited route: this SMB3 screen does not establish active-level performance.
A corrected bounded active-level route is needed before claiming that scope.
No owner feel acceptance has occurred for these new NES builds. LLE remains
the default, and promotion remains subject to the completion gate above.

Raw evidence is `F:/Projects/nesrecomp/_hle-o777-20261006/span-window-pairs/`
(`runs.json`, six per-arm logs, `smb3-hle-current.png`, glance log). The exact
six executable identities are in sibling `window-binary-manifest.json`;
launch commands are in `window-launch-commands.txt`. The isolated Otocky
checkout supplies a strong empty host-extras function to accommodate MinGW's
PE weak-symbol fallback; the engine ABI is unchanged.
One authorized reversed SMB1 pair addressed its smaller initial gain: HLE
250.195 FPS / 4.796256 s, then LLE 209.316 FPS / 5.732959 s (16.34% frame-work
reduction; process CPU 4.9375 versus 5.765625 s, 14.36% reduction). All useful
counters remained equal. Foreign compiler counts were 6/6 and 6/5. Absolute
speed changed substantially since the first pair, so the repeat supports the
gain's direction rather than a precise stable percentage. Its raw logs and
`runs.json` are in sibling `span-window-smb1-reversed/`.

The historical reused `level.png` was itself a world-map picture. Extending
the inherited one-frame A entry tap at frame 1,500 to twelve frames did not
enter a stage in the single authorized progress check at frame 2,451. Its
`smb3-active-stage-check.png` and log are in `span-window-pairs/`; no corrected
SMB3 timing pair was run against that failed route. Further map-position/input
entry diagnosis or an owner-driven route is needed; this is not an HLE-only
rendering defect established by the available evidence.
Read-only route diagnosis found no replay overwrite: the ordinary SDL host
passes the scripted NES bitmask through session input to the core pad state,
and SMB3's extras supplies no input override. The historical save held Mario
at map x=$40/y=$40, tile=$4A, map operation=$0D: the junction below panel 1.
A deliberate late UP (2,450..2,480) then A (2,600..2,612) corrected the location
and timing. One authorized preparation check at frame 3,001 reached active
World 1-1 with coherent Mario, enemies and HUD; its image/log are
`span-window-pairs/smb3-ready-map-stage-check.*`. This is route-readiness
evidence, not a performance measurement. The corrected 4,200-frame pair awaits
the next serial timing window; engine source and all six binaries are unchanged.
The authorized corrected SMB3 pair then completed 4,200 frames in ordinary
SDL presentation/audio: LLE 260.398 FPS / 16.129169 s, HLE 282.532 FPS /
14.865590 s (8.50% FPS gain, 7.83% frame-work reduction). Process CPU fell
16.125 to 14.84375 s (7.95%). Both arms produced 125,075,723 guest cycles,
120,641,750 native cycles and 3,354,411 audio samples, with audio device/enabled
1 and exit 0. Compiler counts were 10/10 before/after in both arms; observed
foreign contention still limits precision. This whole route includes boot,
map entry and confirmed active World 1-1, not an isolated stage-only timing.
Raw logs and `runs.json` are in sibling `span-window-smb3-active/`; the exact
route is `smb3-active-route.txt`. No further repeats were run.

Together with focused boundary checks, coherent active-stage rendering and
gain direction across the three selected SDL workloads, this supports the
normal-paced owner handoff. It does not record subjective owner acceptance.
Use the exact HLE binaries from `window-binary-manifest.json`; normal play uses
no replay, frame limit, uncapping or hidden window. SMB3 accepts
`--widescreen adaptive`; the measured SMB1 binary requires `--widescreen fit`
for adaptive framing and rejects the literal `adaptive` argument. Exact
arguments and controls are in sibling `owner-play-handoff.json`. SMB3 is now
accepted and promoted for Windows x64. SMB1 stays opt-in pending its reported
margin fix and recheck. FDS games are excluded at the owner's request.
