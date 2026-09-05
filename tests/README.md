# nesrecomp tests

This directory contains repository-level synthetic and focused native tests.
Per-game regression tests live in each game repo's `tests/` directory.
The orchestrator script lives in `nesrecomp-release/run-tests.sh`.

The focused native self-tests remain standalone CMake projects, matching the
historical mapper test:

```sh
cmake -S tests/mod_runtime -B build/mod-runtime-tests
cmake --build build/mod-runtime-tests --config Release
ctest --test-dir build/mod-runtime-tests -C Release --output-on-failure
```

`external_rom_gate_selftest` generates synthetic 16 MiB fixtures at runtime;
it never requires or distributes a commercial ROM. It covers N64 z64/v64/n64
normalization, raw hashing, missing and wrong files, the PLAY-time rehash and
stale-plan clear, and package-scoped resource-ID uniqueness.

The mapper and runtime-boundary tests should be run with optimized code while
keeping C `assert()` checks active. With single-config generators, override the
Release flags so `NDEBUG` is not defined:

```powershell
cd F:\Projects\nesrecomp\nesrecomp
C:\msys64\mingw64\bin\cmake.exe -S tests/runtime_boundary -B F:\Projects\nesrecomp\build\runtime-boundary-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2"
C:\msys64\mingw64\bin\cmake.exe --build F:\Projects\nesrecomp\build\runtime-boundary-tests --config Release -j 1
C:\msys64\mingw64\bin\ctest.exe --test-dir F:\Projects\nesrecomp\build\runtime-boundary-tests -C Release --output-on-failure

C:\msys64\mingw64\bin\cmake.exe -S tests/mapper -B F:\Projects\nesrecomp\build\mapper-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="-O2"
C:\msys64\mingw64\bin\cmake.exe --build F:\Projects\nesrecomp\build\mapper-tests --config Release -j 1
C:\msys64\mingw64\bin\ctest.exe --test-dir F:\Projects\nesrecomp\build\mapper-tests -C Release --output-on-failure
```

## Render/audio focused self-tests

The render/audio tests cover the APU timer-period cache, rejected partial APU
state restores, and PPU background side-channel behavior used by sprite
priority, retained frames, widescreen, IRQ splits, and HD-pack metadata. Run
them as standalone native tests:

```powershell
cd F:\Projects\nesrecomp\nesrecomp
C:\msys64\mingw64\bin\cmake.exe -S tests/render_audio -B F:\Projects\nesrecomp\build\render-audio-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
C:\msys64\mingw64\bin\cmake.exe --build F:\Projects\nesrecomp\build\render-audio-tests --config Release -j 1
C:\msys64\mingw64\bin\ctest.exe --test-dir F:\Projects\nesrecomp\build\render-audio-tests -C Release --output-on-failure
```

The September 5, 2026 expected hashes were checked against framework baseline
`1ee00e4` while the optimization candidate was still uncommitted. The same
tests also passed under VS 2022 Win32 Release and VS 2022 Win32 Release with
`/arch:IA32`; those are 32-bit Windows compiler checks, not Xbox hardware
validation.

## Render/audio opt-in benchmarks

`tests/render_audio/bench_render_audio.ps1` builds synthetic subsystem
benchmarks with baseline sources from `-BaselineRef` and candidate sources from
the working tree. Use it before committing, or pass the pre-change ref
explicitly:

```powershell
cd F:\Projects\nesrecomp\nesrecomp
.\tests\render_audio\bench_render_audio.ps1 -BaselineRef 1ee00e4
```

The script uses MinGW GCC by default. The September 5, 2026 handoff run used
framework baseline `1ee00e4` and GCC 15.2.0 with
`-O2 -DNESRECOMP_TRACE=0 -DNESRECOMP_ENABLE_MODS=0`. Reported percentages are
elapsed-time reductions, calculated as
`(base_ms - cand_ms) / base_ms`; they are not throughput multipliers. These are
subsystem timings only and should not be reported as total-frame/title gains.

APU sample vector, final candidate:

| run | base ms | candidate ms | elapsed reduction | base hash | candidate hash |
| --- | ---: | ---: | ---: | --- | --- |
| 0 | 165.308 | 141.768 | 14.24% | 34a89b2b | 34a89b2b |
| 1 | 156.600 | 149.962 | 4.24% | 34a89b2b | 34a89b2b |
| 2 | 160.543 | 154.850 | 3.55% | 34a89b2b | 34a89b2b |
| 3 | 153.668 | 147.011 | 4.33% | 34a89b2b | 34a89b2b |
| 4 | 163.066 | 140.916 | 13.58% | 34a89b2b | 34a89b2b |
| 5 | 164.283 | 144.768 | 11.88% | 34a89b2b | 34a89b2b |
| 6 | 175.130 | 157.162 | 10.26% | 34a89b2b | 34a89b2b |

Median paired APU elapsed-time reduction: 10.26%.

PPU sidecar sample vector, final capacity-tracking candidate:

| pair | case | base ms | candidate ms | elapsed reduction | hash |
| --- | --- | ---: | ---: | ---: | --- |
| 0 | classic | 346.875 | 337.507 | 2.70% | c63bd627 |
| 0 | widescreen_full | 514.543 | 502.432 | 2.35% | bb51dff2 |
| 0 | hdpack_record | 661.029 | 647.716 | 2.01% | c63bd627 |
| 1 | classic | 370.752 | 341.981 | 7.76% | c63bd627 |
| 1 | widescreen_full | 495.714 | 482.727 | 2.62% | bb51dff2 |
| 1 | hdpack_record | 675.332 | 676.338 | -0.15% | c63bd627 |
| 2 | classic | 359.016 | 379.326 | -5.66% | c63bd627 |
| 2 | widescreen_full | 525.009 | 489.461 | 6.77% | bb51dff2 |
| 2 | hdpack_record | 706.242 | 685.354 | 2.96% | c63bd627 |
| 3 | classic | 384.901 | 363.703 | 5.51% | c63bd627 |
| 3 | widescreen_full | 547.053 | 503.123 | 8.03% | bb51dff2 |
| 3 | hdpack_record | 706.697 | 710.912 | -0.60% | c63bd627 |

Median paired elapsed-time reductions: classic 4.11%, widescreen_full 4.70%,
hdpack_record 0.93%. Treat the 256px classic result as noisy and do not claim a
meaningful speedup from it.

PPU 256px lower-envelope sample vector:

| pair | base ms | candidate ms | elapsed reduction | hash | opaque |
| --- | ---: | ---: | ---: | --- | ---: |
| 0 | 978.035 | 1018.377 | -4.12% | c63bd627 | 0 |
| 1 | 1063.844 | 1059.458 | 0.41% | c63bd627 | 0 |
| 2 | 1020.734 | 997.830 | 2.24% | c63bd627 | 0 |
| 3 | 975.761 | 986.334 | -1.08% | c63bd627 | 0 |
| 4 | 984.870 | 973.230 | 1.18% | c63bd627 | 0 |
| 5 | 1014.753 | 1034.993 | -1.99% | c63bd627 | 0 |

Fastest baseline was 975.761 ms; fastest candidate was 973.230 ms. This did
not show a clear regression in the fastest samples; measurements remain noisy
and are not Xbox validation.
