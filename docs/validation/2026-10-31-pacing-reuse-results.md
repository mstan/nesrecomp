# NES26 pacing: reuse the rejected design

Defer a new pacing service. The existing entry calls for reusing the negative
receipt and testing only a materially different deadline design. Another
high-resolution Windows timer with a different final spin guard repeats the
old candidate. No new build, launch or speedup is claimed for this decision.

The retained historical SMB1 trial has 18 legs: two warmups, four A/A controls
and twelve ABBA observations. All preserve 900 frames/presents and identical
state, PCM and pixel hashes. Recomputed CPU block reductions are 6.744%,
6.577% and 5.333%, median 6.577%. Median per-run end-of-wait p99 lateness
worsens from 14.35 to 537.4 microseconds. The old default rejection remains.
These are historical source/package/host results, not today's common package.

The old design already used high-resolution CreateWaitableTimerExW, dynamic
lookup, a portable SDL fallback and a final 500 microsecond spin. The measured
lateness boundary was **after waiting relative to the next loop deadline**.
It did not measure physical input/display latency. Future work need not keep
private counter values identical, but CPU gains alone cannot establish a good
presentation tradeoff.

Current selected source still uses SDL_Delay(ms-1) plus counter spin. Guest
deadlines remain independent of VSync, bounded catch-up drops pictures, and
fast/uncapped/replay bypass waiting. This audit does not quantify current wait
cost; NES25 whole-process CPU is not a pacing attribution or savings bound.

A coherent admission/presentation service remains potentially useful and
unimplemented. A decisive future trial should capture actual event/pad sample,
admission, target/wake and present timestamps, cadence and p95/p99 lateness,
event-to-admitted-frame latency, CPU and audio behavior. Preserve fast-forward,
slow-display catch-up and portable fallbacks/handle lifetime. Frame-index input
equality alone cannot prove wall-clock input latency. No architectural
exhaustion is claimed, and obsolete quiet-host/large-matrix policies are not
reused. Final builds and owner checks remain separate. See the
[raw historical identities and recomputation](2026-10-31-pacing-reuse-results.json).
