# TCP.md — NESRecomp Debug Server Protocol

The TCP debug server is the **only** sanctioned debugging interface for
nesrecomp game projects. If a piece of state cannot be observed over TCP,
**add a command** in `runner/src/debug_server.c` — do not work around it.

The same protocol is used by both the native recomp build and the embedded
Nestopia oracle.

---

## IMPORTANT — How to Find the TCP Server

The TCP server is **built into the nesrecomp runner**, not a separate tool.

- Implementation: `runner/src/debug_server.c` (~1750 lines)
- Header: `runner/include/debug_server.h`
- Game-specific extensions: via `game_handle_debug_cmd()` hook in each game's `extras.c`
- Python client scripts: each game repo has `tcp_*.py` helpers in its root

**If you cannot find the TCP server, you are looking in the wrong place.**
Search `runner/src/debug_server.c` — it is always there.

---

## Ports

Each game project uses its own port pair. Selection lives in
`extras.c::game_on_init()` — that is the single source of truth.

| Project | Native Port | Oracle Port |
|---------|-------------|-------------|
| Super Mario Bros. | 4370 | 4371 |
| Dr. Mario / Faxanadu / LoZ / Yoshi's Cookie | 4370 | 4371 |
| Mega Man 3 | 4372 | 4373 |
| Yoshi | 4380 | 4381 |
| Metroid | 5370 | — |

Do not change a project's ports without updating sibling docs.

---

## Activation

The TCP server is **not always on**. It is enabled by:

1. A `debug.ini` file in the same directory as the game executable, OR
2. The `--verify` or `--emulated` CLI flags (which imply debug mode)

To enable for a game: create an empty `debug.ini` next to the `.exe`.

---

## Transport

- TCP over `127.0.0.1`
- Single-threaded, non-blocking, polled once per NES frame on the runner
  side. Do not expect sub-frame latency.
- Line-based: send one command per line, terminated by `\n`. Receive one
  JSON response per line, terminated by `\n`.
- Two request encodings are accepted:
  - **JSON** (preferred): `{"cmd":"read_ppu","addr":"3F00","len":32,"id":7}`
  - **Bare**: `ping\n` — only for the simplest commands
- Responses are always single-line JSON: `{"ok":true,...}` or
  `{"ok":false,"err":"..."}`. The `id` field is echoed when supplied.
- Max command line: **8192 bytes** (`RECV_BUF_SIZE` in `debug_server.c`).
- Only one client at a time.

---

## Ring Buffer

Both servers maintain a per-frame ring buffer recording CPU/PPU/RAM/mapper
state. **Query it retroactively** with `get_frame`, `frame_range`,
`frame_timeseries`, and `history`. Do not pause the game to inspect a
single frame — pull it from the ring buffer instead.

Buffer size: 36,000 frames (~10 minutes at 60fps).

When you find a divergence, walk the ring buffer backwards to the FIRST
frame where it appears. That is the only frame worth tracing.

---

## Command Set

Source of truth: the `s_commands[]` table in
`runner/src/debug_server.c`. If you find a discrepancy with this list,
**the code wins** — fix this file.

### Heartbeat / Status
| Command | Purpose |
|---------|---------|
| `ping` | Liveness check. Returns frame counter. |
| `frame` | Current frame number + last function name. |

### CPU / RAM
| Command | Purpose |
|---------|---------|
| `get_registers` | A, X, Y, S, P, flags, current bank, frame. |
| `read_ram` | Read N bytes at addr (CPU address space). |
| `dump_ram` | Bulk RAM dump (up to 8192 bytes). |
| `write_ram` | Poke a RAM byte. Use sparingly — corrupts state. |
| `read_frame_ram` | Read RAM from a specific ring-buffer frame. |
| `restore_frame` | Restore full state from a ring-buffer frame. |

### PPU / VRAM
| Command | Purpose |
|---------|---------|
| `read_ppu` | Read CHR / nametable / palette ($0000–$3FFF PPU space). |
| `ppu_state` | PPUCTRL, PPUMASK, PPUSTATUS, scroll, latch state. |
| `scroll_trace` | Recent PPU $2005/$2006 write history. |
| `scroll_info` | High-level effective scroll state (origin, split, mirror). |
| `read_nametable` | Formatted 32×30 nametable grid + attribute table. addr=$2000/$2400/$2800/$2C00. |
| `dump_nametables` | Full 4KB nametable RAM dump in one call + mirror mode. |
| `read_palette` | Formatted palette dump (universal + 4 BG + 4 sprite palettes). |
| `read_oam` | Formatted sprite list (64 entries with x/y/tile/attr/palette/flip/visible). |
| `read_chr` | CHR tile dump. Optional `decode:1` returns 2-bitplane pixel arrays. |
| `screenshot` | Render current frame to PNG (optional path param). |

### Mapper
| Command | Purpose |
|---------|---------|
| `mapper_state` | Current PRG/CHR bank mapping, mapper registers. |

### Reverse debugger (rdb_*) — see `REVERSE_DEBUGGER.md`

Enabled by regenerating with `NESRecomp.exe --reverse-debug` and building
with `-DNESRECOMP_REVERSE_DEBUG=ON`. Implementation in
`runner/src/reverse_debug.c`. Zero cost when off — generator emits plain
`nes_write` and runtime omits the whole translation unit.

**Tier 1 — synchronous bus-write ring** (1 M entries, range-filtered):
| Command | Purpose |
|---------|---------|
| `rdb_status` | Unified status of all rdb_* state. |
| `rdb_range` `{lo,hi}` | Arm an address-range filter. Up to 8. |
| `rdb_range_clear` | Drop all armed ranges. |
| `rdb_reset` | Clear the store ring. |
| `rdb_count` | Entries in the ring. |
| `rdb_dump` `{start,max}` | Emit entries as JSON (`block`, `frame`, `addr`, `val`, `pc`, `func`). |

**Tier 1.5 — call trace** (64 k entries):
| Command | Purpose |
|---------|---------|
| `trace_calls` / `trace_calls_reset` | Arm / reset. |
| `get_call_trace` `{from,to,max}` | Dump `(frame, func, caller)`. |

**Tier 2 — block-level trace** (256 k entries, A/X/Y/P captured per block):
| Command | Purpose |
|---------|---------|
| `trace_blocks` / `trace_blocks_reset` | Arm / reset. |
| `trace_blocks_range` `{lo,hi}` | Restrict to a PC range (up to 8). |
| `get_block_trace` `{from,to,max}` | Dump `(frame, pc, func, a, x, y, p)`. |

**Tier 2.5 — block breakpoints + synchronous WRAM watchpoints**
(supersedes legacy `watch`/`follow`/`watch_s`):
| Command | Purpose |
|---------|---------|
| `rdb_break` `{pc}` / `rdb_break_clear` / `rdb_break_list` | Block-PC breakpoint table (16 slots). |
| `rdb_break_continue` | Resume from any parked state. |
| `rdb_step_block` | One-shot break on the next block entered. |
| `rdb_watch_add` `{addr[,val]}` / `rdb_watch_clear` / `rdb_watch_list` | WRAM-write watchpoints (16 slots). `val` omitted = any. |
| `rdb_watch_continue` | Resume from a watch hit. |
| `rdb_parked` | Unified park report: `reason` (1=break, 2=watch, 3=step), `pc`, `func`, `watch_addr`, `watch_val`. |

Parking calls `debug_server_poll` in a tight spin, so TCP stays live
while the CPU is stopped — send `rdb_break_continue` / `rdb_watch_continue`
to release. `rdb_parked` is safe to poll from outside the parked thread.

**Tier 3 — native WRAM anchors + reconstruction**:
| Command | Purpose |
|---------|---------|
| `rdb_anchor_on` `{interval}` | Snapshot 2 KB WRAM every `interval` blocks (default 4096, 64 slots). Also auto-records all $0000-$07FF stores into the Tier 1 ring. |
| `rdb_anchor_off` | Stop snapshotting; existing anchors retained. |
| `rdb_anchor_status` | Anchor count, interval, current block index. |
| `rdb_wram_at_block` `{block}` | Reconstruct 2 KB WRAM at `block` by finding the nearest prior anchor and replaying store-ring entries forward. Reports `store_ring_wrapped` if the store ring has evicted pre-anchor entries (indicates incomplete replay). Returns `hex` = 4 KiB hex string for the 2 KB. |

### Retired commands (use `rdb_*` instead)

Removed in the Tiers-1-3 cleanup. If a script still calls these, migrate:
| Retired | Replacement |
|---------|-------------|
| `watch` / `unwatch` | `rdb_watch_add` / `rdb_watch_clear` (same 16-slot limit, but **synchronous** — fires inside `RDB_STORE8`, not by polling). |
| `follow` / `unfollow` / `follow_history` | `rdb_watch_add` + `rdb_dump` (full attribution: `pc`, `func`, `block`). |
| `watch_s` / `unwatch_s` / `watch_s_history` | `trace_blocks` with PC filtering — S is one of many registers now captured per block. |

### Input
| Command | Purpose |
|---------|---------|
| `set_input` | Set controller bits permanently (hex bitmask). |
| `press` | Press buttons for N frames then auto-clear (default 2). |
| `clear_input` | Release all input overrides. |

**NES Button Bitmask:**
```
0x01 = Right    0x08 = Up
0x02 = Left     0x04 = Down
0x10 = Start    0x20 = Select
0x40 = B        0x80 = A
```

### Execution Control
| Command | Purpose |
|---------|---------|
| `pause` | Pause execution. |
| `continue` | Resume execution. |
| `step` | Step N frames (default 1). |
| `run_to_frame` | Run forward to a target frame number. |
| `quit` | Exit the runner cleanly. |

### Time-Travel Queries
| Command | Purpose |
|---------|---------|
| `history` | Ring-buffer recording summary. |
| `get_frame` | Full snapshot of one frame (CPU/PPU/mapper/RAM). |
| `frame_range` | Snapshots over a range (compact). |
| `frame_timeseries` | Specific fields over a range (very compact). |
| `first_failure` | First frame where verify-mode failed. |
| `frame_diff` | Show verify diffs for one frame, or compare two frames' RAM/NT/PAL/OAM. |
| `memory_diff` | Compare current RAM/NT/PAL/OAM vs a historical frame. region param: ram/nt/pal/oam/all. |

### Diagnostics
| Command | Purpose |
|---------|---------|
| `watchdog_status` | Why the watchdog tripped (if enabled). |
| `call_stack` | Recompiled call stack (if `RECOMP_STACK_TRACKING` enabled). |
| `dispatch_miss_info` | Dispatch table misses (if `ENABLE_DISPATCH_MISS_TRACKING`). |

### Oracle Tier 4 — rewind / step / delta (requires ENABLE_NESTOPIA_ORACLE + `--emulated`)

| Command | Purpose |
|---------|---------|
| `emu_step {frames}` | Run 1+ frames on the embedded Nestopia oracle (cap 600). Auto-captures WRAM for next `emu_wram_delta`. |
| `emu_snapshot` | Serialize full oracle state (~5 KB, 16-slot FIFO ring). Returns monotonic `tag`. |
| `emu_rewind_to {tag}` | Unserialize oracle back to the snapshot with that tag. |
| `emu_rewind_list` | List live snapshots: slot, tag, frame, len. |
| `emu_wram_delta` | `(addr, before, after)` for bytes that changed in the last `emu_step`. |

The Nestopia oracle must be initialized — pass `--emulated` (or
`--verify`) when launching the game. Without it, every `emu_*`
command returns `unknown command`.

Per-instruction oracle trace (`emu_insn_trace_*`) is deferred;
see REVERSE_DEBUGGER.md §Tier 4b for rationale.

### Game-Specific Commands
Game-specific commands dispatch via `game_handle_debug_cmd()` in each
game's `extras.c`. See each game's TCP.md for its custom commands.

---

## Python Tooling

Each game project has `tcp_*.py` scripts in its root. Conventions:
- Use simple socket connections to `127.0.0.1:<port>`
- Send JSON + `\n`, recv JSON + `\n`
- All scripts should accept `--port` for flexibility
- Never embed inspection logic inside the runner — keep it in Python

**Minimal Python client pattern:**
```python
import socket, json

def send_cmd(cmd, port=4370):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(('127.0.0.1', port))
    s.sendall((json.dumps(cmd) + '\n').encode())
    data = b''
    while b'\n' not in data:
        chunk = s.recv(4096)
        if not chunk: break
        data += chunk
    s.close()
    return json.loads(data.decode().strip())
```

**Shell note:** Inline Python in bash often fails due to shell escaping on
Windows. Write a `.py` file instead of using `python -c "..."`.

---

## Adding a New Command

1. Add a `handle_xxx` function in `runner/src/debug_server.c`.
2. Register it in the `s_commands[]` table.
3. Mirror it on the oracle side if it inspects emulator-internal state.
4. Document it in this file under the right section.
5. Rebuild the runner.
6. **Never** add a side-channel debug log instead. If TCP can't see it,
   TCP needs to grow until it can.
