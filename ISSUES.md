# NESRecomp — Known Issues

---

## ISSUE #1 — Eolis outdoor background renders garbled

**Status:** FIXED ✅ (session ~2026-03-11)

Eolis outdoor area now renders correctly: dark teal sky, castle/fortress, trees, dirt
ground, character sprite. Fixed by adding the bank14 $A5E7 dispatch table (66 entries)
to the known_tables scanner in function_finder.c, bringing function count from 5480 →
5570. The previous bank-14 dispatch misses ($A978, etc.) were the root cause.

---

## ISSUE #2 — Dialogue box causes black corruption and soft lock

**Status:** OPEN

### Symptom
When the player walks into an NPC and triggers a dialogue box, large black rectangular
regions appear across the screen and the game soft locks (stops responding to input).

### How to reproduce
Run with input script to Eolis outdoor area, walk right into the first NPC/building.

### Suspected root cause
The dialogue/text rendering system uses a different PPU write path (possibly via the
CF3C queue or a text-specific VRAM writer in bank15). One or more of these paths likely:
- Writes to an incorrect nametable region, overwriting valid tile data with zeros
- OR uses a VRAM address that wraps incorrectly in g_ppu_nt[]
- OR the text box background fill is using tile index $00 (black) instead of the correct
  border/fill tiles because a CHR bank is not loaded

### What to investigate
1. Check ppu_trace.csv during dialogue trigger — look for large bursts of $2007 writes
2. Check bank15 $F8F2-$F907 (text/logo writes) — likely the text renderer
3. Check CF3C queue entries during text box draw
4. Cross-reference with reference emulator (Nestopia) to see correct text box appearance

---

## ISSUE #3 — No sound / APU dispatch misses

**Status:** OPEN

### Symptom
Game runs silently. No sound effects or music.

### Root cause
Several bank5 APU routines are not generated because they are only reachable via
dispatch tables that aren't in known_tables. Active dispatch misses during gameplay:

| Address | Bank | Hits | Description |
|---------|------|------|-------------|
| $8C73   | 5    | ~70/run | APU channel write (mid-function entry point) |
| $8C69   | 5    | ~14/run | APU channel write |
| $8680   | 5    | ~2/run  | APU dispatch |
| $8408   | 5    | ~1/run  | APU routine |

### Root cause detail
Bank5 has two dispatch tables:
- $826A–$828D (18 entries): dispatcher at $8265. Fixes $8408 miss.
- $85AF–$8620 (58 entries, skip entry 0): dispatcher at $8678. Fixes $8680, $8C69, $8C73.

**WARNING:** Entry 0 of $85AD table → target $8681 (a dispatcher that recurses via the
same table with X=0 → infinite recursion). Start table scan at $85AF to skip it.

**WARNING:** Adding bank5 tables caused a `$8003/$8009 bank=15` dispatch flood in
earlier testing. Root cause not fully identified — may be a secondary entry in one of
the tables that generates bad code. Approach carefully: add one table at a time and
verify no flood in console output before proceeding.

### Investigation state
Both bank5 tables were REMOVED from known_tables after the flood regression. The
hardcoded seed `add_function(0x828E, 5)` remains.

---

## ISSUE #4 — $A60F bank=14 dispatch miss (source unknown)

**Status:** OPEN — low priority

Appears ~55 times per run with bank=14. Not in any known dispatch table, no static JSR
reference found in any bank. Likely a dynamic/RAM-computed dispatch. Since no function
is generated for it, the call is silently dropped. Unknown whether it affects gameplay
visuals. Leave alone until a visible symptom is tied to it.

---

## Historical: bank-14 dispatch misses (old Issue #2)

Addresses `$8C0F`, `$8C98`, `$89EF`, `$A6FF`, `$0001` in bank 14 were previously
reported as dispatch misses. Status unclear after adding the $A5E7 table — some may
now be resolved. Re-check console output to confirm.
