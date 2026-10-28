# Native interval measurements on both hosts

Runtime candidate `520a95885225ce66284a0b60a8673847f86d17f6`; frozen reference
`fc0bf3fa46a710464960c0a1f2d3b83b4d345600`. Tracking `beads-2dw.1.110` and
optimization campaign `beads-2wy8.1`. The owner explicitly directed measuring
despite contention, including on the workstation. Foreign jobs/reservations
were preserved; no quiet-host prerequisite was added.

| Game / host | Reference CPU median | Candidate CPU median | Raw reduction | Interpretation |
|---|---:|---:|---:|---|
| SMB1 / SERVER i5-4670 | 8.234375s | 7.250000s | 11.95% | Positive: all five pairs faster |
| SMB1 / Ryzen 9800X3D, own build also running | 4.500000s | 4.578125s | -1.74% | Mixed; inconclusive |
| SMB1 / Ryzen 9800X3D, after own build ended | 5.406250s | 4.984375s | 7.80% | Mixed; inconclusive |
| SMB3 / Ryzen 9800X3D | 3.468750s | 2.843750s | 18.02% | Unchanged fallback control; mixed/load drift, not a claimed gain |

Each batch has five order-balanced A/B pairs plus excluded warmups, identical
per-game inputs/current-build states, real SDL/software1280x720 and audio
emulated/mixed/queued through dummy output. CPU time is for resumed frames plus
normal shutdown, excluding TCP/menu/state setup. SMB1 measures frames361..1200;
SMB3 frames2401..3000. These are gameplay/death workloads; no level completion
or level-only claim follows. Profile and host differences are not pooled.

The local first batch began with all16 logical CPUs busy; the second began
with88..97% per-core busy. Three pairs are faster and two slower in each SMB1
workstation batch. The variability exceeds the small mixed result; neither a
proven local win nor a reproducible regression is claimed. SMB3 has the same
mixed pattern and large drift, while its interval path is compiled out. Calling
its18% raw median an optimization gain would misrepresent the control.

The corrected NAS batch has9..24% preflight per-core busy and five faster
candidate pairs. Reference CPU legs range8.1875..8.46875s; candidate legs
7.21875..7.609375s. This qualifies a positive SMB1 result on that measured host
and stock workload; it is not an expected percentage for every game or feature.

Untraced raster/memory records, endpoint hardware/memory/picture and full PCM
match exactly for both games. SMB1 executes29,376,956 native-interval cycles
in1,438,160 spans; SMB3 is explicitly the zero-interval fallback control. SMB1
artifacts also match across the Intel and AMD hosts using these same binaries.
Traced fallback alone is not used to validate active native execution. Existing
inactive-mod state warnings appear equally in original and candidate and stay
classified as shared baseline defects. Native saves/settings are preserved.

Raw paired CPU/wall samples, core contention, binary SHA-256 identities,
participation labels, artifact hashes and evidence locations are retained in
[the machine-readable results](2026-10-28-interval-results.json). The complete
NAS evidence archive and local logs/states/pictures are under
`G:/Temp/Codex/nesrecomp/pipeline-burndown-20261028`; these are workspace
evidence, not delivery-folder builds.

Metroid, Zelda and Gumshoe builds/checks and actual feature profiles remain.
NES01-NES31 are still the active goal; this first ownership package does not
close the per-pixel raster or per-cycle APU algorithm opportunities. No owner
visual handoff, main/master merge, pin update or release has occurred.
