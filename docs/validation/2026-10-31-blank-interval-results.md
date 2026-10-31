# NES08 settled nonvisible device intervals

Select [PR87](https://github.com/mstan/nesrecomp/pull/87), runtime
`06f7b48`, for final combined integration. The device service now advances
settled nonvisible NTSC spans in bulk, including rendering-enabled VBlank.
CPU instructions, bus accesses and elapsed guest cycles are retained.

| Stock game | Baseline median CPU s | Candidate median CPU s | Raw reduction | Paired directions | Interpretation |
| --- | ---: | ---: | ---: | --- | --- |
| smb3 | 3.312500 | 3.171875 | +4.245% | 2 faster, 1 slower, 0 tied | Neutral/inconclusive |
| smb | 3.546875 | 3.687500 | -3.965% | 2 faster, 1 slower, 0 tied | Neutral/inconclusive |
| metroid | 3.531250 | 3.312500 | +6.195% | 3 faster, 0 slower, 0 tied | Observed positive |
| gumshoe | 2.859375 | 2.796875 | +2.186% | 2 faster, 1 slower, 0 tied | Neutral/inconclusive |
| zelda | 4.562500 | 4.609375 | -1.027% | 1 faster, 1 slower, 1 tied | Neutral/inconclusive |

Each first trial has three matched pairs on this PC after excluded warmups.
All five routes have identical frame/device/memory, picture and PCM hashes.
Timing restores each build's current gameplay state; input routes, features,
affinity within a trial, settings, package identities and host telemetry are
retained. Within each game's A/B pair, only the two device objects change.

SMB1's initial median is slower despite two faster pairs and one slower pair.
Its proportional follow-up repeats timing only: five pairs using the same
binaries, route and current states, with a separately retained affinity.
Median CPU is 3.375000 to 3.390625 seconds (-0.463%), one 15.625 ms timer tick;
one pair is faster, three slower and one tied. This does not establish a
reproducible regression. Both trials remain separate and include all samples.
SMB3 and Gumshoe likewise have widely mixed pairs; Zelda has one faster,
one slower and one tie. Their positive or negative raw medians are not proven
effects. Metroid is faster in all three sampled pairs, with a 6.195% median
CPU reduction. Results are scoped to these workloads and this host; no
aggregate gain offsets an individual regression.

The new proof stops within scanlines 240 through 260 before scanline/frame
edges, excludes unsettled register/status/OAM state and HD recording, and
preserves Zapper elapsed dots, color/background pipelines and I/O bus decay.
NMI input and output must already agree with no pending edge; the service
retains the real level instead of forcing it low. PAL, other boards, DMA,
IRQ, trace and unsupported configurations retain working clock paths.

HLE and LLE native-versus-coupled fixtures pass across four boards and four
NTSC alignments. Explicit cases exercise held-high NMI, status/mask changes,
two decay deadlines and current snapshot replay. The sprite fixture remains
on visible scanlines. Independent review found and fixed an unsettled render
counter admission, then verified source recipes, untouched object identities,
all output hashes, per-game samples and the SMB1 follow-up.

Early fixture setup failures were test setup issues: the wider valid bound
needed updating, a new scenario polluted old coverage before being isolated,
and overlapping toggles underexercised held-high NMI. The final fixture keeps
its positive coverage requirements and warms through the real power-on write
inhibit. A private relinker needed CRLF normalization and preservation of
Zelda's existing canvas extra object; neither failure was a game regression.

This is a scoped selection of blank device work. Content-qualified polling
loop elimination, visible rendering-off bulk output and broader regions remain
unqualified residuals. These measurements use frozen selected-object relinks;
final supported CLI/five-title regeneration, combined feature/performance
checks and owner visual approval remain separate. No merge or release yet.

[Machine-readable results](2026-10-31-blank-interval-results.json) include all
raw CPU/wall samples, source/binary/settings/package identities, capacity
receipts and hashes for the verified local evidence archives. Disposable live
QA data is cleaned after archiving; selected binaries/sources remain for final
integration. Owning issue: `beads-2dw.1.110`; campaign: `beads-2wy8.1`.
