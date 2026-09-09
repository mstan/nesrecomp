# Symbol tables (`symbol_file`)

A game may point `game.toml` at a symbol table so generated code carries the
names from a disassembly instead of bare `func_XXXX` identifiers:

```toml
[game]
symbol_file = "symbols.sym"     # resolved relative to game.toml
```

The recompiler emits, for every named address, a `/* Name */` comment on the
function/label and a `#define Name func_XXXX[_bN]` alias in
`<prefix>_full_decls.h`; `ram` and `const` names become `#define Name 0xADDR`.
Generated machine code is unchanged -- the overlay is comments and aliases only,
so a regen with and without a symbol file must produce identical object code.

## Format

```
# comment
XXXX Name              # bankless: applies to every bank (fixed-bank code, RAM, MMIO)
XXXX Name func|ram|const|label
BB:XXXX Name func      # bank-scoped: BB = hex PRG bank the name belongs to
```

On banked mappers (MMC1, MMC3, ...) `$8000-$BFFF` holds different code per
bank, so a bankless name there is ambiguous. Bank-scoped entries resolve per
function: `symbol_lookup_bank(addr, bank)` prefers an exact-bank name, then a
bankless one, and never borrows a name scoped to another bank.

Alias emission rules (`emit_symbol_aliases`, `code_generator.c`):

- a name used by exactly one bank at an address: `#define Name func_XXXX_bN`
- a name shared by several banks (shared engine code assembled into every
  bank): only `Name__bN` aliases, plus a warning -- an unsuffixed alias would
  silently bind to whichever bank's define came last
- banks without a name at that address get no alias

`ram` vs `const` matters: a RAM location and an unrelated id can share a
number (SMB `$33` is a zero-page variable and an enemy id). A `const` is a
value, never an address.

## Producers

- SMB: `symbols.sym` from the ca65 disassembly (`ca65 -g` + `ld65 --dbgfile`).
- Metroid: `tools/gen_symbols.py` in the game repo converts the WLA-DX `.sym`
  written by `wlalink -S` when the `disasm/m1disasm` submodule is assembled
  (bank-scoped labels, RAM from `constants_ram.asm`, everything else `const`).
