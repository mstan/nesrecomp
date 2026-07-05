# nesref

A standalone, hardware-accurate **reference interpreter with debugging tools**,
used as the differential oracle when chasing bugs in the recompiled NES build.

The NES sibling of [`snesref`](../../../../snesrecomp/snesrecomp/tools/snesref)
(SNES) and `mdref` (Genesis / Genesis Plus GX). Like those, it is a minimal SDL2
[libretro](https://www.libretro.com/) frontend that loads a real NES emulator
core (**`mesen_libretro.dll` preferred — cycle-accurate**; `nestopia_libretro.dll`
/ `fceumm_libretro.dll` also work) and drives it headless or windowed while
exposing the instrumentation the co-sim coordinator (`tools/nes_cosim.py`)
consumes. You run the same ROM on a known-good interpreter and diff its
state / video / cycles / audio against the recompiled run.

This file is the tracked source of the standalone working copy at
`F:\Projects\nesref\` (its own repo, where the core DLLs and SDL2 runtime
live); keep the two `frontend.cpp` copies in sync when either changes.

## What it exposes (all env-gated, all headless-safe)

| Var | Effect |
|-----|--------|
| `NESREF_FRAMES=N` | headless: run N frames (no window, unthrottled, deterministic), then exit |
| `NESREF_TRACE_FILE=p.jsonl` | per-frame WRAM(+SRAM) trace for `nes_cosim.py abram` / gate 2 |
| `NESREF_CYCLE_FILE=p.jsonl` | per-frame guest cycle counts (from the serialize blob) for `abcycle` |
| `NESREF_PPU_FILE=p.jsonl` | per-frame OAM/palette/NT dump (from the serialize blob) for `abppu` |
| `NESREF_PPU_NT_D`, `NESREF_PPU_OAM_D`, `NESREF_PPU_PAL_D` | serialize-blob offset overrides (CHR-RAM games shift NT) |
| `NESREF_WAV=path.wav` | stream the core's audio to a stereo s16 WAV at the core rate (48 kHz for Mesen) — the audio-fidelity oracle for `nes_cosim.py abaudio` / `tools/nes_audio_ab.py` |
| `NESREF_SCRIPT=path` | per-frame input script — same syntax as the runner's `--script` (`HOLD`/`RELEASE`/`WAIT`/`WAIT_RAM8` state-anchoring; other commands ignored) so both engines can be driven through identical inputs |
| `NESREF_SHOT=frame`, `NESREF_SHOT_FILE=p.png` | screenshot at a frame |
| `NESREF_STATEDUMP=frame:path` | raw `retro_serialize` blob dump at a frame |
| `NESREF_FREEZE=addr:val[,...]` | force WRAM bytes each frame (shared-RNG pinning, matches `NESRECOMP_FREEZE`) |
| `NESREF_DUMP`, `NESREF_MEMPROBE` | ad-hoc memory probes |

Windowed mode (no `NESREF_FRAMES`) runs at 60.098 Hz with the recomp keybinds
(arrows / Z / X / Tab / Enter, Esc quits).

A previous iteration of this tool instead mirrored the runner's TCP
debug-server protocol; it was superseded by the trace-file + serialize-blob
interface above (which reaches PPU/APU internals the libretro memory API
cannot) and lives in git history if ever needed.

## Why a separate interpreter, not the recompiler

The recompiler translates 6502 to native C ahead of time; subtle timing/state
bugs only show up as a *divergence from real hardware*. You need a trusted
reference to diff against. Keeping it as a separate tool (rather than embedded
in the runner) means the shipping game exes carry none of it, and the reference
can be swapped for any libretro NES core.

## Build

```bat
build.bat
```

Produces `nesref.exe`. SDL2 is taken from `nesrecomp/runner/external/SDL2`
(already in-tree); `SDL2.dll` is copied next to the exe.

## Run

```bat
nesref.exe <core.dll> <rom.nes>
:: e.g. nesref.exe mesen_libretro.dll baserom.nes
```

## Licensing

This tool ships **only its own source**: `frontend.cpp` (this project's license)
and `libretro.h` (**MIT**, RetroArch team); SDL2 is **zlib**. The emulator core
is a **runtime DLL you supply** — e.g. `mesen_libretro.dll` — licensed
separately. No core source or binary is committed here (see `.gitignore`).
