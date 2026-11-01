# Resident GPU HD rendering: NES32

Selected the first scoped HD GPU implementation after independent review.
The combined supported five-title gate and owner visual handoff remain pending.
No main/master merge or release has occurred.

| Built workload | Before | Candidate | Decision |
|---|---:|---:|---|
| Zelda Remastered, authored 2x, median CPU for 600 resumed frames | 5.468750 s | 4.828125 s | 11.71% lower; all five matched pairs faster |
| Zelda Remastered, true 16x, 600 resumed frames | unavailable in old renderer | 93.24–108.30 FPS | Full speed with headroom in three runs; zero GPU fallback |

The matched comparison uses the same ROM, pack, route, OpenGL host, core,
audio and rewind settings. Wall time improves in four of five pairs amid
substantial variation; every wall-time sample is not a promised improvement.
The 16x result is a separate feature qualification, not an equivalent old/new
16x speedup. Each timed interval includes normal shutdown and excludes setup.
These results must not be added to earlier overlapping campaign percentages.

The GPU draws shared immutable HD jobs from a bounded cache of visible source
pages. The real pack contains about 2.13 GB of decoded artwork; the earlier
whole-catalog buffer exceeded the driver's buffer limit. The selected cache
reserves 128 MiB of source pages, protects the complete visible working set,
and uploads only missing pages plus changed jobs. Output size does not change
CPU job preparation or the upload stream. Unneeded CPU preview composition is
omitted; rewind capture and paused menus still refresh their own previews.
Unsupported contexts retain the portable consumer at authored pack resolution.

## Evidence and exactness

- Runtime source: `1ed8235ca2603ec41ab09cb3bc91d296399f535d`.
  Frozen candidate EXE SHA-256: `d255ad28f16ac9cca375961457f637b028ca70875e35fdfd1c8c0071e4afb52a`.
- Actual owner GPU: NVIDIA RTX 3080 Ti, OpenGL 4.6, driver 616.86.
  Required target is actually allocated at 4096x3840; all three unprofiled
  600-frame runs report every GPU attempt rendered and zero fallback.
- Baseline and GPU 1x/2x/4x/16x have identical complete canonical state bytes
  (SHA-256 `36a6aeca0fb94c62a11b215c7b71c25641c69607d7071c68ce4b495eb935b8f6`)
  and identical native-density pictures. Native outputs/PCM/RAM/hardware and
  both 600-frame current-build continuations also match.
- Actual 16x SDL/UI save/load, invalid-state refusal, rewind and real resizes
  pass. State bytes remain identical after load and every resize. Existing
  state tests exercise 210 exact payload roundtrips, successful/failed restore
  invalidation and the exclusion of private display epochs from serialization.
- The existing 192-picture corpus compares CPU jobs, reference pictures, GPU
  readback and actual window copies, including exact full 16x. Retained jobs
  survive real resize, GPU recreation and complete renderer/window recreation.
- Portable shared jobs, cache contracts and the no-SDL GPU stub build/run on
  Steam Deck using the existing checker. This is not a complete Deck game or
  lower-end 16x performance qualification.

Median GPU composition at 1x/2x/4x/16x in the scale-profile cohort is
0.0184/0.0297/0.0686/0.8013 ms. The final 16x cohort measures 1.0230 ms
composition and 0.0133 ms presentation. Median upload is 1,966,272 bytes per
frame at every scale. CPU process samples vary with host load; the code has no
scale-dependent CPU preparation loop. The final profile retains 670 intact
GPU timestamp records and one explicitly excluded stdout-interleaved record;
its summary reports 671/671 GPU draws and zero fallback. The parser exception
occurred after the successful run; its process timing was not retained and is
not used for headroom. The three unprofiled runs establish that result.

Modeled GPU residency at 16x is about 200 MB, not an independent driver-memory
measurement. Paused working set is 2.283 GB versus 2.270 GB for the baseline;
private commit is 2.642 GB versus 2.354 GB, including the pack and driver
allocations. No total process-memory reduction is claimed.

## Combined-gate Metroid finding

The initial fresh combined Metroid artifact (`2180ce26`) is rejected: its
CPU median regresses 11.64%, then 10.61% in five slower confirmation pairs.
Zelda's gains do not offset that result. All raw negative evidence is retained.

Only settings/menu object code differed; the CPU/PPU/APU, generated code, SDL,
compiler, DLLs and object order matched. Restoring the two old settings/menu
bodies against the current header did not establish a recovery. The coherent
fix scopes the HD-only field, parser, persistence and menu code to targets that
compile the HD provider, preserving the old unsupported-target layout. Existing
HD-enabled two/four-player input tests pass 880/1120 checks.

The supported scoped Metroid control (`5104bf06`, source `035051c`) has
byte-identical `.text`, `.data`, `.rdata`, `.pdata`, `.xdata`, `.bss`, `.idata`,
`.tls`, `.rsrc` and `.reloc` sections versus selected `b6b87add`, including their
RVAs and virtual/raw sizes. Execution headers are identical; only debug extent
differs by one page. Exact outputs, both 600-frame continuations and SDL checks
pass. Its five mixed CPU pairs have a 3.60% higher unpaired median, classified
independently as neutral/inconclusive for this unchanged runtime image. No gain
or precise causal timing mechanism is claimed. The original slower artifact
stays rejected. Final supported artifacts must use the scoped integration;
HD-enabled rendering behavior is unchanged.

## Scope and limits

This selects Zelda Remastered's HD composition path. General native PPU and
voxel GPU rendering remain residual opportunities. Per-pixel native jobs still
upload about 1.97 MB/frame: scale-independent, but larger than the guide's ideal
small scanline stream. The original pack has 2x artwork; higher internal density
does not invent additional detail. 32x/64x remain experimental desired targets;
full-speed qualification is 16x on this PC. Shared unsupported pack conditions
and the missing `selectscreen.png` background remain unchanged.

No historical old/new GPU reduction percentage is claimed. Earlier fallback
attempts are excluded; the selected raw binary and complete matched samples
are identified in the [machine-readable result](2026-10-31-resident-gpu-results.json).
Raw evidence is retained at
`G:/Temp/Codex/nes110-gpu-candidate-20261031/zelda-hd-v4`, with fixture and
portable results under `pipeline-burndown-20261028`.
