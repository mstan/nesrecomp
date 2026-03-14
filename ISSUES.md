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

**Status:** FIXED ✅ (2026-03-12)

Music and SFX (footsteps, dialogue scroll) both work. Commits: bf49fa2, 8723a35.

### What was built
- `runner/src/apu.c` + `runner/include/apu.h`: pulse1, pulse2, triangle, noise
  channels with envelopes, sweep, length counters. DMC: output-level ($4011) only.
- SDL_QueueAudio at 44100 Hz mono, 735 samples/frame. 4 frame-counter quarter-frame
  ticks per VBlank (envelope/length/sweep updates at ~240 Hz).
- `runtime.c`: $4000–$401F writes route to apu_write(); $4015 reads return status.

### Bank5 dispatch table flood — root cause (now understood)
Previous entry `{5, $85AF, $8623}` was wrong: start should be $85AD (dispatcher
reads lo from $85AD+X, hi from $85AE+X). Old start added $8680 (bare RTS of the
dispatcher) as a function. code_generator saw PHA at $867F and wrongly emitted a
2-PHA dispatch there, reading stale stack data → $8003/$8009 bank=15 flood every frame.
Fix: `{5, 0x85B1, 0x8621}` — skips $8681 and $8680, adds $8C69/$8C73 correctly.

### Remaining
- DMC ROM DMA not implemented (DMC samples silent, if any).
- $8680 dispatch miss (~1/run) — intended no-op, harmless.

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

## ISSUE #7 — Magic projectile sprites not visible

**Status:** FIXED ✅ (2026-03-13)

### What was wrong
The entity state machine dispatch table at bank14 `$BAF7` was missing entries beyond
state 16 (`$BC5B`). State 16 was the initial state for projectile/effect entities.
Without the full table, entities would execute the wrong state handler and the sprite
draw path for magic would not be reached.

Fix: added `{14, 0xBAF7, 0xBB25}` to `known_tables` in `function_finder.c` (23 entries).

### Verified fix (2026-03-13) — runtime confirmed
Recompiler rebuilt; `func_BBF3_b14` (Deluge state 4) and `func_BC5B_b14` (state 16)
now present in `generated/faxanadu_full.c` and registered in the dispatch table.

Runtime test with `--password "k8fPcv?,TwSYzGZQhMIQhCEA" --script C:/temp/projectiles.txt`
and force-injection of `$03C1 = 0x10` (Deluge action state) in frames 200–1200:
- `$02B3` reached `0x4` → `func_BBF3_b14` called (Deluge state-machine handler)
- `$02B3` transitioned to `0xFF` (entity completed normally)
- No dispatch miss for bank14 `$BAD9`/`$BBF3` region
- Force-injection required because the password gives `$03C1 = 0x7` (wrong magic type);
  Deluge IS in inventory (`$0210 & $1F = $10`) but not equipped in that save

Mesen ground truth (established separately): `oam5_t=0x90`, `oam5_a=0x43`
(tiles `$90–$96`, `attr=$03`, orange/gold orb in-front-of-BG, palette 3).

### No debug code remaining
All debug traps removed from `runtime.c`, `ppu_renderer.c`, and `main_runner.c`.

---

## ISSUE #8 — Enemy drops not spawning

**Status:** OPEN

### Symptom
Enemies die (death animation plays) but no coin/bread item drop appears.

### Code analysis (Ghidra, 2026-03-13)
Call chain in bank14 is intact:

1. **Entity death state handler** (~$ABD8): runs each frame after `Sprite_SetDeathEntity`
   sets the entity to a death-animation type. Increments `$02EC,X` each frame; once it
   reaches 8 it calls `JSR $AC21` then sets entity to `$FF` (destroy).
2. **`$AC21` (Sprite_HandleDeathDropIfPossible)**: calls `$A236` to check if max sprites
   on screen; if not full, calls `JSR $AC2D`.
3. **`$AC2D` (Maybe_Sprite_HandleDeathDrop)**: saves Y (free slot index) in `$00`, loads
   original entity ID from `$02FC,X`, looks up `$B672[entity_id]` (drop table). Returns
   no-drop if result is `$FF` or `>= $40`; otherwise uses `$ACED[drop_index]` for
   probability, then spawns coin (entity `$02`) or bread (entity `$01`).
4. **`Sprite_SetDeathEntity`** (`$ABF8`) initialises `$02EC,X = 0` (confirmed at `$AC19`).

`$AC21` is called from `JSR $AC21` at `$ABE3` and `JMP $AC21` at `$ABAC`. Both are
reachable via JSR tracing; `func_AC21` and `func_AC2D` will be generated.

### Updated analysis (2026-03-14) — two death paths found

Ghidra trace of the bank14 damage/death system reveals **two distinct kill paths**:

**Path A — instant destroy, NO drops** (`$87CB`):
```
JSR $87DC          ; side effects (EXP/score update via bank12 + $C08E)
LDX $0378          ; reload entity slot
LDA #$FF
STA $02CC,X        ; entity type = $FF (instantly inactive)
```
This path fires when a bounding-box collision check at `$87C8` (BCC) detects a hit.
Entity vanishes without death animation and without calling `$AC21` (no drop).

**Path B — death animation WITH drops** (`$88A9`):
```
LDA #$03 / JSR $D0E4  ; bank-switch prep
STA $034C,X / JSR $8B87 / LDY $02CC,X / LDA $B544,Y
CMP #$07 → JMP $ABEC  ; type $64 "big death" for special enemies
           JMP $ABF1  ; type $13 normal death → SetDeathEntity → drop check
```
This path fires when entity HP subtraction at `$8891–$8897` underflows (HP ≤ 0).

### Root cause hypothesis
If the player sword attack or magic projectile collision uses **Path A** (bounding-box
kill) rather than the HP-subtraction path, drops never spawn. Path A is reachable from
`func_87CA_b14` (in dispatch). Path B at `$88A9` is reachable from `func_88A9_b14`
(also in dispatch).

The symptom — enemies die (death animation?) but drops missing — needs confirmation that
either: (a) Path A is being used instead of B, or (b) Path B fires but `$B672[entity_id]`
returns `$FF` (no-drop entry for that enemy type).

### Runtime test status
Automated combat tests could not confirm any enemy deaths during scripted runs:
- The DEATH_TYPE watch ($02CC changing to $13/$14) never fired — no enemies entered
  the death animation state.
- `$02EC` at slot 5 (entity type $0B) incremented continuously — this is a BEHAVIOR
  counter for that entity type, NOT a death counter.
- Entity type $0B (`$B544[$0B]=0`) would use death type $13 if damaged via Path B.

### Next debug step
To confirm which path is used: add `log_on_change` for `g_ram[0x0344]` (entity HP slot 0
= `$0344+slot`). If HP decrements → Path B is reached. If HP never changes but entity
vanishes → Path A (instant destroy). Monitor `$02CC` across all slots for `$FF` appearance.

Dispatch entries confirmed present: `func_88A9_b14`, `func_87CA_b14`, `func_87DC_b14`,
`func_822E_b14`, `func_828B_b14`, `func_AC21_b14`, `func_AC2D_b14`.

---

## ISSUE #9 — Magic sprite invisible near castle walls (priority behind BG)

**Status:** CLOSED ✅ — correct NES behavior (2026-03-13)

### Symptom
Magic projectile deals damage but is not visible near castle walls (zone codes 4/D/9).
OAM attribute observed as `$61` (priority=1, flip-H, palette 1) vs `$03` in open areas.

### Full code trace (Ghidra, 2026-03-13)

**Magic render entry point: bank15 `$C2E9`**
Called after the `$BAD9` entity-state dispatch returns to bank15. Sequence:
1. `LDA $02B3; BMI $C314` — if magic inactive ($FF), skip to RTS
2. `JSR $C315` — zone-based attr computation → ZP `$26`
3. Sets ZP `$27`/`$28` = magic X/Y position hi bytes (for metasprite offset)
4. 2-PHA dispatch from table `$BB27` (indexed by `$02B3` magic state) → finish handler

**`$C315` — zone-based attr computation:**
- ZP `$B8` = 0
- Checks zone code at X+4 via `$E86C`/`$E8C3`; if zone ∈ {4, D, 9} → `ORA #$01`
- Checks zone code at X+12; if zone = 4 → `ORA #$02`
- If direction bit (`$02B4 AND $40`) = LEFT → `EOR #$03` on palette bits (flip L/R)
- Result stored in ZP `$26`

**`$BB27` finish-handler table** (addr−1, 5 spell types):
| State | Target |
|-------|--------|
| 0 (Deluge) | $C39B |
| 1 (Thunder) | $C3A7 |
| 2 (Fire) | $C3B6 |
| 3 (Death) | $C3C9 |
| 4 (Tilte) | $C3D6 |

Each finish handler calls `$C393` (`AND $02B4,#$40 → ZP $29` = flip-H flag), extracts
animation frame from ZP `$1A`, then `JMP $C37D → ADC $C387,Y; JMP $F057`.

**`$F057` — metasprite OAM writer:**
Reads multi-tile sprite layout from CHR pointer table. For each sub-tile:
- `LDA ($3A),Y` = metasprite base attr byte
- `EOR $29` (apply direction flip-H)
- `ZP $01` = result
- **`LDA $26; AND $F224[Y_sub]; BEQ skip; LDA $01; ORA #$20; STA $01`**
  → if zone-palette byte (`$26`) overlaps mask for this sub-tile, **set priority=1**
- `STA $0700,X` → write attr to OAM buffer (page $07)

**`$F224` mask table:** `{01, 02, 02, 01}` — alternates between checking palette bit 0
(zone at X+4) and bit 1 (zone at X+12) per sub-tile within the metasprite.

### Conclusion
`attr=$61` is **100% correct NES behavior**. The game's metasprite engine deliberately
sets priority=1 (behind BG) on any magic sub-tile whose pixel position overlaps a castle
zone tile. This is the same per-pixel BG-priority system used for the player walking
behind foreground pillars (Issue #5). Magic projectiles pass through castle walls visually
— they show through transparent BG pixels only, which is sparse in castle zones.

No fix needed. Close.

---

## Historical notes

- bank-14 dispatch misses ($8C0F, $8C98, $89EF, $A6FF, $0001): status unclear after
  adding the $A5E7 table. Re-check console output to confirm resolution.
- bank5 dispatch flood regression: adding $826A and $85AF tables previously caused a
  $8003/$8009 bank=15 dispatch flood. Root cause not identified. Proceed carefully.
