# Session Handoff — nesrecomp multi-tier interpreter fallback (Phase 1)

**Date:** 2026-06-18
**Phase:** Phase 1 (interpreter fallback) — implemented, committed, unit-validated.
Live in-game validation + Phase 2 (auto-manifest) are the next work.

This doc is idempotent: read only this (+ the linked docs) to continue with zero prior context.

---

## Where the work lives

- **Git worktree:** `F:\Projects\nesrecomp\_wt-multitier`, branch **`explore/multitier-recomp`** (off `master`@`068cfcf`). The core repo is `F:\Projects\nesrecomp\nesrecomp` (a junction-shared git repo; the session cwd `F:\Projects\nesrecomp` is NOT a git repo).
- **Commits on the branch:**
  - `3f3cfca` feat(interp): Phase 1 interpreter fallback tier
  - `0a534da` test(interp): deterministic self-test (33 checks)
- **Design docs (read these):** `docs/MULTITIER_PORT_PROPOSAL.md` (why a full psxrecomp-style port is unnecessary for NES; what to build instead), `docs/PHASE1_INTERP_FALLBACK_PLAN.md` (the boundary-handoff contract — the core design).
- **Memory:** `nesrecomp-multitier-phase1` (project state) and `nesrecomp-claude-build-recipe` (build/observability recipe) are auto-loaded each session via MEMORY.md.

## The idea (one paragraph)

psxrecomp runs 4 tiers (AOT → gcc shard → sljit JIT → interpreter) because PSX DMAs game code into RAM at runtime (overlays). NES has all code in the ROM at build time, so the JIT/shard/persist tiers have no inputs and are **not worth porting**. The valuable slice for NES is an **interpreter fallback** (so a `call_by_address` miss runs the missed routine and the game keeps going, instead of silently truncating control flow) plus a future **auto-manifest loop** (fold discovered misses back into the AOT build). sljit is explicitly out of scope (NES fallback is cold + tiny). See the proposal doc.

## What was done this session

Implemented Phase 1 entirely in the framework (never hand-edited generated code):
- `runner/src/interp.c` + `runner/include/interp.h` (NEW) — a 6502 interpreter. Opcode semantics mirror `recompiler/src/code_generator.c` exactly; shares the recompiler's `cpu6502_decoder.c` table (compiled into the runner). Entry `nes_interp_dispatch(addr)`.
- `runner/src/runtime.c` — split `nes_log_dispatch_miss` into `nes_record_dispatch_miss` + `nes_dispatch_miss_apply_policy` (so interp can record-and-run without applying FATAL/TRAP, yet still apply policy when it declines).
- `recompiler/src/code_generator.c` `emit_dispatch` — all three miss paths (`addr<0x8000` guard, bank-switch defaults, outer default) now emit `return nes_interp_dispatch(addr)`, and it emits `int g_recomp_push_all_jsr = <0/1>`.
- `runner/runner.cmake` — added `interp.c` + the shared `cpu6502_decoder.c` (+ its include dir).
- `runner/include/nes_runtime.h` — declarations for the new entry points + `extern int g_recomp_push_all_jsr`.
- `tests/interp_selftest.c` (NEW) — deterministic self-test (build note in its header).

### The boundary-handoff contract (the crux — from PHASE1_INTERP_FALLBACK_PLAN.md)
`interp_run` runs its own PC; it **never `call_by_address`es a return address** (that re-enters the world and overflows the C stack — the documented depth-510 trap in `code_generator.c:~2442`). It tracks `S_floor = g_cpu.S` at entry; when an RTS/RTI/unbalanced-pop lifts `S` above `S_floor`, it returns (the native C stack carries the return). Nested **missed** calls stay in one C frame on the shared 6502 RAM stack. Covered JSR/JMP targets hand off via a `call_by_address` **probe** (`s_probe_armed`/`s_probe_addr`): interp calls `call_by_address(target)`; on a miss the generated dispatcher re-enters `nes_interp_dispatch`, which sees the armed probe and returns 0 (so interp handles it inline — no recursion). **Precondition: `push_all_jsr`** (the 6502 stack must mirror the C call stack); interp self-disables when it's off. Env `NESRECOMP_INTERP_FALLBACK=off` force-disables.

## Verified state (my validation)

- **Compiles + links under MSVC**: recompiler, `interp.c`, `runtime.c`, and a regenerated Mega Man 3 dispatch all build clean.
- **MM3 fully builds** against the worktree (interpreter linked; `nes_interp_dispatch` resolved), **runs and renders the robot-master select screen correctly**, clean exit, no watchdog trips.
- **Self-test: 33/33 checks, 0 failures** (`tests/interp_selftest.c`): arithmetic+flags (ADC/SBC/NZC), the S_floor boundary, nested missed calls in one C frame via the probe (depth-510-safe), branch loops, covered-target handoff, indexed / `(indirect),Y` / RMW addressing.

## Outstanding / next steps (priority order)

1. **LIVE in-game play-test (do first).** The built interpreter exe is `Megaman3NESRecomp\build_release\MegaMan3Recomp.exe` (MM3 source is restored to pristine, but this already-linked exe still has Phase 1). Run it **with a physical controller** (no `--script`, so physical input is live): START → D-pad onto a Robot Master → A/START into a stage. Watch `build_release\dispatch_misses.log` and console `[Dispatch] MISS …`. EXPECT: interpreter fires on MM3 gameplay misses (banks 11/12: `0xC8DB`, `0xC782`, `0xC7C2`, `0x900F`, `0xFF1D`, `0x96BA`, `0xD701`) and the game keeps playing past them. If it freezes/glitches at a miss → real interp bug to chase. NOTE: scripted D-pad input did NOT navigate the MM3 select screen this session (every prior-session screenshot is also stuck on select) — looks like a pre-existing MM3 recomp nav gap, separate from Phase 1; a controller should get past it.
2. **Optional: automated A/B.** Validate on a game that reaches a miss headlessly, comparing `NESRECOMP_INTERP_FALLBACK=on` (continues) vs `off` + `NESRECOMP_DISPATCH_MISS=fatal` (exits at the miss).
3. **Phase 2 — auto-manifest discovery loop.** Emit discovered misses (already in `g_miss_ring` / `dispatch_misses.log`) to a machine-readable JSON + a fold tool that merges them into `game.toml` `fixed`/`bankN` seeds, so the next regen promotes interpreted blocks into AOT. (psxrecomp's manifest idea, resolved at build time.)

### Known Phase-1 limitations (documented, not bugs)
- Exotic return-address manipulation in a *missed* routine (PLA/PHA computed-goto, multi-level bail) isn't fully modeled — the uniform `S>S_floor` rule can return a step early; the recompiled caller's `S != _cbs` bail check still propagates, but `S` may be off by one. Handle such idioms via `game.toml` directives (the discovery loop surfaces them).
- Interpreted reads bypass `ram_read_hook` virtualization (game-specific; rare in missed routines).
- Coroutine `longjmp` through an interpreted frame is an untested edge.

## Build & run recipe (THIS harness — see memory `nesrecomp-claude-build-recipe`)

msys2/mingw gcc + devkitPro cmake are BROKEN here. Console stdout is NOT captured (redirect to files + Read them). `cmd /c <bat>` does NOT run the bat. All builds/runs need `dangerouslyDisableSandbox: true`. Use MSVC via PowerShell:

```powershell
$vs="C:\Program Files\Microsoft Visual Studio\2022\Community"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64'   # 'vswhere not recognized' warning is harmless
```

```
# 1) Recompiler (pure C; cl direct, no cmake). From _wt-multitier\build_recomp:
cl /nologo /O2 /I ..\recompiler\src ..\recompiler\src\*.c /Fe:NESRecomp.exe

# 2) Interpreter self-test (expect exit code 0):
cl /nologo /I ..\runner\include /I ..\recompiler\src ..\runner\src\interp.c ..\recompiler\src\cpu6502_decoder.c ..\tests\interp_selftest.c /Fe:interp_selftest.exe
# run via Start-Process -RedirectStandardOutput <log>; exit code == failures
```

### Re-prime MM3 to BUILD against the worktree (if you need to rebuild, not just run the existing exe)
MM3's `nesrecomp` is a junction → main checkout, with a SHA pin. To point it at the worktree:
```powershell
$mm3="F:\Projects\nesrecomp\Megaman3NESRecomp"
[System.IO.Directory]::Delete("$mm3\nesrecomp",$false)                       # remove junction only (never -Recurse)
New-Item -ItemType Junction -Path "$mm3\nesrecomp" -Target "F:\Projects\nesrecomp\_wt-multitier"
# set nesrecomp.pin sha = 068cfcff48ecc218eb7d04bba548c50d30aa09c1
# regenerate generated/ with the worktree NESRecomp.exe:
Copy-Item "$mm3\Mega-Man 3 # NES.NES" "$mm3\mm3.nes"   # space-free name (Start-Process arg-splitting)
# run NESRecomp.exe mm3.nes --game game.toml  (from $mm3, writes generated/mega-man-3_*.c)
# configure+build (force VS-bundled cmake + ninja + cl; copy ninja to a space-free path first):
$vscmake="$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $vscmake -S . -B build_release -G Ninja "-DCMAKE_MAKE_PROGRAM=<space-free ninja.exe>" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DENABLE_NESTOPIA_ORACLE=OFF
& $vscmake --build build_release
```
**Restore MM3 to pristine** (backups in `Megaman3NESRecomp\_phase1_bak\`): junction → `F:\Projects\nesrecomp\nesrecomp`; copy back `nesrecomp.pin` + `generated\mega-man-3_{full,dispatch}.c` + `_coverage.txt`. (Currently MM3 IS restored to pristine; the build_release exe is still the Phase-1 build.)

## Hard rules (nesrecomp — always repeat)

- **RULE 0:** No Ghidra = no action on game-specific 6502 behavior (`mcp__ghidra__get_program_info`). Framework work (the interpreter mirrors the recompiler's own semantics) is the exception, but verify any game-specific claim with Ghidra.
- **RULE 1:** Fix the tool, never the output. `generated/*_full.c` / `*_dispatch.c` are build artifacts — never read whole, never hand-edit. Bugs → `recompiler/src/code_generator.c` or `runner/src/*.c`; missing function → `game.toml` `extra_func` + regen.
- **RULE 2:** Check `PATTERNS.md` before implementing any Ghidra-discovered 6502 dispatch idiom.
- One game instance at a time; kill before relaunch. Prefer scripted runs / structured state over printf.

## Scope constraints for next session

Allowed: the live play-test (priority 1), the optional A/B (priority 2), and starting Phase 2 (auto-manifest). Iterate on `interp.c` only if the play-test exposes a real interpreter bug (chase via the boundary contract). Do NOT build the sljit/gcc/persist tiers (explicitly out of scope for NES — see proposal). Keep all work on `explore/multitier-recomp`. Don't disturb the user's Metroid work (separate).
