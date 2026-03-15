# Super Mario Bros. — Known Issues

---

## ISSUE #1 — Black screen / CHR ROM zeroed at startup

**Status:** FIXED

**Root cause:** Two bugs combined:
1. `runtime_init()` called `memset(g_chr_ram, 0)` *after* `load_rom()` had already loaded
   the 8KB CHR ROM, destroying it.
2. SMB's RESET routine writes zeros to PPU address `$0000` (CHR space) as standard NES
   init. On hardware with CHR ROM this is a no-op, but our runtime wrote through to
   `g_chr_ram`, destroying it a second time.

**Fix:** Added `g_chr_is_rom` flag (set when `chr_banks > 0` in iNES header).
- `runtime_init()` skips `memset(g_chr_ram)` when `g_chr_is_rom` is set
- `ppu_write_reg($2007)` ignores writes to `$0000-$1FFF` when `g_chr_is_rom` is set

---

## ISSUE #2 — Background color black instead of blue

**Status:** FIXED

**Root cause:** NES PPU palette addresses `$3F10/$3F14/$3F18/$3F1C` physically mirror
`$3F00/$3F04/$3F08/$3F0C` (transparent/backdrop slots). SMB writes `$22` (blue) to
`$3F10`, which on real hardware also sets `$3F00`. Our runtime stored them separately,
so the universal background color stayed `$0F` (black).

**Fix:** In `ppu_write_reg` and `ppu_read_reg`, remap indices `$10/$14/$18/$1C` to
`$00/$04/$08/$0C` before accessing `g_ppu_pal[]`.

---

## ISSUE #3 — HUD flickers

**Status:** OPEN

### Symptom
The score/lives/coins HUD at the top of the screen flickers heavily during gameplay.

### Likely cause
SMB uses a sprite-0 hit split-screen technique: it spins on `$2002` bit 6 to detect
when the raster crosses the HUD boundary, then updates the scroll register to lock the
HUD in place while the play-field scrolls independently. Our sprite-0 hit simulation
(pulse after 3 consecutive reads) is approximate and has no scanline timing, so the
HUD scroll state is captured at the wrong time or jitters frame-to-frame.

### Next steps
- Compare `g_ppuscroll_x_hud` values per-frame against Mesen to see if they match
- May need to tune the sprite-0 hit counter threshold or tie it to the NMI cycle

---

## ISSUE #4 — Enemy hit detection unreliable

**Status:** OPEN

### Symptom
- Walking into Goombas usually deals no damage to Mario
- Occasionally takes damage when standing still near an enemy
- Jumping on enemies phases through (no stomp kill)

### Likely cause
Hit detection in SMB is performed during the main game loop (not NMI), comparing
Mario's bounding box against enemy bounding boxes each frame. Our VBlank simulation
fires NMI based on wall-clock time rather than CPU-cycle count. If the game loop runs
many iterations between NMI callbacks, hit detection may be running at the wrong
cadence, or enemy positions may be updated at a different rate than Mario's position.

Alternatively, this could be a dispatch miss — if an enemy-state or collision function
is not recompiled correctly, it silently returns without processing the hit.

### Next steps
- Check `C:/temp/smb_stdout.txt` for `[Dispatch] MISS` lines during gameplay
- Ghidra the collision check routine to confirm it's being called
- Check `g_miss_unique_addrs` for any missed function calls

---

## ISSUE #5 — Items don't spawn from ? blocks

**Status:** OPEN

### Symptom
Hitting a `?` block does not spawn a mushroom, coin, or other item. The block
animation may play but no item entity appears.

### Likely cause
Item spawning likely goes through the entity state machine (similar to Faxanadu's
entity dispatch). If the spawn function is missing from the recompiled output (dispatch
miss) or the entity slot initialization code has a bug, the item never appears.

### Next steps
- Check for `[Dispatch] MISS` near the moment of hitting a `?` block
- Ghidra the item-spawn routine to find which function is responsible
- Verify the entity table is being populated correctly

---

## ROM / config facts
- SMB ROM: `F:/Projects/nesrecomp/Super Mario Bros. (World).nes`
- Mapper 0 (NROM-256), 2 PRG banks × 16KB, 1 CHR bank × 8KB (CHR ROM, read-only)
- Ghidra server: `mcp__ghidra_smb__*` (bank0.bin loaded, base `$8000`)
- Runner build: `build/runner_smb/Release/NESRecompGame.exe`
- Test script: `C:/temp/smb_test.txt`
