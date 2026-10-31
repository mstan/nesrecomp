# NES30: prepared MMC5 pattern-window candidate

Reject this implemented package: correctness passes, but matched active gameplay
does not establish a benefit. Local prototype `d68741e` is absent from the selected
runtime. This decision does not exhaust complex-board or expansion-audio work.

Both sprite/background CHR windows are prepared at register writes, reset and
both state restore paths. Fetch-time /RD, split, extended-attribute, IRQ and audio
timing stay unchanged. The derived 64-byte table is outside canonical state.
Existing mapper contracts, including discarded-future mapping restore, pass.

The original 1,500-frame route spent much of its time in the opening animation.
The independent validator caught that limitation. Those samples remain retained
as boot/intro evidence and are excluded from gameplay conclusions. The corrected
workload uses normal stage-one movement/combat during frames 1,500–2,099. Images at
1,500/1,800/2,099 show the HUD, changing player position, rightward scrolling,
enemies and countdown 498→489. No success flags or game state were forced.

The fresh supported native build executes 15,307,477 native cycles and 2,560,824
interpreted cycles in that active interval (85.67% native). Both binaries match
all 20 output products (PCM, frame records, hardware, RAM and pictures), and each
passes its own 600-frame save/load continuation. The old private smoke's budget
and evidence were neither rerun nor reset.

Matched same-core process CPU includes process startup/load and 600 resumed
active frames; timed legs have no observers. Initial 3pairs were mixed. The five
active confirmation pairs were:

| Pair | Reference CPU seconds | Candidate CPU seconds |
|---|---:|---:|
| 1 | 2.609375 | 2.187500 |
| 2 | 2.156250 | 2.203125 |
| 3 | 2.125000 | 2.171875 |
| 4 | 2.218750 | 2.656250 |
| 5 | 2.171875 | 2.109375 |

Median 2.171875→2.1875s, two faster/three slower, with strong variation: no
established gain or precise regression. No runtime adoption or successful-change
PR. The five standard games receive no change. Raw products, exact commands,
identities and per-core telemetry remain at
`G:/Temp/Codex/nes110-complex-candidate-20261031/castlevania3-gameplay/evidence`.
[Machine-readable result](2026-10-31-mmc5-mapping-candidate-results.json).
