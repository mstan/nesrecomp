# NES30: prepared MMC5 pattern-window candidate

Reject this implemented package: correctness passes, but matched gameplay timing
does not establish a benefit. It remains local prototype `d68741e`; its runtime
code is absent from the selected integration branch. This is an actual candidate
decision, not complex-board or expansion-audio architectural exhaustion.

The candidate prepares both sprite/background CHR windows at register writes,
reset and both state restore paths. Fetch-time /RD, split, extended-attribute,
IRQ and audio timing stay unchanged. The 64-byte derived table is excluded from
canonical state. Existing mapper contracts, including restored mapping after a
discarded future, pass.

The current supported compiler generated a fresh native Castlevania III USA
build from the retained seeds. The retained 1,500-frame stage-one gameplay route
executes 98.5% native cycles. Both binaries match all 16 output products
(PCM, frame records, hardware, RAM and pictures) and pass their own 600-frame
save/load continuations. The old private smoke's budget and evidence were not
rerun or reset.

Matched single-core process CPU includes startup and the entire native route;
no observers run in the timed legs. The initial three pairs were mixed. Five
confirmation pairs on the same core produced:

| Pair | Reference CPU seconds | Candidate CPU seconds |
|---|---:|---:|
| 1 | 5.125000 | 4.906250 |
| 2 | 5.203125 | 5.156250 |
| 3 | 4.796875 | 5.156250 |
| 4 | 5.203125 | 5.140625 |
| 5 | 5.062500 | 5.234375 |

The confirmation median is 5.125 to 5.15625 seconds, with three faster and two
slower pairs. Variation prevents claiming a win or a precise regression.
No further matrix or profiler is needed to reject this package. The five
standard games have other boards and receive no runtime change.

Raw products, commands, hashes, affinity and per-core telemetry remain at
`G:\Temp\Codex\nes110-complex-candidate-20261031\castlevania3\evidence`. [Machine-readable result](2026-10-31-mmc5-mapping-candidate-results.json).
