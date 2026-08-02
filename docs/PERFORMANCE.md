# Performance work

This document is the burn-down and evidence contract for portable runner
performance work. The immediate target is the original Xbox port, but retained
changes must also improve normal desktop production builds without weakening
the emulated machine.

## Measurement contract

- Build `Release` with `NESRECOMP_ENABLE_TRACE=OFF`.
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

The first Release measurements use Super Mario Bros. 3, MinGW GCC 15.2,
`NESRECOMP_ENABLE_TRACE=OFF`, 600 warmup frames, and 3,000 measured frames.
Each result is five order-balanced baseline/candidate pairs.

- Nonlinear APU mixer memoization was rejected. Baseline/candidate medians were
  857.363/859.771 FPS, a 0.28% change with a negative pair.
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
- Fetching background nametable, attribute, palette, and CHR state once per
  8-pixel tile span was retained. The old loop redundantly performed those
  constant lookups for every pixel. Five order-balanced pairs were all
  positive: paired median throughput improved 21.89%, with raw
  baseline/candidate medians of 806.56/972.09 FPS (+20.52%). PPU time fell to
  0.258 ms/frame (about 57% lower), the executable size was unchanged, the
  final CRC remained `fa267494`, and all smoke hashes and dispatch checks
  matched.
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
- [x] Reduce per-frame PPU background work from per-pixel state lookup to
      per-tile-span fetches (retained: +21.89% paired median).
- [ ] Audit generated per-instruction hooks and dynamic dispatch boundaries.
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
