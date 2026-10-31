# NES10 native neighboring-room sprite service

Select [engine PR88](https://github.com/mstan/nesrecomp/pull/88), runtime
`ad88eb3`, and [Zelda PR14](https://github.com/mstan/LegendOfZeldaNESRecomp/pull/14),
`13f475b`, for final combined integration. The complete sprite reset/cursor
memory job now runs directly during isolated neighboring-room simulation.
The active room and unsupported cases retain the original routine.

| Zelda profile | Baseline median CPU s | Candidate median CPU s | Raw reduction | Paired result | Disposition |
| --- | ---: | ---: | ---: | --- | --- |
| atlas | 5.812500 | 5.546875 | +4.570% | 3 faster | Observed positive |
| fit | 1.906250 | 1.875000 | +1.639% | 1 faster, 2 slower | Neutral/inconclusive |

Each profile has three matched 600-frame pairs after excluded warmups on this
PC. SDL uses software rendering at 1280x720; actual views are Fit 426x240 with
15 simulated rooms and Atlas 4096x1472 with 128. CPU and wall samples are
retained separately with host telemetry and exact identities. Fit's positive
median is not a demonstrated gain: its pairs are mixed within variation.
Atlas's three pairs are all faster. No aggregation offsets a regression.

Both profiles match frame/device/memory, pictures and PCM, plus every mid/end
save MOD record containing room/actor state. Timings restore each build's own
current gameplay state. Logs prove 121,285 actual native jobs in the Atlas
correctness route and 11,910 in Fit, with no isolated-call failures.

Current post-NES08 sampling selected this repeatedly executed native service;
old interpreter-era profiles were not treated as current evidence. There are
1,313 successful samples and two failures; 661 are in isolation and 48 have
guest dispatch PC $6E1D. This is coarse diagnostic attribution, not an exact
exclusive cost or possible gain. The initial clear-only reading missed the
routine tail; independent review corrected that contract before implementation.

The actual 25-byte routine and 13-byte helper clear forty sprite Y slots,
preserve inner JSR stack writes, advance both cursor bytes and reproduce
ADC/CMP flags including overflow. The native prefix preserves its 752/753-cycle
budget, publishes the original last driven read and resumes at $6E32. The CPU
executes the final store and outer RTS, completing the original 762/763 cycles.
The existing isolated scope keeps devices frozen equally in both paths.

Readiness checks every code byte, driven fixed SRAM/read gates, read/return
observers, substitutions, interrupts, intervening hooks and remaining budget.
Unsupported cases run the original code. All stores use existing code watches.
No platform-specific instruction or dependency is added.

The actual ROM routine is the differential oracle: all 256 cursor values with
varied flags/Y/stack, 35 budget endpoint cases, 38 code mutations and six guard
refusals pass full RAM, registers, hardware hash and cycle parity. The first
QA launcher lacked required exit-after with uncapped and exited before testing;
the corrected launcher reused the same binary and passed. Optional QA reporting
was added afterward; all four core source recipes remain identical and the
earlier QA source/executable are retained by hash. Normal builds leave the
optional ZELDA_TEST_NATIVE_JOBS oracle off.

Supported regeneration changes only hook metadata and one RAM-entry check;
the other nineteen generated files are byte-identical. The experiment replaces
four objects and adds the oracle, retaining 118 other objects and Zelda's
canvas. This is a frozen relink experiment, not a fresh complete supported
title/CLI build. Final supported regeneration and the combined five-title,
feature/performance and owner checks remain separate. No merge or release.

Other entity/collision/decompression/streaming services remain residuals; this
does not claim architectural exhaustion. Evidence archives preserve raw results,
source/binary identities, settings and capacity receipts. Disposable profile,
oracle and performance staging roots were audited and removed; selected source
and products remain for final integration. See the
[machine-readable record](2026-10-31-native-service-results.json).

NES08 metadata correction: its four other games were stock 256x240, while Zelda
was Seamless Fit 426x240. PR87 now labels that accurately; no matched sample or
runtime changed and no accepted check was repeated.
