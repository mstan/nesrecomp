# COSIM.md — Differential Co-Simulation tooling

**What it is.** A recomp-vs-oracle **first-divergence decision procedure**: run
the recompiled game and a reference NES emulator (Mesen, via `nesref`) over the
same deterministic attract sequence, hash/compare full machine state per frame,
and report where — and in which subsystem — they first disagree. It answers "is
the recompiler faithful?" with measured state, not inspection.

- **Oracle B** = `F:\Projects\nesref\` — an in-process libretro host running
  `mesen_libretro.dll`. No external Mesen.exe: the wide state channel is Mesen's
  `retro_serialize` blob (CPU cycle count, PPU OAM/palette/nametables, APU), from
  which `nesref` extracts per-frame traces.
- **What it is NOT.** Not an emulator, not a renderer check. It measures *state
  convergence* across two implementations. It is blind to display/feel — see the
  owner rule below.

## The tool

Coordinator: `tools/nes_cosim.py`. All commands take `<exe> <rom> [frames]`;
the A/B commands also take `<nesref.exe> <core.dll>`.

| Command | What it checks | Reads |
|---|---|---|
| `gate1 <exe> <rom> [f]` | **Recomp determinism** — two recomp runs must be byte-identical (A-vs-A = 0). Catches nondeterminism leaks (uninit padding, host state). | recomp cosim-hash JSONL ×2 |
| `gate2 <nesref> <core> <rom> [f]` | **Oracle determinism** — two nesref runs identical. | nesref RAM trace ×2 |
| `gate3 <exe> <rom> [f]` | **Fault injection** — inject a 1-byte flip, the diff must halt at the injected frame in the injected subsystem (proves the hasher isn't blind). | recomp cosim-hash JSONL |
| `abram <exe> <rom> <nesref> <core> [f]` | **RAM convergence** (2 KB, stack-masked) vs Mesen. The strongest cross-impl signal the libretro oracle exposes. | recomp WRAM trace + nesref RAM |
| `abcycle <exe> <rom> <nesref> <core> [f]` | **Cycle convergence** — recomp `g_nes_cycles` per-frame advance vs Mesen's cycle counter. Drift → NTSC frame-length skew. | recomp cosim-hash `bclk` + nesref cycle |
| `abppu <exe> <rom> <nesref> <core> [f]` | **PPU-internal memory** — OAM, palette, nametables vs Mesen. The channel the libretro memory API can't reach (VIDEO_RAM is null on all NES cores). | recomp PPU-mem trace + nesref PPU blob |
| `diff <a.jsonl> <b.jsonl>` | First-divergence report between two hash streams. |  |
| `run <exe> <rom> <out> [f] [inject]` | One instrumented run → JSONL. |  |

Gates print PASS/FAIL and exit non-zero on FAIL (CI-gateable).

## The measurement axes (convergence ladder)

- **Rung 0 — logic/RAM:** converges by default; the regression floor.
- **Rung 1 — cycle accounting:** monotonic `g_nes_cycles`, OAM-DMA charge,
  remainder carry. Done.
- **Rung 2 — dot-accurate frame length** (`NESRECOMP_DOT_CLOCK`, **default ON**):
  frame boundary is a dot event (89342/89341 dots, odd-frame skip) → cycle drift
  ~0. Done, live-validated (SMB, MM3). `=0` restores the legacy 29781 threshold.
- **Rung 3 — per-dot identity:** sub-scanline sprite-0/A12, intra-instruction R/W
  placement. Deferred as gold-plating (see `ENHANCEMENTS.md` §3).

## How alignment works (and the fire-count fix)

The recomp and oracle free-run; frame `N` of the recomp is compared to oracle
frame `N + offset` (a constant boot latency). This is exact for a deterministic,
phase-locked attract demo.

The recomp co-sim trace is tagged with **`g_cosim_vframe`**, a video-frame index
**derived from the cycle ruler** — `round(g_frame_boundary_cyc / 29780.5)` = the
true NTSC video-frame number — computed at each emit, NOT counted per NMI. This
matters because the oracle counts every video frame: a per-NMI counter (like
`g_frame_count`) STALLS whenever the frame boundary is deferred or depth-suppressed
(post-NMI work during boot/scene transitions), so real video frames elapse
un-counted and byte-identical state looks phase-divergent — a pure measurement
artifact (this was the entire Zelda "FrameCounter-phase" false alarm). Deriving the
index from cycles removes it: whichever frames emit carry their true video-frame
number and align to the oracle across suppressed gaps. Emission stays POST-NMI-handler
(so the sampled phase matches the oracle's end-of-frame serialize); NMI-off frames
emit at the boundary. The trace path is env-gated and `g_frame_count` (the
game-logic clock) is untouched, so there is **zero effect on shipped gameplay**.

When trajectories still drift (host-modeled state, RNG/FrameCounter phase), the
coordinator's `adaptive_offset_*` (windowed piecewise offset) and, for scrolling
attract, `scroll_phase_match` (per-frame horizontal column-shift on the visible
nametable) **classify** the residual: a discrete shift that recovers a high match
= alignment; a match that stays low = a genuine divergence or phase desync. See
`ENHANCEMENTS.md` §1 for the state/phase-sync roadmap.

## Interpreting a verdict

- **CONVERGED / byte-identical** — faithful (bar documented benign flags, e.g.
  SMB `$1a` frame-sync).
- **CONVERGED once re-aligned** — the spread was an NMI-off frame-count desync,
  not a divergence.
- **CONVERGED modulo scroll-phase** — the visible screen matches under a scroll
  shift; off only by attract-demo timing.
- **still low after re-align → genuine divergence / FrameCounter-host-state
  phase** — a real state difference to chase (Ghidra), or a phase desync needing
  a state-sync fixture (`ENHANCEMENTS.md` §1).

## Build & run

Build a game against the co-sim engine (msys cmake is broken — use the VS dev
shell + VS-bundled cmake; see the build recipe in `CLAUDE.md`):

```
cmake -S . -B build_cosim -DENABLE_NESTOPIA_ORACLE=OFF   # first time
cmake --build build_cosim --config Release
```

Run (from a game dir; dot clock is default-on; battery `.srm` is auto-cleared):

```
NESREF=/f/Projects/nesref/nesref.exe
CORE=/f/Projects/nesref/cores/mesen_libretro.dll
COORD=../nesrecomp/tools/nes_cosim.py
python "$COORD" gate1   <exe> <rom> 900
python "$COORD" abram   <exe> <rom> "$NESREF" "$CORE" 900
python "$COORD" abcycle <exe> <rom> "$NESREF" "$CORE" 900
python "$COORD" abppu   <exe> <rom> "$NESREF" "$CORE" 900   # auto-detects CHR-RAM
# RNG-seeded games (Zelda): prefix both sides with a seed freeze:
#   NESRECOMP_FREEZE="0x18=0x00" NESREF_FREEZE="0x18=0x00" python "$COORD" abram ...
```

## Coverage (2026-07-02)

Cycle + RAM + PPU converge on **SMB** (NROM: RAM 99.94, drift +0.000, OAM 99.96,
NT 99.97), **Faxanadu** (MMC1/CHR-RAM: OAM 100, NT 99.92), **MM3** (MMC3, logic-
converges — coroutine ZP is host-modeled; OAM 99.48, NT 99.92); gates 1/2/3 pass
across all. **Zelda** (MMC1/CHR-RAM): **RAM 100.00% CONVERGED** — the former
"FrameCounter-phase block" was a stalled-index measurement artifact, fixed by the
cycle-derived index; its `abppu` still reads low (animated-title alignment, RAM
proves faithfulness — `ENHANCEMENTS.md` §1). **Gumshoe** (GxROM) converges on
RAM/OAM/palette/cycle-mean; its attract nametable has one genuine ~1-frame
cycle-timing lead through the NMI-off scene transition (`ENHANCEMENTS.md` §1).

## Rules

- **Never ship / flip default-on on headless co-sim numbers alone.** Headless is
  blind to display/feel; the owner must launch, play, and approve. (Co-sim
  *tooling* changes that are env-gated and don't touch gameplay are exempt —
  verify them headless.)
- Land each timing rung **behind** the co-sim: build the certifier, then change
  the timing, then measure. Never ship a timing change unmeasured.
- One runtime instance per game at a time.

See `ENHANCEMENTS.md` for the deferred-capability roadmap and worked examples,
and `recomp-template/NES/DIFFERENTIAL-COSIM-PROPOSAL.md` for the design rationale.
