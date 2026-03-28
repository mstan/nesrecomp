# NESRecomp

A static 6502 recompiler framework for NES games. Translates NES ROM machine code to C, which is then compiled to native x64 for direct execution on modern PCs.

**This is NOT an emulator.** Each 6502 instruction is translated to equivalent C code at build time. JSR becomes a direct C function call, branches become gotos, and the NES hardware (PPU, APU, mapper) is simulated by the runner library.

## Game Projects

| Game | Status | Repository |
|------|--------|------------|
| The Legend of Zelda | Believed 100% playable | [LegendOfZeldaNESRecomp](https://github.com/mstan/LegendOfZeldaNESRecomp) |
| Faxanadu | Believed mostly playable, minor bugs | [FaxanaduRecomp](https://github.com/mstan/FaxanaduRecomp) |
| Super Mario Bros. | Believed mostly playable, minor bugs | [SuperMarioBrosNESRecomp](https://github.com/mstan/SuperMarioBrosNESRecomp) |

## Architecture

```
NES ROM (.nes)
    |
    v
NESRecomp.exe + game.cfg
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

### game.cfg Directives

| Directive | Description |
|-----------|-------------|
| `bank_switch <addr>` | MMC1/mapper bank-switch routine address |
| `inline_dispatch <addr>` | Indexed dispatch via inline address table after JSR |
| `sram_map <sram> <rom> <bank> <size>` | SRAM-to-ROM code mapping |
| `extra_func <bank> <addr>` | Force-create a function at this address |
| `extra_label <bank> <addr>` | Secondary entry point within an existing function |

### extra_func vs extra_label

- **`extra_func`**: Creates a standalone function. Use for addresses NOT inside any existing function.
- **`extra_label`**: Creates a secondary entry point within the parent function that contains this address. The parent emits a `_body(int _entry)` function with switch/goto dispatch. Use for addresses that fall inside existing functions (e.g., SRAM-mapped subroutines within a larger code block).

This distinction is critical when importing complete disassembly function lists. Adding an address as `extra_func` when it's inside an existing function **splits that function**, breaking internal gotos and causing freezes. `extra_label` preserves the parent's control flow while making the address callable from the dispatch table.

## Building

Requires Visual Studio 2022 and CMake 3.20+.

```bash
# Build the recompiler
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Recompile a game ROM
build/Release/NESRecomp.exe <rom.nes> --game <path/to/game.cfg>
```

## Adding a New Game

See [CLAUDE.md](CLAUDE.md) for detailed instructions. In short:

1. Copy [FaxanaduRecomp](https://github.com/mstan/FaxanaduRecomp) as a boilerplate
2. Create `game.cfg` with mapper, bank switch, and dispatch configuration
3. Create `extras.c` implementing the `game_extras.h` hook interface
4. Run `NESRecomp.exe <rom.nes> --game game.cfg` to generate C code
5. Build with CMake, linking against the runner library and SDL2
