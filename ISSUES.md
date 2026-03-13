# NESRecomp — Known Issues

---

## ISSUE #1 — Eolis outdoor background renders garbled

**Status:** FIXED ✅ (2026-03-11)

Eolis outdoor area now renders correctly. Fixed by adding the bank14 $A5E7 dispatch
table (66 entries) to known_tables in function_finder.c.

---

## ISSUE #2 — Dialogue box freeze (application locks up)

**Status:** FIXED ✅ (2026-03-12)

### What was wrong
Three layered bugs prevented dialogue from working:

1. **4-PHA dispatch S-register drift** (bank14 $A6A0, $A78B): The S restore value
   after a 4-PHA RTS-as-dispatch was `_s4+2` instead of `_s4+4`. `_s4` is captured
   AFTER all 4 PHAs execute (S_orig - 4), so correct restore is `_s4+4 = S_orig`.
   The drift corrupted g_ram[0x100] (MMC1 bank register), causing spurious bank=15
   dispatch misses every NMI frame.

2. **Missing CHR-update handlers** (func_D654/D673/D699/D6B1): func_D61D dispatches
   to 4 CHR tile-update handlers via split table at $D64C/$D650. Added entry
   `{15, 0xD64C, 0xD650, 4}` to known_split_tables in function_finder.c.

3. **Branch-bypass false-positive 2-PHA dispatch** ($C49C in func_C478): Four
   branches (BNE/BEQ/BEQ/BCS) jump directly to the RTS at $C49C, bypassing the two
   PHAs that load the dispatch target. The recompiler was emitting a 2-PHA dispatch
   at $C49C regardless, reading stale JSR return addresses off the stack as targets
   ($047C and $100F, bank=12, ~275×/run). This silenced the scene dispatcher every
   frame, leaving the dialogue box permanently empty.

   Fix: track branch_targets[] in emit_function. When a PHA is immediately followed
   by a branch-target RTS, emit the dispatch inline after the PHA (fall-through path).
   Emit `return` at the branch-target label (branch path). Both paths now correct.

### Verified fix
"I've been on a long journey." renders correctly with cursor arrow. Game runs to
EXIT 0 at frame 1151 with no freeze and no spurious bank=12 dispatch misses.

---

## ISSUE #3 — No sound (APU / SDL audio not implemented)

**Status:** OPEN — significant feature work required

### Symptom
The game runs completely silently. No music, no sound effects of any kind — not
title screen music, not area music changes on room entry, not damage/death/gold SFX.

### Root cause
SDL audio is not initialized anywhere in the runner. There is no APU emulation and
no audio output path. Building sound requires:
1. Initialize SDL audio (SDL_Init with SDL_INIT_AUDIO, open audio device)
2. Emulate the NES APU (pulse 1, pulse 2, triangle, noise, DMC channels) or at minimum
   stub the APU register writes ($4000–$4013, $4015, $4017) to produce output
3. Mix and stream audio samples to the SDL audio callback at ~44100 Hz

Additionally, several bank5 APU routines are not yet generated due to missing dispatch
table entries. Active dispatch misses:

| Address | Bank | Rate | Note |
|---------|------|------|------|
| $8C73   | 5    | ~14/run | APU channel write |
| $8C69   | 5    | ~14/run | APU channel write |
| $8680   | 5    | ~1/run  | APU dispatch |

Bank5 dispatch tables that need adding (add ONE AT A TIME — a previous attempt caused
a $8003/$8009 bank=15 dispatch flood, root cause not yet identified):
- $826A–$828D (18 entries), dispatcher at $8265
- $85AF–$8620 (58 entries, skip entry 0 — entry 0 recurses infinitely), dispatcher $8678

### Priority
Low — requires substantial new subsystem. Tackle after all visual bugs are resolved.

---

## ISSUE #4 — HUD garbles after left/right screen transition

**Status:** FIXED ✅ (2026-03-12)

### Root cause (final)
`split_y` was hardcoded to 16 in `ppu_renderer.c`. The Faxanadu HUD tiles live in
**NT0 nametable rows 2–3** (scanlines 16–31), not rows 0–1. With split_y=16, those
scanlines were assigned to the *game* scroll region (not HUD scroll), so after the
screen transition switched the game area to NT1, the HUD tiles at NT0 rows 2-3 became
invisible and were replaced by NT1 game tiles.

### Fix
`split_y` is now derived from sprite-0 OAM Y: `(g_ppu_oam[0] + 9)`. For Faxanadu
(sprite-0 Y=$17=23) this gives split_y=32, putting scanlines 0–31 in the HUD scroll
region (NT0, scroll=0) and scanlines 32+ in the game scroll region. NT0 rows 0-1
(scanlines 0-15) are spacer rows (tile $00), rows 2-3 (scanlines 16-31) hold the actual
HUD content (M:, P: health/magic bars; E:, G: counters; T:, item brackets).

### Investigation path
- NT dump revealed NT0 rows 0-1 = all zeros, rows 2-3 = HUD tile IDs
- OAM dump confirmed no HUD sprites; HUD is purely BG tiles in NT0 rows 2-3
- Pre-transition partial garble (right-side orange circles) was NT1 column 0-10 showing
  through when game scroll (sx=88) wrapped into NT1 at screen x≈168+
- After fix: HUD stable across all frames before, during, and after transition

### Verified fix
Frame 2400 (pre-transition), 2460 (mid-transition), 2520 (post-transition): M:, P:
bars and E:/G: counters all render correctly throughout the screen transition.

---

## ISSUE #5 — Sprite priority / z-ordering: player renders in front of foreground tiles

**Status:** PARTIALLY FIXED — low priority, acceptable as-is

### Symptom
The player character sprite renders in front of foreground background tiles (pillars,
columns, walls) that should occlude it. On real NES the character should appear to
walk "behind" these foreground elements. See screenshot NESRecompGame_1MU49WjyIf.png:
the character overlaps the stone pillar/column instead of appearing behind it.

### Root cause
The NES PPU supports a sprite priority bit (OAM byte 2, bit 5):
- Priority 0 = sprite rendered in front of background (normal)
- Priority 1 = sprite rendered behind background non-zero color pixels (behind FG)

The player sprite has priority=1 set in its OAM entry, but our `ppu_renderer.c` almost
certainly ignores this bit and draws all sprites on top of all background tiles. Fixing
this requires checking the priority bit during sprite rendering and, when priority=1,
only drawing the sprite pixel if the corresponding background pixel is color 0
(transparent / background color).

### Implementation note
The PPU renders background first, then sprites. For priority-1 sprites, the sprite
pixel should only be drawn where the BG pixel is the universal background color
(palette index 0). The framebuffer pixel at that position will already have the BG
color written; comparing it against the backdrop color (g_ppu_pal[0] lookup) tells
us whether the BG is transparent there.

### Progress (2026-03-12)
Priority bit check implemented: `spr_attr >> 5 & 1` → when priority=1, only draw the
sprite pixel where `framebuf[px] == NES_PALETTE[g_ppu_pal[0]]` (universal backdrop).
Result: player now appears "semi-transparent" through the brick pillar rather than fully
in front of it. This is actually correct NES behavior — priority-1 sprites show through
BG pixels drawn with color index 0. The brick tiles have some color-0 pixels in them
(mortar gaps), so the sprite shows through those. On a real CRT this effect is less
visible than on a flat monitor.

See screenshot: `NESRecompGame_8QhoxotcMq.png` (user's Documents/ShareX folder).
Acceptable as-is — the major visual artifact (fully in front of pillars) is resolved.
The residual semi-transparency is correct-per-spec behavior.

### Investigation starting point
In `runner/src/ppu_renderer.c`, find the sprite rendering loop and locate where OAM
byte 2 attributes are read. Check if bit 5 (priority) is tested before drawing.

---

## ISSUE #6 — $A60F bank=14 dispatch miss (unknown call site)

**Status:** OPEN — low priority

Appears ~55 times per run at bank=14. Not in any known dispatch table, no static JSR
reference found. Likely a dynamic/RAM-computed dispatch. No visible symptom tied to it
yet. Leave alone until a visible bug is linked to it.

---

## Historical notes

- bank-14 dispatch misses ($8C0F, $8C98, $89EF, $A6FF, $0001): status unclear after
  adding the $A5E7 table. Re-check console output to confirm resolution.
- bank5 dispatch flood regression: adding $826A and $85AF tables previously caused a
  $8003/$8009 bank=15 dispatch flood. Root cause not identified. Proceed carefully.
