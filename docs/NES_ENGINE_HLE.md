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
