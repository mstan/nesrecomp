# NES26 deadline presentation admission candidate

Reject this package. It implements and evaluates a materially different design
from the old waitable-timer trial: overdue presentation admission applies to
both synchronized and unsynchronized backends, while guest work/audio/input
and the existing wait remain unchanged. The six-period heartbeat and
unpaced/menu/inactive bypass remain. Viewport updates commit at frame boundaries
even when a picture is skipped. The selected NES20 runtime is unchanged.

The existing virtual elapsed-time model passes slow synchronized displays,
costly unsynchronized rendering, fast mode and heartbeat checks. The actual
live Atlas screen requests a 3840x2160 software-SDL window, paced gameplay,
volume zero and WASAPI. Both variants add the same existing pacing capture,
restricted to normal frames 1802 through 2100. Reference and candidate use
their respective inline headers via -iquote; this is not a geometry-only test.
No new timing fields, collector or timer/guard variant were added.

| Metric | Reference | Candidate |
| --- | --- | --- |
| Paired CPU seconds | 3.953125, 4.046875, 4.171875 | 3.968750, 4.015625, 3.781250 |
| Guest interval wall seconds | 5.009315, 5.076905, 5.010449 | 4.996477, 5.147872, 5.026048 |
| Scoped SDL present calls | 299, 299, 299 | 281, 284, 285 |
| Calls per measured wall second | 59.689, 58.894, 59.675 | 56.240, 55.168, 56.705 |

CPU is mixed; the raw independent-median 1.93% reduction is not an established
win. Present-call delivery falls in all three pairs, while the guest wall
interval does not improve consistently. End-of-wait p99 is lower, but it is
not present-spacing or physical input/display latency. Lower call counts alone
are not proof of worse perceived smoothness and are not an internal-callback
identity requirement. The available evidence does not qualify a useful balanced
default tradeoff, so the package is rejected without more collectors or tuning.

Headless output dictionaries and both 300-frame current-build continuations
match. All six paced complete end-state hashes and native-pixel hashes match.
Captured host PCM has the same three varying hashes in both modes, including
baseline variation; no paced bit-exact assertion or candidate-only audio defect
is claimed. These distinctions keep simulation checks separate from physical
presentation/audio judgments.

Raw identities, actual commands/settings, samples and per-leg state/PCM capture
remain at G:/Temp/Codex/nes110-pacing-candidate-20261031/zelda. Products/actual
header recipes are in products-pacing-capture.json and its manifests. See the
[machine-readable record](2026-10-31-presentation-admission-results.json).
No implementation PR, main/master merge or release. Continue to the remaining
complex-board and startup candidates after independent durable handoff.
