# NESRecomp — Known Issues

---

## ISSUE #1 — Eolis outdoor background renders garbled

**Status:** FIXED ✅ (session ~2026-03-11)

Eolis outdoor area now renders correctly: dark teal sky, castle/fortress, trees, dirt
ground, character sprite. Fixed by adding the bank14 $A5E7 dispatch table (66 entries)
to the known_tables scanner in function_finder.c, bringing function count from 5480 →
5570. The previous bank-14 dispatch misses ($A978, etc.) were the root cause.

---

## ISSUE #2 — Dialogue box freeze (application locks up)

**Status:** ROOT CAUSE IDENTIFIED — fix not yet applied

### Symptom
When the player triggers a dialogue box, the entire application freezes (window
becomes unresponsive). This is a hard freeze, not a soft lock.

### Root cause (CONFIRMED)
`func_C9D6` ($C9D6, bank15 fixed) is called every NMI. It contains a sprite-0 hit
spin-wait loop at $CA02–$CA0A:

```
CA02: BIT $2002  → loop while bit6=1 (wait for sprite-0 to clear)
CA07: BIT $2002  → loop while bit6=0 (wait for sprite-0 to fire)
```

`ppu_read_reg($2002)` only simulates sprite-0 hit when `g_ppumask & 0x18 != 0`
(rendering enabled). During dialogue box setup, the game temporarily disables PPU
rendering (`$2001=0`) but leaves NMI enabled. When the NMI fires in this window,
`func_C9D6` runs, writes `g_ram[0x0B]` (which is 0) back to `$2001`, then hits the
`BVC $CA07` spin loop. With rendering disabled, `ppu_read_reg` never sets bit6
→ loop spins forever → entire application freezes.

### Fix required
In `runner/src/runtime.c`, `ppu_read_reg` case `0x2002` — remove the
`if (g_ppumask & 0x18)` guard so sprite-0 is ALWAYS simulated after 3 reads:

```c
// BEFORE:
if (g_ppumask & 0x18) {
    static int s_spr0_reads = 0;
    ...simulate...
}

// AFTER: always simulate, regardless of render state
{
    static int s_spr0_reads = 0;
    ...simulate...
}
```

This lets the `BVC $CA07` loop exit after ~4 reads even when PPU is off, preventing
the hang. On real NES, sprite-0 hit is hardware-cleared at VBlank and doesn't fire
when rendering is off — but our simulation just needs to not loop forever.

### Partial fix applied (this session — may be incomplete)
`main_runner.c`: NMI now gated on `g_ppuctrl & 0x80` (PPUCTRL bit7). This is correct
hardware behavior but trace shows `$2000=$90` throughout cave/dialogue transitions, so
NMI is always enabled — the gate alone does not prevent the freeze. The sprite-0
simulation fix above is the correct solution.

### How to reproduce
Run the game, press START, walk right until character enters first indoor area / talks
to an NPC. Application freezes immediately when the dialogue box opens.

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
