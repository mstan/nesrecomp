# Qualified HD lookup and composition service

Engine `113ec6476a3a12cd91f5675b42026e708151c7ae`; Zelda feature commit `769aeec56b90731a6796593acd9886ebb17cc243`.
Tracking `beads-2dw.1.110`, campaign `beads-2wy8.1`, opportunity NES14.

Zelda Remastered resolves immutable tile-key groups and frame-only gates once
per render, keeps pixel-dependent gates at their original coordinates, and
composes clipped image rows in the same per-pixel layer order. Derived plans
are pack-owned and never serialized. All native simulation/fetch/audio work
is preserved.

| Qualified workload / host | Reference CPU median | Candidate CPU median | Reduction | Pairs |
|---|---:|---:|---:|---|
| Zelda Remastered / nas | 11.031250s | 6.359375s | 42.35% | 5/5 faster |
| Zelda Remastered / local | 8.140625s | 5.093750s | 37.43% | 5/5 faster |

These are whole built-game CPU measurements of the selected module, against
the committed APU/sprite engine, with the same actual enabled Remastered pack.
Five alternating pairs and excluded warmups; actual SDL/software1280x720,
audio emulated/mixed/queued to dummy output. Setup is excluded and retained
separately. Frames1801..2101 are gameplay/death on the existing deterministic
route; no level-completion or actual-FPS claim. Both hosts were shared under
the owner instruction. No quiet-host gate or minimum gain floor.

The Remastered PRG patch makes both binaries interpret: native cycles and
native-interval counters are0. Do not credit previous stock-game interval,
sprite or APU gains to this result or add incremental percentages together.

The broad versions were rejected despite large Zelda gains. SMB1 LyonHrt
regressed on NAS by2.25% in v1,2.19% in v2 and1.13% in v3. Local SMB results
were mixed, including negative samples. After the first two failed controls,
the design was reassessed and plan ownership separated; the remaining SMB
slowdown still required a narrower package. No aggregate offset was accepted.

The final module is compiled only by the qualified Zelda cycle target using
`NESRECOMP_HDPACK_LAYER_PLANS=1`. Other targets keep the original direct
sampler. SMB1 default preprocessed code, compiled code/data sections and all
executable runtime sections are identical to its APU/sprite control. This is
an unchanged-runtime control, not a claimed SMB optimization. The default
translation unit has no plan records, branches or extra allocations.

Both hosts match seven real HD checkpoints, native raster/memory/hardware,
endpoint picture and full PCM.192 independent reference pictures cover both
CHR modes/dialects, scales1/2/3/10, conditions/priority/negation/aliases,
nearby/origin/mirror rules, transparency/add/sub/brightness, clipping/HUD,
same-frame invalidation, epoch wrap and fallback. Whole-machine HLE and LLE
checks pass CHR RAM/ROM, banks,8/16px sprites, flips and current-build replay
at all four NTSC alignments. Native saves/settings remain untouched.

Missing selectscreen.png and unsupported pack conditions/backgrounds reproduce
in both builds and remain shared limitations. Derived memory is bounded by
loaded tile/key records and a fixed lookup cache; no per-frame allocation.
Raw startup/setup CPU samples are retained; no isolated startup/RSS claim.

[Machine-readable results](2026-10-28-hd-plan-results.json) retain every
accepted and rejected pair, host/core activity, binary/object/source hashes,
pack identities and archive hashes. Source/build/recipe evidence is in
`G:/Temp/Codex/nesrecomp/pipeline-burndown-20261028`. Private ROM/pack payloads
are excluded from source Git and evidence archives.

Final supported regeneration and combined feature/owner checks remain. Other
burndown evaluations continue, FDS stays parked, and no main/master merge or
release has occurred.
