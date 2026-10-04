# Using the cycle backend in an existing game project

MMC3 projects can opt into extra CPU time with `cyc_set_extra_scanlines(128)`
between frames, or with the host's `--extra-scanlines 128` option. Zero restores
stock timing. This enhancement adds blank scanlines before NMI and pauses APU
clocks during those lines; the window continues at the normal frame rate. Keep
the setting visible and reversible in a game's menu. Stock hardware parity is
tested with zero extra lines; enhanced runs compare native and interpreted CPU
execution instead. Other boards currently reject nonzero values.

Cycle save states now write version 2 with the enhancement's selection and
progress. The reader accepts version 1 states with the same program and hardware
layout, restoring stock timing. Version 1 readers reject version 2 files.

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
`RECOMPILER /path/to/NESRecomp`, `SEED_FILE /path/to/seeds.txt` and
`CAPTURE_FILE /path/to/captures.txt`. Relative
paths are relative to the calling CMake source directory; a seed path inside
`game.toml` is relative to that configuration file. `GAME_CONFIG` is optional.
Existing legacy-generated C and `extras.c` are not cycle-backend inputs.

`PLAYERS 1` or `PLAYERS 2` sets the launcher's controller count for the title;
omitting it retains two controllers. Keep an explicit `legacy` selection in
projects migrating their default build, with separate build directories for
the two backends. Enhancements using legacy runtime globals need a cycle
adapter before the project's migration is complete.

Windowed cartridge games with battery storage automatically load and atomically
save `<executable dir>/saves/<ROM stem>.sav`. Headless runs persist only with
`--save-file FILE`; `--no-save` disables loading and writing. A damaged save is
refused before execution, and a save path pointing at the ROM is refused.
Cartridge save-state shortcuts use `.cycstate`, leaving previous `.state` files
available. Save states from the legacy CPU backend are incompatible; raw battery
RAM can be copied after checking its size. FDS state-slot paths keep `.state`.

For owner playtests, `--pause-unfocused` pauses the window while it lacks
keyboard focus. This allows several separately built titles to remain open.
The option is off by default; hidden automated windows keep running.

## Famicom Disk System titles

Point `ROM` (or `NESRECOMP_ROM`) at the `.fds`/`.qd` image. The BIOS comes from
`BIOS` (`NESRECOMP_FDS_BIOS`), else `game.toml` `[fds] bios`, else
`bios/disksys.rom` beside the image; configure fails unless it matches the
identity in its `.toml` (`bios/disksys.toml`: size, CRC32, SHA-1). The BIOS
and its identity file are configure dependencies. The built program runs the
image and BIOS named at configure time when started without arguments; any
image and `--fds-bios` can be given on its command line. See the FDS section
of [README.md](README.md) for the host's disk options.

```cmake
nesrecomp_add_cycle_game(MyFdsGame
    ROM "${CMAKE_SOURCE_DIR}/Game.fds"
    GAME_CONFIG "${CMAKE_SOURCE_DIR}/game.toml")   # [game] fds = true, [fds] bios = "bios/disksys.rom"
```

## The window: recomp-ui, dev builds, game additions

A windowed build (not `HEADLESS`, SDL2 found) takes recomp-ui's launcher and
in-game menu from the game's `recomp-ui/` submodule when that checkout has
what the cycle host needs (the runtime toast and the ROM-picker override);
`-DNESRECOMP_RECOMP_UI=<checkout>` names another, `OFF` builds without it,
and `NO_RECOMP_UI` opts one target out. An older submodule builds without it
and says so. `BOXART file.tga` gives the launcher its art. Settings and every
binding live in config.ini beside the executable
([README.md, The window](README.md#the-window)).

`-DNESRECOMP_DEV_UI=ON` builds the developer surface (the FDS drive bar, the
F1-F4 and F6+ keys, coverage in the title bar); release builds leave it off.

`HOST_EXTRAS <sources>` adds a game's own code: `cyc_host_extras()`
(`cyc_host_extras.h`) can present a wider picture (widescreen), offer view
modes to the menu, add menu rows, keep its own config.ini `[Game]` keys, run
per-frame work, take developer options and add TCP commands. Headless builds
compile it too.

`MODS GAME_ID <id>` builds the mod runtime in (packages beside the executable
in `mods/`, the launcher's Mods screen, the runtime menu's rows, mod records in
save states); game.toml `[[mod_function_hook]]` sites give its plugins hooks
into the program. See [README.md, Game mods](README.md#game-mods). The
project needs `CXX` among its languages.

## Audio output stage

`game.toml` `[game] console = "nes"` or `"famicom"` compiles in the console
whose analog output stage the audio goes through (see the APU section of
[README.md](README.md)); `"default"` or no key picks by board (famicom for the
FDS, Namco 163, VRC6 and VRC7, nes otherwise). The program's `--console`
option overrides it. Any other value fails code generation.

## Native coverage and rebuilding

Unseen code runs through the cycle interpreter. Profile a route with
`nes_game ROM --frames 3000 --input route.txt --miss-log seeds.txt`, then set
`NESRECOMP_CYCLE_SEEDS` (or `SEED_FILE`) to the resulting file. This overrides
`[game].cycle_seed_file`. It is a build input, not a save or RAM image.

Code in RAM that no compiled view covered (copied or generated code, disk code
reached only through indirect jumps, code the program rewrites) is captured the
same way: `nes_game ROM --frames 3000 --input route.txt --capture-log
captures.txt`, then `NESRECOMP_CYCLE_CAPTURES` (or `CAPTURE_FILE`), which
overrides `[game].cycle_capture_file`. See [Code in RAM](README.md#code-in-ram).

CMake reconfigures when the ROM, configuration, seed or capture file, host compiler or
code-generator sources change. Each input revision gets its own generated
directory, preventing obsolete bank translation units from entering the
target. Prior revisions remain in the build directory and can be discarded
with that build directory. A missing configured seed or capture file is an error.
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
video options, persistent cartridge saves, recomp-ui's launcher and runtime
menu with host-owned bindings (above), save states, and game mods (packages,
hook sites, isolated calls, custom renderer). Legacy HD rendering, custom
`extras.c` hooks, Lua interfaces, netplay and the older runtime's save-state
format use the older runtime APIs and are not automatically ported by this
build switch; a game's additions go through `HOST_EXTRAS` instead of
`extras.c`.
The older scanline runtime still has its original mapper set. Its renderer
does not emit the PPU bus events needed for accurate MMC2/MMC4/MMC5 behavior;
the integration therefore runs the validated cycle implementation directly.

## NTSC and PAL regions

`REGION NTSC` or `REGION PAL` on `nesrecomp_add_cycle_game` selects the
title's hardware timing. The host's `--region ntsc|pal` overrides it. Without
a project or CLI choice, a NES 2.0 PAL header selects PAL; other headers use
NTSC because old dumps often omit region. FDS remains NTSC only; Dendy and
arcade console variants remain unsupported.

PAL runs the 2A07 CPU at 26.6017125 MHz / 16, with the 2C07 PPU at / 5:
16 PPU dots per five CPU cycles, 341 dots per line, 312 lines and about
50.007 frames per second. It has no odd-frame skipped dot. The APU uses PAL
frame-sequencer, noise and DMC tables, and audio sampling and host pacing use
the PAL clock. DMA begins at an opcode fetch on the 2A07; OAM refresh starts
on line 265. Red and green emphasis bits swap meaning on the 2C07.
Sources: [NESdev clock chart](https://www.nesdev.org/wiki/Cycle_reference_chart),
[CPU variants](https://www.nesdev.org/wiki/CPU_variants),
[PPU registers](https://www.nesdev.org/wiki/PPU_registers), and
[Mesen2 NES implementation](https://github.com/SourMesen/Mesen2/tree/master/Core/NES).

PAL states use version 4 and preserve the /5 divider phase. Loading a state
from another region is refused before changing the machine. NTSC writes
remain version 2 (version 3 with a Zapper), and same-layout NTSC versions
1 through 3 remain readable. In-memory isolated-call snapshots also preserve
the region phase.

Validation: the clock, rendered frame length, APU sequence, opcode-only DMA,
audio duration, and corrupt/cross-region state contracts have a CTest target,
`cyc_region_test`. The ten blargg PAL APU ROMs, which were tested on a PAL NES,
all report PASSED. European Dr. Mario native/interpreter execution matches
for 1,800 frames at all five PAL alignments and state continuation is exact.
Its RAM matches independent Mesen frame-for-frame after startup, and sampled
PPU color indices match exactly after the game initializes its palette.
Startup comparison records one transient stack flags byte at frame 8; initial
palette contents differ before initialization. These comparisons cover the
tested route, rather than every 2C07 register quirk or every PAL game.
The seven migrated NTSC game routes retained their complete prior 1,800-frame
trace, memory, CPU and hardware hashes.

## Password saves and content tools

`PASSWORD_SAVE "faxanadu.srm" PASSWORD_SAVE_LABEL "Mantra"` on
`nesrecomp_add_cycle_game` supplies the launcher's existing password editor.
The file lives beside the executable, even when `--config` points elsewhere.
Games implement their own encoder, text-file persistence and restore path in
`HOST_EXTRAS`. `cyc_host_saves_enabled()` exposes `--no-save` before the launcher
and game callbacks run; a game must honor it for sidecar loading and writing.
The password editor is also omitted with `--no-save`.

Trusted text tools can edit the in-memory PRG with `cyc_mod_prg_data_rw`.
This disables generated dispatch until another image loads, because its
folded constants would otherwise retain the original bytes. Execution still
uses the cycle CPU interpreter; disk ROM files are unchanged. Save states
retain their PRG hash identity and refuse a different patch set.

`cyc_mod_set_ppu_write_hook` observes completed CPU `$2006` addresses and CPU
`$2007` writes for CHR transfer tracking. Isolated calls suppress these hooks.
`cyc_mod_chr_poke` updates mapped CHR RAM and invalidates rendering caches;
it refuses CHR ROM, out-of-range addresses and isolated calls. The shared
`override_chr.c` tool supports this path when built with `CYC_CONTENT_TOOLS`.
`cyc_content_test` checks memory, isolation and hook contracts. Faxanadu runtime
checks cover native/interpreter/reference parity, complete machine restoration
after mantra capture, accepted password restore, persistence/history, no-save,
text replacement, tile round trips, visible tile overrides and PNG compilation.
