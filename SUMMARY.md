# NESRecomp — Development Summary

## What This Project Is
Static 6502 recompiler. Faxanadu NES ROM → C → native x64 with SDL2. Not emulation.
Recompiler (NESRecomp.exe) reads baserom.nes → emits faxanadu_full.c.
Runner (NESRecompGame.exe) links that C with runtime.c + ppu_renderer.c.

## Current State
- [ ] Recompiler generates faxanadu_full.c
- [ ] Runner links and opens window
- [ ] NMI handler runs without crash
- [ ] PPU writes in ppu_trace.csv
- [ ] Any pixel visible

## Current Blocker
Nothing yet — project files created, build not attempted.

## ROM Facts
- baserom.nes: 262,160 bytes, iNES, **Mapper 1 (MMC1)** — NOT Mapper 2 (plan had error)
- 16 PRG banks × 16KB, CHR RAM (no CHR ROM — tiles loaded by game)
- Fixed bank 15: file offset 0x3C010, always at NES $C000-$FFFF (MMC1 mode 3)
- Switchable banks 0-14: file_offset = 0x10 + bank*0x4000, mapped to $8000-$BFFF
- Vectors: NMI=$C999, RESET=$C913, IRQ=$C9D5 (confirmed from ROM)
- NMI vector bytes at: $FFFA/$FFFB, RESET at $FFFC/$FFFD, IRQ at $FFFE/$FFFF

## Ghidra Setup
- Ghidra: F:/Software/Ghidra/ghidra_12.0.3_PUBLIC_20260210/ghidra_12.0.3_PUBLIC/
- MCP plugin: INSTALLED (already configured)
- Fixed bank project: bank15.bin, Raw Binary, 6502, base=0xC000
- MCP port: 9090

## Build Quick Reference
  C:/temp/nesrecomp_build_recompiler.bat
  F:/Projects/nesrecomp/build/recompiler/NESRecomp.exe F:/Projects/nesrecomp/baserom.nes
  C:/temp/nesrecomp_build_runner.bat
  powershell -File C:/temp/kill_nes.ps1 && C:/temp/nesrecomp_run_game.bat

## Session History
[empty]

## Next Steps
1. Create all project files (this session)
2. Copy baserom.nes and SDL2 from prior projects
3. Run extraction tools to populate assets/
4. Build recompiler, generate faxanadu_full.c
5. Build runner, open black screen window (Phase 0)
6. Enable NMI, observe first PPU writes (Phase 1)
