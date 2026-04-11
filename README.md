# NESRecomp

A static 6502 recompiler framework for NES games. Translates NES ROM machine code to C, which is then compiled to native x64 for direct execution on modern PCs.

**This is NOT an emulator.** Each 6502 instruction is translated to equivalent C code at build time. JSR becomes a direct C function call, branches become gotos, and the NES hardware (PPU, APU, mapper) is simulated by the runner library.

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

### Mapper Support

| Mapper | Name | Supported | Notable unsupported games |
|--------|------|-----------|---------------------------|
| 0 | NROM | Yes | — |
| 1 | MMC1 / SxROM | Yes | Mega Man 2, Castlevania II, Blaster Master |
| 2 | UxROM | No | Mega Man, Castlevania, Contra, DuckTales |
| 3 | CNROM | No | Gradius, Paperboy, Arkanoid |
| 4 | MMC3 / TxROM | Yes | Mega Man 3–6, Kirby's Adventure, Super Mario Bros. 2/3 |
| 7 | AxROM | No | Battletoads, Marble Madness |
| 9 | MMC2 / PxROM | No | Punch-Out!! |
| 66 | GxROM | No | Super Mario Bros. / Duck Hunt (multicart) |

Mappers 0, 1, and 4 cover roughly 75% of the licensed NES library.

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

### Configuration Formats

Games can use either `.cfg` (legacy text) or `.toml` (recommended) for recompiler configuration. Format is auto-detected by file extension. See Dr. Mario's `game.toml` for an example of the TOML format.

### Configurable Controls

A `keybinds.ini` file is auto-generated next to the game executable on first run. Both player 1 and player 2 keyboard bindings are configurable. Edit the INI file and restart the game to apply changes.

### game.cfg / game.toml Directives

| Directive | Description |
|-----------|-------------|
| `bank_switch <addr>` | MMC1/mapper bank-switch routine address |
| `inline_dispatch <addr>` | Indexed dispatch via inline address table after JSR |
| `sram_map <sram> <rom> <bank> <size>` | SRAM-to-ROM code mapping |
| `extra_func <bank> <addr>` | Force-create a function at this address |
| `extra_label <bank> <addr>` | Secondary entry point within an existing function |
| `inline_pointer <addr> <zp_lo> <zp_hi> [call]` | JSR reads 2 inline bytes into zero page; `call` = also call the function |
| `nop_jsr <addr>` | Skip this JSR entirely (for stack-manipulation routines incompatible with recompilation) |
| `data_region <bank> <start> <end>` | Exclude byte range from pointer scanner (known data, not code) |

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

The NMOS 6502 has 256 possible opcodes but only 151 are officially documented. The remaining 105 have deterministic behavior on the physical chip because the internal decode logic combines signals from multiple "official" instruction paths. Some are genuinely useful:

| Opcode | Name | Behavior |
|--------|------|----------|
| LAX | Load A+X | `LDA addr; LDX addr` in one instruction |
| SAX | Store A&X | `STA addr` with value `A AND X` |
| DCP | Decrement+Compare | `DEC addr; CMP addr` |
| ISC | Increment+Subtract | `INC addr; SBC addr` |
| SLO | Shift-left+OR | `ASL addr; ORA addr` |
| RLA | Rotate-left+AND | `ROL addr; AND addr` |
| SRE | Shift-right+XOR | `LSR addr; EOR addr` |
| RRA | Rotate-right+Add | `ROR addr; ADC addr` |
| ANC | AND+set-carry | `AND #imm` with carry = bit 7 |
| ALR | AND+shift-right | `AND #imm; LSR A` |
| ARR | AND+rotate-right | `AND #imm; ROR A` (with special flag behavior) |
| AXS | A&X minus imm | `X = (A & X) - imm` |
| DOP/TOP | Double/Triple NOP | 2-byte or 3-byte NOPs (skip operand bytes) |

A handful of NES games use these intentionally for speed or code density. Examples include *Elite* (LAX), *Super Cars* (LAX/SAX), and some unlicensed titles.

**Current status in NESRecomp:** The decoder (`cpu6502_decoder.c`) recognizes all undocumented opcodes with correct instruction sizes and cycle counts, but the code generator treats them as sized NOPs — it skips the correct number of bytes without emitting any operation. This means:

- **Instruction stream stays aligned**: the decoder advances by the right number of bytes, so subsequent instructions decode correctly.
- **Side effects are lost**: the actual read-modify-write behavior is not emitted. Games relying on LAX to load two registers, or DCP to decrement-and-compare, will behave incorrectly.
- **Dispatch safety**: the dispatch table (`emit_dispatch`) skips bank variants where >50% of the first 8 instructions are illegal opcodes. This prevents data tables (which frequently contain byte values in the illegal opcode range) from being misidentified as code and executed, which could corrupt RAM.

**If a game uses undocumented opcodes**: implement their semantics in `code_generator.c` alongside the existing official opcodes. The decoder already provides the correct addressing mode and operand size — only the emit logic is missing.

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
