# nesrecomp tools

Reusable utilities for recompiling NES ROMs.

## Symbol extraction from ca65 disassemblies

Many community disassemblies use the ca65/ld65 toolchain (part of cc65).
These emit per-label address mappings via `ld65 -Ln <file>`, which produce
an FCEUX/Nintendulator NL-format `.lbl`:

```
al 00XXXX .LabelName
```

`build_symbols_from_lbl.py` converts these into the nesrecomp `symbols.sym`
format that the recompiler reads via `symbol_file` in `game.toml`.

### End-to-end workflow

```bash
# 1. Clone a ca65 disassembly (Zelda shown; substitute SMB/Metroid/etc.)
git clone https://github.com/aldonunez/zelda1-disassembly
cd zelda1-disassembly

# 2. If the disasm uses .INCBIN with a bins.xml manifest, extract the .dat
#    blobs from the original ROM.  (Skip this step for disasms that inline
#    all data in the .asm source.)
python /path/to/nesrecomp/tools/extract_bins_from_xml.py \
    --rom "../Original.nes" \
    --xml src/bins.xml

# 3. Assemble with debug info and link with label output.  The -g / -Ln
#    flags are what make the complete label table available.
ca65 -g --debug-info src/Z_00.asm -o obj/Z_00.o --bin-include-dir . -I src
# ... (repeat per .asm file or use the project's Makefile/build script)
ld65 -C src/Z.cfg -Ln zelda.lbl --dbgfile zelda.dbg -o zelda.bin obj/*.o

# 4. Convert to nesrecomp format
python /path/to/nesrecomp/tools/build_symbols_from_lbl.py \
    zelda.lbl \
    /path/to/my-game-project/symbols.sym \
    --header "The Legend of Zelda (NES)" \
    --source "github.com/aldonunez/zelda1-disassembly" \
    --verbose

# 5. Wire into your game.toml
#    [game]
#    symbol_file = "symbols.sym"

# 6. Regenerate
/path/to/nesrecomp/build/recompiler/Release/NESRecomp.exe my-game.nes
```

### Address collisions (multi-bank ROMs)

nesrecomp's `symbol_lookup` keys on the 16-bit 6502 address alone — it does
not differentiate by bank. When a multi-bank ROM has labels at the same
runtime address in different banks, the extractor's default behavior is to
keep the longest name (usually the most descriptive). Cheap-local `@`-labels
from ca65 are dropped.

For a single-bank NROM game (like SMB), collisions are rare or zero. For
multi-bank MMC1/MMC3 games with many banks loaded at `$8000-$BFFF`, expect
dozens to hundreds of collisions — but the unique symbols still cover the
vast majority of function addresses.

## Other tools

- `check_ppu.py` — PPU state inspector over TCP
- `mesen_mcp.lua` / `mesen_mcp_server.py` — MCP bridge for Mesen emulator
- `parse_trace.py` — parse recompiler trace output
