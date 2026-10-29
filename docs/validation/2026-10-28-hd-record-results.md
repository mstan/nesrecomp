# HD recorder experiments: no selected change

NES13, `beads-2dw.1.110`, campaign `beads-2wy8.1`. Both implementations were
evaluated against the selected engine through `198b075`; neither established
a whole built-game gain. Keep both out of the selected runtime. The independent validator confirmed the raw samples, archive/PE/source
identities, correctness hashes and capacity receipts; no implementation PR
is warranted.

| Experiment / host | Reference CPU median | Candidate CPU median | Reduction | Paired results |
|---|---:|---:|---:|---|
| Owned tile pool and compact pixel IDs / this PC | 4.312500s | 4.281250s | 0.72% | 3 faster, 2 slower |
| Circular fetch and color queues / SERVER | 5.890625s | 5.921875s | -0.53% | 1 faster, 3 slower, 1 tie |

Both results are neutral/inconclusive. Different implementations were measured
on different eligible hosts; this is not a cross-host A/B comparison. Each
matched pair used one host/configuration under a capacity reservation, with
five alternating pairs and excluded warmups. Actual SDL/software1280x720
with `SDL_VIDEODRIVER=dummy` (no physical-window/GPU presentation timing),
Remastered enabled, gameplay/death frames1801..2101, audio emulated/mixed/queued
to dummy output; setup is excluded. No actual-FPS or stage-completion claim.

The first design owns immutable fetched tile keys and shares them through
compact pixels and circular queues. It eliminates tile-byte duplication but
adds a fixed key pool, reference counts and per-pixel ownership operations.
The second removes that pool and changes only the logical fetched/color delay
queues, canonicalizing their owned-byte payloads at save boundaries. It adds
two queue heads and no frame allocation. After these two unsuccessful attempts,
stop tuning this path and reassess the complete recording/sampling interface.

Both actual-game comparisons match native frames, native hardware/RAM, full
PCM and HD presentation checkpoints. Both production-recorder fixtures match
4,915,200 generated pixels across CHR RAM/ROM/bank changes, bytes latched before
CHR mutation, fine-X/palettes/flips/PAL masks, inactive retained output and
partial-frame save/load. The circular version also matches 80 complete
canonical saved-payload checks. These are bounded differential checks, not
an exhaustive content audit. No new HLE/LLE whole-machine matrix or stock/Fit
benchmark was completed because neither prototype was selected. The attempted
local reservation for additional machine qualification launched zero jobs.
Missing `selectscreen.png` and unsupported pack rules reproduce in both builds
and remain shared limitations.

No implementation PR is warranted for this unsuccessful item. The validator
confirmed the evidence; its final handoff follows the durable Beads record.
NES15 is the next separate candidate: the sampler still materializes full
`HdPixel` records. A later measured end-to-end replacement could justify
reopening NES13; these experiments do not establish architectural exhaustion.
Final combined builds and owner approval remain open; FDS stays parked.

[Machine-readable evidence](2026-10-28-hd-record-results.json) contains every
raw pair, binary/object/source identity, settings, host telemetry, correctness
hash and archive identity. Private reproducer sources, binaries and verified
archives remain under `G:/Temp/Codex/nesrecomp/pipeline-burndown-20261028`.
ROM/pack staging is disposable and excluded from committed evidence.
