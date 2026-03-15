# Super Mario Bros. — Known Issues

---

## ISSUE #1 — Black screen: nametable never written with tile data

**Status:** OPEN — active investigation

### Symptom
After 300 frames the screen is completely black. PPUMASK=$1E is correctly set from
frame 26 onward (rendering enabled). The game does not hang — NMI fires at 60Hz,
submode advances 0→1→2→3 by frame 26, and the PPU upload queue processes palette data
correctly. However, no tile data is ever written to the nametable.

### What we know

**PPU trace (`C:/temp/ppu_trace.csv`) confirms:**
- Frame 0: initial nametable clear — $2006=$24/$00 + $2007=$24 × ~1024 (all blank $24)
- Frame 27+: palette writes only — $3F10 (sprite palette 0) and $3F0C (BG palette 3)
- **Zero nametable ($20xx) writes after the initial blank fill**

**Runtime log (`smb_stdout.txt`) confirms:**
- $0772 submode: 0 → 1 → 2 → 3 (frame 26) — game state machine runs correctly
- $0756: stays **0** for all 300 frames
- $0301 (PPU upload queue): oscillates $3F → $00 every ~8 frames from frame 27 onward
  (confirming PPU upload runs but only processes palette entries)

**PPU upload mechanism confirmed working:**
- NMI handler reads slot index `$0773` → ROM pointer tables `$805A[X]`/`$806D[X]` → ZP `$00/$01`
- `func_8EDD_b0` / `func_8E92_b0` correctly write `$2006`/`$2006`/`$2007` to PPU
- Only palette data (slot 0 → `$0301` = `$3F`) is ever uploaded; no nametable slots are activated

### Active leads

**Lead A — `func_858B_b0` (submode 1 handler) mode=0 shortcut:**
`func_858B_b0` handles submode 1. For `mode=0` (`$0770==0`), it jumps directly to
`label_85C8 → func_8745_b0`, **skipping** the block at `label_85BF` that reads `$074E`
and sets `$0773` to point to ROM tile upload slots (slots 1–5). This means during
submode 1 with mode=0, the PPU upload slot pointer is never advanced beyond slot 0
(palette only). Unknown whether this is intentional or a recompiler bug.

**Lead B — `$0756` never becomes >= 2:**
`func_B624_b0` (called from `func_AEEA_b0` each submode-3 frame) gates its title
screen drawing block behind `$0756 >= 2`. `$0756` stays 0 for all 300 frames.
This may block title screen tile data from ever being queued for upload.

**Lead C — `$0773` slot pointer never advances:**
`$0773` is always reset to 0 at the end of NMI, and nothing during gameplay sets it
to a non-zero ROM slot. Need to confirm whether `$0773` ever becomes non-zero in
a real SMB run (Mesen comparison).

### Next steps
1. Add `log_on_change("$0773", g_ram[0x773])` and `log_on_change("$074E", g_ram[0x74E])`
   to `extras.c`, rebuild, re-run — confirm whether $0773 ever advances
2. Compare with Mesen: what does `$0756` contain after 300 frames in real SMB?
3. Ghidra `func_858B_b0` context more carefully — is the mode=0 path intentionally
   skipping nametable upload, or is a function call missing?

### ROM / config facts
- SMB ROM: Mapper 0 (NROM-256), 2 PRG banks × 16KB, 1 CHR bank × 8KB
- Ghidra server: `mcp__ghidra_smb__*` (bank0.bin loaded, base $8000)
- Runner build: `build/runner_smb/Release/NESRecompGame.exe`
- Test script: `C:/temp/smb_test.txt` (WAIT 300 / SCREENSHOT / EXIT 0)
- stdout: `C:/temp/smb_stdout.txt`
- Screenshot at 300 frames: `C:/temp/smb_shot_300.png`

---
