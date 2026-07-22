# Porting psxrecomp's multi-tier recompilation to nesrecomp — feasibility & proposal

Branch: `explore/multitier-recomp` (worktree `_wt-multitier`). Read-only exploration; no
changes to the master checkout. Dated 2026-06-17.

## TL;DR

- **Full port: not necessary, and largely inapplicable.** psxrecomp's multi-tier
  system exists to solve a problem NES does not have. Most of its tiers (gcc shard,
  sljit JIT, persist cache, async promotion) are machinery for making *hot code that
  arrived at runtime* fast. On NES there is no hot code that arrives at runtime — the
  entire ROM is present at build time — so those tiers would have nothing to chew on.
- **A simplified two-piece slice is genuinely valuable**: an **interpreter-fallback
  tier** wired into `call_by_address`, plus an **auto-manifest discovery loop**. This
  captures the *correctness* and *discovery* value of the psxrecomp design without any
  of the JIT/cache/async machinery.
- Recommended path: build the interpreter fallback + manifest feedback. Skip
  sljit/gcc/persist entirely.

---

## 1. What psxrecomp actually built, and why

psxrecomp runs **four tiers** (`overlay_loader.c`, `dirty_ram_interp.c`, `overlay_sljit.c`):

1. **Tier 1 — AOT static native.** BIOS + main EXE recompiled to C up front, compiled
   into the binary. Dispatch = binary search over a sorted `{addr, func}` table.
2. **Tier 2a — gcc shard DLLs.** Per-overlay native DLLs compiled *offline/async* by a
   background `compile_overlays.py` → gcc → `.dll`, then `LoadLibrary`'d in-session.
3. **Tier 2b — sljit JIT shard.** Synchronous, on a dispatch miss, JITs a leaf fragment
   of live RAM in-process (sub-ms). MIPS→sljit emitter, ~1100 LoC.
4. **Tier 3 — dirty-RAM interpreter.** The correctness floor for not-yet-recompiled
   code, and the parity oracle the JIT/DLL are validated against.

Supporting machinery: a **same-state differential gate** (run interp first, then the
shard, compare full reg+RAM, require 32 consecutive clean diffs before a shard goes
live), **device-touch pinning**, **self-modifying-code invalidation**, a **persist
cache** (serialize position-independent sljit LIR to disk, reload + regenerate at the
new ASLR base, re-validate), and a **manifest** (`overlay_captures.json`: live bytes +
executed PCs) fed back through `compile_overlays.py` to grow either the gcc cache or,
via `--static`, an `overlays_static.c` folded into the AOT (Tier-1) build.

Total subsystem ≈ **7,500–8,000 LoC** plus vendored sljit plus the shared C++ recompiler.

### The forcing function: PSX streams code into RAM at runtime

The reason all of this exists: **PSX game code is not in the executable.** The disc DMAs
overlays into RAM at runtime (capture is literally triggered by CD-DMA completion,
`dma.c:342`). The AOT recompiler physically cannot recompile code it has never seen, so:

- An interpreter tier is **mandatory** just to *run* freshly-arrived overlay code.
- The JIT/gcc/persist tiers exist to **claw back performance** — an overlay can be a
  large, hot fraction of execution, and you cannot afford to interpret it forever.
- The manifest loop exists to **fold discovered overlays back into AOT** over many runs.

Every tier traces back to "code we didn't have at build time shows up hot at runtime."

---

## 2. Why NES is different

nesrecomp today is **purely ahead-of-time static** — no interpreter, no JIT, no runtime
codegen (`CLAUDE.md:233`, confirmed across `code_generator.c` / `runtime.c`).

- **All NES code is in the ROM at build time.** Every PRG bank is recompiled up front;
  banked functions are emitted as per-bank variants `func_XXXX_bN()`.
- **Bank switching is *selection*, not *arrival*.** At runtime, a mapper-register write
  updates `g_current_bank`; `call_by_address`'s inner `switch (g_current_bank)` picks the
  already-recompiled variant (`code_generator.c:2585`). Nothing new is generated.
- **"Misses" are a discovery-completeness problem, not a code-arrival problem.** When the
  static function finder fails to discover an indirectly-reached entry point, the runtime
  hits `nes_log_dispatch_miss` (`runtime.c:1091`): it ring-buffers the miss, appends a
  paste-ready `extra_func` line to `dispatch_misses.log`, and applies a policy
  (`LOG_RETURN` default = **silently terminate that control-flow path**; or `FATAL` /
  `TRAP`). The fix loop is *offline*: re-seed `game.toml`, regenerate, done.

**Consequence:** the entire performance rationale for Tiers 2a/2b and the persist cache
evaporates on NES. The fallback fraction of execution is tiny and cold (a modern CPU
interprets a 1.79 MHz 6502 trivially), and there is no runtime-arriving code to compile
into a "shard." gcc/sljit shards would be **inputs-less machinery**.

### The one place NES genuinely needs a runtime tier

There *is* a real gap, just a much narrower one than PSX:

- **Missed dynamic dispatch into ROM bytes** (the common case): today this silently
  truncates control flow under the default policy — exactly the "early-return" risk class
  flagged in `COVERAGE.md:194`. Subtle freezes/glitches, hard to diagnose.
- **Code executed from RAM/SRAM** (rare but real): the 6502 can run code it builds in
  RAM. `call_by_address` returns 0 immediately for `addr < 0x8000`, so RAM execution is
  simply **unsupported** today. Zelda's SRAM-resident code is handled by a per-game
  `game_dispatch_override` + `sram_map` hack (`extras.c`), not a general mechanism.

Both are correctness problems an interpreter tier solves cleanly. Neither benefits from a
JIT — they are cold.

---

## 3. The clean integration seam already exists

A future tier has exactly one place to hook, and the state model is already
interpreter-friendly:

- **`int call_by_address(uint16_t addr)`** (generated by `emit_dispatch`,
  `code_generator.c:2482`) — the single guest-PC→native entry point. Its `default:` and
  inner-`switch(g_current_bank) default:` cases are where a fallback slots in instead of
  "log miss + return 0".
- **State is centralized**: `g_cpu` register struct + flat `g_ram`/`g_sram` + mapper
  state (`g_current_bank`, MMC3 regs). No guest state hides in per-function locals across
  calls, so an interpreter can share it directly.
- **Live bank-correct bytes are already fetchable** via `mapper_peek_prg`
  (already used at `runtime.c:1102`).
- **Misses are already ring-buffered and classified** (`g_miss_ring`, `MissRecord`) —
  the manifest loop can consume this instead of re-instrumenting.
- **A maintained opcode table exists** in the recompiler's `cpu6502_decoder.c`
  (mnemonic, addr-mode, size, cycles), which a compact interpreter can drive. Reference
  behavior is checked externally with `nesref`/Mesen rather than an embedded emulator.

---

## 4. Proposal — the simplified slice

Build the two pieces of the psxrecomp design that map onto a real NES need, and nothing
else.

### Phase 1 — Interpreter fallback tier (correctness)

A compact 6502 interpreter, callable per-address, that shares `g_cpu` / `g_ram` /
`nes_read` / `nes_write` and reads bank-correct bytes via `mapper_peek_prg`.

- Wire it into the `default:` cases of `call_by_address`: instead of logging a miss and
  returning, **interpret from the missed PC until control returns to recompiled code** —
  i.e. on each JSR/JMP/RTS target, check `call_by_address` coverage; if the target is a
  known recompiled entry, hand off to native; otherwise keep interpreting.
- This converts the most dangerous current failure mode (silent control-flow truncation)
  into correct-but-slow execution, and makes RAM/SRAM-resident code work generally
  instead of via per-game overrides.
- Effort: a 6502 interpreter is a well-trodden ~1,500–2,500 LoC artifact; the opcode
  metadata, memory bus, and register state already exist. The genuinely novel design work
  is the **interp↔recompiled boundary handoff** (defining clean return points), not the
  opcode semantics. Use `nesref`/Mesen as the differential oracle.

### Phase 2 — Auto-manifest discovery loop (closes the loop)

- The interp tier (and the existing miss ring) emits each discovered entry PC + bank into
  a machine-readable **manifest JSON** (today the same data leaks to `dispatch_misses.log`
  as paste-ready text — formalize it).
- A small fold tool merges those entries into `game.toml`'s `fixed`/`bankN` seeds, so the
  **next regeneration promotes interpreted blocks into AOT**. This is psxrecomp's manifest
  idea, but resolved at the next *build* instead of via runtime gcc/sljit — which is
  exactly right for NES, where there's no need to recompile at runtime.

### Explicitly out of scope (and why)

- **sljit JIT tier** — no hot runtime-arriving code to JIT; the fallback is cold. A
  6502→sljit emitter would be a large permanent parity burden (cf. the MIPS emitter) for
  ~zero NES payoff.
- **gcc shard DLLs / async promotion** — there are no "shards" on NES; all code is in the
  ROM. Nothing to compile at runtime.
- **Persist cache** — caches JIT output; no JIT, nothing to persist.
- **Same-state diff gate / device-touch pinning** — these validate *native shards* against
  the interp oracle. With no native shards, the interp result is simply authoritative.

---

## 5. Answers to the three questions

1. **Feasible to port the full system?** Technically yes, but it would be mostly dead
   weight: ~half the tiers solve a problem (runtime-arriving hot code) that NES doesn't
   have. The integration seam (`call_by_address`) is clean and the state model is friendly,
   so the *interpreter* part ports naturally; the JIT/shard/persist parts have no inputs.
2. **Necessary?** No. PSX *requires* a runtime tier to run streamed overlays at all. NES
   requires nothing of the sort — it already runs everything it statically discovers, and
   resolves misses offline via the re-seed loop.
3. **Should we do a simplified version?** Yes — the interpreter-fallback + auto-manifest
   slice. It fixes a real correctness gap (silent truncation on missed dynamic dispatch;
   unsupported RAM/SRAM execution) and automates the discovery loop, at roughly a focused
   multi-day effort versus the multi-week, mostly-inapplicable full port.

---

## 6. Design decisions (follow-up Q&A)

### 6.1 Interp fallback = discover all misses in one run (not bail)

Today a miss flows through `nes_log_dispatch_miss` (`runtime.c:1091`): it records a rich
record (regs, 6502 stack snapshot, `call_site_pc`, caller chain, classified target bytes),
appends a paste-ready `extra_func` line to `dispatch_misses.log`, then under default
`LOG_RETURN` returns; `call_by_address` `return 0`s and the caller does `return;`. So the
missed routine **never executes** — its side effects are skipped and control unwinds up
the C stack. The game doesn't crash but usually freezes/glitches soon after, so misses are
effectively found **one at a time** (hit → fold → regen → next).

With the interp fallback the missed routine **runs correctly** (shared `g_cpu`/`g_ram`/
`g_sram`, bank-correct bytes via `mapper_peek_prg`), control returns to native, and the
game keeps playing — so **one playthrough surfaces the whole batch of misses**, all still
logged to the ring + `dispatch_misses.log`. Fold them all, regenerate once. This is the
primary workflow win. Add a **step-cap watchdog**: if the interpreter runs too long without
returning to native (runaway garbage from a genuinely bad pointer), bail to TRAP/log rather
than spin.

### 6.2 RAM/SRAM execution — why it's blocked, and the fix

The `addr < 0x8000` guard (`code_generator.c:2508`) is commented "never valid code targets
on NES … data-as-code false positives in the function finder." It bundles (1) the true
*build-time* assumption that all code lives in ROM ($8000+), and (2) a blunt filter for
finder noise. The cost: it also rejects **legitimate** runtime RAM/SRAM execution. The
interp tier fixes this as a free consequence:

- **Static-copy SRAM code (Zelda):** recompilable; today handled by a per-game `sram_map`
  + `dispatch_override` hack that the dev must know to configure. The interpreter subsumes
  it (interpret live `g_sram` bytes) — general, no per-game config.
- **Truly dynamic RAM code (built at runtime):** *not* recompilable at all; the bytes don't
  exist at build time. The interpreter is the only possible answer.

The "false positive" worry is a build-time finder concern enforced at the wrong layer:
`call_by_address` is only reached by control transfers the 6502 actually executed, so a
real runtime transfer to a sub-$8000 address is faithful to interpret. The watchdog covers
the pathological "jumped into garbage" case.

### 6.3 Why NOT an sljit cache in Phase 2 (even for "experience during")

Decision: **no sljit.** This is the *more complete* choice for the NES problem, not a
shortcut — the completeness axis is correctness + closing the discovery loop, and sljit
advances neither.

- **No perf problem to solve.** The 6502 runs at 1.79 MHz (~30k insns/frame for the *whole*
  CPU); full NES emulators interpret CPU+PPU+APU in real time with hundreds of FPS of
  headroom. The interp only runs the *missed fraction* — a few hundred insns/frame even for
  a hot indirect-JMP sound engine = sub-microsecond. The host is ~5 orders of magnitude
  faster than the guest; a plain interpreter already is the "good experience during."
- **sljit is the most expensive + risk-prone part of the psxrecomp design:** a net-new
  6502→sljit emitter (a *third* impl of 6502 semantics to keep in agreement), the diff-gate
  + device-touch pinning to trust JIT output (NES MMIO double-writes to $2007/$2005/$4014
  corrupt VRAM/scroll/OAM), content-CRC + bank-switch + SMC invalidation (≈ psxrecomp's
  ~1700-line `overlay_loader.c` reimplemented), and persist position-independence/versioning.
  ~80% of the full-port cost and its entire correctness surface for ~0% perceptible gain.
- **The complete perf answer is build-time, not runtime:** the dev plays through (interp
  keeps it correct), the manifest captures every miss, they fold + regenerate, and the
  *shipped* build is full-native AOT with the interp as a rarely-firing safety net. The
  self-improving-runtime story only pays off when you can't rebuild (psxrecomp's disc-
  streamed overlays). On NES you always can.
- **Measurement trigger (don't assume):** the interp tier reports its per-frame interpreted-
  instruction count from the miss ring. Revisit sljit only if a real game ever shows an
  interpreted fraction that is both un-foldable (truly dynamic RAM code) *and* heavy enough
  to drop frames on a modern host. That game is not believed to exist in the NES library.

## 7. Recommended next step

Prototype Phase 1 against one game with known dynamic-dispatch misses (Faxanadu or
Megaman3 are good candidates per the survey). Validate the interp result against
`nesref`/Mesen traces. If the boundary-handoff design holds up,
add Phase 2's manifest emitter and fold tool. Decide on full Phase-1 rollout only after
the one-game prototype proves the handoff.
