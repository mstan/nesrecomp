# Using the cycle backend in an existing game project

The cycle backend now has a CMake entry point for existing game checkouts.
It uses their ROM and game configuration and builds the cartridge hardware
documented in [MAPPERS.md](MAPPERS.md), including the new mapper families.
All generated code stays in the build directory. The original game's source,
ROM, generated files and submodule checkout are read only.

Requires CMake 3.20+, a C11 compiler and Python 3.11+. The host recompiler is
built automatically during configure. Cross builds must supply an already
built host compiler with `NESRECOMP_HOST_COMPILER` or `RECOMPILER` below.

## Build alongside the existing project

Configure this framework's standalone entry point with the existing game's
absolute paths (quote paths containing spaces):

```sh
cmake -S /path/to/nesrecomp/runner/cyc/project -B build-cycle \
  -DNESRECOMP_ROM="/path/to/game/Game.nes" \
  -DNESRECOMP_GAME_CONFIG="/path/to/game/game.toml" \
  -DNESRECOMP_HEADLESS=ON
cmake --build build-cycle --config Release --parallel 4
```

Run `build-cycle/nes_game` (or `build-cycle/Release/nes_game.exe` with Visual
Studio), followed by the same ROM path and `--frames 600`. Leave `HEADLESS`
off to include SDL2 when available; running without headless options opens
its window. Building or headless verification never launches a game window.
The compiled program checks ROM identity before execution.

## Add an opt-in target to the game's CMakeLists.txt

After `project(...)`, before any legacy SDL/UI dependency setup:

```cmake
option(NESRECOMP_USE_CYCLE_BACKEND "Build the cycle backend" OFF)
if(NESRECOMP_USE_CYCLE_BACKEND)
    include("${NESRECOMP_ROOT}/runner/cyc/project.cmake")
    nesrecomp_add_cycle_game(MyGame
        ROM "${CMAKE_SOURCE_DIR}/Game.nes"
        GAME_CONFIG "${CMAKE_SOURCE_DIR}/game.toml")
    return()
endif()
```

Set `NESRECOMP_ROOT` before this block. The function also accepts `HEADLESS`,
`RECOMPILER /path/to/NESRecomp` and `SEED_FILE /path/to/seeds.txt`. Relative
paths are relative to the calling CMake source directory; a seed path inside
`game.toml` is relative to that configuration file. `GAME_CONFIG` is optional.
Existing legacy-generated C and `extras.c` are not cycle-backend inputs.

## Native coverage and rebuilding

Unseen code runs through the cycle interpreter. Profile a route with
`nes_game ROM --frames 3000 --input route.txt --miss-log seeds.txt`, then set
`NESRECOMP_CYCLE_SEEDS` (or `SEED_FILE`) to the resulting file. This overrides
`[game].cycle_seed_file`. It is a build input, not a save or RAM image.

CMake reconfigures when the ROM, configuration, seed file, host compiler or
code-generator sources change. Each input revision gets its own generated
directory, preventing obsolete bank translation units from entering the
target. Prior revisions remain in the build directory and can be discarded
with that build directory. A missing configured seed file is an error.
Generation logs and the selected source list are under `cycle-<target>`.

`tools/cyc/test_cyc_project.py` checks the integration with an original ROM,
including rebuilds after ROM/config/seed edits, paths containing spaces and
`#`, native coverage, wrong-ROM rejection and source-tree isolation.
`tools/cyc/test_cyc_owner_roms.py` separately compares a supplied local ROM
inventory across native, embedded interpreter, standalone interpreter and
reference core at every CPU/PPU alignment. Its `--resume` option reuses only
completed runs with matching input, executable and output digests.

## Current migration boundary

This provides cartridge hardware, cycle timing, the cycle host's input/audio/
video options and persistent cartridge saves. Legacy HD rendering, custom
`extras.c` hooks, recomp-ui, Lua/mod interfaces, netplay and old save-state
formats use the older runtime APIs and are not automatically ported by this
build switch. A game's native enhancements need explicit cycle-host ports.
The older scanline runtime still has its original mapper set. Its renderer
does not emit the PPU bus events needed for accurate MMC2/MMC4/MMC5 behavior;
the integration therefore runs the validated cycle implementation directly.
