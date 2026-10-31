# NES23 offline observer ownership evaluation

Defer an offline history rewrite. Current producers already aggregate device
decisions, hooks and isolated-call summaries at semantic boundaries. The source
exception worth checking was per-interpreted-RAM-instruction history; neither
selected workload invokes it. No runtime candidate, adoption or PR.

| Workload | Host | API attempts | Muted | RAM interpreter | Body wall ms | Observed wall fraction |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| smb3 stock | DESKTOP-HN15M1H | 300 | 0 | 0 | 2.3553 | 0.03656% |
| zelda full seamless atlas4096x1472/all128simulatedrooms | SERVER | 1031659 | 1025011 | 0 | 16.3155 | 0.03517% |

These are two single actual-SDL diagnostic baselines, not before/after gains.
Each samples1199frame labels after an excluded load/menu setup, within a
nominal1200frame+shutdown wall interval. Software presentation, emulated audio
mixed/queued to dummy output at volume0, and default rewind100/4 stay enabled.
SMB3 follows the retained level/death/map route. Zelda begins with128simulated
rooms and609actors. Native HLE execution remains active; tracing is baselineoff.

The numbers time outermost ring API bodies with QPC; nested push_len→push is
not counted twice. Timer overhead/preemption is included, while callsite and
some diagnostic bookkeeping are outside the timer. This is not exclusive CPU,
a recoverable upper bound, a complete observer profile, or proof of architectural
exhaustion. Contention is retained: core3 busy6.34375s versus SMB3 process4.46875s,
and SERVER46.25s versus Zelda43.171875s. Do not compare their absolute times.

SMB3 has300state notifications. Zelda's1,025,011muted attempts already return
without recording device history during isolated execution; retained frame
summaries remain useful to late probes. Existing aggregation is baseline.
This evidence selects another substantial job without imposing a numerical
retention threshold. Broader dispatch/bookkeeping remains an explicit residual.

Traced netplay is a separate scope: it currently forces cycle tracing, which
disables native intervals. NES22 can evaluate a common qualified simulation
profile before frame0 with actual delayed-peer replay/state/digest evidence.
Preserving requested diagnostic traces does not require forever retaining an
old private digest layout. No offline result here qualifies that NET change.

Independent review restores the original ring source exactly by stripping the
private wrappers and verifies that each relink changed one object only. Raw
counts/logs/source/binary/telemetry agree. There is no runtime correctness patch
to qualify and no claimed improvement. Other three titles remain unchanged.

Capacity denials were retained; a30minute retry later admitted SMB3 locally and
Zelda on SERVER. Both Windows staging trees were removed after verified archive
retention. During the wait, audio diagnostics, NET/compiler recipes, final five
source trees and a cartridge inventory were prepared only, not accepted.

Deck has no native compiler installed. Proton10 loaded the exact Windows SMB3
gameplay state and matched all2048RAM bytes, the native PNG, UI readback and
current-build save/load. Renderer identity/log transport is still unresolved,
so no Deck timing or native Linux qualification is claimed. Its bounded333MB
isolated fixture/prefix is tracked for later qualification or final cleanup.

See [machine-readable evidence](2026-10-31-observer-results.json). Final combined
supported five-game builds/features/performance and owner approval remain open.
No main/master merge or release; FDS remains parked. Next selection: NES07,
starting with real DMA-heavy attribution before choosing a transfer prototype.
