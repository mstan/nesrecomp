# NES22 rollback digest evaluation

Reject and leave the streamed xxHash32 prototype unselected. In actual delayed
SMB1 netplay, all four FNV baseline runs pass. Three candidate runs pass and
one genuinely diverges: confirmed progress stops at tick 306, the peers log
eight/six desyncs, and their full state differs at the same tick 850. The
cause remains unresolved. No equivalent baseline failure was observed, so
this failure is not labeled pre-existing or waived. No runtime PR is proposed.

| Run | FNV baseline | xxHash32 candidate |
| --- | --- | --- |
| Excluded warmup | Pass | Pass |
| Pair 0 | Pass | **Fail: peer state fork** |
| Pair 1 | Pass | Pass |
| Pair 2 | Pass | Pass |

All seven passing legs have the same complete tick-850 state bytes across
peers and profiles. The failed candidate differs in CPU/RAM, PPU/picture,
APU, trace and captured background. Its failure is not a drain-frame labeling
mistake. Source review finds no FNV-specific protocol arithmetic assumption;
a timing-sensitive underlying fork is plausible, but not established shared.

Current supported SMB1 NET/HLE, with UI off, runs two hidden muted peers on
SERVER through a real UDP relay holding every packet at least 20 ms in each
direction. Fixed input delay is zero; forced misprediction is explicitly off.
The route enters actual gameplay (mode 1, task 3). Each leg targets 900 ticks,
and the probe overwrites tick-850 state on replay before confirmation. Its
state header reports frame 851. Existing netplay traces remain enabled and
native intervals remain disabled. This does not implement or qualify the
capability/common-profile work tracked by beads-2dw.1.100.

The private digest algorithm was a legitimate replacement opportunity:
diagnostic elapsed digest scopes were meaningful, and old FNV values need
not be preserved. The portable scalar xxHash32 prototype keeps every master
and partition byte domain, including malformed tails/trailer, with unchanged
authoritative snapshots, simulation and strict build/content identity checks.
The unmodified BSD-licensed v0.8.3 header is retained with its notice. Known
vectors and independent materialized-domain, unaligned, malformed/truncated
fixtures pass under MinGW x64 and actual MSVC Win32. Those checks cannot
override the delayed-peer failure. The old fused digest/cache/presentation
work is baseline and is not credited again.

Both binaries have 107 objects; only digest object 103 differs. Common private
state/PCM probes and timing counters are identical. Raw process CPU, actual
live/replay counts, per-admitted-tick CPU, per-image elapsed scopes and host
telemetry are retained as diagnostics. Replay counts vary with scheduling;
unlike work is not presented as an equal-work speedup. The failed leg is not
admitted, and there is **no accepted performance gain** for this prototype.

The host-frame-850 PCM probe is distinct from the NET-tick-850 state probe.
Its nonempty output differs between peers in all four baseline runs and
successful candidate runs. That observed shared baseline audio-output
difference remains unresolved; caught-up PCM is not claimed qualified.

An initial local diagnostic relay stopped on Windows ICMP when one peer
closed; the corrected run exposed different final drain frames 903/904.
These setup/endpoint exclusions are retained separately. The later same-tick
candidate state fork is a genuine failed gate. The collector stopped on it,
then collected only the remaining four already-planned legs without clearing
the failure. Local capacity denial led to eligible SERVER, not a quiet-host
requirement or a weaker gate.

The rejected source/evidence is retained separately from selected runtime.
Full capture ownership, changed-region hashing, the unresolved fork and a
properly negotiated trace-off profile remain residual opportunities. This
scoped decision does not exhaust rollback architecture. Final supported CLI
and five-title builds, combined checks and owner approval remain pending.
See the [machine-readable record](2026-10-31-rollback-digest-results.json).
