# Whole-view tile plan: Metroid evaluation deferred

NES16, `beads-2dw.1.110`, optimization campaign `beads-2wy8.1`.
Neither prototype is selected. Source correctness passed, but neither actual
built-game experiment demonstrated a CPU win. No runtime commit or PR is filed.

| Version | Metroid profile | Reference CPU s | Candidate CPU s | Median reduction | Faster/slower/tied pairs |
|---|---|---:|---:|---:|---:|
| v1 | 21:9 | 2.703125 | 2.718750 | -0.58% | 3/2/0 |
| v1 | 32:9 | 2.656250 | 2.609375 | +1.76% | 3/1/1 |
| v2 | 21:9 | 2.609375 | 2.671875 | -2.40% | 0/3/2 |
| v2 | 32:9 | 2.593750 | 2.640625 | -1.81% | 1/3/1 |

These are five alternating matched pairs per row on SERVER, each on one host,
configuration and affinity. V1 is neutral/inconclusive within overlapping
variation. V2 has negative medians with mixed/tied pairs; this is no basis to
adopt its added machinery, rather than proof of a universal slowdown. Profiles
and experiments are separate; gains cannot compensate a per-profile loss.
There is no third tuning trial after two unsuccessful improvement experiments.

Metroid already emitted horizontal8pixel spans. The tested alternative resolves
source/attributes once across a two-dimensional tile rectangle, splitting at
world tiles and native/margin boundaries. V1 writes each tile's rows; V2 builds
fixed128-span batches (3072bytes stack onWin64) and writes contiguous scanlines.
Both are portable C, with no persistent cache or guest-state ownership change.
The roughly87% reduction in fixture source-call count is not a runtime win.

Before implementation, actual moving21:9/32:9 diagnostics spent73.53/113.66ms
in background composition inside76.30/116.57ms compositor wall scopes across
399 pictures. Camera moves660..1132 and660..1108. These nested QPC wall scopes
are attribution only, not exclusive CPU or a recoverable bound.

The retained SDL route uses explicit release after the debug menu closes, then
RIGHT+A+B. An initially held route was suppressed by the correct input-release
latch; those fixed-camera attempts are excluded, as is a title-menu boot attempt.
The real32:9 preset is854 pixels (even geometry), correcting a853 test assertion
after the diagnostic had completed. No runtime defect or optimization failure
is inferred from these fixture corrections.

Native frame logs, hardware/RAM, full PCM and native/presented images match in
both versions/profiles. Each version passes336 author full-color/opacity cases
and1536 independent portable plus1536 METROID_CYCLE canvases using actual
cell_source. Boundaries include negative/offmap coordinates, fine scroll,
worldY239→240, captured9bit scroll/currentNT, live PPU versus unstreamedRoomRAM,
decoder/cache/unknown cells, native-only clip, background off, HUD/priority,
fallbacks and the128-span batch edge.

Each version's four actual feature legs (ordinary and optional PC actors in
original/candidate) pass. Twelve corresponding paused pictures per style match:
Fit/adaptive560→426→560→854→256→256→560, unchanged frame/RAM, current-build
save/load, moving-route end-state load, and stock→wide return. Real margin
terrain/entities and edge HUD are visible; no black-margin shortcut. Existing
historical Metroid restore caveats remain separate, and are not a disqualifier.

V1 inherited a stale selected_ppu_object convenience field; both actual link
manifests always used the same selected raster object. The exact correction
and full object hash are in the machine-readable record; raw results stay intact.
Untouched objects, source snapshots, native compile/link commands, settings,
capacity receipts, telemetry and samples are retained in five verified archives.
Nine temporary staging roots and input ZIP copies were removed after retention;
the storage ledger names bounded remaining products/prototypes and cleanup needs.

SMB1, SMB3 and Gumshoe were source-audited and remain unchanged/unmeasured.
Their existing row/span work, default black-canvas fallback contract, HUD and
sprite/priority boundaries are not newly claimed optimizations. Broader whole-view
producer alternatives remain deferred; this scoped decision does not exhaust
NES16. Revisit them when measured inclusive cost selects a different substantial
job. Upload/conversion is still NES24.

See the [machine-readable record](2026-10-31-wide-view-results.json).
Final combined supported five-title generation/link, feature/performance and
owner visual approval remain pending. No merge/release; FDS stays parked.
