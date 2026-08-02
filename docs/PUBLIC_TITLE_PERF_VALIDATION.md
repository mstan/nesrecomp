# Public-title performance regression sweep

This sweep validates the retained production culling and PPU tile-span changes
against every locally available NES title with a public repository. It is a
correctness sweep, not a throughput comparison.

## Method

- Every title was built and run from a detached validation worktree. Original
  title source checkouts and ROMs were read-only inputs.
- Every CMake configure and compile ran at Windows `BelowNormal` priority with
  Ninja `-j1`. This deliberately favors host stability after parallel title
  compiles crashed the validation machine.
- The engine's Vitest recompiler/runtime suite passed all 52 tests after the
  sweep.
- The smaller titles and the representative SMB3 build used production
  optimization. The largest remaining generated-code builds used
  `-O0 -DNDEBUG` in a validation-only profile; the production engine path was
  already covered by the complete engine suite, SMB3 benchmarks, and eight
  production-optimized title builds.
- A normal smoke run covers 600 frames with hashes every 100 frames. When a
  checked-in baseline exists, every hash must match exactly.
- Attract/demo coverage is at least 1,800 no-input frames. Kirby uses its full
  7,192-frame attract script.
- Existing title gameplay routes were copied into the validation worktrees and
  screenshot destinations were rewritten so the original repositories were
  not modified.
- Basic fuzzing is deterministic. Controller titles use
  `tools/perf_basic_fuzz.script`; Zapper titles use
  `tools/perf_zapper_fuzz.script`.

## Results

`misses` is shown as unique/total dispatch misses. A zero means the whole route
completed without a missing recompiled target.

| Public title (source commit) | Smoke | Attract | Gameplay | Fuzz misses | Result |
|---|---:|---:|---:|---:|---|
| Dr. Mario (`d616968`) | 600, exact baseline | 1,800 | 1,950 | 0/0 | Pass |
| Duck Hunt (`cb95b01`) | 600, exact baseline | 1,800 | 1,705 | 0/0 | Pass |
| Faxanadu (`80d9189`) | 600, exact baseline | 1,800 | 1,983 | 0/0 | Pass |
| Gumshoe (`4a4077f`) | 600 | 1,800 | 1,764 | 0/0 | Pass |
| Kirby's Adventure (`aa2469d`) | 600, paired baseline | 7,192 | 5,005 | 1/52 | Pass; pre-existing coverage hole |
| The Legend of Zelda (`371c7f8`) | 600, exact baseline | 1,800 | 1,451 | 0/0 | Pass |
| Mega Man 3 (`509bbec`) | 600 | 1,800 | 1,990 | 0/0 | Pass |
| Metroid (`5e6b1a8`) | 600, exact baseline | 1,800 | 2,509 | 2/26 | Pass; pre-existing coverage holes |
| Mother 1 (`981c42b`) | Build pass | Blocked | Blocked | Blocked | ROM unavailable locally |
| Super Mario Bros. 2 (Japan) (`2f360ea`) | 600 | 1,800 | 496 | 0/0 | Pass |
| Super Mario Bros. 2 (`7ec82cf`) | 600 | 1,800 | 2,039 | 0/0 | Pass |
| Super Mario Bros. 3 (`c5f2f4f`) | 600, exact established hashes | 1,800 | 3,395 | 5/512 | Pass; pre-existing coverage holes |
| Super Mario Bros. (`42a2eb4`) | 600, exact baseline | 1,800 | 1,387 | 0/0 | Pass |
| Yoshi (`98b7fe2`) | 600, exact baseline | 1,800 | 1,805 | 0/0 | Pass |
| Yoshi's Cookie (`820cc97`) | 600, exact baseline | 1,800 | N/A | 0/0 | Pass |

Mother 1's candidate executable builds successfully. Its public README requires
an iNES payload CRC32 of `00F0FBBC`, but no matching ROM or even a
Mother/EarthBound-named NES image is present in the NES workspace or the
targeted local Emulation/Recomp roots. Its checked-in attract and gameplay
scripts remain ready to run when that user-supplied ROM is available.

Mega Man 1 (`MegamanNESRecomp`, `acee5f5`) has no configured origin remote and
was therefore treated as non-public under the requested rule.

## Paired classification of fuzz misses

The three nonzero fuzz results are title recompiler coverage debt, not a change
introduced by the performance work:

- Metroid: baseline and candidate both produced 26 total misses at `$BBE1` and
  `$C106`; their fuzz frame hashes, frame count, and miss arrays were identical.
- SMB3: baseline and candidate both produced 512 total misses at `$80CF`,
  `$8398`, `$83B1`, `$83CD`, and `$840E`; their fuzz frame hashes, frame count,
  and miss arrays were identical.
- Kirby: baseline and candidate both produced 52 misses at bank 30 `$B865`.
  An initial MSVC/O2 versus GCC/O0 comparison diverged after the miss, so a
  second pre-optimization engine build used the same GCC 15.2 compiler,
  `-O0 -DNDEBUG`, title source paths, clean SRAM, and input script as the
  candidate. Smoke hashes, fuzz hashes, frame count, and miss arrays then
  matched exactly.

## Build compatibility findings

The sweep also exposed build issues that are independent of PPU behavior:

- The shared runner now exports its Windows `dbghelp` link dependency, so older
  title CMake files do not need to know that `launcher.c` implements native
  crash backtraces.
- The NES netplay launcher adapter was updated to the current recomp-ui create
  and join callback ABI. The public SMB1 netplay target builds without removing
  netplay.
- Two title-local worktree-only accommodations were needed: Metroid's existing
  MSVC duplicate-symbol policy was mirrored with MinGW
  `--allow-multiple-definition`, and Mega Man 3's MSVC-only `__try` wrapper was
  correctly restricted to MSVC. These title changes were not made in the
  original checkouts.

## Checkout hygiene note

The first Dr. Mario gameplay invocation used the original script path before
the isolation rule was established. That script rewrote three pre-existing
untracked regression PNGs in the original checkout. The route and hashes
matched and candidate copies were retained, but the original files' timestamps
changed. Every later screenshot-producing script was copied and rewritten
before execution.
