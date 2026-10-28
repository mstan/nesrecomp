# Guarded native cartridge SRAM with device deadlines

NES04 / `beads-2dw.1.105`, execution `beads-2dw.1.110`, campaign `beads-2wy8.1`.
Selected for Zelda's qualified fixed 8 KiB SNROM profile on the isolated feature
branch. No main merge or release. [Machine-readable results](2026-10-28-sram-native-results.json)
retain source/binary identities, raw sample references and capacity receipts.

Zelda already declares a utility-code copy from ROM bank 1 at A500 into
SRAM 6C90–7FFF. The cycle compiler now reuses that declaration and discovered
ROM calls/jumps. Runtime validates each view's folded bytes and physical/read
profile. CPU/mod writes and committed deltas invalidate code; mapper stores
end the function, and every subsequent admission checks both A12-selected
SRAM gates. Banked/unknown profiles interpret. Dedicated SRAM code reads
preserve driven bus values without applying ROM-only overlays. Qualified
views use the existing native device deadline service; DMA, interrupts,
observers, dense audio and unqualified intervals retain their normal path.

These are incremental results over the already selected CPU/sprite/APU and
Zelda HD package. Do not add these percentages to earlier packages.

| Profile | Host | Reference CPU | Candidate CPU | Reduction | Decision |
|---|---|---:|---:|---:|---|
| Stock | SERVER |9.890625s|9.843750s|0.474%|Neutral/inconclusive;2 faster,2 slower,1 tie|
| Seamless Fit | SERVER |4.265625s|4.046875s|5.128%|Positive;5/5 pairs faster|
| Remastered HD | SERVER |6.500000s|6.406250s|1.442%|Small positive;5/5 pairs faster|
| Stock | Workstation |6.750000s|6.546875s|3.009%|Mixed under contention; inconclusive|
| Seamless Fit | Workstation |2.875000s|2.531250s|11.957%|Mixed under contention; no independent win claim|

Each batch uses ordinary SDL/software at 1280x720 gameplay, emulated audio queued
to a dummy output, its own current-build paused state, excluded setup/warmups
and five alternating pairs. Stock covers 1801–3001; Fit/HD cover 1801–2101.
The shared installed `benchmark_hosts.py` admits and reserves actual capacity.
The focused SERVER stock/Fit run resolves mixed workstation timing and reuses
the exact same current-build states; it does not repeat accepted output captures.
All pairs remain on one host/configuration. Core counters and before/after
capacity telemetry are retained; other jobs were preserved.

Native raster, hardware, RAM, full PCM and presented checkpoints match the
reference in all three profiles. Fit also matches serialized room/actor state
at both checkpoints, with zero failed isolated calls. The retained non-isolated
coverage counters report no interpreted stock/Fit SRAM instructions. Isolated
coverage counters restore with their snapshot, so these counters do not prove
complete speculative native coverage. Two Remastered variants
correctly reject and interpret; coverage is not an acceptance requirement.
The pack's missing image and unsupported rules remain shared limitations.

Synthetic HLE and LLE cases pass at all four alignments: bus/memory/hardware
and PCM parity under self-modification, OAM/DMC DMA and NMI; code gate closure;
refusal when A12 can disable reads; refusal of banked SRAM; public CPU/mod code
writes; isolated and committed deltas; current-build snapshots; PRG overlays.
Separate untraced cases actually exercise the SRAM deadline cursor and match
interpreter hardware/RAM/raster/PCM. The single-thread fixture batch inherited
one logical CPU and held a 0.75 physical-core claim on the workstation's 2-way
SMT topology. FDS games/testing remain parked.

The earlier native-SRAM-only prototype gave SERVER Fit +4.151%, with mixed
stock −0.799% and HD −1.741%. It was replaced by this integrated package rather
than selected as a universal win. Those small mixed results are inconclusive.

Steam Deck passed the SRAM guard executable under installed Proton 8. Its full
SDL runtime was not qualified: direct invocation lacked Wine dependencies,
the Proton 8 launcher hit an unimplemented propsys function, and the Proton 11
prefix upgrade stalled before any game samples. Only task-owned processes
were stopped. All 939,835,881 bytes of disposable Deck staging and its 1418
links were removed without following links into owner folders. Keep Deck in
the shared pool for compatible, verified workloads; no performance claim here.

The next NES09 audit found that Remastered changes 2902 PRG bytes but only 79
marked compiled instruction/operand bytes. A conservative physical 1 KiB chunk
audit leaves 34 of 42 compiled chunks unchanged. This identifies an overly broad
global native-ROM ban worth evaluating; it is not a speedup or dispatch proof.
Final combined five-title builds, feature checks and owner approval remain open.
