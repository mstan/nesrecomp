# NES_ACCURACY_BURNDOWN.md — nesrecomp 7-axis accuracy scorecard

> Living scorecard. Modeled 1:1 on `psxrecomp/_wt-tomba2/.../ACCURACY_BURNDOWN.md`.
> Branch: `accuracy/nes-burndown` (engine `_acc/nesrecomp`, game `_acc/SuperMarioBrosRecomp`).
> Stomping ground: **Super Mario Bros** (Mapper 0, simplest APU/PPU usage).
> Last updated: 2026-06-28.

---

## 0. The non-negotiable principle

**Self-agreement is NOT accuracy.** "The recompiled C agrees with our own runner's
interpreter / our own APU" proves *backend equivalence*, not *correctness* — both can be
identically wrong. Every axis goes GREEN only when it is:

1. **Cross-referenced against an external reference** — the NESdev Wiki hardware docs
   and/or a hardware test ROM (blargg `cpu_test5`, `apu_test`, `ppu_vbl_nmi`,
   `sprite_hit`, `mmc3_irq`, Klaus Dormann 6502 functional test), AND
2. **Runtime-validated against an accurate oracle** — diff our output (state / cycle /
   PCM) against **Mesen2** on the *same input*, comparing *outputs*, not agreements.

A research-claimed discrepancy is a **hypothesis**, not a bug, until the oracle diff
confirms it on identical input.

### Always-on ring buffers, never arm-then-capture

No probe may "arm a trace, run a workload, dump the trace." Every instrument is an
always-on ring (bounded by eviction, compiled into Release) that records continuously;
probes **query the window of interest** after the fact. The recomp already follows this
for audio (`recomp_audio_debug.h` — five always-on taps T0–T4, dumped as a trailing
window). Extend that model to every axis; never synthesize state by pause/step.

---

## 1. The oracle

| | Old | New (this branch) |
|---|---|---|
| Emulator | **Nestopia UE** (`runner/nestopia-core`) | **Mesen2 / MesenCE 2.2.1** |
| Accuracy | mid-tier (~94.9% NESAccuracyTests); cycle-ish | **cycle-accurate** (PPU dot-level, APU, sub-instruction CPU) |
| Exposes | state + video only (audio libretro cb stubbed; no cycle counter) | guest cycle counter (`NesCpuState.CycleCount`), full PPU/APU state, PCM, APU reg writes |
| Drive | in-process `extern "C"` bridge + TCP cmds (`nestopia_oracle_cmds.c`) | `--testrunner <rom> <lua>` headless + Lua callbacks; PCM via Sound Recorder / source hook |

**Decision (locked):** Mesen2 replaces Nestopia as the single accurate oracle for
state-divergence, cycle timing, and audio. Nestopia bridge stays in-tree until Mesen2
parity (state + cycle + audio) is reached, then is retired.

Oracle binary on disk: `_acc/audio_slice/mesen/Mesen.exe` (MesenCE 2.2.1 Windows).
Repo: https://github.com/nesdev-org/MesenCE (GPL-3.0).

**PREFERRED ORACLE (2026-06-28): in-process `nesref`** at `F:/Projects/nesref/` — a
~250-line libretro frontend (`frontend.cpp`, ported 1:1 from `snesrecomp/tools/snesref`)
hosting the **cycle-accurate Mesen NES libretro core** (`cores/mesen_libretro.dll`,
"Mesen 0.9.9") in-process. Headless, deterministic, no GUI/Lua/UIA. Taps per frame: CPU
RAM (`retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM)`, 2KB) → JSONL in the SAME shape as
the recomp's `NESRECOMP_WRAM_TRACE` (drop-in for `wram_diff.py`); video/audio/savestate
callbacks available. Env: `NESREF_FRAMES=N` (headless N-frame capture), `NESREF_DUMP="a,b"`
(print RAM $00..$0F at those frames), `NESREF_TRACE_FILE`. **VALIDATED = agrees with the
MesenCE GUI oracle**: on `irq_frame_test.nes` both read counter **598** at frame 600 (recomp
599). This is the sibling-standard pattern (snesref=snes9x/bsnes libretro, mdref=clownmdemu
source dual-build, psxref=Beetle); `/f/Projects/{snesref,mdref}/cores/` already existed,
`nesref/` is new. Use nesref for Axes 1/4/5a/5b/7. The earlier "NOT the libretro Mesen core
(no getState/cycle)" caveat was about the *Lua* API — irrelevant to the libretro frontend,
which reads RAM/video/PCM/savestate directly. **Gap:** the libretro ABI exposes no guest
*cycle counter*, so Axis 2 (cycle) stays on the APU-anchor delta method (or a future
mdref-style source hook). MesenCE GUI / `--testrunner` Lua remains available for cycle.
Use MesenCE, **not** the archived `SourMesen/Mesen2`.

### What Mesen2 gives each axis

- **State/divergence:** `emu.getState()` (CPU PC/A/X/Y/SP/PS, PPU v/t/x/w/status, APU,
  cartridge/mapper); memory via `addMemoryCallback`. C++ `NesConsole::GetConsoleState()`
  returns the whole `NesState` if Lua proves too thin.
- **Cycle timing:** `NesCpuState.CycleCount` (uint64 guest cycles) +
  `NesConsole::GetMasterClock()`. Deterministic on movie/state replay. This is the NES
  analogue of the Beetle `beetle_get_guest_cycles()` hook the PSX project added.
- **Audio:** PCM via Sound Recorder (uncompressed WAV, sample-rate = Mesen audio config;
  deterministic given pinned settings) **or** a source hook in `NesSoundMixer`
  (blip_buf). APU register-write stream via Lua `addMemoryCallback("write",0x4000,0x4017)`
  timestamped by `CycleCount`. **No Lua audio-sample callback exists** — PCM needs the
  recorder or a Core hook.

---

## 2. Comparative reference shelf

1. **NESdev Wiki** — canonical hardware reference (CPU, PPU rendering/timing, APU,
   2A03 quirks, mapper specs). Cite the page/section.
2. **Mesen2 / MesenCE source** (`Core/NES/**`) — `NesCpu`, `NesPpu`, `APU/`,
   `NesSoundMixer`, `NesMemoryManager` — the in-tree accurate model to transcribe from.
3. **Hardware test ROMs** (ground truth above any emulator):
   - CPU: Klaus Dormann 6502 functional test, blargg `cpu_test5`, `instr_test-v5`,
     `instr_timing`, `branch_timing_tests`.
   - PPU: blargg `ppu_vbl_nmi`, `sprite_hit_tests`, `sprite_overflow_tests`,
     `ppu_open_bus`, `oam_read`/`oam_stress`.
   - APU: blargg `apu_test`, `apu_mixer`, `dmc_dma_during_read4`, `dpcmletterbox`,
     `square_timer_div2`, `dmc_basics`.
   - Mapper: `mmc3_test` / `mmc3_irq_tests`.
4. **Mesen2 oracle (runtime)** — first-divergence on the state surface (RAM/PPU/OAM for
   state, `CycleCount` deltas for timing, PCM + APU-reg stream for audio).

---

## 3. The 7-axis burndown

Status scale: **0 NOT-MODELED · 1 WEAK · 2 PARTIAL · 3 STRONG · 4 GREEN** (GREEN ⇒ both
gate conditions in §0 met).

---

### AXIS 1 — Instruction semantics (6502 / 2A03 core)

**Status: 3 STRONG** (static recompiler; emitted C, not an interpreter).

- Decoder `recompiler/src/cpu6502_decoder.h`; emitter `recompiler/src/code_generator.c`
  (instruction switch ~`658-1446`). JSR→direct C call, RTS→C return; branches→`goto`.
- All 151 official opcodes with correct N/Z/C/V (`FLAG_NZ` `code_generator.c:78`,
  `FLAG_NZC_ADD/SUB` `:79-84`).
- **Decimal mode correctly disabled** for 2A03 (`code_generator.c:709,:720`) — matches HW.
- Stack ops on real `g_ram[$0100+S]`; LAX + 2 sized NOP_READ implemented.

**Gaps (cross-ref → fix):**
- 96 of 105 unofficial opcodes emitted as sized skips (SAX/DCP/ISC/SLO/RLA/SRE/RRA/ANC/
  ALR/ARR/AXS missing) — ref NESdev "CPU unofficial opcodes"; SMB does not use them but a
  GREEN gate needs them or a proof they're unreachable per title.
- `JMP ($xxFF)` page-wrap bug **not** modeled — `nes_read16` (`runtime.c:403`) reads
  `addr+1` linearly; HW wraps low byte (`code_generator.c:1242`). Ref NESdev "CPU
  addressing modes".
- `BRK` skipped, not a software interrupt (`code_generator.c:1443`).

**Validation:** Klaus Dormann functional test + blargg `instr_test-v5` on recomp vs
Mesen2; diff final RAM signature. GREEN when both pass and the SMB instruction stream
shows zero unofficial/`BRK`/`JMP($xxFF)` reachable sites (instruction-coverage tool).

---

### AXIS 2 — Cycle / timing

**Status: 3 STRONG** (MEASURED slice cycle-001 vs Mesen2).

- Every instruction charges `maybe_trigger_vblank(e->cycles)` (`code_generator.c:1059`)
  from the decoder base-cycle table. Per-frame accumulator `s_ops_count`; frame budget
  `OPS_PER_FRAME = 29781` (`runtime.c:414`).
- **Static page-cross penalties ARE emitted** (`code_generator.c:574,617,623,633` —
  `tramp_load` adds +1 on page cross for compile-time-known addresses). Corrects the
  earlier recon claim that penalties were absent.

> **Measured (slice cycle-001, delta-cycle via the bit-identical APU stream, 106 anchors,
> 1773 frames):** drift **0.5064 cyc/frame** vs predicted 0.5 (29781−29780.5) → the cycle
> error is **dominated by the frame-length constant**; the per-instruction model otherwise
> tracks hardware on average. Sub-frame residual **std ~111 cyc (±0.4 % frame)** = the
> data-dependent penalty jitter. `_acc/audio_slice/CYCLE_SLICE_001.md`.

**Remaining gaps / levers:**
- **Frame length 29781 vs NTSC 29780.5** (the 0.5 cyc/frame drift). Top lever; deferred —
  load-bearing NMI driver, 0.0017 % impact, needs a dedicated regression-tested change
  (alternating budget + monotonic `g_cpu_cycles`).
- **Data-dependent page-cross** (runtime index) + flat-`+2` branch penalty (`:1213`) →
  the ±111 cyc residual; tightening it helps Axis 3 / Axis 5a raster timing.
- RMW dummy-read cycles; DMC DMA stalls not charged (Axis 5b).

**Tooling built:** `cycle_compare.py` (delta-cycle via APU anchors). blargg `instr_timing`
/ `branch_timing_tests` remain as a future cross-ref.

---

### AXIS 3 — Interrupt / event timing (NMI / IRQ / DMC)

**Status: 3 STRONG** (NMI corroborated; MMC3 IRQ pre-render fix landed → scanline-correct vs oracle; dot-granularity deferred).

- NMI fires when `s_ops_count >= OPS_PER_FRAME` or is deferred to the next backward
  branch for spin-wait games (`runtime.c:382-462`, deferred fire `:439-469`, 1.5× safety
  cap `:456`). `$2002` vblank bit set at NMI; sprite-0/overflow cleared (`:622-624`).

**Gaps:**
- NMI take-point is the **frame boundary / loop back-edge**, not the exact instruction
  at the exact PPU dot (HW raises NMI at scanline 241 dot 1). Ref NESdev "NMI".
- Mapper/APU-frame/DMC **IRQ has no CPU-level ISR dispatch**; mapper sets
  `g_cpu_irq_pending` but only MMC3 A12 counting exists (`runtime.c:57-59`). DMC IRQ
  configured but never fires (Axis 5b).
- No exception-entry record (which vector, at which cycle) to diff.

**Validation:** blargg `ppu_vbl_nmi`, `cpu_interrupts_v2`, `mmc3_irq_tests` vs Mesen2;
add an always-on `event_ring` (NMI/IRQ raises with `CycleCount`) and diff first
mismatch. SMB uses NMI only — GREEN on SMB is reachable before full IRQ slicing.

> **Corroborated (slices ppu-001 + cycle-001):** NMI-driven frames are pixel-perfect and
> the per-frame cycle accounting matches the oracle (only the 0.5 cyc/frame frame-length
> constant). NMI timing on SMB is therefore validated indirectly.
>
> **IRQ MEASURED (slice irq-001, MM3 / MMC3 vs Mesen):** MMC3 scanline IRQ fires at the
> right cadence (once/frame) and top-of-frame position, but the recomp lands **~1 scanline
> (~114 cyc) late** (fires at visible scanline 0; Mesen at pre-render −1, dot ~290) and is
> **scanline-granular** (no sub-scanline dot variation). Root: recomp clocks the MMC3
> counter per visible scanline only, not the pre-render line / A12 dots. Benign for a
> top-of-frame IRQ; offsets a precise mid-screen split. `_acc/audio_slice/IRQ_SLICE_001.md`.
>
> **FIX LANDED + MEASURED (2026-06-28):** added the pre-render-line (-1) MMC3 counter
> clock in `ppu_renderer.c` (`service_mmc3_scanline_irq` helper; counter now clocks
> scanlines -1..239 = 241/frame, per NESdev). MM3 recomp IRQ fire scanline **0 → -1**,
> now exactly matching the Mesen oracle (60/60 samples, `irq_probe.py`); title renders
> clean (no regression); git-diff-isolated to this edit (engine roll-forward didn't touch
> the MMC3 clock path). The **~1-scanline-late offset is CLOSED.** Remaining gap = sub-
> scanline **dot** precision (A12 fetch timing), which needs a cycle-accurate PPU
> (deferred, couples with Axis 5a). No-op for non-MMC3 mappers (SMB unaffected).
>
> **GENERAL PENDING-IRQ DELIVERY HOOK LANDED (2026-06-28):** the maskable IRQ sources that
> are NOT scanline-timed (APU frame counter, DMC sample-end) now have a real CPU-level ISR
> dispatch — the gap noted above ("Mapper/APU-frame/DMC IRQ has no CPU-level ISR dispatch").
> `maybe_deliver_irq()` (`runtime.c`) is polled from the per-instruction
> `maybe_trigger_vblank` hook (the recompiler emits it at every instruction boundary, so the
> IRQ line is sampled between instructions — where the real 6502 samples it). Gated on
> `!g_cpu.I` (maskable, unlike NMI), no-reentry, and top-level only (`s_vblank_depth==0`, so
> an IRQ never preempts the batched NMI frame driver — that interleave is Axis 2). Level-
> triggered: the handler clears the source (`$4015` read → frame flag; `$4015` write / `$4010`
> IRQ-disable → DMC flag); pushes P/PCL/PCH in the MMC3-path convention and calls `func_IRQ()`.
> SMB: zero regression (all SMB IRQ sources inhibited → hook inert, title renders identical).
>
> **CYCLE-DRIVEN APU IRQ SOURCE (Option A, 2026-06-28):** the APU frame-counter IRQ flag is
> driven on the CPU-cycle stream via `apu_clock_cycles()` (called from `maybe_trigger_vblank`),
> NOT per-NMI — so it advances for NMI-disabled main-thread code (every blargg APU test, any
> `$4015` poll). 4-step sequence = 29830 CPU cycles; flag asserted at the step-4 offset unless
> inhibited; `$4017` write resets the phase. (DMC reader/IRQ cycle-driving is the rest of
> Option A — see Axis 5b.) Replaces the earlier per-NMI `apu_frame_tick`.
>
> **blargg IRQ tests are ARCHITECTURALLY INFEASIBLE on the static recompiler (verified
> 2026-06-28, from the recompiled `apu_3-irq_flag` handler `func_E8DB`).** Two independent
> blockers: (1) **No interrupted-PC.** blargg's IRQ handler reads the pushed return address
> (`PLA→$1C` = interrupted PCL) to measure *when* the IRQ fired — the entire point of a
> cycle-timing IRQ test. Static recomp has no meaningful interrupted PC; the delivery hook
> pushes a `0x0000` placeholder, so blargg reads garbage and derails (observed: a `$0002`
> ZERO_FILLED dispatch miss). Intrinsic to recompilation, unfixable by `extra_func` tuning.
> (2) **Cycle-exact APU** sub-cycle frame timing needs the full sample-accurate engine
> (Option B, Axis 5b). Net: blargg APU/IRQ tests that *vector through IRQ* cannot pass;
> only *polling* APU tests benefit from the cycle-driven source. The delivery hook is
> therefore validated by a purpose-built minimal IRQ ROM vs Mesen2, not by blargg. Harness
> stood up at `_acc/BlarggTest/` (reusable recomp regen→build→run for any test ROM).
>
> **VALIDATED vs Mesen2 (slice irq-002, 2026-06-28).** Custom ROM `_acc/custom_roms/
> irq_frame_test.nes` (hand-assembled NROM: 4-step frame IRQ enabled, NMI on, spin loop;
> IRQ handler increments a 16-bit counter at $00/$01 and acks via `$4015` read — does NOT
> read the pushed PC, so it recompiles cleanly). After ~600 frames: **recomp = 599 IRQs,
> Mesen2 oracle = 598 IRQs** — a 0.17 % (1-count) match. 599 (not millions) confirms the
> `$4015` ack clears the level flag (no storm); 599 (not 0) confirms delivery fires + RTI
> returns cleanly. The 1-count residual is the documented ~0.5 cyc/frame Axis-2 frame-
> length constant (29781 vs 29780.5), not an IRQ-hook error. The general pending-IRQ
> delivery hook + cycle-driven frame source are **MEASURED-correct against the cycle-
> accurate oracle.** Oracle Lua `_acc/audio_slice/mesen_irqtest.lua`; full writeup
> `_acc/audio_slice/IRQ_SLICE_002.md`.

---

### AXIS 4 — Memory map / MMIO

**Status: 3 STRONG** (MEASURED slice state-001: direct RAM diff vs Mesen).

> **Measured (nesref state-divergence, `wram_diff.py`):** recomp CPU-RAM matches Mesen
> **99.04%**; the **only** divergences are 17 dead stack-page bytes ($01ED-$01FF) — all of
> zero-page + general RAM match **byte-for-byte**. The stack-page diff is the static
> recompiler's JSR=C-call boundary (no 6502 return-address pushes; below live SP, never
> read), benign for SMB. This also independently confirms Axis 1 (wrong opcodes would
> corrupt zero-page). `_acc/audio_slice/STATE_SLICE_001.md`.

> **CROSS-TITLE MEASURED — Zelda (MMC1) via nesref (2026-06-28).** First non-SMB state diff,
> on the in-process nesref oracle (Mesen libretro). Free-running attract-demo diff is UNSOUND
> for RNG games: Zelda's `$0018 Random` is seeded by `$0015 FrameCounter`, so any boot-timing
> offset (here −17 frames) desyncs the demo and cascades into total state divergence (97.8%,
> entire `$0200-$0325` object region differs — coincidental-static "match"). **Fix = RNG-seed
> freeze** (user's idea): env `NESRECOMP_FREEZE="0x18=0x00"` (recomp, `runtime.c:nes_apply_freeze`)
> + `NESREF_FREEZE="0x18=0x00"` (nesref) force Random identical each frame on both sides →
> demo stays in lockstep → differential diff becomes valid. Result: **98.47% raw; ZERO
> game-logic divergence.** The only 21 divergent bytes = 12 stack-page (JSR=C-call boundary,
> same class as SMB's 17) + 9 frame-phase counters that differ ONLY by the constant boot offset
> ($0015 FrameCounter, $041A DemoTimer, $0412 BlueWizzrobe turn-counter, $0611-$061D song
> note/vibrato counters — all FrameCounter-derived). **MMC1 banking + RAM are byte-exact** —
> proven directly, corroborating the 0-dispatch-miss (Axis 6) + pixel-perfect render (5a)
> results. Methodology lesson: cross-engine state diff REQUIRES RNG-seed freeze (the
> free-run/frame-lock approach is unsound for RNG-heavy titles). Reusable for Kirby/MM3.

> **CROSS-TITLE MEASURED — Mega Man 3 (MMC3 + coroutine kernel) via nesref (2026-06-28).**
> The hardest title (push_all_jsr, fiber-modeled coroutine scheduler, MMC3). nesref(Mesen) vs
> recomp --smoke 600, offset +7: **98.4% raw, ZERO persistent game-logic divergence.** All
> other_ram persistent = 0 (objects/sprites byte-exact); no RNG-freeze needed (title is
> low-RNG). The only PERSISTENT divergences are two STRUCTURAL backend boundaries: (1) stack
> page — 25 bytes (more than SMB's 17 because push_all_jsr uses more stack; JSR=C-call, never
> read); (2) **coroutine-scheduler zero-page `$7c-$93`** — `$90/$91/$92/$93` are the task
> entry/resume vectors (`coroutine_start(g_ram[0x91],…)`, `JMP ($0093)`), and the recomp models
> the coroutine kernel via HOST FIBERS (`coroutine.c`), so the GUEST scheduler bookkeeping RAM
> isn't bit-faithful — a new state-fidelity boundary EXACTLY analogous to the stack page
> (correct behavior via a different mechanism; bookkeeping differs, never game-visible). Masking
> stack + coroutine ZP → **99.9%**; the remaining ~0.1% is TRANSIENT (not persistent) sound-
> engine playback-phase counters ($6c0-$6cb/$780-$79d, PostNMI sound engine, +7 frame phase).
> So MMC3 banking + coroutine-kernel + game-logic RAM are CORRECT. Corroborates Axis 6 (0
> dispatch misses / 4000 frames attract) + Axis 3 (MMC3 IRQ scanline −1, irq-001).

- `nes_read/write` (`runtime.c:540-648`): 2KB RAM mirrored ×4; PPU regs with side-effects
  — `$2002` clear-on-read latch (`:803-872`), `$2005`/`$2006` shared w-toggle + 15-bit
  t/v Loopy model (`:721-766`), `$2007` buffered read / immediate palette (`:767-799`,
  `:875-890`); `$4014` OAM DMA (`:630-645`); `$4016/$4017` controller strobe/shift;
  palette mirror `$3F10/14/18/1C→$3F00/...` (`:689`); CHR-ROM write protect (`:784`).

**Gaps:**
- **Open-bus PARTIAL (landed 2026-06-28).** `runtime.c` now tracks the CPU data bus
  (`s_open_bus`, updated on every read via the `nes_read` wrapper + every `nes_write`);
  unmapped `$4020-$5FFF` and write-only APU regs (`$4000-$4013`) return open bus instead of
  0/0xFF. SMB zero-regression. **Deferred:** `$2002` lower-5-bit open-bus — the recomp's
  `ppu_read_reg($2002)` encodes boot-readiness in low bits, so mixing open-bus there hangs SMB
  RESET; needs investigating the recomp $2002 model first. Unmapped-read behavior is
  NESdev-spec but not yet oracle-tested (needs a test ROM reading `$4020-$5FFF`).
- `$4015` APU status read (length-counter/IRQ flags) — verify it reflects real channel
  state and clears the frame-IRQ flag.
- Mapper read side-effects beyond MMC3 A12 not generalized.

**Validation:** blargg `ppu_open_bus`, `oam_read` vs Mesen2; MMIO write trace (always-on
ring of `addr,val,PC,cycle`) diffed against Mesen2 `addMemoryCallback`. Port `mmio_tally.py`.

---

### AXIS 5a — Peripherals: Video / PPU

**Status: 3 STRONG** (MEASURED slice ppu-001: pixel-perfect vs Mesen2).

> **Measured (slice ppu-001, palette-independent structural framebuffer match):** static
> frames **1.000** (clean bijection, all 5); scrolling **0.996-0.998** after a 2-3 px
> de-shift (the residual is a 1-frame input-alignment harness artifact, not a render bug);
> **HUD sprite-0 split = 1.000 even while scrolling**. PPU rendering is pixel-perfect; also
> corroborates Axis 4 (MMIO/PPU-state). `_acc/audio_slice/PPU_SLICE_001.md`.

(Original posture, still the structural gap for non-SMB games:)

- Renderer `runner/src/ppu_renderer.c` composites a full 256×240 frame after each NMI.
- Sprite-0 hit **simulated**: consecutive-`$2002`-read counter captures scroll/ctrl and
  sets a split (`runtime.c:477-859`), with an OAM[0] fallback scan (`:834-859`); split
  scanline `(spr0_y+15)&~7`, 8×16-aware (`:` per ISSUES #9). Cycle→scanline
  `(cyc*3)/341` (`runtime.c:57-59`).

**Gaps:**
- Rendering is **per-frame, not dot/pixel-accurate**: mid-scanline writes, precise
  sprite-0 dot, sprite-overflow per-cycle, and mid-frame scroll splits beyond the HUD
  case are not pixel-exact. Ref NESdev "PPU rendering", "Sprite zero hits".
- Sprite-0 via heuristic, not true BG/sprite opaque-pixel overlap.

**Validation:** blargg `sprite_hit_tests`, `sprite_overflow_tests`, `ppu_vbl_nmi`;
per-frame framebuffer diff vs Mesen2 (the Nestopia `framebuf_diff` cmd already exists —
re-point at Mesen2). Per-scanline diff is the GREEN bar.

---

### AXIS 5b — Peripherals: Audio / APU (2A03)  ← **characterized DONE this branch**

**Status: 3 STRONG** (bit-identical APU inputs; nonlinear DAC; band-limited synth; timbre
L1 0.083 vs oracle. Residual: small mid-band tilt; DMC DMA steal unmodeled.)

> **Slice 001** (SMB 1-1 vs Mesen2): rhythm faithful — onset match 96.6%, timing median
> 0.0 ms; pitch octave-folded median 0.0 cents. `_acc/audio_slice/AUDIO_SLICE_001.md`.
>
> **Slice 002** — the definitive result: the recomp's **APU register-write stream is
> BIT-IDENTICAL** to the oracle (note-sequence LCS **1.000** on every melodic register;
> value-set Jaccard 1.0). APU *input* fidelity is semantic-GREEN. Findings:
> (a) frame timing ~**0.5 cyc/frame** slow (`OPS_PER_FRAME=29781` vs NTSC 29780.5) → Axis-2;
> (b) mixer switched linear→**hardware nonlinear DAC** + de-clipped (`apu.c:mix_sample`),
> but measured (alignment-invariant timbre L1 0.180→0.178) the mixer was **not** the timbre
> lever; (c) residual ~0.18 timbre gap is **per-channel band-limited synthesis** (Mesen
> blip_buf vs recomp naive) — the real next lever. Also added the always-on
> `NESRECOMP_APU_TRACE` ring (`runtime.c:674`). `_acc/audio_slice/AUDIO_SLICE_002.md`.
>
> **Slice 003 (audio finished):** implemented band-limited synthesis (oversample×32 +
> box-decimate, `apu.c:apu_generate`). Removed the slice-002 capture-offset confound by
> comparing full music segments **independently** → true timbre **L1 0.083** (floor ~0.04):
> the recomp audio is **highly faithful**. Band-limiting was correct anti-aliasing but
> NOT the lever — the residual is a small mid-band tilt (110-220 low / 440-880 high), not
> high-band aliasing. Verdict: APU **input bit-identical**, rhythm 0.99, timbre near floor.
> `_acc/audio_slice/AUDIO_SLICE_003.md`.

- `runner/src/apu.c`: pulse×2 (duty/env/sweep/length `:52-76`), triangle (linear+length
  `:78-90`), noise (LFSR mode0/1+env+length `:92-107`), DMC (`:109-131,214-251`).
- **Mixer is the linear approximation** (`apu.c:274-305`): `0.00752*(p1+p2)+0.00851*tri+
  0.00494*noise+0.00335*dmc`, ×2, int16. The **nonlinear NESdev DAC** path exists only as
  opt-in shadow verification (`apu_shadow.c:44-58`, env `NESRECOMP_AUDIO_SHADOW`,
  default OFF → canon linear returned byte-identical).
- **Sync: batched per-frame** — `apu_generate(frame, 735)` once per NMI
  (`main_runner.c:706`); 4 quarter-frame events spread across 735 samples. Output bridge
  `recomp_audio_drc.h` (polyphase resampler 44100→device + P-servo + stall conceal +
  200 ms preroll). Frame counter **4-step only** (`apu.c:138`); 5-step parsed not clocked
  (`:424-429`).

**Gaps (cross-ref → fix):**
- **DMC DMA CPU-cycle stealing not modeled** — DMC timer free-runs, never stalls the CPU
  (`apu.c:129`); DMC IRQ never fires. Ref NESdev "APU DMC", "DMA". (SMB doesn't lean on
  it; MM3/Kirby will.)
- **Linear mixer ≠ hardware nonlinear DAC** on the canon path. Ref NESdev "APU Mixer".
- **5-step frame counter** not internally clocked.
- **No APU register-write ring** (no `addr,val,cycle` stream) and **no APU state getter**
  — the two taps needed to diff cycle-exact reg writes vs Mesen2. T0 per-channel tap is
  declared but not wired.

#### Cycle-driven APU sequencer — Option A (LANDED 2026-06-28) vs Option B (DEFERRED)

The APU's frame sequencer and channel timers were historically clocked **inside the
per-frame batched `apu_generate(frame, 735)`** — 4 quarter-frame events spread across the
735-sample buffer. That batching is fine for audio but freezes the frame counter / DMC
reader whenever `apu_generate` is skipped (NMI-disabled main-thread code, headless,
turbo, smoke), which is exactly how every blargg APU test runs → `$4015` polls never
advance. Two scoped responses:

- **Option A — frame sequencer cycle-driven, audio untouched (LANDED, low risk).** The
  **frame-counter IRQ flag** moves onto the CPU-cycle stream via `apu_clock_cycles(cpu_cycles)`
  (called from the per-instruction `maybe_trigger_vblank`): asserted at the exact 4-step
  offset (29829 CPU cyc), held to the sequence wrap (29830), `$4017` write resets the phase.
  The pulse/triangle/noise/DMC **audio** synthesis (all channel timers + envelope/length/
  sweep + the DMC output unit) stays in `apu_generate` exactly as tuned (band-limited ×32,
  nonlinear DAC mixer) — **byte-unchanged**, so the Slice-001/002/003 audio result is
  preserved. Only the frame-IRQ flag is driven here, so the frame-IRQ phase and the audio-
  envelope phase are two independent clocks (correct for each observable; never compared).
  - **DMC IRQ under Option A:** the DMC sample-end flag (`dmc_refill` → `s_dmc.irq_flag`) is
    still set by the existing `apu_generate` DMC clocking and **delivered by the new hook**.
    For real (NMI-on) games this is correct — `apu_generate` runs every frame, the DMC reader
    advances, the flag is set, and `maybe_deliver_irq` delivers it on the next main-thread
    instruction (≤1-frame latency). The *only* case Option A does NOT cover is DMC IRQ for
    **NMI-disabled** code, which would need the DMC reader's byte-consumption timing driven
    on the CPU-cycle stream **independently of the audio output unit** — and that isolation
    can't be done without desyncing or duplicating DMC state, i.e. it is effectively the
    Option B sample-accurate rewrite. NMI-off DMC IRQ only matters for the blargg DMC tests,
    which are infeasible anyway (Axis 3, interrupted-PC). So full DMC cycle-driving is folded
    into Option B.

- **Option B — full sample-accurate APU engine (DEFERRED; documented per request).**
  Rewrite `apu.c` so a single `apu_clock_cycles(cpu_cycles)` is the *entire* APU engine:
  it advances the frame sequencer + **all** channel timers (pulse ÷2, triangle ÷1, noise
  per `NOISE_PERIOD`, DMC per `DMC_RATE`) + envelope/length/sweep at their true CPU-cycle
  offsets, **and emits audio samples into a ring buffer** at the output rate (band-limited
  integration over each output-sample window, preserving the current ×32 oversample +
  nonlinear-DAC quality). `main_runner` then drains that ring to SDL each frame instead of
  calling `apu_generate(frame, 735)`.
  - **What it buys (over A):** intra-frame envelope / sweep / length changes land at their
    true sub-frame sample times (best-possible audio-timbre accuracy), and the sequencer/
    audio share one phase. It is the prerequisite for any blargg APU **timing** subtest
    (`irq_flag_timing`, `len_timing`, `dmc_rates`) and for Axis 2's DMC-DMA CPU-cycle steal
    (the DMC reader would then stall the CPU cycle stream at the exact fetch cycles).
  - **Cost / risk (why deferred):** large rewrite of the timing core; the committed audio
    (Slice 003 timbre L1 0.083, the de-clipped nonlinear DAC, the DRC resampler bridge in
    `recomp_audio_drc.h`) must be re-matched against the Mesen2 oracle — sample rate/count,
    levels, pitch, and queue **drift** all re-verified via `apu_stream_diff.py` /
    `audio_drift_diff.py` / `compare_*.json`. Its *extra* payoff over A is on the **audio
    -quality axis**, not the IRQ/validation axis, so it belongs in a dedicated **audio**
    accuracy slice with full oracle re-verification — not bundled into IRQ work.
  - **Note:** Option B still does **not** make blargg's IRQ-*vectoring* tests pass — the
    interrupted-PC blocker (Axis 3) is independent of APU cycle accuracy.

**Validation (this slice):**
1. **PCM, drift-tolerant** (primary, GREEN-able now): recomp T1 (`t1_apu.wav`, 44100 Hz
   mono, post-mix) vs Mesen2 SMB title-theme WAV. Metric: cross-correlation alignment →
   per-band envelope correlation + onset-timing histogram + per-note pitch error
   (drift-tolerant; bit-exact is NOT realistic across two APU/resampler implementations).
2. **APU reg-write stream, cycle-exact** (rigorous half of the committed bar): add an
   always-on ring in `apu_write()` recording `(addr,val,nes_cycle)`; capture Mesen2's via
   Lua `addMemoryCallback("write",0x4000,0x4017)` + `CycleCount`; diff write order +
   inter-write cycle deltas. Bit-exact IS realistic here.
- blargg `apu_test`, `apu_mixer`, `dmc_basics` cross-ref.

---

### AXIS 6 — Static-vs-dynamic recompiler fidelity

**Status: 3 STRONG** (MEASURED slice coverage-001: zero dispatch misses on SMB).

> **Measured:** `--smoke 6000` (boot+title+attract-demo gameplay) and a varied
> player-driven `--script` run both produced **0 dispatch misses** (total + unique).
> Every indirect/computed target SMB executed resolved against the 3238-entry
> `call_by_address` table; no unofficial-opcode/`BRK`/`JMP($xxFF)` traps. Static dispatch
> coverage is **complete for SMB's reachable code**. Self-modifying/bank-switch paths and
> backend-equivalence are N/A on Mapper-0 SMB — validate on MMC3 (MM3).
> `_acc/audio_slice/COVERAGE_SLICE_001.md`.
>
> **Stack-page fidelity boundary (slice state-001):** JSR/RTS modeled as C call/return →
> the recomp never pushes 6502 return addresses, so the stack page ($0100-$01FF) below the
> live SP is NOT bit-faithful (17 dead bytes differ vs Mesen on SMB; benign — never read).
> Matters only for games that read the stack region directly (PATTERNS.md).
>
> **MM3 (MMC3) coverage — REFRAMED 2026-06-28 (was mis-attributed to banking):** the 7
> "misses" are a **coroutine-kernel dispatch** gap, NOT a bank-switch gap. MM3 runs a
> cooperative coroutine scheduler ($FEAA / $FF21 yield); the recompiler already models it
> (fiber subsystem in `runner/src/coroutine.c`), but `function_finder.c` fails to discover
> some task ENTRY / RESUME points, so `call_by_address()` misses them. Of the original 7,
> **$D701 + $FF1D are now discovered**; **5 remain** ($C8DB/$900F/$96BA/$C782/$C7C2).
> They are **state-specific** and did NOT reproduce in normal stage play (Snake Man stage =
> 0 misses); boot/title/select are clean. A **band-aid** (`[[extra_func]]` in `game.toml`)
> is in place but is NOT a fix and NOT runtime-verified. Real fix = a generic finder
> detector (sibling to the RTI-hijack discovery, `function_finder.c` ~827-906). Full rooted
> writeup: `Megaman3NESRecomp/MM3_COROUTINE_DISPATCH_LATENT.md`. **Do NOT** treat the
> `extra_func` band-aid as closing this axis.

- Static recompiler: 6502→C, JSR/RTS as call/return, dispatch via `*_dispatch.c`.
  PATTERNS.md catalogs stack-RTS computed-goto / inline-data-after-JSR idioms.

**Gaps:**
- Self-modifying / bank-loaded code: no dirty-RAM reinterpretation path documented for
  SMB (mapper-0, low risk; matters for MMC1/3 titles).
- Dispatch-miss completeness: misses are logged but **not aggregated** — no
  instruction-coverage / unresolved-indirect histogram (`COVERAGE.md:254`).
- Backend equivalence (compiled == runner interp) is necessary, **not** sufficient (§0).

**Validation:** build an instruction-coverage + dispatch-miss tool (port
`build_instruction_coverage.py`); a function-call-trace diff vs Mesen2 exec callbacks to
catch divergent indirect targets. GREEN when SMB shows zero unresolved dispatch on a full
playthrough and coverage is reported (no silent caps).

---

### AXIS 7 — Determinism

**Status: 3 STRONG** (MEASURED slice ppu-001: byte-identical re-runs).

> **Measured:** two identical-input runs produced **byte-identical screenshots** (frames
> 360/480/600) and a **byte-identical APU stream** — both the (addr,val) sequence (3507
> writes) AND the cycle stamps matched exactly. Game logic, rendering, and cycle accounting
> are run-to-run reproducible. (The wall-clock boot watchdog / ISSUES #10 non-turbo jitter
> did not manifest in these runs.)

- Operation-counted frame timing (no wall-clock in the state path); no `srand`/time-seeded
  RNG in the runner; single-threaded (audio callback mutex only). Input from keybinds /
  `--script` / TCP override — reproducible. Full-frame state ring in `debug_server.c`.

**Gaps:**
- Non-turbo wall-clock jitter if a game's poll-count-to-29781 varies (ISSUES #10) —
  affects timing, not state.
- No automated run-to-run determinism harness (boot ×3 to same frame, hash rings,
  assert identical).

**Validation:** determinism harness hashing the always-on rings across 3 boots; must be
identical. Cross-ref: Mesen2 movie replay is itself deterministic, so a fixed `--script`
makes both sides reproducible for the other axes' diffs.

---

## 4. Status table (verdict · gap · lever)

| Axis | Verdict | Biggest gap | One lever to close it |
|------|---------|-------------|------------------------|
| 1 Instruction semantics | 3 STRONG | 96 unofficial opcodes as skips; `JMP($xxFF)`; `BRK` | Emit stable illegals + `nes_read16` page-wrap; prove unreachable per title via coverage |
| 2 Cycle/timing | 3 STRONG | cross-title cycle_compare drift: **SMB 0.51 / Zelda 0.41 / MM3 0.37 cyc/frame** — all dominated by frame-len 29781 vs 29780.5; model holds NROM/MMC1/MMC3 | alternating frame budget + monotonic `g_cpu_cycles` (deferred); dynamic penalties |
| 3 Interrupt/event | 3 STRONG | NMI corroborated; MMC3 IRQ ~1-scanline-late **FIXED**; **general pending-IRQ delivery hook LANDED** (APU-frame + DMC, instruction-granular, `maybe_deliver_irq`); cycle-driven IRQ source (Option A). blargg IRQ tests infeasible (no interrupted-PC) → validate via custom ROM. Sub-scanline dot precision still open | custom IRQ ROM vs Mesen2; dot-precise A12 (needs cycle-accurate PPU; deferred) |
| 4 Memory/MMIO | 3 STRONG | MEASURED SMB 99% + **Zelda/MMC1 + MM3/MMC3 ZERO persistent game-logic divergence** (nesref diff; Zelda needs RNG-freeze, MM3 needs coroutine-ZP+stack mask); open-bus/PPU-mem unmodeled | extend nesref to PPU memory; open-bus |
| 5a Video/PPU | 3 STRONG | structural framebuf vs nesref: **SMB 1.000, MM3/MMC3 1.000, Zelda/MMC1 0.992** (title anim-phase); dot-accurate raster still untested | dot-accurate raster; capture nesref shots at synced anim phase (polish) |
| 5b Audio/APU | 3 STRONG | reg-stream BIT-IDENTICAL ✓; rhythm 0.99 ✓; timbre L1 0.083 ✓; frame-counter + DMC IRQ now **cycle-driven** (Option A); residual = mid-band tilt + DMC DMA steal unmodeled; sample-accurate engine = **Option B (deferred)** | Option B sample-accurate engine (own audio slice); DMC DMA stall; windowed-sinc decimator (polish) |
| 6 Recompiler fidelity | 3 STRONG | 0 dispatch misses: SMB + **Zelda/MMC1 (6000f) + MM3/MMC3 (4000f attract)**; MMC1/MMC3 banking validated. NEW boundary: MM3 coroutine-scheduler ZP ($90-$93) not bit-faithful (fiber-modeled, like stack page). 5 latent MM3 coroutine misses still gameplay-state-specific | reproduce + fix the 5 latent MM3 misses (Ghidra) |
| 7 Determinism | 3 STRONG | non-turbo jitter; no harness | run-to-run ring-hash harness |

---

## 5. Tooling to build (mirrors the PSX shelf)

- [x] `cycle_compare.py` — delta-cycle via the bit-identical APU-write anchors (Axis 2). **DONE cycle-001 + CROSS-TITLE (SMB 0.51 / Zelda 0.41 / MM3 0.37 cyc/frame).** Oracle APU stream = `mesen_apu_capture.lua` (generic, cycle-stamped) — the one place cycle still needs MesenCE Lua (libretro has no cycle counter).
- [ ] **Path C — in-process source-core oracle (DEFERRED):** would give in-process guest cycles + PPU-internal memory (VRAM/OAM/palette) with zero MesenCE-GUI/Lua dependency. Full plan + why-deferred + pickup steps: `_acc/audio_slice/PATH_C_SOURCE_CORE_ORACLE.md`.
- [ ] `nes_cycles.{c,h}` monotonic counter + alternating frame budget (the deferred frame-length fix).
- [x] APU register-write ring (`NESRECOMP_APU_TRACE`, `runtime.c:674`) + `apu_stream_diff.py` (Axis 5b). **DONE slice 002.**
- [x] `audio_drift_diff.py` — onset-train align + onset histogram + timbre band-energy. **DONE.**
- [ ] Mesen2 oracle: `mesen_oracle.lua` (state/cycle/APU-reg via `--testrunner`) + a
      `mesen_bridge.cpp` source-hook (PCM ring in `NesSoundMixer`, cycle ring at
      `NesCpu::EndCpuCycle`) to replace `nestopia_*`.
- [x] `framebuf_diff.py` — palette-independent structural framebuffer match (Axis 5a). **DONE ppu-001.**
- [x] Dispatch-miss coverage (Axis 6) — in-tree `--smoke` JSON + `dispatch_misses.log` + always-on miss ring (`runtime.c`). **MEASURED coverage-001: 0 misses on SMB.** (No port needed.)
- [x] **nesref** state-divergence (Axis 4) — `nesref.lua` (Mesen per-frame RAM tap) + `NESRECOMP_WRAM_TRACE` (recomp tap) + `wram_diff.py`. **MEASURED state-001: RAM 99% match, only dead stack-page bytes differ.** (snesref pattern, Mesen-based.)
- [ ] extend nesref to PPU memory (nametable/OAM/palette) for explicit PPU-state diff (Axis 4/5a).
- [ ] `event_ring` NMI/IRQ first-mismatch on an MMC3 game (Axis 3 IRQ).
- [ ] determinism harness (Axis 7).

## 6. Phasing (vertical slices)

1. **NOW** — Audio slice on SMB: recomp T1 PCM vs Mesen2 PCM, drift-tolerant diff
   (deliverable ii). → see `_acc/audio_slice/`.
2. APU register-write ring (recomp) + Lua APU-reg capture (Mesen2) → cycle-exact reg diff.
3. Mesen2 `mesen_bridge` replaces Nestopia for state + cycle (Axis 1–4).
4. Cycle axis (delta method) + coverage tool on SMB.
5. PPU dot-accuracy + IRQ slicing; cross-title (Zelda/Metroid/MM3); retire Nestopia.

---

## 7. Audio-first comparison plan (the metric)

Bit-exact PCM is **not** realistic across two independent APU + resampler chains. Use a
**drift-tolerant** metric, computed after a global alignment:

1. **Align**: normalized cross-correlation of the two mono streams (and/or per-channel
   envelopes) → integer + fractional lag; report the lag and the peak correlation.
2. **Envelope correlation** per octave band (filterbank) — robust to phase/resampler
   differences; the headline "do they sound like the same music" number.
3. **Onset-timing histogram**: detect note onsets on both (spectral-flux), match nearest
   pairs, histogram the timing error (ms). Tight, zero-centered ⇒ rhythm matches.
4. **Per-note pitch error**: for matched onsets, dominant-frequency ratio in cents.
   Pulse/triangle pitch is set by 11-bit timer reloads — large systematic cents error ⇒
   a timer/period bug, not a mix difference.

Where bit-exact IS realistic (next slice): the **APU register-write stream** — same
writes, same order, same inter-write cycle deltas — diffed directly.
