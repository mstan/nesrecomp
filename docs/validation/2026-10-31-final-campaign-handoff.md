# Final campaign handoff

The supported combined five-title v3 gate passes. All five games are open in
normal windows with normal audio for the owner's visual check. No main/master
merge, tag, publication or delivery replacement has occurred.

The 32-area burndown has 31 actual scoped candidate decisions: 18 overlapping
selections and 13 prototype dispositions. NES29/FDS remains owner-parked.
These counts are neither independent speedups nor architectural exhaustion.

NES30 prepared MMC5 mapping and NES31 empty RAM-view service were implemented,
checked and not selected after their apparent gains failed confirmation.
NES32's first resident HD GPU implementation is selected for Zelda Remastered:
authored 2x median process CPU falls 11.71% in five faster matched pairs; true
16x (4096x3840) runs 93–108 FPS in three 600-frame runs without GPU fallback.
The [scoped GPU results](2026-10-31-resident-gpu-results.md) preserve memory,
upload, portability and experimental 32x/64x limitations.

## Final supported build gate

Runtime engine source is `5c749a2ec3d3ff32a9f8ea0225db2a9ede5c8387`.
Each consumer uses its supported CMake regeneration/build path. SMB uses the
qualified GCC 15.2 O3 target-IPO package; the other four use explicit O2/g1.
Exact source, ROM, compiler settings and binary identities are in the
[machine-readable handoff](2026-10-31-final-campaign-handoff.json).

| Game | Version | Reference/candidate median CPU seconds | Disposition |
|---|---|---:|---|
| Super Mario Bros. | 1.12.5 | 1.828125 / 1.765625, five-pair confirmation | Neutral/inconclusive |
| Super Mario Bros. 3 | 0.2.5 | 1.500000 / 1.500000 | Neutral/inconclusive |
| Metroid | 0.2.5 | 1.515625 / 1.484375 | Neutral/inconclusive |
| Gumshoe | 1.5.3 | 1.281250 / 1.312500 | Neutral/inconclusive |
| Zelda, Seamless Fit | 1.11.0 | 1.953125 / 1.984375 | Neutral/inconclusive |

These comparisons use the preceding qualified selected package, not the
original engine; they do not establish additional gains. SMB's initial three
slower pairs were retained and prompted one frozen five-pair confirmation:
four faster and one slower, so the initial magnitude did not reproduce.
Small mixed standard-game results remain neutral/inconclusive. Zelda HD's
separate measured improvement does not offset a regression in another game.

The earlier v2 Metroid binary `2180ce26...` reproducibly regressed and remains
rejected. HD-only settings were then scoped out of consumers lacking the HD
provider. The supported scoped control's runtime PE sections match the prior
qualified Metroid binary byte-for-byte. The final v3 build passes its own gate;
no exact causal recovery percentage is claimed for the earlier regression.

All five v3 builds match their scoped native/presented images, frame records,
PCM, hardware and RAM outputs. Each reference and candidate passes 600-frame
same-build continuation. Normal SDL/recomp-ui checks pass load, invalid-state
refusal and actual rewind; selected SMB/Zelda Fit profiles pass real window
resizes. Independent review passed the final combined gate and directed the
owner handoff. Historical rejected artifacts remain separate evidence.

## Release handoff

The official nesrecomp CLI 0.5.0 Windows archive is built from the same engine
source and verified: wrapper version/help, native help, nine public headers,
and archive exclusions pass. SHA256:
`fa1d75fc1c9fadc7d6b522ea6a877083be5f9ab772b1d7cd3b01571a430f80a7`.
Game archives are staged privately with their supported packagers. Explicit
trace-OFF cache metadata needed by release guards does not change compiled
commands or game binaries. Packaging/documentation-only followups are recorded
separately from the qualified runtime source; they do not require another
routine gameplay gate.

The remaining gate is the owner's visual approval. Then merge the reviewed
integration and game branches, publish the versioned CLI/game releases,
replace each delivery archive only after verification, and close
`beads-2dw.1.110` with links to the landed heads and delivered builds. Preserve
native saves/settings and the running owner playtests. Symbols, raw evidence
and temporary staging stay outside the one-current-build delivery folder.
