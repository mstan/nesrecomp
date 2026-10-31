# NES15 mapped CHR snapshot candidate

Reject this package. The block snapshot is functionally correct in the scoped
checks, but the initial apparent Mario speedup does not reproduce. The selected
runtime is unchanged; no new optimization gain or PR is claimed.

The actual implementation replaces8192 per-byte bridge reads with eight bounded
page snapshots on boards0/1/4/66. Unknown mappings, partial ROMs, wrapping RAM and
unaligned pages retain the scalar reader. Compatibility arrays remain separate
writable host products; no cartridge storage is exposed for writes.

The existing video test compares the complete product against the original
reader on boards0/1/4/66/5/19, ROM/RAM, full/partial/wrapped/unaligned mappings and
NULL storage. It passes. The existing five-game checks then pass exact native
and presented pictures, PCM, frame products, hardware/RAM and600-frame own-state
continuations. Compiler packages, features and input are matched per game.

Mario's initial cold comparison diverged. Repeating the unchanged original
binary reproduces every candidate product exactly. That discrepancy is retained
under initial-evidence and baseline-repeat; its cause remains unproven. The
subsequent matched gate passes without changing either binary. The old binary's
repeatability issue is not treated as an introduced snapshot regression.

Mario's first three timing pairs all improved, with an apparent5.17% median
CPU reduction. One same-binary/configuration/workload/affinity confirmation gives
four slower pairs and one faster, with a raw1.83% slower median. Neither number
is a reproducible performance finding. The remaining games are mixed or within
one/two accounting ticks and stay neutral/inconclusive. No established benefit
and failure to qualify Mario as net-neutral reject this candidate; this is not
a numerical retention floor or a precise regression claim.

No further copy-loop variant, profiler or matrix is warranted. Proceed to the
larger NES20 isolation ownership candidate after independent durable review.
Raw binaries/settings/samples and the retained initial discrepancy are at
G:/Temp/Codex/nes110-presentation-candidate-20261031. Compile/link manifests and
source/test logs remain in the existing campaign workspace. No merge/release.
See the [machine-readable record](2026-10-31-chr-snapshot-candidate-results.json).
