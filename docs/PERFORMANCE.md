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
- Both experiments preserved the final benchmark CRC (`fa267494`) and zero
  dispatch misses. The retained stack change also matched six smoke CRC
  checkpoints through frame 500.

## NES burn-down

- [x] Add an uncapped, deterministic, fully rendered benchmark mode.
- [x] Establish a quiet Release baseline with a representative title.
- [ ] Attribute CPU time across APU, PPU, generated CPU/runtime, mapper, and
      dispatch work.
- [x] Measure nonlinear APU mixer memoization (rejected: below retention gate).
- [x] Strip shadow-stack tracking from trace-off production hot paths while
      retaining an explicit diagnostic opt-in.
- [x] Compile post-mortem event-ring writes and storage out of production
      builds while retaining an explicit diagnostic opt-in.
- [ ] Measure inline common bus paths before changing generated code.
- [ ] Measure PPU work by rendering mode and pursue algorithmic reductions.
- [ ] Audit generated per-instruction hooks and dynamic dispatch boundaries.
- [ ] Validate the retained set on the Xbox toolchain and report code size,
      static memory, uncapped throughput, and worst representative frame cost.

## Follow-on engines

Apply the same harness and gates to snesrecomp, then segagenesisrecomp. Their
first candidates should be selected independently from profiles: the SNES PPU,
APU/DSP, coprocessor, and indirect-dispatch paths differ substantially from
the NES, while Genesis adds VDP rendering, YM2612/PSG synthesis, Z80
coordination, and a different 68K dispatch/bus shape.
