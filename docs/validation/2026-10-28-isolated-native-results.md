# Native memory service for isolated Zelda room updates

NES18/NES20, execution `beads-2dw.1.110`, game follow-up `beads-2dw.18.4`,
optimization campaign `beads-2wy8.1`. Runtime `55644d2`; Zelda qualification
`040d295`, on the existing isolated performance branches. No main/master merge
or release; final supported combined regeneration and owner handoff remain open.

The larger demonstrated cost was guest execution, rather than snapshot copies.
One instrumented actual SDL Fit interval measured 696.429 ms executing guest
routines, 10.421 ms taking snapshots and 11.252 ms restoring them across
4,313 scopes and 4,539 calls. Copies accounted for 3.018% of those scoped QPC
wall timings. These diagnostic timings include instrumentation and possible
preemption; they are not process CPU shares or a recoverable-cost bound.
They select the execution service first. Persistent room actors, timers, RNG,
view-plus-adjacent activation and room rasters already existed.

Compiled isolated routines previously used individual bus start/read-or-write/
finish adapters even though their device clocks are frozen. Qualified builds
now use the existing native memory cursor for RAM and folded/checked code
accesses, publishing the final bus state and guest budget count together.
Credit stops at the budget: the next ordinary access reports exhaustion at
exactly the original cycle. Authoritative cycle queries exclude deferred
isolated work. Device and mapper accesses, watched code writes, operand/return
observers and uncertain cases retain their observable adapters. No simulation
frequency, activation distance or actor logic was reduced to obtain the gain.

Only Zelda selects `NESRECOMP_ISOLATED_NATIVE=1`. Boards 0/1/4/66 qualify;
other boards keep the working per-access path. The implementation is portable
C, independent of the HLE/LLE PPU. Default-off runtime preprocessing produces
identical C tokens to the prior engine checkpoint; other titles keep their
existing path. Current enabled preprocessing also matches the measured source
snapshot after formatting and default-off maintenance. This proves source
identity for those selections, not a new performance result on other games.

| Zelda profile | Host | Reference CPU | Candidate CPU | Reduction | Faster/slower/tied pairs | Decision |
|---|---|---:|---:|---:|---:|---|
| Seamless Fit | This PC | 1.375000 s | 1.234375 s | 10.227% | 5/0/0 | Positive in this matched run |
| Stock | SERVER | 7.468750 s | 7.421875 s | 0.628% | 3/2/0 | Neutral/inconclusive |
| Remastered HD | This PC | 4.187500 s | 4.156250 s | 0.746% | 4/0/1 | Small positive in this matched run |

Each case has five alternating pairs and excluded warmups on one shared
capacity-reserved host/configuration. These are incremental process CPU
measurements over the selected raster/CPU/device/sprite/APU/HD/SRAM/guarded-ROM
package, not additive percentages or FPS claims. Ordinary hidden SDL/software
1280x720 gameplay retains emulated and dummy-queued audio. Each binary loads
its own current-build paused state; setup costs and raw telemetry are retained.
Fit/HD run frames 1801-2101; stock runs 1801-3001. Other owner work remained
running. Capacity denials launched no jobs; target rotation used available
Windows headroom. Deck had no installed compiler/qualified runtime, so it was
not used for timing and no remote packages were installed.

Fit's native worker actually executes deferred isolated memory intervals.
Stock and HD have no isolated calls in these routes; the small HD result must
not be presented as a new HD algorithm or generalized to all HD workloads.
The one tie does not disqualify the observed improvement. Stock remains
inconclusive; no reproducible per-game regression was established.

All three profiles match native raster, hardware, RAM, full PCM, presented
checkpoints and serialized mod records. Fit matches 9,668 calls in 9,313 scopes,
104,614,306 guest cycles and zero failures, plus its room/actor records. Current
HLE and LLE fixtures each pass 5,120 cases: four alignments, NTSC/PAL frozen
clocks, four qualified boards plus unsupported-board fallback, precise budget
boundaries, PPU/APU/mapper accesses, ROM substitutions/checked data, interrupt
fallback, operand/return observers and exact authoritative restoration. The
registered CTest runs pass. Forced trace diagnostics in the fixture restore
their own trace policy; normal isolated scopes mute comparison tracing.

Both real-game builds also pass the existing focused probes: neighboring
actors evolve before entry; four overworld seams; dungeon entry, key-door
consumption, dungeon seams and exit; exact decoder isolation; adaptive window
sizes; and a live full-map view with all 128 rooms simulated and zero failures.
These checks preserve the existing mechanics and are not a new exhaustive
content audit. Other deferred Zelda caveats remain recorded in the game issue.
No FDS images/execution, native-save writes or visible owner-facing launches.

NES18 has a qualified selection decision for the isolated guest execution
service. Further persistent scratch contexts are deferred on the measured
cost split. NES20 has a measured deferral decision for replacing full snapshots;
this does not claim copies are free or that the architecture is exhausted.
Ten of 31 opportunities now have evaluation decisions; FDS remains parked.

[Machine-readable evidence](2026-10-28-isolated-native-results.json) includes
every pair, source/binary/object identities, raw telemetry, feature results,
fixture results and verified local/SERVER archive identities. Private recipes,
source snapshots and retained captures are in
`G:/Temp/Codex/nesrecomp/pipeline-burndown-20261028`. ROM and pack files are
excluded from Git and the retained evidence archives. Disposable staging is
tracked for cleanup; selected prototypes remain for the ongoing campaign.
