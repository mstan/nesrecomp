# SMB3 HLE candidate attribution

Investigation: 2026-10-06, central issue `beads-wtuu`. No HLE replacement or
default change is proposed by this investigation. The available Windows host
is the measurement scope; there is no Xbox performance claim.

## Baseline and scope

SMB3 now selects the cycle backend by default. Its framework pin is
`0c061070aebe5caa0b4e22cce70892f52d04a0a3`. The workspace framework checkout
and title submodule were on older, different branches lacking `runner/cyc`.
An isolated worktree at the title's exact pin supplies the inspected/built
runtime. User changes and old build trees were preserved.

The current title was configured with native Windows CMake and MinGW GCC
15.2, Release (`-O3 -DNDEBUG`), the cycle backend, and launcher UI disabled.
The official title target still links SDL support; `--frames` selects its
headless path. This runs the actual cycle PPU and APU, including rendered
pixels, rather than a CPU-only loop. It excludes window presentation/audio
device pacing. Mods are linked as the title normally configures them; this
capture does not enable a mod.

GCC rejects an existing missing `cyc_trace_mix` declaration in
`runner/cyc/hw_fds.c:766`. This diagnostic build forces inclusion of
`cyc_trace.h` through `CMAKE_C_FLAGS`; it changes no source and does not disable
the compiler error. SMB3 is a cartridge game and does not exercise FDS media
hashing. The workaround must be stated when reproducing this build.

The current compiler reports 13,013 compiled instructions from 4,884 seeds,
and one instruction left to interpretation because it crosses a bank or
`$FFFF`. Historical seed files match the current seeds, but three generated
payload hashes differ (`game_cyc.c`, banks 01 and 3F); historical native versus
interpreter checks therefore do not prove this particular executable.

## Diagnostic method

`tools/host_sample.cpp` samples the launched process's main-thread instruction
pointer roughly every 2 ms. It suspends that thread to read its context, so it
perturbs the workload. Its elapsed time is unsuitable for speedup evidence.
Regular sampling can alias periodic work. There is no warmup exclusion:
startup, menus and loading contribute alongside gameplay. Worker/audio
threads are outside this main-thread capture.
It records the executable module path, actual ASLR base, image size, child
exit code and sampler errors, and stores the child's stdout/stderr separately.
It launches the child without a visible console. The executable and sampler
must have the same architecture; this run uses x64 for both.

`tools/attribute_host_samples.py` maps addresses to the nearest preceding text
symbol from GNU `nm` output of the exact executable, correcting ASLR using the
preferred PE image base. It reports external addresses separately and refuses
captures missing module identity. These are self samples, not inclusive
call stacks or per-guest-routine timing. Inlining and generated chunk boundaries
limit source-level attribution. Known PPU helper symbols establish subsystem
cost; a high chunk entry count alone would not establish a guest HLE candidate.

After the recorded gameplay evidence was captured, review hardened the
sampler with a finite runtime deadline (60 seconds by default), an owned
kill-on-close Windows job, bounded module-identity retries, architecture
checks and explicit wait/resume/exit failures. Only its own spawned child
can be terminated. The analyzer now refuses failed/empty captures and symbol
tables outside the preferred image. These tooling changes do not alter or
retroactively revalidate the recorded game executable or capture. Focused
synthetic tests check the revised tool; no additional game capture is used.

The first historical legacy capture lacks the executable's actual ASLR base.
Its raw instruction pointers cannot support routine attribution; the legacy
phase output is discovery context only, not current cycle-backend evidence.

The first current-cycle capture has 796 samples and zero sampler errors, but
ran while another title was building and lost the child's telemetry through
uninherited output handles. Its final screenshot is the world-one map; that
still image alone cannot prove level entry or progression. Its provisional
ranking is retained transparently, and a bounded follow-up resolves those
specific evidence gaps. No throughput or default-promotion claim uses it.

The final bounded capture ran after the other title's builds finished. Parent
checkout activity could still incur disk I/O; these data remain diagnostic
self samples, not an uninstrumented throughput benchmark. It captured 731
samples, zero sampler errors and child exit zero. Native execution covered
90,533,461 of 95,295,224 CPU cycles (95.0%); 4,761,756 cycles used ROM
interpretation, and no RAM code was interpreted. All 16 configured mod hook
sites reported zero callbacks/handled operations and zero content mismatches.

Snapshots confirm gameplay: frame 2000 shows Mario alive in world 1-1 with
four lives and the clock at 291; frame 2400 shows the world map with three
lives after a death; frame 2800 and the final snapshot retain an active world
map. This is a short level-entry/death/return route, not a level completion,
full campaign, multiplayer or interactive audio/presentation qualification.

| Final self symbol | Samples | Share |
| --- | ---: | ---: |
| `ppu_dot` | 263 | 35.978% |
| `ppu_half_dot` | 83 | 11.354% |
| `bg_fetch` | 56 | 7.661% |
| `apu_cycle` | 42 | 5.746% |
| `output_pixel` | 32 | 4.378% |
| `eval_objects` | 29 | 3.967% |
| `hw_cycle_start` | 26 | 3.557% |
| `vram_fetch` | 25 | 3.420% |
| `hw_cycle_finish` | 21 | 2.873% |
| `shift_sprites` | 18 | 2.462% |
| Largest generated chunk, `chunk_s1_9400` | 7 | 0.958% |

Only 17 samples (2.326%) fall in all named generated chunks combined. This
does not assign their shared PPU/cycle/helper costs inclusively. The three
largest PPU symbols alone account for 402 samples (54.993%). The capture
identifies a subsystem opportunity; it does not demonstrate a particular
costly guest routine with a clear replacement boundary.

The uninstrumented current executable and its `--interp-only` path then ran
the same 3,200-frame input route. All 3,200 per-frame trace, memory/pixel,
register and hardware hash records matched byte for byte (both files SHA256
`7d5087791668f036f3f7e1b23ef1908eae58caa87814ab35959a5f2ee3b7b777`).
Both paths completed 95,295,224 CPU cycles. This supplies a bounded functioning
LLE floor for this route; it does not prove every hardware corner or full-game
progression. The sampled executable's SHA256 is
`dfe54f3d845f517cf1a07729da69a71ad48e16ed5c10b84aac9f22d83583440b`.
The exact GNU `nm` symbol artifact used for attribution has SHA256
`8186c53165cf595c80caa7f42eacfa517301da0da402a23394b687d846cb3077`.

The sampler compiled cleanly with `-O2 -Wall -Wextra`. A short native fixture
verified hidden child output capture, module identity, successful exit and
zero sampling errors. Three Python tests cover ASLR relocation, adjacent
symbol boundaries, exclusion of external addresses, missing identity,
failed/empty captures and mismatched preferred image bases. No production
runtime or generated code was modified.

## Reproduction

Use your own legally supplied ROM and matching title checkout. Keep ROMs,
generated code, logs, images and private symbol artifacts outside Git. On
Windows use native executables through the workspace-required hidden process
wrapper. Shell examples below describe executable arguments; they do not
override that launch policy.

```text
cmake -S TITLE -B BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=NATIVE/gcc.exe -DCMAKE_CXX_COMPILER=NATIVE/g++.exe -DCMAKE_MAKE_PROGRAM=NATIVE/ninja.exe -DNESRECOMP_ROOT=PIN -DNESRECOMP_BACKEND=cycle -DNESRECOMP_ROM=ROM -DNESRECOMP_HEADLESS=ON -DNESRECOMP_RECOMP_UI=OFF -DPython3_EXECUTABLE=NATIVE/python.exe "-DCMAKE_C_FLAGS=-include PIN/runner/cyc/cyc_trace.h"
cmake --build BUILD --target SuperMarioBros3Recomp --parallel 2
g++ -O2 -Wall -Wextra -static tools/host_sample.cpp -o ARTIFACTS/host_sample.exe
nm -n EXACT_BUILD/SuperMarioBros3Recomp.exe > ARTIFACTS/symbols.txt
host_sample.exe ARTIFACTS/samples.csv EXACT_BUILD/SuperMarioBros3Recomp.exe ROM --frames 3200 --input PRIVATE/level-route.txt --screenshot ARTIFACTS/route.png --shot-every 400
python tools/attribute_host_samples.py ARTIFACTS/samples.csv ARTIFACTS/symbols.txt --preferred-base 0x140000000
python tests/host_sample_attribution_test.py
EXACT_BUILD/SuperMarioBros3Recomp.exe ROM --frames 3200 --input PRIVATE/level-route.txt --hash-out ARTIFACTS/native.txt
EXACT_BUILD/SuperMarioBros3Recomp.exe ROM --frames 3200 --input PRIVATE/level-route.txt --interp-only --hash-out ARTIFACTS/interpreter.txt
```

For equivalent private input, the existing level route holds START at frames
240–240 and 420–420, RIGHT at 1200–1229, UP at 1350–1379, A at 1500–1500,
RIGHT at 2200–2399, RIGHT+A at 2400–2449 and RIGHT+B at 2450–2699, then
releases all buttons. It starts with no input. Validate that the route reaches
the intended gameplay with snapshots and forward frame/native-cycle telemetry;
do not infer it from the script's filename.

## Next boundary to investigate

Follow-up tracking: central issue `beads-o777`.

The final samples identify PPU work rather than a costly guest routine
with a demonstrated clear interface. A later experiment should investigate a
build-selected batched PPU/rendering implementation while retaining the
working cycle PPU as LLE. Establish its boundary before changing it: frame
pixels, CPU-visible PPU register reads/writes, sprite-zero/status behavior,
VBlank/NMI visibility, OAM evaluation, and MMC3 address/A12 IRQ observations.
`hw_ppu.c` explicitly describes why the MMC3 sees address changes during each
half-dot. A framebuffer-only comparison cannot validate that interface.

The HLE implementation may compute pixels with a different algorithm and use
coarse internal timing where callers tolerate it. It must publish the required
status, interrupts, memory effects and completion order. No live handoff or
cross-build savestate conversion is required. Promote only after contract
checks, a materially faster uninstrumented build on the stated host, and
gameplay progression checks without softlocks. Keep unsuccessful experiments
as draft branches/PRs. This report does not justify replacing a guessed
guest leaf or enabling HLE by default.
