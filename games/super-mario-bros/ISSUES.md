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

**Status:** FIXED

### Symptom
Hitting a `?` block did not spawn a mushroom, coin, or other item.

### Root cause
`function_finder.c` inline_dispatch target bank tagging was wrong for NROM-256.
When scanning the fixed bank (bank 1), `switchable_bank == fixed_bank == 1`. The
old code:
```c
int dest_bank = (dest >= 0xC000) ? fixed_bank : switchable_bank;
```
Tagged all `$8000-$BFFF` inline_dispatch targets as bank 1, generating garbage
`func_XXXX_b1` functions decoded from the wrong ROM region. The entity state machine
runs in `$C000+` with `g_current_bank=1`, so `call_by_address(0xBC60)` dispatched to
the garbage `func_BC60_b1` instead of the correct `func_BC60_b0`.

### Fix
`function_finder.c`: changed inline_dispatch bank tagging to match regular JSR logic:
```c
if (dest >= 0xC000) {
    add_function(list, dest, fixed_bank);
} else if (switchable_bank != fixed_bank) {
    add_function(list, dest, switchable_bank);
} else {
    /* Fixed bank scanning switchable region: add for all switchable banks */
    for (int _b = 0; _b < fixed_bank; _b++)
        add_function(list, dest, _b);
}
```
Mushroom now spawns and rises from `?` block correctly (verified by screenshot).

---

## ISSUE #6 — Luigi unable to move (2-player mode) *(lowest priority)*

**Status:** OPEN

### Symptom
When Mario dies and play switches to Luigi, Luigi cannot move.

### Likely causes (two candidates — check the simple one first)

**Candidate A — No player 2 controller bound (likely):**
The runner currently only implements controller 1 (`$4016`). Controller 2 (`$4017`)
always returns `$40` (no buttons pressed). In 2-player alternating mode SMB reads
controller 2 for Luigi's input. If that's the case this is a one-liner fix: map
keyboard or a second button layout to `g_controller2_buttons` and wire it into the
`$4017` read in `runtime.c`.

**Candidate B — Player 2 init bug:**
If the game's player-2 initialization routine has a dispatch miss or uninitialized
state, Luigi may be stuck regardless of controller input.

### Next steps
1. Check `runtime.c` `$4017` handler — confirm it always returns `$40` (no buttons)
2. Add a temporary second controller binding (e.g. WASD + numpad) and test
3. Only if Luigi still can't move after binding → Ghidra the player-switch routine

---

## ISSUE #7 — World 1-2 causes random game over / warp pipe crash

**Status:** OPEN

### Symptom
Entering world 1-2 sometimes causes an immediate game over or a crash. Suspected to
be related to the underground secret warp pipes (world 1-2 contains pipes that warp
to worlds 2, 3, 4).

### Likely cause
The warp pipe mechanic likely triggers a different code path for pipe-entry vs. normal
pipe traversal. If the warp zone entity or the screen-transition routine has a dispatch
miss, the game may jump to an invalid address or corrupt Mario's world/level state,
causing an immediate game over.

### Next steps
- Check for `[Dispatch] MISS` or `[Dispatch] INLINE MISS` when entering the warp pipes
- Ghidra the pipe-entry and world-transition routines
- Verify `g_miss_count_any` is zero through a clean 1-2 run

---

## ROM / config facts
- SMB ROM: `F:/Projects/nesrecomp/Super Mario Bros. (World).nes`
- Mapper 0 (NROM-256), 2 PRG banks × 16KB, 1 CHR bank × 8KB (CHR ROM, read-only)
- Ghidra server: `mcp__ghidra_smb__*` (`smb_prg.bin` loaded, base `$8000`, full 32KB)
- Runner build: `build/runner_smb/Release/NESRecompGame.exe`
- Test script: `C:/temp/smb_test.txt`
