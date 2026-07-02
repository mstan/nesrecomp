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
(`NESRECOMP_FREEZE`/`NESREF_FREEZE`) so attract stays aligned. Does **not** help
host-modeled state (MM3 fibers): you cannot freeze host scheduler state into
alignment; that is why MM3 needs (a) or (b), not (d). **And it is only partial for
FrameCounter-driven attract (Zelda, below)** — freezing the RNG *seed* (`$18`)
does not align the *FrameCounter* (`$15`), and animated/demo attract derives from
`$15`, so the two engines still display different moments.

### Worked example — Zelda (MMC1, CHR-RAM): FrameCounter-phase desync

Measured 2026-07-01; **root-narrowed 2026-07-02**. Gate 1 PASS; **cycle CONVERGED
(+0.004, phase-independent — Rung 1/2 hold)**. But free-run attract RAM/PPU do NOT
give a clean verdict: `abram` 98.5% with the top divergent byte being **`$0015`
(the FrameCounter) at 100%**, plus everything derived from it (`$0600`-page timers,
`$02f`-page); `abppu` OAM 78% / palette 68% / **NT 3%** because the animated title
+ demo are a phase-shifted sequence.

**Measured mechanism (raw early-boot `$15` dump, no alignment, `$18` frozen):**
both sides hold `$15=$00` through frame 11 and both increment to `$01` at frame
12 (same start) — but then **the recomp increments `$15` every frame** (12→`01`,
13→`02`, …, 19→`08`) while **Mesen increments it only ~once per 7 frames** during
f12–35 (`01` held to f20, `02`@f21, `03`@f28, `04`@f36) before switching to
every-frame at ~f36. So during boot-init the recomp runs its `$15`-incrementing
NMI handler ~7× more often than Mesen, leaving `$15` a **constant +6 ahead** in
value thereafter (f444 rc `$b1` vs `$ab`; f884 rc `$69` vs `$63`). This is why
`$15` wants a different frame offset (+21) than the bulk of RAM (+15) — the two
are phase-inconsistent. **This is a boot-cadence divergence (recomp advances the
NMI-off/boot-init sequence faster than Mesen), the SAME class as Gumshoe's
transition slip — not a rendering bug.** Everything `$15`-derived (title
animation, demo) is therefore a phase-shifted sequence, which is the entire "NT
3%" reading.

**Why it stops here (RULE 0):** determining *why* the boot loop advances 7× faster
— a runtime NMI/PPU-flag timing issue vs a game-code boot-path difference (e.g. a
wait-for-vblank / delay loop the recomp resolves too fast) — requires reading
Zelda's 6502 boot code in Ghidra. Not yet done. **Next diagnostic when Ghidra is
loaded:** trace `g_ppuctrl` bit 7 (NMI-enable) per frame across f12–36 on the
recomp — if it is set every frame, the game is enabling NMI every frame (a boot
wait-loop resolving early → find what it waits on, `$2002`/sprite-0/a counter);
if it toggles, something else drives the `$15` increment. Then map to Zelda's boot
sequence. **Tooling workaround for a clean verdict meanwhile:** §1(a) scripted-to-
a-static-state, or a one-time boot-time counter alignment (force recomp `$15` =
oracle `$15` once, distinct from a per-frame freeze which stops it advancing).
Until then, Zelda's clean RAM/PPU verdict is deferred; cycle/determinism are green.

### Worked example — Gumshoe (mapper 66 GxROM): NMI-off frame-count shift

Measured 2026-07-01. Gumshoe (deterministic no-input attract demo) actually
**CONVERGES** — but a naive whole-run diff hides it. The co-sim aligns the
recomp's `g_frame_count` (which counts NMI *callbacks*) with the oracle's video
frames. That is exact only while NMI stays on. Gumshoe's attract has ONE NMI-off
transition (~frame 370, a ~4-video-frame scene change; GxROM inline-dispatch runs
a long linear stretch with NMI disabled). During it the recomp emits ONE callback
while Mesen advances ~4 video frames, so the **frame offset jumps +2 → +5
mid-run**. `abcycle` shows it as a single 119123-cycle outlier (all other 898
frames are 29780.5 ± 2.3 — clean); `abram`/`abppu` with a FIXED offset look
broadly divergent because everything after frame 370 is misaligned. Segmenting
proves it: PRE-370 at offset +2 = 99.72%, POST-370 at offset +5 = 99.10% — both
clean. Dot clock is NOT implicated (off/on identical).

**Fix — DONE (2026-07-02), both halves:**
1. *Coordinator, piecewise offset* (`adaptive_offset_match` in `abram`,
   `adaptive_offset_ppu` in `abppu`): a windowed offset that follows a mid-run
   shift; if discrete shifts recover a high match it was alignment, if it stays
   low it is a real divergence. This classifies but does not remove the desync.
2. *Runtime, fire-count alignment* (the fundamental fix): new `g_cosim_vframe`
   ticks on **every** frame boundary — NMI on or off — and `nes_cosim_emit_boundary()`
   emits the WRAM/PPU/hash trace rows on NMI-off frames (where `nes_vblank_callback`,
   which normally emits them, is skipped). All three trace functions now tag rows
   with `g_cosim_vframe` instead of `g_frame_count`. This makes the recomp's co-sim
   frame clock equal the oracle's video-frame count, so the offset is a **constant
   boot latency** across NMI-off stretches. `g_frame_count` (the game-logic clock)
   is untouched, and the whole path is env-gated → zero gameplay impact.
   **Measured effect on Gumshoe: `abram` offset shifts 1 → 0 (was +2→+5 @400),
   RAM 99.36% CONVERGED with a constant offset; regression-clean on MM3/Faxanadu
   (Faxanadu OAM 99.88→100.00%, NT 99.70→99.92%).**

**What the fix REVEALED — a genuine (small) Gumshoe residual, no longer masked.**
With the frame-count desync removed, Gumshoe's `abppu` nametable is *still* ~71%,
recovering to ~91% only under an ~18-column horizontal scroll shift (`scroll_phase_match`).
This is now isolated as a **real state divergence**, not an alignment artifact.

**Measured mechanism (per-frame trajectory of the diverging bytes, 2026-07-02):**
the diverging bytes are frame timers — `$0076` is a countdown (`$ff,$fe,$fd…`),
`$0601`/`$060d` are count-ups — and on every one the recomp is exactly **1 frame
AHEAD** of the oracle, a **constant** lead (e.g. `$0076`: rc hits `$ff` at f367,
Mesen at f368; still +1 at f451 rc `$ae` vs `$af`). All of it originates at the
**NMI-off scene transition ~f367–373**: the recomp runs the transition one frame
faster than Mesen, after which every frame-derived timer — and thus the attract
scroll — sits a constant 1 frame ahead. The `g_cosim_vframe` fix corrected the
frame *count* (0 offset shifts) but not this sub-frame slip in how long the NMI-off
stretch lasts. It is **not a logic or rendering bug** — the recomp renders
correctly, one frame early. SMB (NT = 100% via the same extraction) confirms it is
not an extraction artifact.

This is the ±1 boundary-phase class also seen transiently on MM3/Faxanadu, here
root-located to the NMI-off transition. **Chasing why the recomp's NMI-off
transition is 1 frame short needs Ghidra + Gumshoe transition-code analysis**
(and/or per-instruction cycle-accounting audit over the NMI-off stretch) — deferred
as Gumshoe is attract-focus and ships live, and the slip is sub-frame timing, not
correctness. Everything else on Gumshoe (RAM logic, OAM, palette, cycle,
determinism) converges.

Three distinct free-run breakdowns had worked examples: **MM3** (host-fiber
scheduler, no bit-faithful state), **Zelda** (FrameCounter-phase), and **Gumshoe**
(NMI-off frame-count shift). Gumshoe's alignment breakdown is now **fixed at the
runtime**; what remains for Gumshoe is the genuine scroll residual above. The
general point still holds: free-run *fixed-offset* frame-alignment is exact only
for deterministic, phase-locked attract (SMB, Faxanadu); NMI-off games now get a
constant offset via the fire-count fix, while host-modeled (MM3) and
FrameCounter-phase (Zelda) games still need §1 state/phase sync for a full RAM/PPU
verdict. The phase-independent cycle and determinism axes stay valid everywhere.

**Battery-save note (coordinator, 2026-07-02):** battery games (Zelda, Faxanadu)
write `<exe_dir>/saves/*.srm`; a second run loading it takes a different boot path
and fails Gate 1 with a spurious `sram` "nondeterminism" (really save carryover —
one run has SRAM, the next boots into it). The coordinator now calls
`clear_saves()` before every recomp launch so each session boots from the same
canonical fresh state (matching `nesref`, which boots fresh). Zelda Gate 1 now
PASSES (911 == 911, was 911 vs 903). This is the programmatic form of the old
"delete *.srm before running battery games" gotcha.

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
