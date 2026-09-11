# Performance work

This document is the burn-down and evidence contract for portable runner
performance work. The immediate target is the original Xbox port, but retained
changes must also improve normal desktop production builds without weakening
the emulated machine.

## Measurement contract

- Build `Release` from a fresh CMake tree, or explicitly configure existing
  trees with `NESRECOMP_ENABLE_TRACE=OFF`,
  `NESRECOMP_ENABLE_STACK_TRACKING=OFF`, and
  `NESRECOMP_ENABLE_POSTMORTEM_RINGS=OFF`. These options remain independently
  overrideable for diagnostic builds.
- Use the fully rendered headless path:

  ```text
  GameRecomp.exe "<rom>" --benchmark 1200 --benchmark-warmup 300 \
    --benchmark-output benchmark.json
  ```

- Measure on a quiet host. Run untouched and candidate binaries in alternating
  order, use identical compiler/linker settings, and report every sample plus
  the median. Longer runs are preferred when scheduler noise is visible.
- Compare uncapped throughput (`fps` or `ms_per_frame`), not performance while
  paced to the console's refresh rate.
- Add `--benchmark-breakdown` to an uncapped run when attribution is needed.
  It reports guest-between-callback, NMI, PPU, sprite-prediction, post-render,
  and residual callback time. Normal benchmark runs execute no phase-counter
  calls.
- Reject a local optimization that does not produce a repeatable material
  result. The default retention gate is at least a 3% median improvement with
  no adverse lower-envelope signal. A smaller result needs an independently
  useful code-size or memory reduction and must not add maintenance complexity.
- Profilers, counters, and diagnostic builds select candidates; they are not
  timing evidence.

## Gameplay stutter diagnostics

Fallback telemetry file logging is disabled by default. Set
`NESRECOMP_FALLBACK_LOG` to an output path (for example,
`fallback_telemetry.jsonl`) to record interpreter samples and dispatch
discoveries. An unset/empty value, `0`, or `off` disables it. Relative paths
resolve against the working directory. Interpreter counters and fault
diagnostics remain available with file logging disabled.

Telemetry opens and closes its output synchronously, including a sample every
60 frames while interpreted code is active. Those writes can cause gameplay
and audio stalls on Windows; leave the variable unset during normal play and
timing measurements. Metroid idle comparisons reproduced 83–462 ms gaps with
logging enabled, including with password saving disabled, and no recorded
hitches in the same 20-second route with telemetry disabled and saving enabled.

## Correctness gates

Every retained change must pass the gates relevant to the subsystem:

1. The benchmark's final framebuffer CRC matches the untouched binary and
   `dispatch_miss_count` remains zero.
2. A smoke run matches at multiple frame checkpoints.
3. The recompiler/runtime test suite passes.
4. APU changes additionally preserve captured PCM byte-for-byte.
5. Longer representative title runs retain framebuffer, dispatch, save-state,
   and input-script behavior.

The faithful implementation remains available whenever an optimization relies
on a runtime policy or a title/content assumption.

## Transferable lessons from ndsrecomp

The useful patterns are:

- remove universally executed host ABI boundaries only when measurement shows
  the call itself is material;
- put common RAM/register cases in small inline fast paths with an exact slow
  fallback;
- cache expensive pure results or rare-condition state instead of recomputing
  them for every guest instruction/cycle;
- compile observability out of production hot paths;
- prefer algorithmic rendering work and safe worker parallelism over tiny
  instruction-level tweaks;
- keep same-binary or identical-build A/B controls and reject plausible-looking
  changes that do not win end-to-end.

Call counts alone are not evidence: ndsrecomp rejected several high-coverage
specializations that were flat in whole-workload timing.

## NES results

### Session 2026-09-05 - production trace defaults and generated-boundary specialization

Retained changes:

- Fresh consumer CMake configurations now default `NESRECOMP_ENABLE_TRACE=OFF`.
  Because `NESRECOMP_ENABLE_STACK_TRACKING` and
  `NESRECOMP_ENABLE_POSTMORTEM_RINGS` default from the trace option in a fresh
  tree, clean production builds select `debug_server_stub.c`, define
  `NESRECOMP_TRACE=0`, and omit generated shadow-stack tracking plus the
  postmortem rings unless a diagnostic build opts back in.
- Existing CMake caches preserve all three option values independently. To
  force an old cache back to production instrumentation settings, pass:

  ```text
  -DNESRECOMP_ENABLE_TRACE=OFF
  -DNESRECOMP_ENABLE_STACK_TRACKING=OFF
  -DNESRECOMP_ENABLE_POSTMORTEM_RINGS=OFF
  ```

- `runner/src/logger.c` now compiles the tracking table and `printf` path only
  for trace-enabled builds. Trace-off builds still export
  `log_on_change()`/`log_reset_frame()` no-op symbols so game/custom code that
  includes only `nes_runtime.h` keeps linking.
- The unconditional per-frame `NMI_enable` log call in `main_runner.c` is now
  behind the existing `s_debug` gate.
- Generated normal instruction-boundary calls and generated in-body/back-edge
  boundary calls use `nes_cpu_instruction_boundary()` directly for mappers that
  do not need generated-PC projection. Mapper 4 and mapper 40 retain
  `nes_instruction_boundary()` at those original projected-boundary sites.

Rejected during review:

- Replacing pre-existing `nes_cpu_instruction_boundary()` transfer prefixes in
  dynamic calls/JMP continuations was reverted. Those operands are destination
  CPU addresses, not current generated PCs, so projecting them through the
  current window base can corrupt continuation timing on 8KB-window mappers.
- A mapper-40 fixed-window self-loop shortcut was reverted because it changed
  dispatch semantics outside the scoped generated-boundary specialization and
  needs a separate proof.

Verification run on Windows host:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' `
  'F:\Projects\nesrecomp\nesrecomp\build_dedup_msvc\NESRecomp.vcxproj' `
  /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal

$env:NESRECOMP_EXE='F:\Projects\nesrecomp\nesrecomp\build_dedup_msvc\Release\NESRecomp.exe'
npm --prefix tests test -- codegen.test.ts
```

Result: recompiler rebuild passed; focused codegen suite passed
(`54 passed`). Added coverage verifies mapper 0/1/66 direct CPU boundaries,
mapper 4 projected boundaries, and mapper 40 projected normal/back-edge
boundaries while preserving mapper 40's existing dynamic tail-dispatch transfer
boundary.

Independent review validation also passed against the narrowed
`build_dedup_msvc/Release/NESRecomp.exe` build:

- Full Vitest suite: `72 passed`.
- Interpreter selftest rebuilt from `interp.c`, `cpu6502_decoder.c`, and
  `tests/interp_selftest.c` with GCC `-O2 -std=c11`: `70 passed`, including
  native handoffs, bank projection, save-state continuation, and mapper-40
  single-step fallback coverage.

CMake option validation used a temporary consumer project that included
`runner/runner.cmake` and wrote the resolved source list/compile definitions:
fresh default configured all three trace/stack/ring options OFF and selected
`debug_server_stub.c`; explicit trace ON selected `debug_server.c`,
`NESRECOMP_TRACE=1`, and `RECOMP_STACK_TRACKING`; reconfiguring an existing
trace-on cache with only `NESRECOMP_ENABLE_TRACE=OFF` left stack tracking and
postmortem rings ON, confirming the documented all-three override requirement.

Logger compatibility syntax checks passed for `NESRECOMP_TRACE=0` and
`NESRECOMP_TRACE=1` with a translation unit including both `nes_runtime.h` and
`logger.h`, plus direct syntax checks of `logger.c` in both modes.

The production trace default also avoids a large debug-server allocation in
titles that initialize the server path. A compiled size probe against the
current headers reported `sizeof(NESFrameRecord) == 23272`; with
`FRAME_HISTORY_CAP == 36000`, the real `debug_server_init()` path requests
837,792,000 bytes, about 799 MiB, through `calloc()`. The older comment in
`debug_server.c` saying about 540 MiB is stale. This is a concrete memory
benefit for Xbox-class systems when a clean production build links
`debug_server_stub.c`; explicit trace builds retain the real debug server.

Small-title generated-boundary validation used Super Mario Bros. (mapper 0)
from isolated worktrees so the title source repository was not edited:

```text
Framework baseline: F:\Projects\nesrecomp\_wt_boundary-20260905003359_nes_base
Title baseline:     F:\Projects\nesrecomp\_wt_boundary-20260905003359_smb_base
Title candidate:    F:\Projects\nesrecomp\_wt_boundary-20260905003359_smb_cand
ROM:                F:\Projects\nesrecomp\SuperMarioBrosRecomp\baserom.nes
```

The baseline recompiler was rebuilt from framework baseline `1ee00e4`. The
candidate recompiler was `build_dedup_msvc/Release/NESRecomp.exe` from the
narrowed generated-boundary change. Both title builds used the same baseline
framework/runtime source and identical MSVC x64 Release production flags:

```powershell
& $cmake -S $titleDir -B $buildDir -G 'Visual Studio 17 2022' -A x64 `
  -DNESRECOMP_ENABLE_TRACE=OFF `
  -DNESRECOMP_ENABLE_STACK_TRACKING=OFF `
  -DNESRECOMP_ENABLE_POSTMORTEM_RINGS=OFF `
  -DNESRECOMP_REQUIRE_FALCON_OWNER_HELPER=OFF

& $msbuild (Join-Path $buildDir 'SuperMarioBrosRecomp.vcxproj') `
  /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal
```

The build output confirmed `debug_server_stub.c` and the production
`/URECOMP_STACK_TRACKING` override. The generated-code shape changed as
intended for mapper 0. Exact regex counts across the generated C folder showed
the same total boundary-call count in both builds; candidate output was split
into one additional bank-01 part file, but both builds compiled
`generated/super-mario-bros_full.c` as the aggregator:

| Counted generated call text | Baseline | Candidate |
| --- | ---: | ---: |
| `nes_cpu_instruction_boundary` | 2,040 | 91,568 |
| `nes_instruction_boundary` | 89,528 | 0 |
| Total boundary calls | 91,568 | 91,568 |

Both rebuilt title binaries passed a 600-frame smoke run with zero dispatch
misses and matching checkpoint CRCs. Root independently reran both exact
baseline/candidate smoke executables and confirmed the same five checkpoint
CRCs and zero dispatch misses:

```powershell
& $exe $rom --smoke 600 --smoke-interval 120 --smoke-output smoke_600.json
```

```json
{
  "frames_run": 600,
  "dispatch_miss_count": 0,
  "frame_hashes": {
    "0": "644b46ed",
    "120": "37d8c73d",
    "240": "07800bf0",
    "360": "37d8c73d",
    "480": "07800bf0"
  }
}
```

After the final runtime-side APU/PPU/logger/main-runner changes landed, the
SMB1 candidate executable was rebuilt in place using the existing generated
boundary C objects as the title input and recompiling only the changed runner
sources. The original timing candidate executable was preserved at
`F:\Projects\nesrecomp\_wt_boundary-20260905003359_smb_cand\build_boundary_release_vs\preserved_timing_candidate\SuperMarioBrosRecomp.exe`.
The combined executable for independent smoke validation is
`F:\Projects\nesrecomp\_wt_boundary-20260905003359_smb_cand\build_boundary_release_vs\Release\SuperMarioBrosRecomp.exe`.
It passed the same 600-frame smoke command with the same five checkpoint CRCs
and zero dispatch misses; the normal `NMI_enable` log line was absent with the
current logger/main-runner trace gating.

The first five order-balanced 1,200-frame benchmark pairs were CRC-correct
with zero dispatch misses, but the delta was inside host scheduling noise
(raw medians: 0.484813 ms/frame baseline, 0.483600 ms/frame candidate). A
longer three-pair run used 1,200 warmup frames and 12,000 measured frames:

```powershell
& $exe $rom --benchmark 12000 --benchmark-warmup 1200 `
  --benchmark-output bench.json
```

| Run | Baseline ms/frame | Candidate ms/frame | Baseline FPS | Candidate FPS | CRC | Misses |
| --- | ---: | ---: | ---: | ---: | --- | ---: |
| 1 | 0.511506 | 0.486287 | 1955.010 | 2056.399 | `9ad9b089` | 0 / 0 |
| 2 | 0.546243 | 0.480721 | 1830.687 | 2080.208 | `9ad9b089` | 0 / 0 |
| 3 | 0.733452 | 0.684862 | 1363.416 | 1460.148 | `9ad9b089` | 0 / 0 |

The longer paired median was a 6.6% elapsed-time reduction, equivalent to a
7.1% throughput increase, on this desktop host. The third pair shows visible
host slowdown in both binaries, so this is evidence for the generated-boundary
direction on a small mapper-0 title, not a substitute for target hardware
measurement. Existing title binaries generated before this change still contain
their old boundary calls. Original Xbox validation still requires the target
toolchain/port build and should report code size, static memory, uncapped
throughput, and worst representative frame cost separately from these desktop
checks.

The first Release measurements use Super Mario Bros. 3, MinGW GCC 15.2,
`NESRECOMP_ENABLE_TRACE=OFF`, 600 warmup frames, and 3,000 measured frames.
Each result is five order-balanced baseline/candidate pairs.

- Nonlinear APU mixer memoization was rejected. Baseline/candidate medians were
  857.363/859.771 FPS, a 0.28% change with a negative pair.
- The APU timer-period cache was retained. The opt-in subsystem benchmark
  `tests/render_audio/bench_render_audio.ps1 -BaselineRef 1ee00e4` used GCC
  15.2.0 with `-O2 -DNESRECOMP_TRACE=0 -DNESRECOMP_ENABLE_MODS=0`; all seven
  paired samples improved and preserved the PCM hash `34a89b2b`. The paired
  elapsed-time reductions were 14.24%, 4.24%, 3.55%, 4.33%, 13.58%, 11.88%,
  and 10.26%, for a 10.26% median elapsed-time reduction. This is a synthetic
  APU subsystem result, not a whole-title throughput claim. Final review also
  invalidated cached timer-period derivations before `apu_set_state_blob()`
  decodes incoming state, preserving legacy partial-setter behavior without a
  hot-path cost.
- Disabling generated shadow-stack tracking in trace-off builds was retained.
  Baseline/candidate medians were 733.627/764.196 FPS, a 4.17% improvement;
  all five paired signals were non-negative. The executable shrank from
  75,886,183 to 74,430,404 bytes (1.92%).
- Compiling post-mortem dispatch and frame-event rings out of production was
  retained for its Xbox-relevant memory reduction, not as a claimed speed win.
  Stack-only/ring-free medians were 843.447/861.898 FPS (+2.19%) but paired
  signals were mixed. BSS fell by 1,578,048 bytes and text by 6,012 bytes.
- A header-inline internal-RAM read fast path was rejected. It preserved all
  correctness checkpoints but reduced median throughput by 1.47% and enlarged
  the executable from 74,424,037 to 86,188,934 bytes (15.81%).
- Conservative sharing of context-independent, weakly-discovered overlapping
  function bodies was also rejected. A bounded bank-13 probe kept all 1,183
  public wrappers while reducing that bank's generated C from 49,008,519 to
  47,727,663 bytes (2.61%) and the complete executable from 74,424,037 to
  73,782,577 bytes (0.86%). Eight order-balanced pairs were throughput-neutral
  (+0.27% paired median, 902.44 versus 901.60 FPS).
- Generalizing the same structurally safe proof to every bank reduced all
  generated C from 307,788,719 to 303,865,755 bytes (1.27%) and the executable
  from 74,424,037 to 69,124,798 bytes (7.12%). However, all five balanced pairs
  regressed: the paired median was -5.25%, and raw baseline/candidate medians
  were 747.50/725.60 FPS (-2.93%). Shared entry wrappers add a native
  wrapper-to-body boundary, and static discovery evidence does not identify
  whether an indirect entry is hot. The change was therefore not retained
  despite its size win.
- An unconstrained version demonstrated the larger opportunity but was
  rejected: bank-13 generated C fell 28.08% and the executable fell 8.02%,
  but jump/return lowering can depend on the canonical body and therefore was
  not structurally safe to generalize.
- All ownership variants preserved the final benchmark CRC (`fa267494`), zero
  dispatch misses, and six smoke CRC checkpoints through frame 500. The
  earlier retained stack change passed the same gates.
- Opt-in phase attribution showed where SMB3's uncapped time was going before
  renderer work: 49.35% in guest work between frame callbacks, 8.68% in NMI,
  and 41.89% (0.598 ms/frame median) in the per-frame PPU compositor. All
  remaining measured callback phases together were below 0.1%.
- Avoiding same-size PPU color-sidecar reallocations was retained for reduced
  allocation churn while preserving the existing shrink behavior. The opt-in
  subsystem benchmark preserved hashes in classic, widescreen, and HD-pack
  cases. Median paired elapsed-time reductions were 4.11% for classic 256px,
  4.70% for `widescreen_full`, and 0.93% for `hdpack_record`; the classic
  256px result was noisy, including one negative pair, so no meaningful speedup
  is claimed for that case. A separate 256px lower-envelope run had mixed
  samples from -4.12% to +2.24%; fastest baseline/candidate samples were
  975.761/973.230 ms, which did not show a clear regression.
- Caching mapper capability flags for `nes_instruction_boundary()` projection
  and mapper-40 low-PRG bus checks was rejected. The patch was measured in an
  isolated runner-only SMB3 build and then reverted. A ten-sample 1,200-frame
  set was throughput-neutral-to-negative: baseline/candidate median FPS
  2578.1295/2564.3095 (-0.54%) and median ms/frame 0.387878/0.3903065
  (+0.63%). A longer five-sample 6,000-frame set was clearly negative:
  baseline FPS `[2767.230, 2777.439, 2761.567, 2703.811, 2809.711]`,
  candidate FPS `[2738.952, 2711.614, 2694.923, 2742.089, 2666.023]`;
  medians were 2767.230/2711.614 FPS (-2.01%) and 0.361372/0.368784 ms/frame
  (+2.05%). Correctness stayed clean: 600-frame smoke hashes matched, final
  benchmark CRC was `fe741f0a`, and dispatch misses were zero. The rejected
  binary grew from 74,535,350 to 74,535,702 bytes; `.text` grew 128 bytes,
  `.rdata` grew 32 bytes, and `.data`/`.bss` were unchanged.
- The final combined SMB3 host check used the current retained source set
  after reverting the mapper capability patch: APU timer-period cache with
  state-restore invalidation, same-size PPU color-sidecar realloc avoidance,
  and retained logger/runner instrumentation changes. The baseline executable
  was built from clean engine `1ee00e43d2467fa0967a2d59aca2cd8567fea38a`;
  the combined candidate was linked directly from current runner objects and
  the existing generated-object set to avoid a second path-hash-driven full
  generated rebuild. During the interrupted path-switch build, 36 of 293
  generated SMB3 objects were recompiled from the same generated C with the
  same production options but the current workspace include path. No generated
  source was regenerated, `nes_runtime.h` had no diff, and the mapper
  capability globals had already been reverted before these objects were
  compiled. Both builds used MinGW GCC 15.2 Release with
  `NESRECOMP_ENABLE_TRACE=OFF`, `NESRECOMP_ENABLE_STACK_TRACKING=OFF`, and
  `NESRECOMP_ENABLE_POSTMORTEM_RINGS=OFF`. ROM path:
  `F:\Projects\nesrecomp\SuperMarioBros3Recomp\Super Mario Bros. 3 (USA).nes`.
  The independent smoke command is:
  `SuperMarioBros3Recomp.combined.exe "F:\Projects\nesrecomp\SuperMarioBros3Recomp\Super Mario Bros. 3 (USA).nes" --smoke 600 --smoke-interval 100 --smoke-output smoke.combined.cand.json`.
  Baseline and candidate smoke outputs matched at frames 0/100/200/300/400/500
  with zero dispatch misses. Root independently reran the exact baseline and
  combined SMB3 executables and confirmed all six checkpoints matched, zero
  misses, and the normal `NMI_enable` log line was absent in the combined
  candidate. Root also independently reran the combined SMB1 smoke check and
  confirmed all five checkpoints matched. Five order-balanced 6,000-frame
  benchmark samples
  were baseline FPS `[2459.604, 2483.163, 2459.608, 2377.036, 2412.642]` and
  candidate FPS `[2545.362, 2519.559, 2462.300, 2517.493, 2564.659]`;
  medians were 2459.604/2519.559 FPS (+2.44%) and 0.406570/0.396895 ms/frame
  (-2.38%). Final CRC was `fe741f0a` and dispatch misses were zero in every
  sample. This is positive host evidence but below the 3% whole-title gate.
  The candidate artifact was 74,535,842 bytes (+492 versus baseline); `.text`
  was 71,585,664 bytes, `.data` 6,672 bytes, `.rdata` 373,864 bytes, and
  `.bss` 12,636,976 bytes.
- User manual desktop playtests on the current retained builds passed visual
  and audio checks for Super Mario Bros., Kirby's Adventure, The Legend of
  Zelda, Metroid, Dr. Mario, and Yoshi. This is manual playtest evidence, not
  a replacement for the automated smoke/CTest/benchmark suite and not an
  original-Xbox validation result.
- The 64-bit desktop SMB3 artifact is not an Xbox-fit binary. Its `.text`
  alone is about 68.3 MiB and `.bss` is about 12.0 MiB, before heap, stack,
  renderer assets, allocator overhead, and platform libraries. Original Xbox
  validation remains an explicit 32-bit target-toolchain gate and must report
  target code size, static memory, uncapped throughput, and worst
  representative frame cost.
- Final render/audio CTest validation passed under MSVC Win32 Release with
  `/arch:IA32` after the APU partial-state-restore invalidation fix. This is a
  32-bit Windows compiler check, not Xbox hardware validation.
- The aggressive PPU opacity/color-copy/per-pixel rewrite was reverted after
  final review because the evidence was inconsistent and did not satisfy the
  no-degradation requirement.
- Skipping the now-redundant initial framebuffer fill was rejected. An early
  five-pair batch was positive, but a second batch had an adverse
  lower-envelope signal and all three end-to-end pairs were negative. The
  cache-sensitive result was not stable enough to retain.

## NES burn-down

- [x] Add an uncapped, deterministic, fully rendered benchmark mode.
- [x] Establish a quiet Release baseline with a representative title.
- [x] Attribute uncapped frame time with opt-in phase counters (SMB3:
      guest-between-callbacks 49.35%, NMI 8.68%, PPU 41.89%).
- [x] Measure nonlinear APU mixer memoization (rejected: below retention gate).
- [x] Cache APU timer-period work where hashes stay identical (retained:
      synthetic subsystem median elapsed-time reduction 10.26%).
- [x] Strip shadow-stack tracking from trace-off production hot paths while
      retaining an explicit diagnostic opt-in.
- [x] Compile post-mortem event-ring writes and storage out of production
      builds while retaining an explicit diagnostic opt-in.
- [x] Measure an inline common read path (rejected: slower and excessive code
      growth).
- [x] Measure sharing context-independent weak-entry bodies while preserving
      every public wrapper (rejected: all-bank throughput regression).
- [ ] Represent jump/return context per block so the remaining overlapping
      generated regions can be shared without changing native semantics.
- [ ] If Xbox executable size becomes limiting, measure profile-guided cold
      weak-entry sharing or compiler basic-block outlining; do not infer
      hotness from static discovery evidence.
- [x] Avoid same-size PPU color-sidecar reallocations while preserving shrink
      behavior (retained: no classic 256px speed claim; widescreen synthetic
      subsystem median elapsed-time reduction 4.70%).
- [x] Audit generated per-instruction hooks and dynamic dispatch boundaries.
- [x] Measure cached runtime mapper capability flags for instruction-boundary
      projection and mapper-40 low-PRG bus checks (rejected: longer SMB3 host
      run regressed 2.01% median FPS and grew code slightly).
- [x] Run every locally runnable title with a public repository through its
      attract/demo path and deterministic basic input fuzzing. Fourteen titles
      passed; Mother 1 built but is explicitly blocked on its absent
      user-supplied ROM. See `docs/PUBLIC_TITLE_PERF_VALIDATION.md`.
- [ ] Validate the retained set on the Xbox toolchain and report code size,
      static memory, uncapped throughput, and worst representative frame cost.

## Follow-on engines

Apply the same harness and gates to snesrecomp, then segagenesisrecomp. Their
first candidates should be selected independently from profiles: the SNES PPU,
APU/DSP, coprocessor, and indirect-dispatch paths differ substantially from
the NES, while Genesis adds VDP rendering, YM2612/PSG synthesis, Z80
coordination, and a different 68K dispatch/bus shape.
