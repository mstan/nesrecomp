# Guarded native ROM execution under atomic patches

NES09, execution `beads-2dw.1.110`, campaign `beads-2wy8.1`.
Engine `e358f8112a3b8c67356ad953b877e7bf0e096508`; Zelda `cf52e9786b7f241caee2b5432d84f99a5bb1db08`.
Selected on the isolated feature branches. No main merge or release.

Remastered changes 2,902 PRG bytes, but only 79 bytes are marked compiled
instructions/operands. A conservative audit leaves 34 of 42 physical 1 KiB
compiled chunks unchanged. The prior global native-ROM ban was broader than
the code dependency contract required. These audit counts selected the
experiment; they are not a speedup or execution-coverage prediction.

Atomic edits now advance a monotonic image epoch. Optional compiled metadata
compares exact original instruction bytes for each physical chunk, including
its conservative two-byte operand tail. Cached admission belongs to that
epoch. Changed chunks interpret; exact restored code becomes eligible again.
External ROM data reads the current mapped image rather than an old constant.
Ordinary and checked chunks are separate generated families, selected at
dispatch, so ordinary folded data reads have no new per-read patch test.
Qualified checked execution uses the existing device deadline service.

Borrowing a writable PRG pointer disables ROM admission until a new image is
loaded. Epoch exhaustion fails closed for patched admission, including after
reload. Mapper changes refresh cached data pointers; unsupported mappings
fall back before forming pointers from RAM/open-bus flags. The qualification
covers cart profiles 0/1/4/66. FDS execution/testing remains parked. Host caches
are derived policy rather than serialized simulation. Native saves are intact.

Only Zelda selects `NESRECOMP_NATIVE_PRG_GUARDS=1`. Other titles retain the
default 0 implementation. Twenty-seven representative default-off Zelda runtime
and generated object files have identical code/data/unwind sections to the
selected SRAM package. This is a default-off code control, not a performance
claim or a substitute for final combined title checks.

| Profile | Host | Reference CPU | Candidate CPU | Reduction | Decision |
|---|---|---:|---:|---:|---|
|Stock|SERVER|9.328125s|9.281250s|0.503%|Neutral/inconclusive; 2 faster, 1 slower, 2 ties|
|Seamless Fit|DESKTOP-HN15M1H|1.593750s|1.515625s|4.902%|Positive in this matched run; 4 faster, 0 slower, 1 ties|
|Remastered HD|SERVER|6.109375s|5.937500s|2.813%|Neutral/inconclusive; 4 faster, 1 slower, 0 ties|

These are incremental results against SRAM/device-deadline engine `111b264`,
with the same CPU/sprite/APU/HD packages. Do not add them to earlier percentage
gains. Five alternating pairs plus excluded warmups; ordinary SDL/software
1280x720 gameplay with audio emulated and dummy-queued. Each binary loads its
own paused state; setup costs are excluded and retained separately. Stock
starts at frame 1801 and stops at 3001; Fit and HD stop at 2101. This is
whole-game process CPU evidence, not actual-FPS or level-completion evidence.
Shared capacity receipts and core telemetry are retained; other work remains
running. A capacity admission is not itself evidence of a performance gain.
Fit has four faster pairs and one tie; a five-of-five improvement rule is not
imposed. HD's final median is lower with four faster and one slower pair;
variation is retained and that mixed final run is conservatively inconclusive.

The first per-read-policy version was not selected: SERVER stock CPU rose
0.835%, all five pairs slower, despite HD improving 6.122%. Separate dispatch
families removed that repeated policy. The intermediate v2 measurements are
retained, including mixed workstation stock/Fit and SERVER HD +8.621%. The
selected exact binary also includes the unsupported-mapper pointer fix and
was measured again; only its table above defines the selected result.

Stock, Fit and HD native raster, hardware, RAM, full PCM, presented checkpoints
and serialized mod records match the reference. Fit room/actor records match
at both checkpoints with zero isolated-call failures. HLE and LLE fixtures
pass all four alignments, traced and untraced, for atomic code/data edits,
transactional data-only refusal, changed-chunk fallback/restoration, pointer
escapes, image reload/cache lifetime, epoch wrap, mapper changes under a live
cursor and current-build snapshot/serialized-state replay. Untraced modified
images actually use native ROM and the device cursor. The implementation is
portable C. Missing Remastered images/unsupported rules remain shared defects.
The selected HD run executes 58,192,369 guest cycles in compiled ROM (93.0%),
against zero in the reference. Its device cursor runs 36,069,836 cycles against
1,132,769 in the reference's SRAM scope. Coverage is not a speedup measurement.

Reference executable SHA256:
`69e94a24784f8453c981b77618d19ea34a8f2d285a0425742225a262f1f1b876`.
Selected executable SHA256:
`8bebd46a56ab92b51a802d919c99fb4a477a81fad5288943e4b01c3e83b0bb7d`.
It replaces the safe fallback object over the fully regenerated v2 build.
Final supported combined regeneration, feature checks and owner visual
approval remain open before merging/releasing.

[Machine-readable evidence](2026-10-28-guarded-rom-results.json) retains every
pair, source/patch/binary identities, output hashes, receipts and the verified
SERVER evidence archive. Source/build recipes and private staging are under
`G:/Temp/Codex/nesrecomp/pipeline-burndown-20261028`. No ROM or pack payloads
are committed or included in the retained evidence archive. Task scratch
remains for the active campaign and must be cleaned after final handoff.
