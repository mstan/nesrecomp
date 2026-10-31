# NES31: empty compiled RAM-view service candidate

Do not select this package: implementation and correctness pass, but startup
and runtime benefit is unestablished. Local prototype `b09415a` remains outside
the selected runtime; no successful-candidate PR or broader timing matrix.

The empty-view build avoids two CSR allocations (278,536 requested bytes), uses
a no-op write callback and rejects empty dispatch before mapping checks.
Nonempty view validation and invalidation are unchanged. All 24 exact Metroid
products and both 600-frame own-state continuations pass. Existing synthetic
cartridge checks pass self-modification, capture, access gates, isolated and
committed changes, snapshot replay and untraced deadlines at four alignments.

The initial three Metroid pairs were positive, with median process CPU
1.671875→1.5625 seconds (apparent 6.54%). That did not confirm: five same-affinity
pairs had one faster and four slower results, medians 2.3125→2.328125 seconds.
Strong cohort drift makes this neutral/inconclusive, with no established gain
or precise regression. Raw samples retain both sets.

The existing startup collector measured first TCP response in 0.521866 versus
0.519828 seconds. Paused stock 256×240 Metroid private commit was 45,744,128
versus 45,187,072 bytes; working set was 30,588,928 versus 30,347,264 bytes.
These are one diagnostic sample each, not cold boot, first-visible-frame,
first-use or qualified RSS/latency improvements. The collector's inherited
Fit label is corrected to match the actual stock commands/state. Its first
attempt had a path-escaping error; the fresh v2 run loaded both own states and
completed normally, so the failed collector attempt is not a game failure.

No currently reported startup stall is resolved. Stop further tuning/matrices
for this candidate and move to NES32. Raw evidence: `G:\Temp\Codex\nes110-startup-candidate-20261031\metroid`;
contracts: `test-startup-cart-ramviews.log`; startup: `startup-cost-metroid-result.json`
in the existing campaign task folder. [Structured result](2026-10-31-empty-view-candidate-results.json).
