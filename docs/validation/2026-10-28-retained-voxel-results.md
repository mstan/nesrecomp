# Retained Zelda voxel terrain

NES17, `beads-2dw.1.110`, campaign `beads-2wy8.1`. Runtime engine `4e8a5e0`,
title `fb448b6`, with its actual engine gitlink pinned to that runtime commit.
The independent validator accepts the scoped implementation and evidence;
unmerged PR/record handoff is pending.

| Zelda voxel view | Host | Reference CPU s | Candidate CPU s | Median reduction | Faster pairs |
|---|---|---:|---:|---:|---:|
| first-person | DESKTOP-HN15M1H | 1.593750 | 0.859375 | 46.08% | 5/5 |
| voxel | SERVER | 3.843750 | 2.140625 | 44.31% | 5/5 |

These incremental results compare against NES11's prepared-picture candidate,
not the original delivered game. Each matched pair uses identical settings,
host and affinity. Actual hidden SDL uses dummy video, software1280x720 and
emulated/mixed/queued audio to dummy output. Warmup and setup/loading are
excluded. The resumed1801..2101 route has movement followed by stationary
periods. Reuse benefits stable resolved terrain; these are bounded CPU results,
not FPS, GPU, all-motion or animated-terrain speedup promises. Percentages are
not additive and results from different hosts are not pooled.

The initial diagnostic with three real camera key events attributed roughly
965ms of nested triangle work within1050ms of total voxel QPC wall work across
299 frames. Geometry-only retention would miss that larger job. These are
instrumented nested wall scopes with scheduling/clock overhead, not exclusive
CPU attribution or recoverable-time bounds.

The opt-in renderer resolves the existing terrain callbacks into an owned,
bounded command stream, copying texture pixels including temporary side/card
textures. Exact geometry/material and camera/sky comparisons qualify reuse of
one terrain color/depth layer. Actors, contact shadows and the HUD remain
current. Allocation failure runs the ordinary portable renderer. Cached host
data is not serialized, is discarded on loads and mod activation, and uses
about1.28MiB in these scenes, bounded below36MiB. Ordinary callers and other
titles keep the original path; only Zelda enables the build flag.

Both actual game profiles match native frame logs, hardware/RAM, full PCM,
native/presented pictures and mod payloads. Original/candidate diorama and
first-person feature checks match all named pictures through paused reuse,
three aspects and current-build load/reload. Resumed candidate checks show one
terrain build and21/22 hits across21/22 advanced frames. Root and independent
fixtures exercise animated material/CHR/palette, geometry, view/pose/sky,
viewport/offsets, OAM cards/shadows/HUD, flag-off fallback and allocation failure.
The independent extension checks256 pictures and16 allocation-failure points.

Supported current CMake configuration and its exact compile commands build all
three changed C sources with the declared flag/new API. The title pins the real
engine commit. Full supported generation/link and final five-title owner builds
remain the combined campaign gate. No merge, release or owner-facing launch.
Dynamic actor assembly and other title voxel opt-ins remain explicit residuals;
this is a scoped selection, not architectural exhaustion. Shared Metroid restore
defects did not disqualify anything here. FDS remains parked.

The review's INI-newline concern was a false alarm and was retracted after
direct AST/byte counts. Original and repeated feature checks are valid; no
defect was fixed. v2 is retained as the final feature evidence. Raw staged
product metadata inherited older source hashes; the canonical exact retained
source hashes/frozen paths and object/link manifests in the
[machine-readable record](2026-10-28-retained-voxel-results.json) correct that
metadata without changing any binary or sample. Verified raw archives,
telemetry, settings and validation tools remain in the recorded evidence root.
