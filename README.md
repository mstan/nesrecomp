<p align="center">
  <img src="docs/assets/nesrecomp-logo.png" alt="NESRecomp" width="640">
</p>

# NESRecomp

A static 6502 recompiler framework for NES games. Translates NES ROM machine code to C, which is then compiled to native machine code for direct execution on modern PCs.

**This is NOT an emulator.** Each 6502 instruction is translated to equivalent C code at build time. JSR becomes a direct C function call, branches become gotos, and the NES hardware (PPU, APU, mapper) is simulated by the runner library.

## Platform Support

| Platform | Status |
|----------|--------|
| Windows (x64, MSVC) | Primary / mature |
| macOS (Apple Silicon + Intel) | **Experimental — newly added** |
| Other UNIX (Linux) | Likely works via the same POSIX path; less tested |

macOS support is recent and should be considered experimental. The toolchain
(recompiler + runner) builds cleanly with Apple Clang and games run natively —
The Legend of Zelda, for example, plays well. Individual titles may show minor
behavioral quirks that don't appear on Windows (Super Mario Bros. has some known
timing/demo differences); please file an issue if you hit one. The macOS/POSIX
port was contributed by [**Nat Budin (@nbudin)**](https://github.com/nbudin) in
[#10](https://github.com/mstan/nesrecomp/pull/10) — thank you! 🙏

## Game Projects

| Game | Mapper | Status | Repository |
|------|--------|--------|------------|
| Super Mario Bros. | NROM (0) | Fully playable | [SuperMarioBrosNESRecomp](https://github.com/mstan/SuperMarioBrosNESRecomp) |
| Duck Hunt | NROM (0) | Fully playable (mouse-as-Zapper) | [DuckHuntNESRecomp](https://github.com/mstan/DuckHuntNESRecomp) |
| Dr. Mario | MMC1 (1) | Playable (1P tested) | [DrMarioNesRecomp](https://github.com/mstan/DrMarioNesRecomp) |
| The Legend of Zelda | MMC1 (1) | Believed 100% playable | [LegendOfZeldaNESRecomp](https://github.com/mstan/LegendOfZeldaNESRecomp) |
| Metroid | MMC1 (1) | Starting area playable, early foundation | [MetroidNESRecomp](https://github.com/mstan/MetroidNESRecomp) |
| Faxanadu | MMC1 (1) | Fully playable, text override showcase | [FaxanaduRecomp](https://github.com/mstan/FaxanaduRecomp) |
| Yoshi | MMC1 (1) | Believed fully playable | [YoshiNESRecomp](https://github.com/mstan/YoshiNESRecomp) |
| Yoshi's Cookie | MMC3 (4) | Believed 100% playable | [YoshisCookieRecomp](https://github.com/mstan/YoshisCookieRecomp) |
| Mega Man 3 | MMC3 (4) | Work in progress — title/menu/early stages playable | [Megaman3NESRecomp](https://github.com/mstan/Megaman3NESRecomp) |
| Gumshoe | GxROM (66) | Playable end-to-end (mouse-as-Zapper, one HUD cosmetic bug) | [GumshoeNESRecomp](https://github.com/mstan/GumshoeNESRecomp) |

### Mapper Support

| Mapper | Name | Supported | Games |
|--------|------|-----------|-------|
| 0 | NROM | Yes | Super Mario Bros., Duck Hunt |
| 1 | MMC1 / SxROM | Yes | Zelda, Metroid, Dr. Mario, Faxanadu, Yoshi |
| 2 | UxROM | Not yet | — |
| 3 | CNROM | Not yet | — |
| 4 | MMC3 / TxROM | Yes | Yoshi's Cookie, Mega Man 3 |
| 7 | AxROM | Not yet | — |
| 9 | MMC2 / PxROM | Not yet | — |
| 66 | GxROM | Yes | Gumshoe |

Mappers 0, 1, 4, and 66 cover roughly 78% of the licensed NES library.

### Text Override System

NESRecomp includes a runtime text replacement system that allows modifying in-game
text without editing the ROM. The runner exposes writable PRG ROM bank accessors
(`runner_get_prg_bank_rw()`) that game plugins use to patch string data at load time.

[FaxanaduRecomp](https://github.com/mstan/FaxanaduRecomp) is the first game to
exercise this feature, implementing a JSON-driven override system with:
- Multiple encoding registries (ASCII, tile-based dialogue fonts)
- PRG ROM patching for all rendering paths (direct PPU writes, DMA queues)
- Hot-reload — edit `text_overrides.json` while the game is running and changes
  apply within ~1 second

This enables localization, retranslation, and accessibility improvements without
ROM hacking. See `override_text.h` in FaxanaduRecomp for the full API.

## Architecture

```
NES ROM (.nes)
    |
    v
NESRecomp.exe + game.toml
    |
    v
generated/<game>_full.c      (recompiled 6502 -> C)
generated/<game>_dispatch.c  (call_by_address runtime dispatch)
    |
    v
Game executable (linked with runner library + SDL2)
```

### Key Components

| Component | Purpose |
|-----------|---------|
| `recompiler/src/code_generator.c` | 6502-to-C emitter |
| `recompiler/src/function_finder.c` | Static analysis: discovers functions via BFS from vectors |
| `runner/src/runtime.c` | NES memory map, PPU register stubs, mapper |
| `runner/src/ppu_renderer.c` | Background + sprite rendering (8x8 and 8x16) |
| `runner/src/main_runner.c` | SDL2 window, NMI loop, frame timing |
| `runner/src/debug_server.c` | TCP debug server with ring buffer |

### Configuration Format

Game configuration is **TOML** (`game.toml`). The legacy plain-text `.cfg` format
has been removed — passing a `.cfg` path now prints a migration message and
exits. All in-tree game projects already use `game.toml`; see Dr. Mario's
`game.toml` or Faxanadu's for working examples.

### Configurable Controls

A `keybinds.ini` file is auto-generated next to the game executable on first run. Both player 1 and player 2 bindings are configurable for **keyboard and gamepad**. Edit the INI file and restart the game to apply changes.

**Keyboard** — `[player1]` / `[player2]` sections map each NES button to an SDL key name.

**Gamepad** — game controllers are supported cross-platform via SDL's
`SDL_GameController` API (Xbox, PlayStation/DualSense, Switch Pro, and generic
pads; on Windows this uses XInput under the hood). The first connected pad is
NES port 1, the second is port 2, and hotplug is handled — plug in or unplug at
any time. Keyboard and gamepad input are merged, so both work simultaneously and
no configuration is required to start playing.

`[gamepad1]` / `[gamepad2]` sections make the mapping fully editable. Each NES
button takes a comma-separated list of SDL controller button names (so multiple
physical buttons can drive one NES button). Defaults:

```ini
[gamepad1]
a = a,b          ; both right-hand face buttons act as NES A
b = x,y          ; both left-hand face buttons act as NES B
select = back
start = start
up = dpup
down = dpdown
left = dpleft
right = dpright
deadzone = 16000 ; left-stick threshold (0-32767)
analog = true    ; left analog stick also drives the d-pad
```

Valid button names: `a b x y back start guide leftshoulder rightshoulder
leftstick rightstick dpup dpdown dpleft dpright` (use `none` to unbind).

### game.toml Directives

The full schema lives in `recompiler/src/game_config.c`; the most commonly used
sections are summarized below.

| Section | Purpose |
|---------|---------|
| `[game]` | Output prefix, symbol file path, and recompiler flags (`push_all_jsr`, `disable_ptr_scan`, ...) |
| `[mapper] bank_switch = [...]` | Addresses of MMC1/mapper bank-switch routines |
| `[[inline_dispatch]] addr` | Indexed dispatch via inline address table after JSR |
| `[[inline_pointer]] addr, zp = [lo, hi], call` | JSR reads 2 inline bytes into zero page; `call` also invokes the resulting target |
| `[[extra_func]] bank, addr` | Force-create a function entry at this address |
| `[[extra_label]] bank, addr` | Secondary entry point within an existing function |
| `[[sram_map]] sram_start, rom_start, bank, size` | SRAM-to-ROM code mapping |
| `[[nop_jsr]] addr` | Skip this JSR entirely (stack-manipulation routines incompatible with recompilation) |
| `[[data_region]] bank, start, end` | Exclude byte range from pointer scanner (known data, not code) |
| `[functions] fixed = [...], bankN = [...]` | Bulk `extra_func` lists keyed by bank |

Numeric values may be given in hex (`0xC000`) or decimal. Address-only directives
are TOML arrays of tables; per-bank lists under `[functions]` accept plain
integer arrays.

### Function Discovery: Table-Run Scanner

The recompiler discovers functions in two phases:

1. **BFS from vectors**: Walk RESET/NMI/IRQ, follow JSR/JMP, discover all statically reachable functions.
2. **Pointer table scanner**: Find dynamically-dispatched functions (enemy AI handlers, state machine callbacks) that are only reachable via indirect jumps at runtime.

The table-run scanner finds pointer tables by looking for runs of 4+ consecutive 16-bit LE values in $8000-$BFFF where each target passes a deep-decode validation (7+ valid 6502 instructions without hitting an illegal opcode, or clean termination via RTS/JMP).

It scans both:
- **Switchable banks** (tables embedded in the same bank as handlers)
- **Fixed bank → switchable** (dispatch tables in the fixed bank pointing to switchable bank handlers)

This catches dispatch patterns that use indirect addressing (`(ZP),Y`, stack-based) where no direct `abs,X`/`abs,Y` code reference to the table exists.

Set `NESRECOMP_LEGACY_FUNCTION_FINDER=1` to disable the newer heuristic discovery passes and fall back to the older BFS-first finder behavior for comparison runs.

**Harmful vs harmless false positives**: The deep-decode check eliminates "harmful" false positives (data misidentified as code, which would generate invalid C). "Harmless" false positives (valid code in another bank's context) are accepted — they generate unused but compilable functions. Adding `data_region` entries for known data areas eliminates remaining edge cases where structured data happens to decode as 7+ valid instructions.

**Expected results** (validated against Zelda with exhaustive disassembly ground truth):
- ~80-93% recall of dispatch table targets, 0 harmful false positives
- Remaining targets (small inline tables, direct-call-only functions) require `extra_func` entries

### extra_func vs extra_label

- **`extra_func`**: Creates a standalone function. Use for addresses NOT inside any existing function.
- **`extra_label`**: Creates a secondary entry point within the parent function that contains this address. The parent emits a `_body(int _entry)` function with switch/goto dispatch. Use for addresses that fall inside existing functions (e.g., SRAM-mapped subroutines within a larger code block).

This distinction is critical when importing complete disassembly function lists. Adding an address as `extra_func` when it's inside an existing function **splits that function**, breaking internal gotos and causing freezes. `extra_label` preserves the parent's control flow while making the address callable from the dispatch table.

### Undocumented 6502 Opcodes

The NMOS 6502 has 256 possible opcodes but only 151 are officially documented. The remaining 105 have deterministic behavior on the physical chip because the internal decode logic combines signals from multiple "official" instruction paths. NESRecomp's decoder (`cpu6502_decoder.c`) recognizes every undocumented opcode with the correct size and cycle count; the code generator emits real semantics for the ones that NES games actually use, and treats the rest as sized NOPs (the instruction stream stays byte-aligned, but side effects are lost).

**Emitted with full semantics:**

| Opcode | Name | Behavior |
|--------|------|----------|
| LAX | Load A+X | `A = X = mem`, sets N/Z. Used by *Elite*, *Super Cars*, etc. |
| SAX | Store A&X | `mem = A & X`. No flags, no register changes. |
| DOP / TOP (NOP*) | 2-/3-byte NOP-with-read | Performs the operand read (so `$2002` PPUSTATUS latch clears correctly) and discards the result. Immediate variants skip the operand byte without a read. |

**Recognized but currently emitted as sized NOPs (skipped, side effects lost):**

DCP, ISC, SLO, RLA, SRE, RRA, ANC, ALR, ARR, AXS, KIL, plus the rarely-seen
SHX/SHY/SHA/TAS/LAS family. A game that depends on, for example, `DCP zp` to
decrement-and-compare in one step will misbehave. None of the games currently
in this project rely on these — when one does, fill in the missing emit logic
in `code_generator.c` alongside LAX/SAX (the decoder already provides the
addressing mode and operand size).

**Dispatch safety:** the function finder rejects candidate function entry
points whose first byte decodes as `MN_ILLEGAL`, so byte values in the illegal
range that appear inside data tables don't get misidentified as code and
executed.

## Building

CMake 3.20+ is required. The recompiler itself is pure C11 with no external
dependencies; game runners additionally need SDL2.

### Windows (Visual Studio 2022)

```bash
# Build the recompiler
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Recompile a game ROM
build/Release/NESRecomp.exe <rom.nes> --game <path/to/game.toml>
```

### macOS / Linux

```bash
# Install prerequisites (macOS / Homebrew shown; use your distro's packages on Linux)
brew install cmake sdl2 ninja

# Build the recompiler
cmake -S recompiler -B build/recompiler -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/recompiler

# Recompile a game ROM (note: no .exe suffix)
build/recompiler/NESRecomp <rom.nes> --game <path/to/game.toml>
```

Game projects build the same way on macOS/Linux — from the game directory, run
its `setup.sh` to fetch the pinned nesrecomp, then:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_NESTOPIA_ORACLE=OFF -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build
```

(`ENABLE_NESTOPIA_ORACLE` is a developer verify-mode feature and is off here for
a plain playable build.)

## Adding a New Game

See [CLAUDE.md](CLAUDE.md) for detailed instructions. In short:

1. Copy [FaxanaduRecomp](https://github.com/mstan/FaxanaduRecomp) as a boilerplate
2. Create `game.toml` with mapper, bank switch, and dispatch configuration
3. Create `extras.c` implementing the `game_extras.h` hook interface
4. Run `NESRecomp.exe <rom.nes> --game game.toml` to generate C code
5. Build with CMake, linking against the runner library and SDL2

## Acknowledgements

- [**Nat Budin (@nbudin)**](https://github.com/nbudin) — macOS / POSIX build
  support ([#10](https://github.com/mstan/nesrecomp/pull/10)): POSIX `ucontext`
  coroutine backend, cross-platform directory creation, and the compiler-flag
  groundwork that made native macOS builds possible.
