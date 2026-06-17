# nesref

A standalone, hardware-accurate **reference interpreter with debugging tools**,
used as the differential oracle when chasing bugs in the recompiled NES build.

The NES sibling of [`snesref`](../../../../snesrecomp/snesrecomp/tools/snesref)
(SNES) and [`mdref`](https://github.com/mstan/mdref) (Genesis / Genesis Plus GX).
Like those, it is a minimal SDL2 [libretro](https://www.libretro.com/) frontend
that loads a real NES emulator core (an interpreter such as
`nestopia_libretro.dll` or `fceumm_libretro.dll`) and drives it while exposing
the same instrumentation the recomp runner's `debug_server` does. You play the
game on a known-good interpreter and diff its state against the recompiled run.

## Interface parity — the whole point

nesref speaks **the same TCP debug-server protocol as the recomp runner**
(`nesrecomp/runner/src/debug_server.c`): same loopback port (`4370`), same
line-delimited JSON commands, same response shapes. The exact same probe scripts
that talk to a running `*Recomp.exe` talk to nesref unchanged — so a divergence
is a literal field-by-field diff of two identical interfaces.

Backed by the libretro core's exposed memory (`retro_get_memory_data`):

| Command | Backing | Notes |
|---------|---------|-------|
| `ping`, `frame`, `help`, `quit` | — | identical shapes |
| `read_ram`, `dump_ram`, `write_ram` | `SYSTEM_RAM` (2 KB WRAM) + `SAVE_RAM` (`$6000-$7FFF`) | same `read_byte` address map as the runner |
| `read_ppu` | `VIDEO_RAM` (nametable CIRAM, when the core exposes it) | CHR/palette unavailable from stock cores |
| `read_frame_ram`, `history` | per-frame WRAM+SRAM ring (`RING_CAP` frames) | query a historical frame, like the runner's ring |
| `save_state`, `load_state` | `retro_serialize` | park + replay a window deterministically |

**Not** exposed by a stock libretro core (and therefore not by nesref): PPU
internals (`t`/`v`/`w`, scroll, OAM, palette) and CPU registers. Those remain the
job of the **in-process patched-Nestopia oracle** (`ENABLE_NESTOPIA_ORACLE`),
which is intentionally kept. nesref covers the memory/frame/state layer with a
swappable, ship-free core; the embedded oracle covers the deep PPU/CPU internals.
Commands nesref can't back return the same `{"ok":false,"error":...}` shape.

## Why a separate interpreter, not the recompiler

The recompiler translates 6502 to native C ahead of time; subtle timing/state
bugs only show up as a *divergence from real hardware*. You need a trusted
reference to diff against. Keeping it as a separate tool (rather than embedded in
the runner) means the shipping game exes carry none of it, and the reference can
be swapped for any libretro NES core.

## Build

```bat
build.bat
```

Produces `nesref.exe`. SDL2 is taken from `nesrecomp/runner/external/SDL2`
(already in-tree); `SDL2.dll` is copied next to the exe.

## Run

```bat
nesref.exe <core.dll> <rom.nes>
:: e.g. nesref.exe nestopia_libretro.dll faxanadu.nes
```

Place the libretro core DLL next to the exe (or pass a path). Then point the
recomp's probe tooling at `127.0.0.1:4370` exactly as you would the runner.

### Keys (match the recomp NES keybinds)

| Key | NES | | Key | Action |
|-----|-----|-|-----|--------|
| Arrows | D-pad | | F1-F9 | load state slot 1-9 |
| Z | A | | Shift+F1-F9 | save state slot 1-9 |
| X | B | | Esc | quit |
| Tab | Select | | | |
| Enter | Start | | | |

### Env

| Var | Effect |
|-----|--------|
| `NESREF_PORT` | TCP port (default `4370`) |
| `NESREF_TRACE=1` | also append a per-frame WRAM-diff `nes_trace.jsonl` |
| `NESREF_WAV=path.wav` | dump the core's audio (PCM) for differential audio compare |
| `NESREF_QUIT_FRAMES=N` | run headless for N frames, then exit |

## Licensing

This tool ships **only its own source**: `frontend.cpp` (this project's license)
and `libretro.h` (**MIT**, RetroArch team); SDL2 is **zlib**. The emulator core is
a **runtime DLL you supply** — e.g. `nestopia_libretro.dll` — licensed separately.
No core source or binary is committed here (see `.gitignore`).
