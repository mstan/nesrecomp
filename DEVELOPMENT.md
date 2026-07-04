# Development Log — nesrecomp multi-tier worktree (`_wt-multitier`)

Branch: `feat/kirby-mmc3-trampoline`. Game under bring-up: **Kirby's Adventure**
(MMC3 / mapper 4, 512KB PRG). This log records the recompiler/runner changes made
during Kirby MMC3 boot-cascade debugging. The recompiler and runner are the source
of truth; `generated/*` are build artifacts and are never hand-edited.

---

## Session 2026-06-18 — MMC3 cross-8KB dispatch correctness

### Summary

Advanced the Kirby boot cascade from the frame-0 `BRK $80D5` killer to a new
frame-26 frontier (`BRK $853E` + script-dispatch misses `$A4BD`/`$A860`). Three
changes, in cascade order:

1. **Seed three boot-script call-pointer handlers** (game config, on-disk only).
2. **Recompiler fix:** cross-8KB JSR must dispatch dynamically (remove the
   `same_bank_target` static-bind guard) — it was binding to the wrong 8KB half.
3. **Recompiler/runner fix:** add a **caller-bank fallback** to the dynamic
   dispatch so a stale `g_current_bank` at a cross-8KB call still resolves to the
   caller's own 16KB bank.

Net effect: `BRK $80D5` (bank14, frame 0) and `BRK $8B35/$8B43` (bank17, frame 4)
are both eliminated; boot now runs the full smoke window (`frames_run: 120`,
previously unwound early).

---

### 1. Boot-script handler seeding (`KirbysAdventureNESRecomp/game.toml`)

The boot-script call-pointer command (`$D1EF`, dispatched via `JMP ($6038)` at
`$D20F`) supplies an explicit handler pointer in the `$A000` window. With R7
mapping 16KB bank 30 there, the pointers `$A38D`/`$A27C`/`$A7D1` resolve to bank-30
high-half code (file offsets `$238D`/`$227C`/`$27D1`, all verified real code in
Ghidra). They are reached only through the runtime JMP-indirect dispatch, so the
function finder never sees them; without seeding they fell to the interpreter,
which ran them against the wrong bank and walked into `BRK $80D5`.

Seed (same mechanism as the pre-existing `$DA29` fixed-bank seed):

```toml
[functions]
fixed  = [ 0xDA29 ]
bank30 = [ 0xA38D, 0xA27C, 0xA7D1 ]
```

`game.toml` is on-disk only (KirbysAdventureNESRecomp is not a git repo in this
junctioned setup), so this is documented here rather than version-controlled.

### 2. Cross-8KB JSR must dispatch dynamically (`recompiler/src/code_generator.c`, `MN_JSR`)

The previous session added a `same_bank_target` guard that bound a cross-8KB JSR
statically to `func_<abs16>_b<bank>` whenever a same-bank wrapper existed. That is
**unsound** for window-crossing calls:

> MMC3 can map the SAME physical 8KB bank into EITHER CPU window — R6 odd puts a
> bank's *high* half at `$8000`; R7 even puts a bank's *low* half at `$A000`. The
> recompiler emits ONE function per 16KB-bank model address, but that function
> executes at `$8000` in one mapping and `$A000` in another, and its internal
> `JSR $80xx` therefore resolves to a DIFFERENT physical target each time. No
> static binding can be correct for both mappings.

Concrete failure (bank14): high-half code at model `$A148` does `JSR $80CC`. At
runtime R6=29 (odd) maps bank14's high half into `$8000`, so CPU `$80CC` means
bank14 high-half `$A0CC` (real code, `func_A0CC_b14`). But the static guard bound
it to `func_80CC_b14` — a **data-as-code false positive** (garbage body: `DEC $04`,
illegal `$0B`, illegal `$F7`, then `$00`) whose `$80D5` byte is the `BRK` that
killed boot.

Fix: force dynamic dispatch for all cross-8KB JSR (matching the `MN_JMP` cross-8KB
path, which always forced dynamic). `call_by_address` applies the R6/R7 odd/even
remap and selects the correct `func_<addr>_b<bank>` at runtime: for `$80CC` with
R6=29, `_bank` is computed on the original addr (`g_current_bank`=14), then addr
remaps to `$A0CC`, so the switch reaches `func_A0CC_b14`. Correct.

### 3. Caller-bank fallback in dynamic dispatch (`code_generator.c` + `runner/include/nes_runtime.h`)

Making cross-8KB JSR dynamic fixed bank14 but regressed bank17: `bank17 $A2B9
JSR $8B35` is the same instruction shape (high-half caller → low-window target),
but there the caller runs at `$A000` (R7 maps bank17 high half) with bank17's low
half at `$8000`, so the intended target is bank17 low half `func_8B35_b17`.
Dynamic dispatch missed it because `g_current_bank` was **0 (stale)** at that
call, not 17 — the very symptom the static guard had papered over.

Static-vs-dynamic is statically indistinguishable for these two cases (identical
shape, opposite correct answers); only the runtime mapping disambiguates. Rather
than chase the stale-`g_current_bank` root cause, the dispatch now takes a
**caller-bank hint**, which the recompiler knows statically (the call site is in
`func_*_b17`):

- New generated entry `int call_by_address_cb(uint16_t addr, int caller_bank)`;
  `call_by_address(addr)` becomes a thin wrapper calling `..._cb(addr, -1)`.
- On an inner-switch miss (address has variants but none for the active runtime
  bank), after the existing MMC3 R6-odd retry, retry once with the caller's bank:
  `if (_caller_bank >= 0 && _caller_bank != _bank) { _bank = _caller_bank; ... goto _dispatch_retry; }`.
- Cross-8KB JSR/JMP emit `call_by_address_cb(abs16, <caller_bank>)` via new
  `EmitCallOpts.use_caller_bank` / `.caller_bank` fields.

Rationale: cross-8KB calls are intra-16KB-bank by nature, so the caller's bank is
the correct fallback. The fallback only fires on a miss, so it never overrides a
correct runtime-bank resolution (genuine cross-bank calls with accurate R6 still
hit directly). Restricted to mapper 4 (the only cross-8KB mapper); other mappers
emit the unchanged interp default and pass `caller_bank = -1`.

Result: bank14 hits `func_A0CC_b14` directly (no fallback needed); bank17 misses
on `_bank`=0, retries with caller bank 17, and hits `func_8B35_b17`.

---

### Verified state (this session)

- Recompiler rebuilds; Kirby regen + build link clean; `--smoke 120` exits 0.
- `BRK $80D5` (bank14, frame 0): **eliminated**.
- `BRK $8B35/$8B43` (bank17, frame 4): **eliminated** (no static-bind regression).
- Boot reaches `frames_run: 120` (previously unwound early before the results print).

### New frontier (next session)

- `BRK $853E bank=14 (frame 26)`.
- Dispatch misses `$A4BD` and `$A860` via `call_site=$CCFC` (the script engine's
  `JMP ($6038)` indirect dispatch), recurring across banks 0/14/26. These are
  more boot-script call-pointer handlers — same discovery-gap class as the
  `$A38D`/`$A27C`/`$A7D1` seeding above, but they appear under multiple banks, so
  determine the correct bank(s) in Ghidra (R7 at each dispatch) before seeding.
  Note the JMP-indirect script dispatch does **not** carry a caller-bank hint
  (the caller is fixed bank31), so seeding — not the fallback — is the lever here.

### Reproduce / build

```
# env per PowerShell call (DevShell); dangerouslyDisableSandbox: true
$vs="C:\Program Files\Microsoft Visual Studio\2022\Community"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'

# 1. recompiler (build_recomp/)
cl /nologo /O2 /I ..\recompiler\src ..\recompiler\src\*.c /Fe:NESRecomp.exe
# 2. regen Kirby (from KirbysAdventureNESRecomp/)
& <build_recomp>\NESRecomp.exe "Kirby's Adventure # NES.NES" --game game.toml
# 3. build game
cmake --build build_debug
# 4. run headless
KirbysAdventureRecomp.exe "<rom>" --smoke 120 --smoke-interval 30 *> smoke.log
```

After committing `recompiler/src`, bump `KirbysAdventureNESRecomp/nesrecomp.pin`
`sha` to the new worktree HEAD or the CMake pin check fails the build.

---

## Session 2026-06-22 — single-variant dispatch ignored the live bank

### Summary

The `$853E`/`$A4BD`/`$A860` "frontier" from the previous session was a symptom of
a deeper recompiler bug, not a dropped bank switch. A boot-script call-pointer
handler dispatched via `JMP ($6038)` runs in whatever bank R7 maps at `$A000`,
but `emit_dispatch` emitted an **unconditional** call for any CPU address
discovered in only one bank — ignoring the live bank. So a handler pointer hit
under a different live bank **silently ran the wrong bank's routine**, and the
wrong-bank execution recorded no dispatch miss (the `a000_r6`/caller-bank
fallbacks rescued it to a wrong-bank variant).

Concretely: `$A295` was discovered only in bank17, so `call_by_address($A295)`
always ran `func_A295_b17` even though the boot script dispatched it with
`$A000`=bank30. bank17's `$A295` is a different routine; it flowed into
`$A2B9 JSR $8B35` and limped along via the caller-bank fallback (the single
"load-bearing" fallback fire the prior session chased).

### Fix (committed: `recompiler/src/code_generator.c`, `emit_dispatch`)

For MMC3 (mapper 4) single-variant switchable-region (`$8000-$BFFF`,
non-fixed-bank) addresses, emit the same `switch (_bank)` guard the
multi-variant path uses, so a live-bank mismatch falls through to the
interpreter (which reads the live bank correctly) and is **recorded as a miss**
instead of silently running the wrong bank. Gated to mapper 4 — non-MMC3 output
is byte-identical (verified: MM3 regen + `dispatch.c` compile clean).

Effect on Kirby cold boot: the caller-bank fallback stops firing (no longer
load-bearing); boot still reaches `frames_run: 120`; the previously-**hidden**
wrong-bank dispatches `$A295` and `$A82F` surface as misses.

### Handler seeding (`KirbysAdventureNESRecomp/game.toml`, on-disk only)

The guard surfaced the full set of boot-script call-pointer handlers. All five
dispatch under live `$A000`=bank30 (R7/2=30 — the `0/14/26` banks noted last
session were the misleading R6/2 field):

```toml
[functions]
fixed  = [ 0xDA29 ]
bank30 = [ 0xA38D, 0xA27C, 0xA7D1, 0xA295, 0xA4BD, 0xA82F, 0xA860, 0xA3F8 ]
```

Result: `dispatch_miss_count: 0` (was 188), all handlers native, boot `120`.
The Kirby intro sequence renders correctly.

### New frontier (next session)

- `BRK $853E bank=14 (frame 26)` — unchanged; the boot still unwinds here
  (this is why the title-screen transition closes). Separate from the dispatch
  bug fixed above. Next: Ghidra `$853E` in bank14.

### Methodology notes

- **Battery `.srm` poisons the baseline.** Kirby's boot branches on WRAM
  (`$6600`); a stale `saves/kirbys_adventure.srm` (written at the early-unwind
  exit) feeds garbage back in and changes the boot path. Delete it before every
  run for a deterministic cold boot.
- The caller-bank/`a000_r6` fallbacks should eventually become logged tripwires
  or be removed once discovery is complete; they mask wrong-bank execution.
  The complete follow-up is table-driven / miss-ring-manifest discovery of all
  script call-pointer handlers rather than per-handler `game.toml` seeds.
