# Sprite-output interval measurements

Candidate `935033d63cf1091eaedc7e489db297ffd481d8fd`, compared incrementally
to committed native-deadline engine `021c540f1370d6cd0bbf96c1c24c3679379e3752`.
Tracking `beads-2dw.1.110`, campaign `beads-2wy8.1`, opportunity NES01.

Each stable interval decodes the eight current sprite output units once,
resolves their visible pixels in slot priority, and commits counter/plane
advancement once. Background fetch/shift, next-line OAM evaluation, sprite-zero
publication, palette output, Zapper and mapper events keep their schedule.
Short intervals and active HD recording retain the ordinary raster operator;
the CPU/device interval service remains available. This is a sprite-output
stage replacement; background/fetch/OAM algorithms remain future work.

| Game / host | Reference CPU median | Candidate CPU median | Raw reduction | Interpretation |
|---|---:|---:|---:|---|
| smb / nas | 7.218750s | 6.375000s | 11.69% | Positive; 5/5 faster |
| smb / local | 5.625000s | 4.906250s | 12.78% | Positive; 5/5 faster |
| smb3 / nas | 4.703125s | 4.375000s | 6.98% | Positive; 5/5 faster |
| smb3 / local | 3.296875s | 3.250000s | 1.42% | Mixed/inconclusive; 2/5 faster |
| metroid / nas | 9.437500s | 8.843750s | 6.29% | Positive; 5/5 faster |
| metroid / local | 6.421875s | 6.015625s | 6.33% | Mixed/inconclusive; 4/5 faster |
| zelda / nas | 11.312500s | 9.968750s | 11.88% | Positive; 5/5 faster |
| zelda / local | 7.578125s | 5.875000s | 22.47% | Positive; 5/5 faster |
| gumshoe / nas | 4.265625s | 4.031250s | 5.49% | Mixed/inconclusive; 4/5 faster |
| gumshoe / local | 2.203125s | 2.406250s | -9.22% | Mixed/inconclusive; 3/5 faster |

These are additional CPU-time reductions against v2, not new credit for the
earlier package and not cumulative percentages to add to its results.
Five alternating pairs per row plus excluded warmups, matching own-build
states and inputs, actual SDL/software1280x720 and audio emulated/mixed/queued
to dummy output. Setup is excluded; resumed gameplay/death frames and normal
shutdown are included. Frame intervals are the same per-title routes as v2.
No level-completion, universal FPS, all-feature or netplay claim is made.

Untraced frame memory/raster, endpoint hardware/memory/picture and full PCM
match for every game; checked artifacts match on Intel i5-4670 NAS and Ryzen
9800X3D workstation. Paired HLE/LLE fixtures additionally exercise96 controlled
sprite cases per board/alignment: overlap, priority, transparency, counters,
fine scroll, planes, sprite-zero timing, plus mixed device traffic and own-
build replay. All four board families and NTSC alignments pass.

Gumshoe is neutral/inconclusive here: NAS has four faster pairs/one slower;
workstation three faster/two slower. The local raw median is9.22% worse amid
very large changing contention (candidate wall2.48..11.61s, CPU1.78..2.97s).
Keep these negative samples; do not market either Gumshoe median as a proven
gain or reproducible regression. Neutral games do not disqualify gains in
others. Five faster pairs is evidence in these batches, not a new mandatory
acceptance threshold. No minimum gain floor is imposed.

Complete source snapshots/patch, binary identities, unchanged object hashes,
compile/link recipes, raw pairs/core clocks and archive hashes are retained in
[the machine-readable results](2026-10-28-sprite-span-results.json) and the task
root `G:/Temp/Codex/nesrecomp/pipeline-burndown-20261028`. Prototype relinks keep
the existing generated units; they are isolated QA binaries, not final delivery
or a cross-build compatibility identity. Native saves/settings are preserved.
Private ROM input copies are excluded from evidence archives and source Git.

Next experiment: NES06 constant-output APU event spans, measured against this
selected package. Enhancement profiles and the remaining burndown continue.
No owner visual handoff, main/master merge or release has occurred.
