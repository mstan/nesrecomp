# Stable rewind preview ownership

NES21, `beads-2dw.1.110`, optimization campaign `beads-2wy8.1`.
Selected runtime `8df7b500e88466fe5dfd7fd78a555b114ba85071` on `perf/nes21-state-ownership`.
Actual PR/central durable-record handoff remains next; keep it unmerged.

| Title | Trial | Reference CPU s | Candidate CPU s | Median reduction | Faster/slower/tied pairs |
|---|---|---:|---:|---:|---:|
| smb3 | first-trial | 2.875000 | 2.812500 | +2.17% | 3/1/1 |
| smb | first-trial | 2.984375 | 2.921875 | +2.09% | 4/1/0 |
| metroid | first-trial | 3.062500 | 2.921875 | +4.59% | 5/0/0 |
| gumshoe | first-trial | 2.750000 | 2.625000 | +4.55% | 4/1/0 |
| zelda | first-trial | 4.218750 | 4.343750 | -2.96% | 1/4/0 |
| zelda | targeted-replication | 3.937500 | 3.843750 | +2.38% | 5/0/0 |

Each row has five alternating matched pairs on this PC, one fixed LP and
configuration. Each title carries its recorded selected CPU/PPU/APU package;
Zelda also has the selected isolated world/voxel/HD/SRAM code and NES19 canvas,
with Seamless Fit enabled. Packages are explicitly different; percentages are
incremental, not pooled across titles/trials or added to earlier results.
Small mixed/tied pairs remain neutral/suggestive. Gumshoe's last pair has
high variation(+21.86%): its median is not a precise or guaranteed gain.

Zelda's initial loss did not reproduce in the one exact targeted samehost/LP/
configuration/binary/route replication. Both opposing sets remain visible;
Zelda is neutral/inconclusive, with no pooled win or established individual
regression. Other titles' gains cannot offset an individual game's loss.
No accepted other-game check or broad host matrix was repeated.

The canonical serialized image and snapshot-ring ownership already existed.
The removed work is preview movement: each Entry owns82944bytes of immutable
thumbnail pixels, and erasing a full vector's first entry shifts about8.2MB
of retained entries per capture. A deque destroys only the oldest entry.
Depth100, cadence4,128MiB serialized-state budget, preview contents, texture
release and file/restore behavior are unchanged. This is portable C++17.

The first actual1200-frame diagnostic includes300captures,200full-depth
evictions and100,024,800serialized bytes. Nested QPC wall scopes measured
171.66ms capture,93.79ms serialization,77.87ms history management and24.05ms
thumbnail preparation. These are attribution, not exclusiveCPU/recoverable
bounds. The diagnostic and primary SMB3 route start in the grass level and
include death/world-map afterward: not1200frames entirely active-level play.
The earlier world-map diagnostic is retained separately, not misrepresented.

Ordinary SDL runs use dummy video/software1280x720 and emulated/mixed/queued
audio to dummy output, volume0/no-save. Load/menu/window setup and warmups are
excluded. Each interval starts with empty history after load; it includes
fill then steady eviction. No cadence/depth reduction, trace instrumentation,
physical-GPU/FPS claim or owner-facing window.

Allfive native raster/hardware/RAM/fullPCM/native+presented checkpoint hashsets
match. The actual rewind browser in both builds fills100entries, selects eight
older snapshots, commits the32-frame rewind, drops the future and refills100.
Each restored picture and2048CPU-RAM bytes match an independent native headless
replay at its observed tick. Current-build file load restores exact picture/RAM;
truncated-state refusal leaves frame/RAM/picture unchanged. Saves are isolated.

Independent fixtures use actual frozen UI lifetime functions and real rbengine
ring with stub host snapshots and destructor-tracked textures. Each build passes
3839checks,504previews released exactly once,505captures and3restores: full
default history, byte-budget eviction, blob/preview mapping, refused/committed
branches, refill/backward-frame clear, cadence/reconfigure/unavailable/reset/
shutdown. No serializer reimplementation or old-state migration is claimed.

The first GUI fixture's one-frame shortcut expired before input evaluation;
the QA-only three-frame hold corrected it. Raw failed-harness evidence remains.
Product convenience PPU hashes/sizes and binary sizes inherited stale values.
Actual paired link manifests were always correct; canonical fields were repaired,
ten exact raw originals and per-file normalization receipts retained, with no
CPU/output/settings/binary-ID changes. All ten binaries differ only at the
UI object within their pair. Exact title heads and ROM identities are explicit.

Six SHA/CRC-verified raw/source archives retain recipes, source/compile/link
identities, settings, telemetry and native samples. Audited five-title staging
and two diagnostics were removed afterward; bounded products/sources remain
for final campaign integration, recorded by `history-storage.json`.

The [machine-readable record](2026-10-31-rewind-history-results.json) has the
complete samples and evidence identities. Serializer temporary-buffer/direct
writer and eager-thumbnail work remain explicit residuals. Full supported
five-title regeneration/link and combined feature/performance plus owner visual
approval remain pending. No merge/release; FDS stays parked.
