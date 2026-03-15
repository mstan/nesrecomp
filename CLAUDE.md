# NESRecomp Framework

**What this is**: A static 6502 recompiler framework — NES ROM → C → native x64.
This is NOT an NES emulator. We translate 6502 machine code to C functions, compile them.

**Game repos** consume this as a git submodule:
- [FaxanaduRecomp](../FaxanaduRecomp/) — Faxanadu (Mapper 1/MMC1) — **use as boilerplate for new games**
- [SuperMarioBrosRecomp](../SuperMarioBrosRecomp/) — Super Mario Bros. (Mapper 0)

See `runner/runner.cmake` for how game projects consume the runner source list.
When starting a new game, clone FaxanaduRecomp as a template — it has the simplest
working `CMakeLists.txt`, `game.cfg`, and `extras.c` to copy from.

---

## ██████████████████████████████████████████████████
## ██  RULE 0: NO GHIDRA = NO ACTION. FULL STOP.  ██
## ██████████████████████████████████████████████████

At the start of EVERY session, before touching ANY file:

Call `mcp__ghidra__get_program_info`. If it does not respond:

> GHIDRA IS NOT RUNNING.
> I will not read files, write code, or make any suggestions.
> Load the game's fixed bank into Ghidra as Raw Binary, 6502 processor.
> Start the Ghidra MCP server, reconnect with /mcp, then try again.

This rule has NO exceptions. No guessing 6502 behavior. No action until Ghidra responds.

See `EXTRACTION.md` for bank extraction procedures per game.

---

## ████████████████████████████████████████████████████████████████
## ██  RULE 1: ALWAYS FIX THE TOOL, NEVER THE OUTPUT. ALWAYS.  ██
## ████████████████████████████████████████████████████████████████

`generated/*_full.c` and `generated/*_dispatch.c` are BUILD ARTIFACTS.

**NEVER read them whole. NEVER modify them. NEVER patch them.**

If generated code is wrong → fix `recompiler/src/code_generator.c` and regenerate.
If runtime behavior is wrong → fix `runner/src/*.c` in nesrecomp.
If a function is missing → add `extra_func` to the game's `game.cfg` and regenerate.

**The recompiler and runner are the source of truth.** Game repos ONLY contain:
- `game.cfg` (recompiler config)
- `extras.c` (game-specific hooks)
- `generated/` (regenerated, never hand-edited)

Any bug fix that improves the framework belongs in nesrecomp, not in a game repo.
Grep-and-sed on generated files is NEVER acceptable — it will be overwritten on
the next regeneration and the fix will be silently lost.

---

## ████████████████████████████████████████████████████████
## ██  RULE 2: CHECK PATTERNS.md BEFORE ANY GHIDRA WORK  ██
## ████████████████████████████████████████████████████████

Before implementing ANY function discovered via Ghidra tracing, read `PATTERNS.md`.

If a function does ANY of these, stop and check PATTERNS.md:
- PLA/PHA that touches the return address
- RTS used as a computed goto (jump through stack-stored address)
- Inline data bytes immediately following a JSR
- A function whose body reads the 6502 stack to find out who called it

---

## The Loop

```
1. BUILD recompiler     →  NESRecomp.exe  (only when recompiler src changes)
2. RUN recompiler       →  generates <game>_full.c in game project's generated/
3. BUILD game project   →  GameName.exe  (after runner or game changes)
4. RUN game (timed)     →  start, wait 10s, kill
5. OBSERVE screenshot   →  Read C:/temp/nes_shot_01.png  (saved every 60 NES frames)
6. IDENTIFY bug         →  wrong pixels → ppu_renderer.c;  crash → Ghidra
7. GHIDRA if needed     →  understand what the 6502 code actually does
8. FIX the bug          →  runtime.c / ppu_renderer.c / code_generator.c
9. GOTO 1 (or 3 if only runner changed)
```

## Debugging Hierarchy

**Step 1 — Ghidra any unknown address immediately.**
Call `mcp__ghidra__get_code` before reading source or adding any printf.

**Step 2 — Check PPU register trace.**
`C:/temp/ppu_trace.csv` — every $2000-$2007 write. Format: `W,$2006,$20,PC=?,F=5`
`C:/temp/mapper_trace.csv` — every bank switch. Format: `BANK_SWITCH,bank=3,PC=?,F=12`

**Step 3 — Add a targeted debug log.**
`debug_log_frame()` in main_runner.c logs: frame, bank, key RAM bytes per VBlank.
**ALWAYS remove debug log writes after the investigation.**

Use `log_on_change()` in runtime.c for tracking a single RAM value without flood output.

Session resume after context clear: **say "Run the game."** Screenshot + Ghidra = source of truth.

---

## How to Add a New Game

**Quickstart:** Copy [FaxanaduRecomp](../FaxanaduRecomp/) as a boilerplate. It has
the minimal working set of files: `CMakeLists.txt`, `game.cfg`, `extras.c`, and the
nesrecomp submodule already wired up. Rename, update the CRC32 and game name in
`extras.c`, adjust `game.cfg` for your ROM's mapper/dispatch, and regenerate.

**From scratch:**

1. Create `game.cfg` — tells recompiler the ROM layout, mapper, dispatch idioms.
2. Create `extras.c` / `extras.h` implementing `game_extras.h` (see `runner/include/game_extras.h`).
3. Create a game repo directory with `CMakeLists.txt` using `runner.cmake`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyGameRecomp C)

set(NESRECOMP_ROOT ${CMAKE_SOURCE_DIR}/nesrecomp)
include(${NESRECOMP_ROOT}/runner/runner.cmake)

list(APPEND CMAKE_PREFIX_PATH "${NESRECOMP_ROOT}/runner/external/SDL2/cmake")
find_package(SDL2 REQUIRED CONFIG)

add_executable(MyGameRecomp
    ${NESRECOMP_RUNNER_SOURCES}
    extras.c
    generated/mygame_full.c
    generated/mygame_dispatch.c
)
target_include_directories(MyGameRecomp PRIVATE
    ${NESRECOMP_RUNNER_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
)
target_link_libraries(MyGameRecomp SDL2::SDL2)
```

4. Add `nesrecomp` as a git submodule in the game repo:
```bash
git submodule add <nesrecomp-repo-url> nesrecomp
git submodule update --init
```

5. Run `NESRecomp.exe <rom.nes>` from the game repo to generate `generated/<game>_full.c`.
6. Build and run.

---

## Input Scripts and Save States

### Running with a script
```batch
GameRecomp.exe rom.nes --script C:/temp/session.txt > C:/temp/stdout.txt 2>&1
```

### Script command reference
| Command | Description |
|---------|-------------|
| `WAIT <n>` | Wait n frames |
| `HOLD <BTN>` | Hold button (A B SELECT START UP DOWN LEFT RIGHT) |
| `RELEASE <BTN>` | Release button |
| `TURBO ON\|OFF` | Toggle fast-forward |
| `SCREENSHOT [file]` | Save PNG to C:/temp/ |
| `LOG <msg>` | Print message to stdout |
| `SAVE_STATE <path>` | Save state to file |
| `LOAD_STATE <path>` | Restore state from file |
| `WAIT_RAM8 <hex_addr> <hex_val>` | Block until g_ram[addr]==val (30s timeout) |
| `ASSERT_RAM8 <hex_addr> <hex_val> [msg]` | Assert RAM value |
| `EXIT [code]` | Exit with code (default 0) |

### Save state hotkeys (in-game)
| Key | Action |
|-----|--------|
| F5  | Toggle turbo (fast-forward) |
| F6  | Save state → `C:/temp/quicksave.sav` |
| F7  | Load state ← `C:/temp/quicksave.sav` |

---

## Build Commands

```batch
# Build recompiler (after code_generator.c changes)
cmake --build F:/Projects/nesrecomp/build/recompiler --config Release

# Regenerate game code (run from game project directory)
F:/Projects/nesrecomp/build/recompiler/Release/NESRecomp.exe <rom.nes>

# Build a game project (from game project directory)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

---

## Key Files

| File | Purpose | Edit? |
|------|---------|-------|
| `recompiler/src/code_generator.c` | 6502→C emitter — THE PRODUCT | Yes |
| `recompiler/src/function_finder.c` | JSR/RTS boundary detection | Yes if needed |
| `recompiler/src/game_config.c` | game.cfg parser | Yes if needed |
| `runner/src/runtime.c` | NES memory map, PPU reg stubs | Yes |
| `runner/src/ppu_renderer.c` | Tiles, palettes, BG, sprites | Yes |
| `runner/src/main_runner.c` | SDL2 window, NMI loop, frame timing | Yes |
| `runner/src/launcher.c` | ROM discovery, CRC32 verify, main() | Yes |
| `runner/runner.cmake` | Source list for game project CMakeLists | Yes |
| `runner/include/game_extras.h` | Per-game hook interface | Yes |
| `PATTERNS.md` | 6502 dispatch idioms | Reference |
| `EXTRACTION.md` | Bank extraction procedures | Reference |

---

## Log File Rule

Every .c file implementing hardware behavior gets a sibling .log:
`runtime.c` → `runtime.log`, `ppu_renderer.c` → `ppu_renderer.log`

Format:
```
[function_name or NES address]
Ghidra: <what the decompiler/disassembler showed>
Rationale: <why implemented this way>
```

---

## Architecture Notes

**Static recompiler.** 6502 binary → C → native x64. No interpreter loop.
**JSR = direct C function call.** `func_C123()` calls `func_C456()` directly.
**NMI is the frame driver.** Runner calls func_NMI() at 60Hz wall-clock.
**runtime.c starts minimal.** Implement stubs only as the game calls them. Ghidra first.
**6502 stack is real RAM.** Stack at g_ram[0x100 + S]. JSR/RTS manipulate g_cpu.S.
**ROM picker built-in.** launcher.c opens a file dialog if no ROM is provided on CLI.

---

## Visual Debugging

Screenshots auto-saved as PNG every 60 NES frames, named by frame number.

| File | Contents |
|------|----------|
| `C:/temp/nes_shot_XXXX.png` | Screenshot at frame XXXX |
| `C:/temp/ppu_trace.csv` | PPU register writes: W,ADDR,VALUE,PC,FRAME |
| `C:/temp/mapper_trace.csv` | Mapper bank switches: BANK_SWITCH,bank,PC,FRAME |
| `C:/temp/quicksave.sav` | F6 quick-save slot |

Screenshots are PNG. **BMP is prohibited** — too large for token limits.

---

## What NOT to Do

- Do not pre-emptively implement PPU features "just in case"
- Do not read `*_full.c` whole for "context"
- Do not guess what a function does — Ghidra it
- Do not add exhaustive printf traces — use log_on_change() for one targeted value at a time
