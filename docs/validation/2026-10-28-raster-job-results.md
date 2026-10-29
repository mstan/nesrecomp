# Whole stable HLE raster jobs

NES01, execution `beads-2dw.1.110`, campaign `beads-2wy8.1`.
Selected on isolated branch `perf/pipeline-burndown-20261028`, over source
checkpoint `9c8cbf2`; runtime commit `0f3e5c241f0fbf3a4e39666d98389f1a7aec457f`.
No main/master merge or release.

The current SMB3 gameplay profile contained 761 instruction-pointer samples;
374 (49.15%) resolved to the PPU implementation. Background fetch, half-dot
state, pixel output and OAM work remained among the hot functions after the
selected CPU/deadline, sprite and APU packages. This diagnostic ownership
sample selected the job; it is not a recoverable-cost or speedup estimate.

The HLE renderer now owns complete aligned eight-dot tile intervals inside
the existing native CPU/device cursor. It reads each nametable, attribute and
pattern source once, blends predecoded sprite output with background pixels,
publishes native pixels/opacity and optional background-consumer records,
and materializes fetch, shifter, color-pipeline and scrolling state at the
boundary. Secondary-OAM clearing advances four writes together; active OAM
evaluation keeps the data-dependent pair transition, including overflow
behavior. Sprite-zero timing, Zapper light clocks, open-bus decay deadlines
and MMC3 A12 edge timestamps are preserved without executing the full dot and
half-dot machines eight times.

The cursor still stops before CPU register/mapper/CHR writes, DMA and
observable interrupt/device boundaries. Partial tiles use the existing
adapter. Qualified cart families are NROM, MMC1, MMC3 and GxROM. Active HD
recording, read-triggered/other boards and LLE keep their working paths.
This is portable C, with no new ISA requirement or per-frame allocation.
The comparison-only `NESRECOMP_PPU_RASTER_REFERENCE=1` build retains the prior
sprite-output service. Normal HLE builds select the new service by default.

| Game/profile | Host | Reference CPU | Candidate CPU | Reduction | Faster/slower/tied pairs | Decision |
|---|---|---:|---:|---:|---:|---|
|Super Mario Bros.|SERVER|6.312500s|5.078125s|19.554%|5/0/0|positive in this matched run|
|Super Mario Bros. 3|SERVER|4.109375s|3.406250s|17.110%|5/0/0|positive in this matched run|
|Metroid|DESKTOP-HN15M1H|4.890625s|4.015625s|17.891%|4/1/0|neutral/inconclusive; mixed samples|
|Zelda stock|SERVER|9.812500s|7.687500s|21.656%|5/0/0|positive in this matched run|
|Gumshoe|SERVER|4.046875s|3.187500s|21.236%|5/0/0|positive in this matched run|
|Zelda Seamless Fit|SERVER|3.890625s|3.375000s|13.253%|5/0/0|positive in this matched run|
|Zelda Remastered HD|SERVER|5.609375s|5.656250s|-0.836%|3/2/0|neutral/inconclusive; mixed samples|

These results are incremental against the already selected package for each
game; percentages must not be added to earlier gains. All five titles have
the CPU/device, sprite and APU packages. Zelda also has the selected HD layer
plans, native SRAM and guarded ROM packages. Reference and candidate use the
same generated CPU and unchanged objects, source/flags and host settings;
their PPU build macro selects the old or new service. Source hashes, compile
and link commands, every object identity and executable identities are retained.
Source newline cleanup was verified to leave all ten complete measured PPU
object files byte-identical; both source byte hashes are retained.
The measured prototypes relink the changed runtime objects over supported
generated builds. Final supported combined regeneration remains a separate
campaign handoff requirement.

Each case has five alternating pairs and excluded warmups on one host under
the shared capacity reservation. Ordinary hidden SDL/software 1280x720
gameplay runs with audio emulated and queued to a dummy device. Each binary
loads its own current-build paused state; setup CPU/wall costs and raw core
telemetry are retained. SMB runs frames 361-1201, SMB3 2401-3001,
Metroid/Zelda stock 1801-3001, Gumshoe 601-1201, and Zelda Fit/HD 1801-2101.
Other owner work remained running. These are process CPU results, not FPS
claims; capacity admission alone is not performance evidence. Small mixed
results are neutral/inconclusive, and ties do not disqualify an improvement.
Metroid's local median is 17.891% lower, but its four faster pairs and one
7.029% slower pair occurred amid substantial host variation. It remains
inconclusive here rather than a proven win. HD's 0.836% higher median has
three faster and two slower pairs and is likewise inconclusive; no new HD
algorithm is claimed. Neither mixed run establishes a reproducible regression.

All seven cases match native raster, hardware, memory, full PCM, presented
checkpoints and serialized mod/background records. Gumshoe includes Zapper
input; Zelda Fit includes neighboring-room/actor records. The candidate's
whole-tile counter proves that the new service actually runs in each non-HD
case and remains unused under active HD recording. The HD case qualifies its
fallback behavior and performance; it does not claim a new HD algorithm.

The differential fixture passes 4,096 varied cases separately in HLE and
LLE, comparing the native interval against independent coupled CPU clocks.
Coverage includes four alignments, all four cart families, CHR ROM/RAM,
banking, mirroring/four-screen, fine X/scroll, palettes/emphasis/greyscale,
sprite priority/overflow/zero timing, Zapper, open-bus deadlines, background
records and full PCM. Each configuration also checks current-build serialized
save/load continuation with frame/background products. Existing native cursor
checks pass for DMA, IRQ, delayed registers, mapper changes, tracing and PAL
refusal, plus snapshot replay. No FDS ROM or execution test was run. Native
game saves and owner settings were isolated and preserved.

[Machine-readable evidence](2026-10-28-raster-job-results.json) retains raw
per-game samples, host receipts/telemetry, source/binary/object hashes,
correctness hashes and verified local/SERVER evidence archives. Private recipes,
source snapshots and staging are under
`G:/Temp/Codex/nesrecomp/pipeline-burndown-20261028`; ROMs and pack payloads are
excluded from Git and the retained evidence archive. Scratch remains for the
active campaign and is tracked for cleanup after final handoff.

NES01 now has a qualified selection decision for complete stable HLE raster
jobs. This does not claim architectural exhaustion of active HD recording or
all cartridge boards; the related opportunities remain separately tracked.
Final combined feature builds and owner visual approval are still open.
