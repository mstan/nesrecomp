# Nintendo Switch cycle runner (experimental)

An opt-in libnx/SDL2 host that packages one compiled cartridge per `.nro`.
It reuses the cycle CPU/PPU/APU/mappers, scheduler, ROM identity checks,
session lifecycle and state serialization. Desktop builds are unchanged: the
Switch path is selected only when CMake runs with devkitPro's `Switch.cmake`
toolchain (`NINTENDO_SWITCH`).

Status: boots and plays vanilla cartridges on hardware with controls, video
and audio. **Full speed depends on the PPU implementation** on the stock
1.02 GHz CPU (see the measurements below). Saving is **off by default**.

## Scope

- One vanilla cartridge per NRO, built through the standalone
  `runner/cyc/project` (or any `nesrecomp_add_cycle_game` target).
- The Switch build rejects `HEADLESS`, `MODS`, `HD_PACKS`, `HOST_EXTRAS` and an
  explicit `BIOS`, and forces recomp-ui off. FDS, light guns, more than one
  player, netplay and enhanced game hosts are out of scope for this first host.
- No clocks, bus timing or renderer algorithms change. The PPU implementation
  is chosen by the existing `NESRECOMP_PPU_IMPL` option; see the measurements
  below before picking one.

## Files

| Path | Purpose |
| --- | --- |
| `runner/switch/host.c` | SDL video/audio/controller loop, pacing, optional saves, diagnostics, neutral-input benchmark |
| `runner/switch/switch.cmake` | devkitPro/SDL2 target setup, `-O3` + LTO for Release, NACP/NRO (+ optional RomFS) packaging |
| `runner/switch/diagnostics.c` | Minimal `cyc_diagnostics` provider (no `/proc`, crash reporter or process launching) |
| `tools/cyc/test_switch_host.py` | ROM-free host contract test (SDL dummy drivers) and cross-build fixture |

Small hooks in shared files: `cyc.cmake` dispatches `nesrecomp_cyc_enable_sdl`
to the Switch module, `project.cmake` enforces the vanilla-target scope,
`cyc_host.c` calls the platform init/shutdown and accepts the Switch-only
flags, `cyc_replay.c` uses the no-op recorder, and FDS save paths resolve
inside the game directory.

## CMake options

| Option | Default | Meaning |
| --- | --- | --- |
| `NESRECOMP_SWITCH_GAME_DIR` | *(required)* | Per-game SD directory, e.g. `sdmc:/switch/nesrecomp/smb1` |
| `NESRECOMP_SWITCH_TITLE` | `NESRecomp` | Homebrew menu name (NACP) |
| `NESRECOMP_SWITCH_AUTHOR` | `NESRecomp contributors` | NACP author |
| `NESRECOMP_SWITCH_VERSION` | `0.2.0` | NACP version, also printed in `runtime.log` |
| `NESRECOMP_SWITCH_SAVES` | `OFF` | Experimental SD save/load and automatic resume |
| `NESRECOMP_SWITCH_LTO` | `ON` | Link-time optimization (fails loudly if unsupported) |
| `NESRECOMP_SWITCH_ROMFS_ROOT` | *(empty)* | Optional read-only assets, e.g. a scripted input route |
| `NESRECOMP_SWITCH_HEADROOM_COUNTERS` | `OFF` | Keep the core's host measurement counters (`cyc_headroom.h`); they cost about 5% of emulation time here |

## Build

The code generator must run on the build host, so generate with a host
`NESRecomp` and cross-compile with devkitA64. Tested with the official devkitPro
Docker image
`devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282`
(devkitA64 r29.2, libnx 4.12.0, switch-sdl2 2.28.5). A native devkitPro install
with `switch-dev` and `switch-sdl2` works the same way. Inside a Linux
container, build a Linux `NESRecomp` there; it cannot run a macOS binary.

```sh
cmake -S recompiler -B build/recompiler -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/recompiler
cmake -S runner/cyc/project -B build/game-switch -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DNESRECOMP_PPU_IMPL=FRAME \
  -DNESRECOMP_HOST_COMPILER="$PWD/build/recompiler/NESRecomp" \
  -DNESRECOMP_ROM=/path/to/your/game.nes \
  -DNESRECOMP_GAME_CONFIG=/path/to/game.toml \
  -DNESRECOMP_CYCLE_SEEDS=/path/to/cyc_seeds.txt \
  -DNESRECOMP_SWITCH_GAME_DIR=sdmc:/switch/nesrecomp/mygame \
  '-DNESRECOMP_SWITCH_TITLE=My Game Recomp'
cmake --build build/game-switch
# build/game-switch/nes_game.nro
```

The build-time ROM path is never used at runtime; the NRO reads
`<game dir>/rom.nes` and the core checks its identity as on desktop.

`FRAME` is the fastest PPU on this CPU for the boards it supports (NTSC
mappers 0, 1, 4 and 66 on `master`). A FRAME build runs other boards and
regions on the LLE reference, so use `HLE` for those.

## ROM-free test

```sh
python3 tools/cyc/test_switch_host.py --out build/switch-host --recompiler build/recompiler/NESRecomp
python3 tools/cyc/test_switch_host.py --out build/switch-host-nosave --recompiler build/recompiler/NESRecomp --no-saves
python3 tools/cyc/test_switch_host.py --out build/switch-host-pal --recompiler build/recompiler/NESRecomp --no-saves --pal
```

The test writes its own tiny NROM program (no game data), runs the Switch host
on the desktop with SDL dummy drivers and checks:

- saves on: 100% native coverage, autosave/resume, missing/wrong ROM rejection,
  corrupt-state preservation, save-write and invalid-directory errors;
- saves off: an existing state and an invalid save directory are ignored for
  1,900 paced frames, the audio supply ratio stays near 1, two neutral-input
  benchmarks reach identical state, and a scripted window route reaches the
  same state as the headless `--input` replay;
- `--pal`: the NES 2.0 PAL header is reported as `Region=PAL` (50 FPS target).

It also writes a CMake project that cross-compiles the same generated sources
with the SDK:

```sh
cmake -S build/switch-host-nosave/project -B build/switch-fixture \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build/switch-fixture   # switch_fixture.nro
```

## SD layout and controls

```text
sdmc:/switch/nesrecomp/<game>/
  <name>.nro
  rom.nes          # your own matching ROM
  runtime.log      # rewritten each launch
  saves/           # only used when NESRECOMP_SWITCH_SAVES=ON
```

- D-pad or left stick: move; Nintendo A/B: NES A/B; Plus/Minus: Start/Select.
- L + R + Plus: quit to the homebrew menu.
- With saves on: ZL quick save, ZR quick load, automatic resume at launch
  (hold X at launch to start fresh), checkpoints every 30 seconds and on
  focus loss/exit. Saving is off by default because an SD write failure was
  observed on hardware and has not been qualified since.

Video is a 256x240 texture with nearest filtering at 4:3, letterboxed.
Audio is generated at 48 kHz mono and converted by an SDL audio stream; the
device queue is bounded to about 100 ms. Guest deadlines use the core's frame
duration (60.0988 / 50.007 Hz), independent of display VSync.

## Diagnostics and benchmarking

Every five seconds `runtime.log` reports FPS, average emulation / audio /
present / wait milliseconds per frame, and `audio_supply` (generated PCM
duration relative to wall time; below 1 means the emulator cannot keep up).
Startup logs the version, renderer, PPU implementation, region and sprite
kernel.

- `--nxlink-log`: mirror the log to `nxlink -s` (TCP 28771) as well as the SD.
  Also reports applet/performance mode and a read-only CPU clock query.
- `--exit-after N`: stop after N frames. `--uncapped`: no frame pacing.
- `--benchmark --exit-after N [--benchmark-warmup W]` (saves off): neutral or
  scripted input, discards the first W frames (default 300), then prints one
  `benchmark` line with FPS and per-stage milliseconds, and the final memory /
  hardware state hashes, computed outside the measured time.
- `--window-input romfs:/route.txt`: play a pad route (the existing replay
  format) embedded with `NESRECOMP_SWITCH_ROMFS_ROOT`.

## Measured on hardware

Super Mario Bros. (NROM, NTSC) on a Switch in handheld mode at the stock
1.02 GHz CPU clock, `--benchmark --gameplay`-style run: a scripted 1,200-frame
pad route into World 1-1, uncapped, timing the last 300 frames after a
900-frame warm-up. Saves off, LTO and `-O3` on.

| PPU | Headroom counters | FPS | Emulation ms/frame | Audio supply |
| --- | --- | --- | --- | --- |
| HLE | on (core default) | 48.4 | 19.19 | 0.81 |
| HLE | off (Switch default) | 50.7 | 18.26 | 0.84 |
| FRAME | off | **72.0** | **12.48** | 1.20 |

The NTSC target is 60.1 FPS; an audio supply below 1 means the emulator
cannot keep up in real time. All three runs end in the same CPU memory state
as the desktop host. On the desktop, HLE and FRAME produce byte-identical
`--frame-log` output (RAM, CIRAM, palette, OAM and picture indices) for every
frame of the same route. One title and one route are not a general guarantee,
and FRAME is still marked experimental.

PAL: on `master`, FRAME and native intervals are NTSC-only. Adventure Island
Classic (Europe, CNROM) measures 51.76 ms/frame (18.7 FPS) with HLE. With
#112 (masked-IRQ intervals) and #113 (FRAME on PAL NROM/CNROM) it measures
17.90 ms/frame (51.6 FPS) with FRAME, and 17.25 ms (53.4 FPS) with #114 as
well, against a 20.0 ms PAL budget.

## Not yet qualified on hardware

Suspend/resume and HOME/sleep during long sessions, controller reconnection,
docked versus handheld presentation, and SD saving. Individual HOME-menu icons
need a separately built forwarder; this runner only produces an NRO.
