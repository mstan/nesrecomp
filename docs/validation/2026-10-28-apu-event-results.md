# APU event-span measurements

Candidate `9bbb04aa1ec5a8dc176c82a2ce829177319d8528` compared incrementally to
selected sprite-output engine `935033d63cf1091eaedc7e489db297ffd481d8fd`.
Tracking `beads-2dw.1.110`, campaign `beads-2wy8.1`, opportunity NES06.

Stable base-APU spans advance countdowns and silent channel phases to the next
channel, sequencer, DMC, register-delay or output-sample event. Event cycles
keep the original executor. Every floating-point accumulation keeps its
original scalar order. Dense audible timers, short spans, audio-off and
unqualified devices retain the portable per-cycle path. No expansion-audio
or PAL acceleration is claimed.

| Game / host | Reference CPU median | Candidate CPU median | Raw reduction | Interpretation |
|---|---:|---:|---:|---|
| smb / nas | 6.546875s | 6.468750s | 1.19% | Small positive; 5/5 faster |
| smb / local | 4.781250s | 4.546875s | 4.90% | Neutral/inconclusive; 3/5 faster |
| smb3 / nas | 4.281250s | 4.250000s | 0.73% | Neutral/inconclusive; 3/5 faster |
| smb3 / local | 2.703125s | 2.562500s | 5.20% | Neutral/inconclusive; 3/5 faster |
| metroid / nas | 8.671875s | 8.515625s | 1.80% | Neutral/inconclusive; 4/5 faster |
| metroid / local | 5.015625s | 4.859375s | 3.12% | Neutral/inconclusive; 2/5 faster |
| zelda / nas | 10.031250s | 9.812500s | 2.18% | Small positive; 5/5 faster |
| zelda / local | 4.906250s | 4.718750s | 3.82% | Neutral/inconclusive; 3/5 faster |
| gumshoe / nas | 4.000000s | 3.968750s | 0.78% | Neutral/inconclusive; 4/5 faster |
| gumshoe / local | 1.875000s | 1.828125s | 2.50% | Neutral/inconclusive; 3/5 faster |

SMB1 and Zelda show small NAS improvements; the other rows have mixed or tied
pairs and are inconclusive within the recorded variation. No reproducible
per-game regression was found. Neutral games are acceptable alongside games
that improve. Five faster pairs is supporting evidence here, not a mandatory
acceptance threshold. No minimum gain floor is imposed. These incremental
percentages cannot be added to the previous packages or credited as gains
against master.

All five untraced frame/raster, hardware, memory, endpoint picture and full
PCM comparisons pass on both hosts; checked artifacts also match across
hosts. Expanded HLE and LLE fixtures pass at all four NTSC alignments on
NROM/MMC1/MMC3/GxROM, at44.1/48kHz, including dense and silent channel
timers, delayed writes, DMA/DMC/IRQs, two uninterrupted five-step sequences
and current-build snapshot replay.

Each row contains five alternating pairs and excluded warmups, with own-build
states, matching inputs and the previously recorded per-title gameplay/death
interval. Actual SDL/software1280x720, audio emulated/mixed/queued to dummy
output, setup excluded and shutdown included. The workstation and NAS were
shared as the owner requested; raw core activity, pair order, positive and
negative samples are retained. No level-completion or universal FPS claim.

[Machine-readable results](2026-10-28-apu-event-results.json) retain raw
pairs, source/object/binary hashes and evidence archive identities. Task root:
`G:/Temp/Codex/nesrecomp/pipeline-burndown-20261028`. Runtime-only prototype
relinks preserve generated units and the selected sprite object explicitly.
Final supported regeneration and enhanced-profile qualification remain before
the owner handoff. Native saves/settings are untouched. Private ROMs and pack
assets are excluded from source Git and evidence archives.

Next: measure the real enabled Zelda Remastered HD recording/composition job.
Other burndown opportunities remain; FDS is parked. No main/master merge or
release has occurred.
