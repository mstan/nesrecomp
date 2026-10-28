# Qualified-board native interval results

Candidate `021c540f1370d6cd0bbf96c1c24c3679379e3752` versus reference
`fc0bf3fa46a710464960c0a1f2d3b83b4d345600`. Tracking `beads-2dw.1.110` and
optimization campaign `beads-2wy8.1`. The owner authorized measurements despite
concurrent work, including this workstation. No foreign job or lease was
changed and no quiet-host admission gate was imposed.

This extends the NROM/MMC1 cursor to MMC3 and GxROM, qualifying actual mapper
deadlines. MMC3 preserves every A12 notification and exact CPU timestamp,
refuses an unsampled mapper IRQ, and leaves a possible first qualified rise
to the coupled clock. GxROM writes close the interval before bank selection.
All five games use this same engine package and exercise active intervals.
Earlier v1d zero-interval fallback measurements remain historical evidence
[in the first-package results](2026-10-28-interval-results.md).

| Game / host | Reference CPU median | Candidate CPU median | Raw reduction | Interpretation |
|---|---:|---:|---:|---|
| smb / nas | 8.171875s | 7.171875s | 12.24% | Positive; 5/5 faster |
| smb / local | 4.359375s | 4.078125s | 6.45% | Mixed/inconclusive; 3/5 faster |
| smb3 / nas | 4.953125s | 4.406250s | 11.04% | Positive; 5/5 faster |
| smb3 / local | 3.609375s | 3.234375s | 10.39% | Mixed/inconclusive; 3/5 faster |
| metroid / nas | 10.593750s | 9.062500s | 14.45% | Positive; 5/5 faster |
| metroid / local | 6.062500s | 5.171875s | 14.69% | Positive; 5/5 faster |
| zelda / nas | 12.328125s | 11.078125s | 10.14% | Positive; 5/5 faster |
| zelda / local | 6.437500s | 5.500000s | 14.56% | Positive; 5/5 faster |
| gumshoe / nas | 4.906250s | 4.140625s | 15.61% | Positive; 5/5 faster |
| gumshoe / local | 2.984375s | 2.140625s | 28.27% | Positive; 5/5 faster |

Five alternating A/B pairs per row, excluded warmups, matched own-build
states and input routes. Actual SDL/software1280x720 with audio emulated,
mixed and queued to dummy output. CPU time covers resumed gameplay/death
frames and normal shutdown, excluding TCP/menu/window/state setup. SMB1
frames361..1200; SMB3 frames2401..3000; Metroid/Zelda frames1801..3000;
Gumshoe frames601..1200 with fixed labeled SDL Zapper aim/trigger. No level-
completion, level-only, all-feature speedup or cross-build netplay claim.

All five games match their references in untraced frame memory/raster,
endpoint hardware/memory/picture and full PCM. The same checked artifacts
match on the Intel i5-4670 NAS and Ryzen7 9800X3D workstation. Active interval
counts, full source/binary identities, raw pair samples, per-core contention
and archive hashes are retained in [machine-readable results](2026-10-28-board-deadline-results.json).
The paired primitive/cursor fixtures also pass LLE/HLE on all four mapper
types and NTSC alignments, delayed device traffic and current-build replay.

Small mixed shared-host results are neutral/inconclusive. Positive rows have
five faster pairs in these batches; this is evidence, not a new mandatory
acceptance threshold. Per-game neutrality remains acceptable. Shared inactive
mod-state warnings stay separate from introduced failures. User native saves
and settings were preserved in all isolated muted tests.

Full evidence and temporary input profiles remain under
`G:/Temp/Codex/nesrecomp/pipeline-burndown-20261028`; private ROM copies are
temporary inputs, excluded from evidence archives and source control.
Enhancement profiles and the remaining NES01-NES31 work continue. No owner
visual handoff, main/master merge, pin update or release has occurred.
