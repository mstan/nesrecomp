# NES20 accessible cartridge-storage isolation

Select this bounded storage-ownership change. Zelda's live full-map Atlas
workload improves in every matched confirmation pair: median process CPU falls
from2.625000s to2.500000s, a4.76% reduction over300 resumed frames. The five
standard game workloads remain neutral/inconclusive. These incremental results
are not additive with earlier packages or a general FPS/GPU claim.

An isolated scope previously copied all128KB of the cartridge WRAM array on
capture and restore, even when only8KB physically existed. Qualified boards
0/1/4/66 now capture every physical RAM bank through wram_len, including disabled
or currently unmapped banks, while copying all cartridge/register/device fields.
The captured extent controls restoration and snapshot RAM readback. Unused
storage is outside this isolated scope's ownership. Ordinary public snapshots
still copy the full array; unsupported boards and invalid extents retain the
full portable copy. PPU/APU/CHR/media, native saves, hooks and RAM-view restoration
keep their contracts. This is portable C and needs no host feature detection.

Both existing isolation tests pass under HLE and LLE, with5120 checkpoints each.
The fixtures cover8/32/128KB extents, absent RAM and read/write gating, mutation
of the last physical bank, unsupported-board fallback, budget/observer/device
behavior and exact authoritative restoration. Normal snapshots additionally
restore a mutation at the final byte of the full128KB allocation.

All five standard built games match exact native/presented pictures, PCM,
frame products, hardware/RAM and600-frame own-state continuations. The separately
labeled live Atlas case also matches exact outputs and300-frame continuation.
Actual SDL/UI picture dimensions are4096x1472 in both variants, and its matching
84585 isolated calls/83440 scopes exceed Fit's12406/12052. This is a real heavier
isolation workload. The candidate also passes the existing hidden normal-SDL/UI
own-load, invalid-state refusal, actual rewind and adaptive Fit resize checks.

Atlas first improves in3/3 pairs (raw median4.14%). One same-binary/configuration/
workload/affinity confirmation improves in5/5 pairs (median4.76%). Keep both raw
sets separately. Standard-game small deltas and ties remain neutral; the one
slower Mario3 candidate leg is retained alongside two exact ties, not hidden or
called a reproducible regression. Original supported EXEs are the A/B references;
SMB keeps its qualified O3/IPO package, others keep their qualified O2 package.
No compiler overlaps the serial timing runs. No profiler or new harness was added.

Raw binaries, settings, exact per-game samples and continuations remain at
G:/Temp/Codex/nes110-isolation-candidate-20261031. Compile/link identities and
source/test logs are retained in the existing campaign workspace. Independent
validation and a PR precede the NES23 handoff. Final combined human review is
still pending; no main/master merge or release. The broader context/snapshot
architecture is not claimed exhausted. See the [machine-readable result](2026-10-31-isolation-storage-results.json).
