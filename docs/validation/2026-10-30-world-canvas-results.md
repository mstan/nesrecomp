# Retained seamless world viewport

NES19, `beads-2dw.1.110`, optimization campaign `beads-2wy8.1`.
Zelda `b2bd8fb` on `perf/world-materials-nes19`; engine gitlink remains `4e8a5e0`.
The independent reviewer accepts the source, tests, measurements and current
supported CMake checks and final docs/register. Following its explicit instruction,
[Zelda PR13](https://github.com/mstan/LegendOfZeldaNESRecomp/pull/13) is open,
based on `perf/retained-voxel-nes17`; its head is `b2bd8fb`. Actual PR/durable-record
readback is the remaining serial handoff. The PR remains unmerged.

| Zelda workload | Host | Reference CPU s | Candidate CPU s | Median reduction | Faster/slower/tied pairs |
|---|---|---:|---:|---:|---:|
| Seamless Fit | DESKTOP-HN15M1H | 1.015625 | 1.015625 | +0.00% | 3/0/2 |
| Full seamless atlas | SERVER | 13.312500 | 10.406250 | +21.83% | 5/0/0 |

Fit is neutral/inconclusive within quantization and variation: equal medians
are not a proven win or proof of zero overhead. Full seamless atlas improves
in all five pairs. Each pair uses the same host/settings/affinity; raw negative
or tied pair values are retained. These incremental CPU reductions are not
additive with earlier packages, pooled across hosts, or FPS/GPU promises.
Frames1801..2101 include movement followed by stationary periods. Warmup and
TCP/setup/loading are excluded; resumed gameplay and normal shutdown count.
Actual SDL uses dummy video, software1280x720 and emulated/mixed/queued audio
to dummy output. The full map runs all128 room simulations, not terrain-only
mode2. Existing F4 semantics are selected reproducibly through common QA controls.

Rooms already had persistent index/ARGB rasters. The initial instrumented
diagnostic found only8.73ms of atlas room-raster preparation, versus2835.40ms
of full-canvas clear/copy inside3140.56ms of presentation over299 frames.
Room simulation separately took6339.17ms. These nested SDL-counter wall scopes
include scheduling/clock overhead and are not exclusive CPU attribution or
recoverable bounds. The selected job is the complete terrain viewport: retain
it, restore only old actor rectangles, then draw current actors and HUD.

Camera/size/background, raster identity/revision and owned/valid room flags
qualify reuse. Moving views rebuild. Inventory, local/portal/system/death paths
invalidate as required, and load discards derived validity. Allocation failure
keeps the full compositor. Observed atlas terrain capacity is24,117,248bytes
(23MiB), bounded to32MiB, and may persist after shrinking until reset/release.
Native saves and guest state are not owned by this cache. Engine runtime and
other titles are unchanged; no new timing claim is made for their packages.

Both actual profiles match native frame logs, hardware/RAM, full PCM, native/
presented PNGs and mod payloads. Eight individual original/candidate gameplay
bundles pass: deterministic paused Fit/atlas pictures, adaptive resize/return,
current-build load equality,128-room live atlas, inventory position preservation,
native full-width bomb flash, lethal-contact death/Game Over/Continue/Retry,
three Continue aspects, real cave/dungeon entry/exit and portal-state loads.
All11 deterministic paused pictures match across binaries. Both effects suites
observe grayscale chroma0 across expanded terrain. Root reviewed actual world,
bomb, cave and wide Continue captures. No owner-facing game window was opened.

Review caught an unobservable mutation in the initial100-canvas fixture. The
revised case changes an enabled visible room pixel after reuse and asserts both
rebuild and exact new pixel. Root reran it; the reviewer independently passes
that fixture and240 scalar-oracle canvases including overflow/allocation recovery.
This is a test coverage correction, not a runtime fix or a new timing binary.

Supported current CMake configuration, exact compile commands for the three
changed C files, and the actual optional world-canvas test target pass. Full
supported generation/link and the final combined five-title/owner gate remain
pending. The baseline is prior world logic plus passive common viewport QA
controls/help; it is not labeled as the unchanged preceding EXE. Exact packages,
frozen sources, compile/link identities, raw pairs/settings/telemetry and verified
archives are in the [machine-readable record](2026-10-30-world-canvas-results.json).
Incremental camera-shift reuse remains a deferred residual; upload is NES24.
This scoped selection does not claim architectural exhaustion. FDS stays parked.

Four SHA-256/CRC-verified evidence/source archives are retained (about29MiB).
Audited temporary staging on both physical hosts and the three disposable input
ZIPs were removed. Bounded frozen sources, products, recipes and supported-build
configuration remain for final integration; `world-canvas-storage.json` records
exact retained paths, archive identities and cleanup receipts.
