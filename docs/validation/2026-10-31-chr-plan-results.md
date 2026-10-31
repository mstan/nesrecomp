# NES12 owned CHR preparation evaluation

Defer and leave the prototype unselected. The measured Zelda outputs match,
but neither tested view demonstrates lower CPU time. The candidate adds about
20 KiB of persistent derived cache and another API/invalidation contract without
an observed benefit. This is a value decision, not a positive-gain floor or a
claim that the mixed samples establish a regression.

| Zelda profile | Baseline median CPU s | Candidate median CPU s | Raw reduction | Interpretation |
| --- | ---: | ---: | ---: | --- |
| fit | 1.859375 | 1.859375 | +0.000% | Neutral/inconclusive |
| atlas | 6.000000 | 6.015625 | -0.260% | Neutral/inconclusive |

Three alternating 600-frame pairs per view follow excluded warmups, on this PC
with matched software SDL at 1280x720, fixed inputs and rewind 100/4. Actual
views are Fit 426x240 with 15 simulated rooms and Atlas 4096x1472 with 128.
Fit has two slower pairs and one equal; Atlas has two slower and one faster.
All raw CPU/wall observations, identities, settings and headroom are retained.
No profile pooling or proven gain is claimed. All 40 picture/frame/hardware/
RAM/PCM/MOD hashes match within each profile.

Zelda already caches decoded background tiles. Current post-NES10 diagnostic
outer scopes observe 5.9522 ms for 374 CHR captures (0.2073% of interval wall)
and 69.471 ms for 47,872 actor-room visits (2.4193%). These include required
copying, clipping, priority, color, output, measurement and preemption. They
are neither exclusive CPU attribution nor a bound on removable work.

The prototype captures logical CHR pages with guarded contiguous copies and
an existing-getter fallback, then compares an owned 8 KiB image to prepare
uncolored tile pixels and independent half generations. Zelda consumes it for
terrain, actors and subrooms. Saved scene CHR, rather than live destination
PPU state, supplies retained scenes. A fixture passes five boards, 512 tiles,
65,536 row-plane combinations, real bank changes, truncated/modulo CHR RAM and
owned-image restore. The first fixture target omitted render/video sources;
correcting its link setup produced a passing run.

Unchanged controls reproduce all six runtime COFF sections of the selected
objects. The final 123-object manifests differ only in cyc_render and
zelda_world; accepted native job/oracle and retained canvas stay unchanged.
This is a frozen relink experiment, not a fresh supported build.

Timing legs successfully load each build's own state. Exact post-restore
continuation and retained portal/inventory gameplay are **not qualified**:
these routes have empty portal records. The prepared boundary driver was not
run after the no-benefit result. Other HD/voxel/nametable/OAM consumers and
different revision/ownership algorithms remain residual opportunities. This
scoped decision does not claim architectural exhaustion.

The prototype source and evidence are retained outside the selected runtime.
No runtime PR, merge or release is proposed. Final combined supported builds,
feature/performance checks and owner approval remain pending. See the
[machine-readable results](2026-10-31-chr-plan-results.json).
