# ENHANCEMENTS.md — deferred co-sim capabilities & roadmap

Scope note: this file is the **differential co-simulation tooling roadmap** —
capabilities we have deliberately deferred, and how to build them when we choose
to. It is NOT the "break faithfulness on purpose" game-enhancement doc that
`recomp-template/ENHANCEMENTS.md` defines (widescreen, higher frame rate, etc.);
that meaning is separate. Everything here is about *measurement reach*, not
changing game behavior.

See `recomp-template/NES/DIFFERENTIAL-COSIM-PROPOSAL.md` for the built co-sim and
`tools/nes_cosim.py` for the coordinator (gate1/2/3, abram, abcycle, abppu).

---

## 1. State-synced co-sim fixtures (the big deferred capability)

### What we have now, and where it stops

The co-sim compares recomp vs Mesen (via `nesref`'s `retro_serialize` blob)
**frame-aligned over a free run** with a fixed boot offset (SMB `+7`): recomp
frame `N` is compared to oracle frame `N + offset`. This is exact and powerful
**when the recomp's trajectory is bit-aligned to the oracle's** — i.e. a
deterministic, no-input attract demo like Super Mario Bros. SMB converges to the
byte on this model (RAM byte-identical bar the `$1a` frame-sync flag; NT 100%;
OAM 99.96%; cycle drift −0.001 with the dot clock).

It **breaks down when the two trajectories drift apart**, which happens for:
- **Host-modeled subsystems with no bit-faithful state.** MM3's coroutine
  scheduler runs on host fibers; its scheduler ZP (`$7c–$90`) is not
  bit-faithful, so MM3's attract sequence diverges from Mesen's over time
  (`abram` shows those bytes diverging 100%). Frame `N` on the two sides is no
  longer "the same moment."
- **RNG-seeded-from-timing games.** Zelda seeds `$18 Random` from the frame
  counter; a boot-frame offset cascades to total divergence. (Partly mitigated
  today by `NESRECOMP_FREEZE` / `NESREF_FREEZE` seed-freeze.)
- **Anything that only exercises the interesting hardware in-stage** — e.g. the
  MMC3 scanline IRQ, which never fires on a static title/attract screen.

When trajectories drift, a frame-aligned PPU/RAM diff measures *phase skew*, not
*correctness*. MM3 attract shows this: OAM tracks at 99.43% (sprites align) but
palette 82.8% / NT 95.8% because the title's animated colors/tiles are a frame
out of phase across a drifting sequence. That is **not a fresh bug** — it is the
free-run model hitting its limit.

### The enhancement: compare comparable STATE, not frame number

Drive both implementations to a **known, comparable state**, then compare there
(and/or free-run forward from there). Four ways to get to a synced state, in
rough order of fidelity vs effort:

**(a) Scripted deterministic input (recommended for a targeted fixture).**
Feed the *same* button sequence to both sides to a fixed in-stage state, then
snapshot. The recomp already has `--script` (WAIT/HOLD/RELEASE/…); `nesref`
would need a matching scripted-input mode (today it only reads live keyboard —
add an `NESREF_SCRIPT=<path>` that injects the same frame-stamped inputs into
`cb_input_state`). Cost: writing/maintaining the input script (menu nav is
timing-sensitive), and it only proves the states it visits. This is the
straightforward path and the one to build first when we want in-stage rigor.

**(b) Savestate cross-load.** Capture a Mesen savestate at the target state and
load an equivalent state into the recomp, then free-run both and compare
forward. We already extract cycle/PPU/OAM/palette/NT from the Mesen blob (§`abppu`/
`abcycle`); the reverse — writing a full Mesen→recomp state bridge — is
substantial (map Mesen's serialize layout onto `g_ram`/`g_ppu_*`/`g_cpu`/mapper
regs, incl. the coroutine-scheduler state that has no faithful analog). High
effort; deferred unless (a) proves insufficient.

**(c) State-marker matching (approximate).** Free-run both, then find frames
whose PPU/scroll/palette markers match and compare only those (the burndown's
original approach). Cheap, no new harness, but approximate — good for a first
bracket, not a byte-exact verdict.

**(d) Seed/scheduler freeze.** For RNG-driven games, freeze the seed
(`NESRECOMP_FREEZE`/`NESREF_FREEZE`) so attract stays aligned — works for Zelda.
Does **not** help host-modeled state (MM3 fibers): you cannot freeze host
scheduler state into alignment; that is why MM3 needs (a) or (b), not (d).

### Recommendation

Keep **free-run attract** as the default cheap first pass (it fully nails
deterministic-demo games). Build **(a) scripted-input fixtures per game** only
when a game's interesting hardware is in-stage-only or its attract drifts — and
accept that it is a per-game cost. Do not build the general (b) bridge until a
concrete need justifies it.

---

## 2. MM3 (Mega Man 3) — specific notes

**Attract-level validation is the current ceiling for MM3, and it is good.**
Live-played by the owner with the dot clock on ("passing the smell tests");
headless: Gate 1 determinism PASS, cycle converges with the dot clock
(+0.503 → +0.017), RAM logic converges (residual = the documented
coroutine-scheduler ZP `$7c/$7d/$80/$81/$90` @100% + transient sound-engine
table `$78x` @9% — both host-modeled / transient, not game-state divergence),
OAM 99.43%. PPU blob offsets are **identical to SMB** (OAM `wram-0x16c`, palette
`wram-0x190`, NT `wram+0xab4`) — no per-mapper tuning needed; `abppu` runs on MM3
as-is.

**Why MM3 can't go deeper on free-run attract:**
- The coroutine scheduler is host-fiber-modeled → scheduler ZP is not
  bit-faithful → the attract sequence drifts from Mesen (see §1). So palette/NT
  frame-aligned diffs are phase-skew, not bugs.
- The **MMC3 scanline IRQ only fires in-stage** (the status-bar split, screen
  shakes). Attract/title never exercises it, so the co-sim never sees MM3's most
  interesting timing on the attract path.

**To do a rigorous MM3 PPU + MMC3-IRQ cross-check (when we choose to):**
1. Build a §1(a) scripted-input fixture: drive both sides past the title +
   stage-select into a stage with the HUD split (e.g. an early Robot Master
   stage), to a fixed frame.
2. Compare PPU there (OAM/palette/NT — the extraction already works, offsets =
   SMB).
3. **MMC3 IRQ counter (frame-granular, lower-hanging):** extract Mesen's MMC3
   `irqCounter`/`irqReloadValue`/`irqEnabled` from the serialize blob (find the
   mapper-state region the same way we found OAM — `memmem` a known value or
   anchor off WRAM), and diff against the recomp's `MapperState` (already in the
   cosim `mapper` sub-hash). This checks the IRQ *counter state* matches.
4. **MMC3 IRQ fire timing (Rung 3, hardest):** compare *when* the IRQ fires
   (which scanline/dot). Needs event traces on both sides (recomp already has a
   render-IRQ scanline tap; Mesen would need an IRQ event callback — external
   Lua, or infer from per-scanline blob snapshots). This is sub-scanline and
   only matters if a visible raster effect is wrong.

**Risk to flag:** MM3 has known in-gameplay coroutine-dispatch gaps (latent
misses at `$C782/$C7C2/$C8DB/$900F/$96BA` etc., unreached on attract). A scripted
in-stage fixture may hit them — which is useful (it doubles as a gameplay-coverage
test) but means the fixture is also a debugging surface, not just a clean oracle.

---

## 3. Rung 3 — per-dot Mesen identity (sub-scanline)

The north star of the convergence ladder: sub-scanline sprite-0 / MMC3-A12 IRQ
placement, exact intra-instruction read/write cycle timing, DMA cycle alignment,
and the 2-stage VBlank-flag / NMI-suppression quirks — so the cross-impl hash
matches at *every dot*, not just every frame.

**Status: deliberately deferred as gold-plating.** SMB (NROM) exercises none of
it — scanline-granular sprite-0 already matches, no A12 IRQ, no mid-scanline
raster tricks. It is also architecturally hard for a static recompiler: we
execute whole 6502 instructions and call `maybe_trigger_vblank` once per
instruction, so we have instruction granularity, not cycle-by-cycle memory
timing. Real per-dot precision would require the recompiler to emit
cycle-stepped memory effects — a large codegen change.

**When it becomes worth it:** a title whose *visible* behavior depends on
sub-scanline timing (mid-scanline palette/scroll changes, tight raster splits,
A12 IRQ landing mid-line). The MMC3 A12 IRQ (MM3 and other TxROM games) is the
realistic first surface — and it is gated behind §2's state-sync fixture, since
you can only see it in-stage.

---

## 4. Frame-granular MMC3 IRQ-counter cross-check (attract-compatible, low-hanging)

Independent of state-sync: even on the current free-run attract path, we can
extend the co-sim to extract Mesen's MMC3 IRQ registers from the serialize blob
(offset found the same way as the PPU regions) and diff them against the recomp's
`MapperState` per frame. On MM3 attract the counters are mostly idle (IRQ not
firing), so this is a coarse check — but it is the cheapest step toward IRQ
coverage and reuses the blob-extraction we already built. Worth doing before the
full §2 fixture if we want *some* IRQ signal without scripted input.
