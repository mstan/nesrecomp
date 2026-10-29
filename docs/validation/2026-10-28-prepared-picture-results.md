# Prepared Zelda picture: one preparation per revision

NES11, `beads-2dw.1.110`, campaign `beads-2wy8.1`. Candidate title branch
`perf/prepared-presentation-nes11` over `040d295`; selected engine runtime
through `55644d2`. Independent source/evidence validation passed; [Zelda PR11](https://github.com/mstan/LegendOfZeldaNESRecomp/pull/11) is open and unmerged at `60a6821`.

Zelda's frame-end callback prepared a picture, then its non-HD present callback
repeated that whole preparation. This change owns one stock/voxel picture per
core render revision, reusing it for presentation. Frame completion, load,
input/events/options, output width, voxel configuration/init and paused QA
teleport invalidate it. The cache is derived host state and is never serialized.
The world compositor continues receiving its ordinary present callbacks.

| Zelda workload | Host | Reference CPU s | Candidate CPU s | Median reduction | Faster/slower/tied pairs |
|---|---|---:|---:|---:|---:|
| zelda-hd | DESKTOP-HN15M1H | 3.859375 | 4.062500 | -5.26% | 2/3/0 |
| zelda-hd | SERVER | 5.812500 | 5.890625 | -1.34% | 2/2/1 |
| zelda-stock | SERVER | 7.484375 | 7.171875 | +4.18% | 5/0/0 |
| zelda-voxel | SERVER | 6.328125 | 3.875000 | +38.77% | 5/0/0 |
| zelda-world | SERVER | 2.968750 | 3.015625 | -1.58% | 2/3/0 |

Stock and diorama each improve in all five pairs on SERVER. Seamless Fit and
both Remastered batches are mixed/inconclusive, with the negative medians above
retained. They are not demonstrated wins or demonstrated neutral results. The
SERVER HD replication resolves the concern without establishing a reproducible
regression; it is not pooled with local samples. HD uses the engine's sampler
instead of the extras present callback, so this cache does not remove its main
rendering job. No aggregate offset is used to excuse a per-profile regression.

Each pair uses the same binary/configuration/host, actual hidden SDL with dummy
video, software1280x720 and emulated/mixed audio to dummy output. Five alternating
pairs follow an excluded warmup. CPU intervals exclude setup/loading and include
resumed gameplay and normal shutdown. Stock measures1801..3001, other profiles
1801..2101. These are bounded CPU measurements, not FPS or physical GPU claims,
and incremental percentages must not be added to earlier packages.

Native frame logs, hardware/RAM, complete PCM, native and presented images and
mod payloads match original/candidate for all four profiles. Six actual SDL
feature runs (original/candidate world, diorama and first-person) pass repeated
paused presents, three window aspects, same-build loads and advanced-state
reloads. World also passes paused same-frame teleport and alternating same-frame
states. Matching named pictures agree across binaries; candidate counters prove
repeated presents hit the owned picture without rebuilding it. Headless and SDL
prepare matching mods/profile states independently. No owner saves/settings used.

The implementation changes only Zelda. Other title and engine runtime binaries
remain unchanged. Missing Remastered art/unsupported rules are shared limitations.
Final supported combined regeneration, human visual approval and merge remain
pending. FDS remains parked. The paired samples, settings, source/binary hashes,
capacity receipts, correctness and feature transcripts are in the
[machine-readable record](2026-10-28-prepared-picture-results.json). Verified raw
ZIPs and reproducible products remain in the evidence root recorded there.

The original broader stock/custom raster-product fusion remains unmeasured.
This selected revision-owned Zelda picture removes a demonstrated repeated
consumer job; it does not replace the per-dot producer or fuse the other titles'
wide views. Reassess fusion after decoder/span/material work; raster/status/
mapper/light products are required outputs, not a mandate to retain machinery.

The independent validator checked both archive SHA/CRC,146 raw correctness
file hashes,39 matching feature pictures, all25 matched-pair identities and
medians, reuse counters and frozen/current C source hashes. Inherited historical
`engine_head`/`title_head` fields in archived product records are not exact
rebuilt checkout heads: `source_base`, frozen sources, object/link manifests and
executable hashes identify the tested relink. Disposable staging on both hosts
has been removed after verified retention. Product binaries/sources/manifests,
raw ZIPs, reservation receipts and validation tools remain bounded campaign
evidence; clean those after final combined handoff and durable evidence storage.
