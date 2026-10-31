# Combined supported-build checkpoint

All five fresh supported builds pass the scoped automated game checks.
This preserves the selected package; it does not complete eight deferred areas.
No owner visual approval, main/master merge or release yet.

| Game/profile | Reference CPU s | Fresh CPU s | Disposition |
|---|---:|---:|---|
| smb | 1.750000 | 1.734375 | Neutral/inconclusive |
| smb3 | 1.390625 | 1.375000 | Neutral/inconclusive |
| metroid initial window | 1.437500 | 1.500000 | Initial negative; retained |
| metroid unchanged confirmation | 1.375000 | 1.390625 | Neutral/inconclusive; paired median zero |
| gumshoe | 1.281250 | 1.265625 | Neutral/inconclusive |
| zelda | 2.093750 | 2.015625 | Neutral/inconclusive |

Each pair uses matching inputs/features/host/affinity. The reference is the last
qualified package, not the original unoptimized engine. Mixed small results are
neutral. Initial Metroid4.35% negative is retained separately; same-configuration
five-pair confirmation has three equal pairs and two one-accounting-tick slower,
paired median zero. The earlier magnitude did not reproduce. No new gain claim.

Checks cover native outputs/PCM, current-build continuation, normal UI, safe
invalid-state refusal, actual rewind and relevant Fit resizing. Gumshoe includes
the existing Zapper schedule and fixed timed aim/trigger. Zelda additionally
passes the two existing inactive-voxel-history cases after integrating current
master, retaining both cache invalidation and the save fix.

The CLI was freshly built and used by all five supported regeneration paths.
Source/binary/loader identities and raw per-game evidence paths are in the
[checkpoint record](2026-10-31-combined-build-results.json).

Burndown accounting is22scoped candidate-decision areas (16overlapping selections
and6prototype dispositions),8prepared/deferred areas and owner-parkedNES29.
Selections overlap and are not16independent speedups. Deferred/inventory work
is not completed candidate burndown. Continue remaining areas serially before
the next final owner handoff; preserve accepted checks unless code changes.

Requested engine reviewer/assignee `TechnicallyComputers` is not assignable
(GitHub eligibility404). PR89 has no assignee; repository access is an unresolved
ownership detail.
