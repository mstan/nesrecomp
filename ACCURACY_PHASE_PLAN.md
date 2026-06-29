# Accuracy program — phased plan (Option B → Path-C oracle → dot-accurate PPU)

> Created 2026-06-28. Bail-safe roadmap: **each phase ends with a git checkpoint
> commit on `accuracy/nes-burndown` (the `_acc/nesrecomp` engine repo)** so we can
> abandon a phase and revert cleanly. ROMs are NEVER committed. The harness dirs
> (`_acc/{BlarggTest,LegendOfZeldaNESRecomp,Megaman3NESRecomp}`), `F:/Projects/nesref/`,
> and `_acc/audio_slice/` are reproducible dev tooling OUTSIDE the engine repo.
>
> Why this order: the input/desync blocker is already SOLVED (RNG-seed freeze), so none
> of these are input-gated — they're effort-gated engine work. Option B is the most
> self-contained; Path-C is a prerequisite for cleanly validating the dot-PPU (it
> provides in-process cycle + PPU-memory the libretro nesref can't); dot-PPU is the
> biggest rewrite and goes last.

## Phase 0 — Baseline checkpoint (do FIRST)

The entire current session (IRQ delivery hook + cycle-driven frame IRQ, open-bus,
RNG-freeze, nesref, cross-title harnesses, cycle/state/framebuf measurements, docs) is
**uncommitted**. Commit it as the bail-out baseline BEFORE starting Phase 1.

- Engine repo `_acc/nesrecomp`: `runtime.c`, `apu.c`, `apu.h`, `main_runner.c`,
  `NES_ACCURACY_BURNDOWN.md`, `ACCURACY_PHASE_PLAN.md`, `IRQ_SLICE_002.md`(if moved in).
- Commit message: the IRQ hook + open-bus + cross-title results. Branch only; do NOT
  mirror to master / merge to main (end-of-stream step, owner's call).
- Tooling outside the repo (nesref, harnesses, audio_slice scripts) — left untracked
  (reproducible); optionally archive separately. ROMs excluded.

## Phase 1 — Option B: sample-accurate APU  (most self-contained)

**Goal.** Rewrite the APU timing core so `apu_clock_cycles()` is the single engine that
advances the frame sequencer + ALL channel timers + DMC AND emits audio samples into a
ring buffer at the output rate (band-limited integration per output sample). Replaces the
per-frame batched `apu_generate(frame,735)`; intra-frame envelope/sweep/length/DMC land at
their true sub-frame times.

**Approach.**
1. Move all channel timers + envelope/length/sweep + DMC reader onto the CPU-cycle stream
   (extend the existing `apu_clock_cycles`, which already drives the frame IRQ).
2. Generate audio incrementally: accumulate fractional output-sample position; emit one
   band-limited sample (oversample + box/sinc decimate, preserving the tuned nonlinear DAC)
   per `CPU_FREQ/SAMPLE_RATE` cycles into a ring; `main_runner` drains the ring to SDL.
3. Remove the qf/half + channel clocking from `apu_generate` (now audio-render only, or
   retire it).

**Validation.** Re-run the audio suite vs the oracle: `apu_stream_diff.py` (reg-stream must
stay bit-identical), `audio_drift_diff.py` (onset/timbre — must stay ≈ L1 0.083, no pitch/
drift regression), `compare_*.json`. Cross-check SMB title music + a Zelda/MM3 segment.
Also re-confirm the frame-IRQ (irq-002, recomp 599 vs 598) still holds.

**Risk / bail.** Audio level/pitch/drift regression is the main risk. BAIL CRITERIA: if
timbre L1 worsens > ~0.12 or onset match drops < ~0.9 and can't be recovered in-phase →
`git reset --hard` to the Phase-0 checkpoint. **Checkpoint:** commit `apu.c`/`apu.h`/
`main_runner.c` + an `AUDIO_SLICE_004.md` writeup once verified.

## Phase 2 — Path-C: in-process source-core oracle (cycle + PPU-mem)

**Goal.** An in-process reference core exposing guest CYCLES and PPU-internal memory
(VRAM/OAM/palette) — what the libretro nesref can't. See `_acc/audio_slice/PATH_C_SOURCE_CORE_ORACLE.md`.

**Two-pronged core strategy (research FIRST, then pick):**
- **2a. Find a small accurate core.** Survey for a compact, embeddable NES core that is
  cycle-accurate (or close) AND can expose per-instruction cycles + PPU memory cheaply.
  Candidates to evaluate (accuracy + embeddability + license): tetanes, plastic, other
  Rust/C cores; check vs the known-accurate set (Mesen/puNES/Nintendulator are too big).
  If one qualifies → dual-build it in (mdref/clownmdemu pattern).
- **2b. Augment a modifiable core ("construct it out").** If no small core is accurate
  enough: take a MODIFIABLE base (Nestopia is already in-tree via `nestopia-core` +
  `nestopia_bridge.cpp`, C++, hackable) and PATCH IN cycle accuracy by cross-referencing
  the timing from a large accurate C++ reference (MesenCE `Core/NES` — `NesCpu` per-cycle
  step, `NesPpu` dot timing, A12). Instrument: a per-instruction/per-cycle counter +
  PPU-memory accessors. Validate the augmented timing against MesenCE incrementally.

**Bridge.** Mirror `nestopia_bridge.cpp`: `init/run_frame/get_ram/get_vram/get_oam/
get_palette/get_cycle`; populate a shared `FrameRecord` (mirror mdref `frame_snapshots.c`)
for a unified `divergence_diff` (RAM + PPU-mem + cycle in one surface).

**Validation.** The new bridge MUST agree with the libretro nesref on RAM (the 598=598-style
cross-check) before trusting its cycle/PPU-mem. Then: cycle diff without the APU-anchor
hack; PPU-memory (nametable/OAM/palette) byte diff vs the recomp (`g_ppu*`).

**Risk / bail.** 2b's "construct accuracy out" could stall if the base core's timing is too
far off to patch tractably. BAIL: if after a bounded research+spike the augmented core can't
reach useful cycle agreement vs MesenCE → fall back to MesenCE-Lua for cycle/PPU-mem (status
quo) and document. **Checkpoint:** commit the bridge + any engine-side `divergence_diff`
hooks once it agrees with nesref on RAM.

## STATUS (2026-06-29)
- **Phase 0 DONE** — baseline commit `a8e5141` (IRQ hook + cycle-driven frame IRQ + open-bus).
- **Phase 1 DONE** — Option B sample-accurate APU, commit `6d3ea43` (audio no-regression, self-diff 0.057). See AUDIO_SLICE_004.md.
- **Phase 2 DONE / RESCOPED** — path-C source-embed DROPPED. External MesenCE-Lua serves the two
  things nesref can't: cycle (`cpu.cycleCount`) + PPU memory (`emu.read`+nesSpriteRam/nesPaletteRam/
  nesNametableRam/nesPpuMemory). #3 cycle cross-title done; #4 PPU-mem done (SMB 99.84%), commit
  `da0459a`. See PATH_C_SOURCE_CORE_ORACLE.md (decided against), `runtime.c:nes_ppumem_trace_frame`,
  `_acc/audio_slice/mesen_ppumem.lua`.
- **Phase 3 — Increment 1 DONE (per-scanline parity)** — cycle-driven per-scanline PPU
  behind `NESRECOMP_DOT_PPU` (default OFF, byte-identical when off). New `runner/src/ppu_dot.c`
  + `runner/include/ppu_dot.h`; hooks in `runtime.c` (per-instruction `ppu_dot_advance`, $2002
  sprite-0 fallback) and `main_runner.c` (`ppu_dot_frame_boundary`, skip per-frame render).
  Model: dot clock slaved to the frame driver's `s_ops_count` with a fixed VBlank phase offset
  (cycle 0 = scanline 241; visible scanline 0 at +21 lines); free-runs through the NMI handler
  (so the in-NMI sprite-0 split renders correctly); back-buffer + copy-at-boundary (no tearing);
  sprite-0/overflow cleared at the pre-render line; MMC3 A12 IRQ fired per scanline from the dot
  clock. VALIDATED vs the per-frame renderer (which is already oracle-validated): SMB title
  byte-exact (1.0) + gameplay HUD sprite-0 split correct + 3000f 0 misses; Zelda title 0.9924
  (animated-title baseline + 1-frame pipeline offset) + 2000f 0 misses; MM3 six attract frames
  byte-exact (1.0) incl. the MMC3-IRQ MEGA-MAN-III colour-band logo + 1500f 0 misses.
  Forward-progress safety net: a $2002 sprite-0 spin that polls > ~1 frame with no rendered hit
  (heavy attract transitions where the recomp's NMI sets up VRAM later than the fixed phase
  assumes) pulses bit6 — same role the per-frame path's pulse already serves for SMB's in-NMI
  wait; never fires during normal hits. NOT YET default (parity-gated). Remaining for later
  increments: per-dot precision (sub-scanline sprite-0/A12), scroll_y>=240 "negative-Y" canonical
  path, widescreen (currently falls back to per-frame), DMC DMA cycle-steal (rides this interleave).
- **Phase 3 — further increments NOT STARTED** (per-dot precision; make default once at parity).

## Phase 3 — Dot-accurate PPU  (biggest; needs Phase 2 oracle to validate cleanly)

> **KEY FINDING (2026-06-29): dot-accuracy = a CYCLE-DRIVEN per-dot PPU (CPU/PPU interleave) —
> the PPU analog of Phase 1's cycle-driven APU.** The current renderer composites the whole
> frame AFTER the CPU runs the frame in bulk, so mid-frame writes / sprite-0 / A12 can't be
> dot-exact. The fix: advance the PPU 3 dots per CPU cycle from `maybe_trigger_vblank` (like
> `apu_clock_cycles`), rendering into the framebuffer as it goes. Unlike the APU (a clean
> relocation of an existing per-substep loop), the PPU is FROM-SCRATCH: 8-cycle BG fetch +
> shift registers, secondary-OAM sprite eval, sprite-0/overflow per cycle, palette/mirroring,
> per-fetch CHR/A12 through every mapper. Build behind `NESRECOMP_DOT_PPU` (default OFF) and
> bring to parity (SMB/MM3 structural 1.0, Zelda 0.99 + dot-IRQ vs mesen_mm3_irq.lua) BEFORE
> it can be default — the per-frame renderer is pixel-perfect on current games, so the only
> way this change can go is regress. Validate via the external Mesen-Lua PPU-mem/cycle taps
> (Phase 2). "Phase 4" (DMC DMA cycle-steal) rides the same interleave (CPU stalls at fetch
> cycles) and should follow once the PPU interleave exists.

**Goal.** Rewrite `ppu_renderer.c` from per-frame to per-dot/per-scanline cycle-accurate
rendering: dot-precise sprite-0, sprite-overflow per cycle, mid-scanline writes, A12-per-fetch
MMC3 IRQ (closes Axis-3 dot precision + Axis-5a dot-raster together).

**Approach (incremental).** Per-scanline first (BG + sprites + sprite-0 at scanline
granularity with mid-frame register latching), then per-dot for the A12/sprite-0 cases that
need it. Keep the renderer behind a flag so the per-frame path remains until parity.

**Validation.** Uses the Phase-2 oracle: per-scanline framebuffer + PPU-state diff at synced
frames (RNG-freeze), dot-precise MMC3 IRQ vs `mesen_mm3_irq.lua` (scanline+dot+cycle). Must
keep SMB/Zelda/MM3 structural framebuf at/above current (1.0/0.992/1.0).

**Risk / bail.** Largest blast radius (every game renders through it). BAIL: gate behind a
build flag; if parity regresses and can't recover → keep the per-frame renderer default,
commit the dot path as opt-in/WIP. **Checkpoint:** commit per milestone (per-scanline parity,
then dot-precise IRQ, then full dot).

## Sequencing notes
- One phase per work-stream; expect each to span one+ session. Resume via the checkpoint
  commit + this doc.
- Mirror-to-master / merge-to-main remains the deferred end-of-stream step (owner's call),
  independent of these branch checkpoints.
