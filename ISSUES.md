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

**Status:** FIXED ✅ (2026-03-14)

### Symptom
Enemies die (death animation plays) but no coin/bread item drop appears.

### Root cause (resolved — drops were always working)
Extended runtime test with `C:/temp/kill_test.txt` (projectiles.txt + 20 additional B
presses) confirmed the full kill + drop chain works correctly in the recompiled game.

**Kill path confirmed — Path B (HP underflow → death animation → drop):**
- Entity type `$28` (slot 7), HP=`$38`: took 16 damage per Deluge orb hit
  - Hit 1 (frame 924): HP `$38 → $28`
  - Hit 2 (frame 1014): HP `$28 → $18`
  - Hit 3 (frame 1158): HP `$18 → $08`
  - Hit 4 (frame 1269): HP `$08 → $F8` (underflow) → `$02CC` becomes `$13` **PATH-B**
- Frame 1269: `$02CC[7]` = `$13` (death animation spawned by `SetDeathEntity/$ABF8`)
- Frame 1277: death animation completes → `$02CC[7]` = `$FF`
- Frame 1277: entity type `$01` (bread drop) spawned in slot 4
- Frame 1270: EXP awarded (display: 26000 → 26030)
- drops.png screenshot: small item sprite visible at drop location 120 frames post-kill

### Kill paths
**Path A — instant destroy, NO drops** (`$87CB`):
```
JSR $87DC          ; side effects (EXP/score update via bank12 + $C08E)
LDX $0378          ; reload entity slot
LDA #$FF
STA $02CC,X        ; entity type = $FF (instantly inactive)
```
Fires when bounding-box collision check at `$87C8` (BCC) detects a hit. No drop.

**Path B — death animation WITH drops** (`$88A9`):
```
HP subtraction at $8891–$8897 (LDA $0344,X / SEC / SBC $00 / STA $0344,X)
Underflow → JMP $ABF1 → SetDeathEntity ($ABF8) → AC21 → AC2D → $B672 drop table
```
Fires when entity HP underflows. Deluge magic uses this path. Drops spawn correctly.

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

## ISSUE #10 — Magic projectile sprites not visible in OAM

**Status:** FIXED ✅ (2026-03-14)

### Symptom
Magic spells (Deluge, Thunder, Fire, etc.) cast damage correctly and travel through
the air dealing HP damage to enemies — but the sprites for the magic orbs/effects
were completely invisible. They never appeared in the OAM debug window and were never
rendered on-screen. The player could observe enemies losing HP with nothing visible
causing it.

### Investigation path

**Step 1 — OAM overflow check ruled out.**
`$0039` (OAM overflow flag, written by `$CB5F` / `func_CB3F`) is always zero, so the
overflow check at `$F0FE` (`LDA $39 / BNE $F161`) inside `func_F057` (metasprite OAM
writer) is never blocking sprite writes. This was confirmed by user inspection of the
live debug watch window.

**Step 2 — OAM DMA path confirmed correct.**
`runtime.c` handles `$4014 = $07` by copying `g_ram[$0700–$07FF]` to `g_ppu_oam[0–255]`.
`func_CB4F` (OAM buffer clear: sets `$0025=0`, fills `$0704–$07FF` with `$F0`) IS called
many times per run (30+ call sites confirmed in generated code). OAM management works.

**Step 3 — `func_C2E9` has no direct call sites.**
Grepping `generated/faxanadu_full.c` for `func_C2E9` shows only the forward declaration
and the function definition — zero call sites. Yet `func_C2E9` IS registered in
`faxanadu_dispatch.c` as `case 0xC2E9: func_C2E9(); break;`.

**Step 4 — `call_by_address(0xC2E9)` found at `$BAEC`.**
Grepping for `0xC2E9` in the generated code found one call at line ~158103:
```c
/* $BAEC: 60 */ { ...; call_by_address((...)+1); g_cpu.S+=2; call_by_address(0xC2E9); }
```
This is inside `func_BAD9_b14`. The same `$BAEC` address in another (outer) function
emitted only:
```c
/* $BAEC: 60 */ { ...; call_by_address((...)+1); }
return;
```
No `call_by_address(0xC2E9)` — the magic render call is missing from the inline path.

**Step 5 — Call chain understood via Ghidra (bank14 `$BAD9`).**

The entity-state dispatcher in bank14:
```
$BAD9: LDA #$C2   \ push outer continuation $C2E8 (addr-1 form of $C2E9)
$BADB: PHA         /
$BADC: LDA #$E8   \
$BADE: PHA         /
$BADF: LDA $02B3        ; magic state index
$BAE2: ASL              ; × 2 for table offset
$BAE3: TAY
$BAE4: LDA $BAF8,Y  \   push entity state handler (hi byte, addr-1)
$BAE7: PHA           /
$BAE8: LDA $BAF7,Y  \   push entity state handler (lo byte, addr-1)
$BAEB: PHA           /
$BAEC: RTS              ; 4-item dispatch: entity handler, then outer cont $C2E9
```
The RTS at `$BAEC` is a "2+2 dispatch": pop inner handler → call it, then pop outer
continuation → call it. The inner handler runs the entity state machine (Deluge/Thunder/
etc.). The outer continuation `$C2E9` runs the magic sprite render.

**Step 6 — Root cause identified: `func_base` vs. branch-target block start.**

`code_generator.c` `emit_instruction()` detects this pattern by checking whether the
function STARTS with `LDA #hi / PHA / LDA #lo / PHA` (the outer continuation push at
the function prologue). When processing `$BAEC` inside `func_BAD9_b14`, `func_base =
$BAD9`, and the check finds `A9 C2 48 A9 E8 48` at `$BAD9` — correct, emits
`call_by_address(0xC2E9)`.

BUT: `$BAD9` is also a **branch target** inside a larger outer function. The outer
function has `CMP #$04 / BNE $BAD9` at `$BAC4`–`$BAC6`. The recompiler inlines the
`$BAD9–$BAEC` code block as a `goto label_BAD9` in the outer function. When the same
`emit_instruction()` processes `$BAEC` in this context, `func_base` is the OUTER
function's entry point (not `$BAD9`). The outer function does NOT start with
`LDA/PHA/LDA/PHA`, so the outer continuation check fails → plain 2-PHA dispatch →
entity handler called but `$C2E9` never called → magic render never runs.

This bug is silent: the entity state machine runs correctly (magic travels, deals
damage via separate HP-subtraction path), but the OAM write path in `$C2E9` is never
reached.

### Fix

`code_generator.c`, `emit_instruction()`, 2-PHA dispatch detection:

**Before:** check only `func_base + 0..5` for the `LDA #hi/PHA/LDA #lo/PHA` pattern.

**After:** scan backwards from the RTS (up to 128 bytes) looking for
`A9 hi 48 A9 lo 48`. Scanning nearest-first finds the innermost static address push.

```c
for (int _back = 6; _back <= 128; _back++) {
    uint16_t _probe = (uint16_t)(pc - _back);
    if (rom_read(rom, bank, _probe)   == 0xA9 &&
        rom_read(rom, bank, _probe+2) == 0x48 &&
        rom_read(rom, bank, _probe+3) == 0xA9 &&
        rom_read(rom, bank, _probe+5) == 0x48) {
        // Extract hi/lo, compute tgt = ((hi<<8)|lo)+1
        // If tgt >= 0x8000 and tgt != pc+1 → has_outer_cont = 1
    }
}
```

The backwards scan is safe because the outer continuation push always uses
`LDA #const / PHA` (opcode `A9` = immediate), while the inner handler uses
`LDA $table,Y` (opcode `B9` = absolute indexed), so there is no ambiguity.

For `$BAEC` in the outer function: the scan finds the pattern at `back=19` (address
`$BAD9`), extracts `hi=$C2`, `lo=$E8`, computes target `$C2E9`. Correct.

The fix also handles `func_BAD9_b14` unchanged: scan at `back=19` still finds `$BAD9`.

### Generated code after fix

All 4 occurrences of `$BAEC` in `faxanadu_full.c` now correctly emit:
```c
{ g_cpu.S++; uint8_t _lo=g_ram[0x100+g_cpu.S]; g_cpu.S++; uint8_t _hi=g_ram[0x100+g_cpu.S];
  call_by_address(((uint16_t)_hi<<8|_lo)+1);  /* entity state handler */
  g_cpu.S+=2;                                  /* discard $C2E8 addr bytes */
  call_by_address(0xC2E9);                     /* magic render → func_C2E9 */
}
```

### Verified fix
Rebuilt recompiler (5659 functions unchanged) + runner. No `$C2E9` dispatch misses
in stdout. Kill-test script runs cleanly to frame 1440. Magic projectile sprites
now visible in OAM and on-screen.

### Regression introduced — see Issue #11

---

## ISSUE #11 — Backward scan false positives: enemy flicker + wrong drop positions

**Status:** FIXED ✅ (2026-03-14)

### Symptom
Introduced by the Issue #10 fix. After magic sprites became visible:
- Enemy sprites flicker and "spaz out" (jitter/teleport rapidly)
- Item drops (bread/coin) spawn at wrong map positions
- Broad memory corruption effects visible across many game systems

### Root cause
The Issue #10 fix replaced the `func_base` prologue check with a **backward scan**
(up to 128 bytes) for `LDA #hi / PHA / LDA #lo / PHA` to detect outer continuations.
The scan accepted any target address `>= 0x8000`, which included switchable-bank
addresses ($8000–$BFFF).

This created two false positives in bank14:

**`$A6CE` → `call_by_address(0xA6A1)`:**
The scan found bytes matching `A9 A6 48 A9 A0 48` somewhere in the 128 bytes before
`$A6CE` (a legitimate 2-PHA entity dispatch). The pattern decoded to target `$A6A1`
(= ($A6A0) + 1), which is the continuation address of the adjacent 4-PHA dispatch at
`$A6A0` — NOT an outer continuation for `$A6CE`. The generated code now called
`func_A6A1_b14` after every entity-state dispatch, re-entering the entity processing
loop spuriously.

**`$A7F3` → `call_by_address(0xA78C)`:**
Same problem: backward scan matched a pattern and decoded `$A78C` as an outer
continuation for `$A7F3`.

Effect: entity state handlers were called 2× per frame, corrupting entity coordinates
(→ wrong drop positions) and alternating sprite positions every frame (→ flicker).

### Why switchable-bank addresses can never be outer continuations
The "outer continuation" pattern (`LDA #hi/PHA / LDA #lo/PHA / ... dispatch ... / RTS`)
pushes a FIXED address to call after the dispatched handler returns. For this to be
meaningful across a bank switch, the address must be in the **fixed bank** ($C000–$FFFF,
always accessible). A switchable-bank address ($8000–$BFFF) would be invalid after any
MMC1 bank switch inside the dispatched handler — the bank may be different on return.
In Faxanadu, all confirmed outer continuations are in bank15 (the fixed bank).

### Fix
`code_generator.c`: changed the backward-scan acceptance condition from
`tgt >= 0x8000` to `tgt >= 0xC000` (fixed bank only).

This excludes both false positives (`$A6A1` < `$C000`, `$A78C` < `$C000`) while
keeping the intended detection of `$C2E9` (= `0xC2E9` >= `$C000` ✓).

### Outer-continuation sites after fix (correct — 4 total)
All 4 are `$BAEC → call_by_address(0xC2E9)`:
```c
{ ...; call_by_address(entity_handler); g_cpu.S+=2; call_by_address(0xC2E9); }
```
Two occurrences are inline goto-target blocks (the bug that Issue #10 fixed).
Two occurrences are inside `func_BAD9_b14` and one duplicate bank context.

### Also in this commit
Removed RAM debug watch variables from `main_runner.c` (Magic State, OAM slot idx,
OAM overflow — no longer needed after Issue #10 investigation).

---

## Historical notes

- bank-14 dispatch misses ($8C0F, $8C98, $89EF, $A6FF, $0001): status unclear after
  adding the $A5E7 table. Re-check console output to confirm resolution.
- bank5 dispatch flood regression: adding $826A and $85AF tables previously caused a
  $8003/$8009 bank=15 dispatch flood. Root cause not identified. Proceed carefully.
