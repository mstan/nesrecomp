# Native interval measurements on both hosts

Runtime candidate `520a95885225ce66284a0b60a8673847f86d17f6`; frozen reference
`fc0bf3fa46a710464960c0a1f2d3b83b4d345600`. Tracking `beads-2dw.1.110` and
optimization campaign `beads-2wy8.1`. The owner directed measuring despite
contention, including on the workstation. Foreign jobs and reservations were
preserved. No quiet-host prerequisite was imposed.

| Game / batch | Reference CPU median | Candidate CPU median | Raw reduction | Paired interpretation |
|---|---:|---:|---:|---|
| smb / NAS corrected package | 8.234375s | 7.250000s | 11.95% | Positive; 5/5 faster |
| smb / Workstation during concurrent own build | 4.500000s | 4.578125s | -1.74% | Inconclusive; 3/5 faster |
| smb / Workstation after own build ended | 5.406250s | 4.984375s | 7.80% | Inconclusive; 3/5 faster |
| smb3 / Workstation unsupported-board control | 3.468750s | 2.843750s | 18.02% | Inconclusive; 3/5 faster |
| metroid / NAS corrected package | 10.875000s | 9.328125s | 14.22% | Positive; 5/5 faster |
| metroid / Workstation corrected package | 5.359375s | 4.468750s | 16.62% | Inconclusive; 4/5 faster |
| zelda / NAS corrected package | 12.562500s | 11.343750s | 9.70% | Positive; 5/5 faster |
| zelda / Workstation corrected package | 5.812500s | 5.062500s | 12.90% | Positive; 5/5 faster |
| gumshoe / NAS corrected package | 4.828125s | 4.765625s | 1.29% | Inconclusive; 3/5 faster |
| gumshoe / Workstation corrected package | 2.546875s | 2.328125s | 8.59% | Inconclusive; 3/5 faster |

Each batch has five alternating A/B pairs plus excluded warmups. These use
matched per-game inputs and build-specific states, actual SDL software output
at1280x720, and audio emulated/mixed/queued through dummy output. CPU time
covers resumed frames and normal shutdown; TCP/menu/state setup is excluded.
SMB1 frames361..1200; SMB3 frames2401..3000; Metroid/Zelda frames1801..3000;
Gumshoe frames601..1200. These are bounded gameplay/death routes, with no
level-completion or level-only claim. Different profiles and hosts are not pooled.

SMB1, Metroid and Zelda exercise the native interval service. SMB3 (MMC3)
and Gumshoe (GxROM) compile it out and retain the common fallback. Their
neutral outcomes do not disqualify gains in the other games.

The earlier SMB1 workstation batches remain in the table: both have three
faster and two slower pairs under heavy contention. Neither is a proven win
or a reproducible regression. SMB3 has zero interval cycles: its raw18% median
swing is an unchanged fallback control showing load drift, not an optimization
gain. Positive entries above have all five candidate legs faster; their raw
samples and observed host contention remain available for review.

Metroid on the workstation has four faster pairs with reductions15..20%,
and one candidate leg2.9% slower. Its16.62% observed median improvement is
retained with that caveat. Requiring all five pairs to improve is not a new
acceptance rule; the conservative classification reflects the changing load
in this particular batch. Zelda improves all five workstation pairs.

Untraced raster/memory records, endpoint hardware/memory/picture and full PCM
match exactly for all five games. SMB1, Metroid, Zelda and Gumshoe additionally
have identical checked artifacts on Intel i5-4670 SERVER and Ryzen7 9800X3D
using the same binaries. This is not a cross-build netplay qualification.
Actual interval participation, full binary identities and per-game timing
legs are retained in [the machine-readable results](2026-10-28-interval-results.json).

Current-build SDL state load emits the same inactive mod/reseed warnings in
both versions. These remain shared baseline defects. Native saves and user
settings were preserved; temporary profiles/states and muted audio are isolated.

Complete NAS evidence archives and local raw logs/states/pictures are retained
under `G:/Temp/Codex/nesrecomp/pipeline-burndown-20261028`. These are workspace
evidence, outside delivery folders. Copied private ROMs are temporary task
inputs and must be removed when no longer needed.

Stock measurement is complete. Real enhancement profiles and the remaining
NES01-NES31 evaluations continue. This ownership package does not close the
per-pixel raster or per-cycle APU algorithm opportunities. No owner visual
handoff, main/master merge, pin update or release has occurred.
