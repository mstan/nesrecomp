# Reverse Debugger for nesrecomp

A build-flag-gated stepper + reverse debugger for statically recompiled
NES code. Not an emulator instrument: the recompiler itself emits the
observability, so what we see is the exact C the generator produces,
at C-block and C-line granularity. The embedded Nestopia oracle is
instrumented in parallel so recomp and oracle can be driven in
lockstep from one tool.

This doc mirrors `snesrecomp/REVERSE_DEBUGGER.md` but reflects the
actual forward state of that project (Tier 4 and embedded oracle
have shipped, not been deferred) and adapts the design to
nesrecomp's C-based code generator (shared with segagenesisrecomp).

## North star

Make every bug that's currently "wait why does recomp diverge from
Nestopia" solvable in one session by: watching every observable event,
pausing at any point, stepping forward or back one block or one
instruction at a time, and inspecting state mid-function without
rebuilding. Mid-loop, mid-dispatch, mid-NMI, whatever.

## Non-negotiables

- **Opt-in at compile time.** When the flag is off, ZERO code — no
  stubs, no comments, no `if (0)`, no empty function calls. The
  generator emits a different body entirely based on the flag, and
  the runtime header expands hook macros to `((void)0)`. A non-debug
  build is byte-for-byte what it is today.
- **Slowdown is expected when on.** No budget for "fast enough for
  production." Debug mode is for bisecting, not for playing.
- **In-memory only.** No files, no sockets as the hot path. Logs are
  ring buffers in the binary, dumped on TCP request.
- **Determinism is the axis everything else leans on.** Static recomp
  is already deterministic given (ROM, CIRAM-in, controller input).
  The log just records the sequence; replay = re-run.
- **Oracle parity is not optional.** Every tier that can be mirrored
  into the embedded Nestopia oracle IS mirrored. "Which side is
  right" is answered by diffing two ring buffers, not by reading
  C and speculating.

## Scope & architecture split

nesrecomp is shaped like segagenesisrecomp — a C-based generator
(`recompiler/src/code_generator.c`) and a C runner (`runner/src/`)
with one embedded third-party emulator (`nestopia_bridge.cpp`) as
the oracle. The SNES project is shaped differently (Python generator,
snes9x oracle with a function-pointer backend vtable). We take the
Genesis shape:

- Tiers 1, 2, 2.5, and 4 live in the **native (recompiled)** build,
  emitted by the generator and serviced by `runner/src/reverse_debug.c`.
- Tier 3 (per-insn CPU snapshot, full-emulator rewind) lives in the
  **oracle** build, hooked into the Nestopia bridge and serviced by
  `runner/src/oracle_trace.c`.
- Some Tier 1 commands (bus-write ring) are shared and work in both
  builds — the hook target is the bus callback, so whether the
  writer is the recompiled C or Nestopia's CPU doesn't matter.

Build flags:

```
-DNESRECOMP_REVERSE_DEBUG=ON   # native instrumentation (Tiers 1/2/2.5/4)
-DNESRECOMP_ORACLE_TRACE=ON    # oracle instrumentation (Tier 3, rewind)
```

Both default OFF. Release builds are byte-for-byte unaffected.

## Tier layout

We build tiers in order. Each tier is a standalone tool we exercise
against an actual open bug as its litmus test. If a tier closes the
current bug, we stop. If it doesn't, we build the next one. Later
tiers do NOT wait on earlier tiers' "we don't need this yet"
judgment — if the plan says a tier ships, it ships.

### Tier 1 — synchronous bus-write hook

**What it does.** Every generated `nes_write(x, v)` goes through
`RDB_STORE8(pc_hint, x, v)`, which records to an in-memory ring
buffer filtered by a TCP-configurable set of address ranges (up to
8 simultaneous ranges, 1M-entry ring). Each entry captures
`(frame, addr, val, pc_hint, func, caller)`. The caller slot is
read from the 6502 stack at capture time, so attribution works
across `call_by_address` / dispatch-table jumps where depth-based
attribution doesn't.

In the oracle build, the same ring is also fed by a pre-write
callback installed into Nestopia's bus. Same filter set, same entry
shape — recomp and oracle streams compare byte-for-byte.

Read trace (`RDB_LOAD8`) is the symmetric counterpart. It's gated
on the same flag; it exists to answer "what value was in $XX when
this branch evaluated" rather than just "who wrote $XX".

**What it costs.** Every write becomes a function call with 3 args
plus a range check. Tier 1 builds are ~20-40% slower. Entry cost
~ns when the address isn't in any armed range, ~tens of ns when it
is (ring push).

**What it wins.** No more polling blind spots. Every byte written
to `$072E` (world) or `$074E` (level) or any other scratch WRAM
address is logged with full attribution. Divergence becomes "dump
both write lists to address X and find the first one that
doesn't match" — minutes, not hours.

**TCP commands.**

- `rdb_range <lo_hex> <hi_hex>` — arm an address-range filter.
- `rdb_range_clear` — drop all armed ranges.
- `rdb_reset` — clear the ring.
- `rdb_dump [start] [max]` — emit entries as JSON.
- `rdb_count` — entries in the ring.
- `rdb_status` — is the ring active, how full, which ranges.

Oracle mirror:

- `emu_rdb_range / emu_rdb_reset / emu_rdb_dump / emu_rdb_count`

**Litmus test.** Instrument a `rdb_range 0x0700 0x07FF` and drive
the attract demo through the first level load. Diff the recomp vs
oracle write streams for $072E, $074E, $0770. First divergent write
points at the first broken routine.

### Tier 1.5 — call trace (shipped alongside Tier 1)

**What it does.** Per-`call_by_address` / `func_XXXX` entry ring,
64k entries. Each entry records `(frame, depth, func, caller, pc)`.
Hooked into the recomp stack tracker at function entry.

**TCP commands.**

- `trace_calls` / `trace_calls_reset` — arm / reset.
- `get_call_trace [from] [to] [max_depth] [contains]` — filter.

Stack-depth beyond ~16 is attribution-unreliable (matches SNES's
caveat); prefer Tier 2's `pc` filter past that depth.

### Tier 2 — block-level trace

**What it does.** `code_generator --reverse-debug` emits
`rdb_on_block(0xPCu)` at every basic-block boundary — function
entry and every `label_XXXX:`. The hook writes to a 256k-entry
ring with `(frame, depth, pc, func, a, x, y, p)`. A/X/Y/P are the
actual runtime CPU register values (NES CPU is small; cost of
capture is negligible). For the SNES this field is an "abstract
tracker value" the generator knows at emit time — on NES it's a
direct struct read, no tracking needed.

**TCP commands.**

- `trace_blocks` / `trace_blocks_reset` — arm / reset.
- `get_block_trace [from] [to] [func] [pc_lo] [pc_hi]` — filter.

Oracle mirror uses Nestopia's per-insn pre-hook to synthesize
"block enters" at known recomp block PCs so streams align.

### Tier 2.5 — pause-on-block + WRAM watchpoints

**What it does.** Two pause primitives share a single yield point
(`rdb_wait_if_parked`) inside the recomp main loop's coroutine.

*Block breakpoints.* `s_rdb_break_pcs[16]` holds armed PCs.
`rdb_on_block` checks `g_rdb_break_armed` (volatile-int fast path,
almost always falls through); on hit, parks the main coroutine and
exposes the parked PC via the `rdb_parked` command.

*WRAM watchpoints.* `s_rdb_watches[16]` holds `(addr, match_val)`
entries. `RDB_STORE8` checks `s_rdb_watch_armed` after the ring
record; on hit, parks the main coroutine. An optional `match_val`
restricts the watch to one specific written value (e.g., "pause
when $0E is written with 0x03"). The `rdb_parked` command reports
`watch_addr`, `watch_val`, `watch_width`, and the writing function.

Pause happens *after* trace recording, so the trace still captures
the parked event. Continue commands just unpark; any other armed
breakpoints/watchpoints stay live for the next hit.

**TCP commands.**

- `rdb_break <hex_pc>` / `rdb_break_clear` / `rdb_break_list` /
  `rdb_break_continue`
- `rdb_step_block` — arm a one-shot pause at the very next block
  hook
- `rdb_watch_add <hex_addr> [hex_val]` / `rdb_watch_clear` /
  `rdb_watch_list` / `rdb_watch_continue`
- `rdb_parked` — unified "why / where parked" report.

### Tier 3 — reverse stepping (time travel)

**What it does.** Two complementary mechanisms, pick per question:

*Native-side WRAM anchors.* Every N blocks, `rdb_on_block` snapshots
the full 2 KB CIRAM + 8 KB WRAM into a ring of anchor buffers.
`rdb_wram_at_block <block_idx>` replies with the WRAM state as of
that block by finding the nearest prior anchor and reading the
`RDB_STORE8` log forward. Gives "what did WRAM look like at this
instant in the past" without needing full CPU state rewind.

*Oracle-side full rewind.* In the oracle build, every N instructions
Nestopia's full state is serialized via `retro_serialize` into a
snapshot ring. `rdb_oracle_step_back` rewinds to the nearest prior
snapshot and replays forward. Gives true CPU/PPU/APU/mapper state
rewind at the cost of hundreds of KB per snapshot.

**TCP commands.**

- `rdb_anchor_on [interval]` / `rdb_anchor_off` / `rdb_anchor_status`
- `rdb_wram_at_block <block_idx>`
- `rdb_oracle_step_back` — requires oracle build
- `rdb_oracle_snap_interval <n>`

**Litmus test.** From a paused state where $072E disagrees with
oracle, `rdb_wram_at_block <N-k>` for k=1..100 until the value
still matches; re-walk the block trace from that point to find the
first mismatch.

### Tier 4 — per-instruction granularity

**What it does.** `code_generator --reverse-debug` emits
`rdb_on_insn(0xPCu, mnem_id)` at the top of every 6502 instruction.
The ring captures `(frame, block_idx, pc, mnem_id, a, x, y, s, p)`;
1M entries ≈ 16 MB. Mnemonic is a small int (opcode table index);
the table is published once via `rdb_insn_mnemonics`.

Per-instruction breakpoints live alongside block breakpoints:
`s_rdb_insn_break_pcs[16]` and `rdb_step_insn` give instruction
single-step. Useful for the rare bug where one block is wrong at
one specific insn and the 2.5 block granularity isn't sharp enough.

**TCP commands.**

- `trace_insn` / `trace_insn_reset` — arm / reset.
- `get_insn_trace [from] [to] [max]` — dump.
- `rdb_insn_mnemonics` — published once; caller caches.
- `rdb_insn_break <hex_pc>` / `rdb_insn_break_clear` /
  `rdb_step_insn`

Oracle mirror: `emu_insn_trace_on/off/reset/count`,
`emu_get_insn_trace` hooked into Nestopia's per-insn dispatch.

The SNES project originally marked Tier 4 "deferred indefinitely"
and later shipped it. We ship Tier 4 upfront here because NES
instructions are fixed-width and single-addr-mode per opcode — the
emit cost is a single inline call per insn, negligible next to
Tier 2's block hook.

## Build flag design

`runner/include/debug_server.h` (or a new `reverse_debug.h`):

```c
#ifndef NESRECOMP_REVERSE_DEBUG
#define NESRECOMP_REVERSE_DEBUG 0
#endif
#ifndef NESRECOMP_ORACLE_TRACE
#define NESRECOMP_ORACLE_TRACE 0
#endif
```

`code_generator` takes `--reverse-debug` and emits one of two code
paths for every store, every block entry, and every instruction.
There is no runtime branch in the store-emitting case — the
generator picks which C to write based on the flag, and a non-debug
regen produces byte-for-byte identical `<prefix>_full.c` to the
current baseline.

Build system: CMake options `NESRECOMP_REVERSE_DEBUG` and
`NESRECOMP_ORACLE_TRACE` add compile defs and conditionally link
`reverse_debug.c` / `oracle_trace.c`. Switching requires a full
rebuild and a regen pass (generated C differs). No shared objects,
no mixing debug + non-debug translation units — the runtime will
refuse to link if the generated C's hook calls don't match the
runtime's symbol shape.

## Embedded Nestopia oracle

Already present as `runner/src/nestopia_bridge.cpp` (libretro
Nestopia core). Currently monolithic — direct C entry points, no
vtable. We don't need a vtable yet (no second backend on the
roadmap), so we extend the bridge directly rather than inventing
an `nes_oracle_backend_t`. If and when a second core (Mesen, FCEUX)
becomes useful, refactor to the SNES pattern at that point.

New hooks we add to the bridge:

- `nestopia_bridge_write_hook(addr, old, new, pc24)` — install via
  libretro core options or a direct patch to the Nestopia source
  tree (same pattern as the snes9x oracle patch at
  `snesrecomp/runner/snes9x_oracle.patch`).
- `nestopia_bridge_pre_insn_hook(pc, a, x, y, p)` — single call per
  CPU dispatch, feeds both Tier 3 (snapshot ring) and Tier 4
  (oracle insn trace).
- `nestopia_bridge_serialize(out)` / `nestopia_bridge_unserialize(in)`
  — for rewind. Both already provided by libretro.

New `emu_*` commands (extending the existing 6 in
`nestopia_oracle_cmds.c`):

- `emu_step` — run exactly one instruction.
- `emu_wram_delta` — bytes that changed in the last `emu_step` /
  `run_frame`.
- `emu_wram_trace_add/reset/get` — ring for oracle-side writes.
- `emu_insn_trace_on/off/reset/count` — per-insn ring.
- `emu_get_insn_trace` — dump.
- `emu_nmi_count` — oracle's observed NMI count, for sync.

## Debug client

A thin Python client (`nesrecomp/tests/rdb.py`) wraps the TCP
commands with a REPL-ish interface and side-by-side oracle diffing:

```
> connect recomp 4370
> connect oracle 4371
> break func_8000
> continue
(paused at block 0x8000 frame 143)
> dump A X Y $0E $0F $072E
> step
> step_back 3
> write $072E 0x01   # patch state, keep going
> continue
> diff_writes $0700 $07FF --recomp --oracle
```

Built incrementally alongside each tier. The SMB litmus tests drive
this client first; manual netcat is the fallback.

## Oracle parity, concretely

Every tier, every ring, every filter must exist on both sides when
it makes sense for the tier. Native-only (Tier 2 breakpoint on a
recomp basic block, since Nestopia has no concept of "block") and
oracle-only (Tier 3 full rewind, since recompiled C has no
serializable CPU state) are explicitly marked in the command list
and return a clear "not supported on this side" error rather than
silently no-op'ing.

When a bug needs comparing, the client drives both binaries from
one script via matching ranges/breakpoints/watches, inspecting
diverging state directly.

## Rollout

1. Write this doc (now).
2. Build Tier 1 (native + oracle bus-write ring, filters, TCP).
   Exercise against a known SMB divergence (e.g., first level load
   tilemap) as litmus. If closed, stop.
3. Build Tier 1.5 (call trace).
4. Build Tier 2 (block hook emission in `code_generator.c`, runtime
   ring, TCP).
5. Build Tier 2.5 (breakpoints + watchpoints sharing the park path).
6. Build Tier 3 native (WRAM anchors) and Tier 3 oracle (rewind via
   `retro_serialize`) together — they're the same question from
   two sides.
7. Build Tier 4 (insn hook emission + ring, oracle mirror).
8. Refactor Nestopia bridge to a vtable only if a second backend
   actually appears.

At each tier boundary we gain a reusable tool for every future NES
recomp bug. That amortization is what makes this worth building
even beyond the immediate investigation.

## What this doc does NOT do

- Pick per-commit order below the tier level — that lives in
  commit messages as we land them.
- Define game-specific TOML extensions — `--reverse-debug` is
  framework-level and games don't need to opt in per-ROM.
- Dictate the name of every internal symbol — `rdb_*` is the
  reserved prefix; everything else is implementation detail.
