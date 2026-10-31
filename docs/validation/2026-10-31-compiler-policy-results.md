# NES27: qualified SMB1 target IPO

Select target IPO for the qualified SMB1 Win64 GNU15.2.0 O3Release profile.
The normal UI/NET-compiled matched Fit workload uses median whole-process CPU
6.093750 to 5.203125 seconds: **14.615% lower**, all three pairs positive.
Raw reference/candidate pairs: 6.109375/5.218750, 6.000000/5.203125,
6.093750/5.171875. Startup and normal shutdown are included. This is a local
9800X3D single-LP14 software-SDL, trace-off 2401-frame boot/gameplay workload,
with actual mixed audio and dummy output. Native work and output products match.
The separate UI/NET-OFF diagnostic result is not pooled with this package.

The title-only CMake guard defaults on for cycle/HLE, normal UI+NET compiled,
Win64 x86_64, GNU C/C++15.2.0 and exact default O3Release flags after
CheckIPOSupported. It uses target IPO with one LTRANS worker. Option OFF,
O2, unsupported platform/compiler/configuration retain ordinary compilation.
Other four games receive no new compiler package or inferred gain.

The fresh guarded production binary is 8,290,655 bytes, compared with the
8,645,744-byte reference. Its runtime PE section bytes, RVAs and virtual sizes
match the measured candidate; whole-file metadata differs.
One diagnostic sample gives startup-to-first-TCP-response 1.037/0.607 seconds,
working set36.13/53.89MB and private commit55.443/55.505MB. These samples include
poll/OS residency variation and establish no startup or memory speedup.

The 1000-frame historical Fit route, frame933 memory/932:934 boundary,
current-build stock/co-op2/Fit continuation, PCM/pictures/state, normal UI,
safe invalid-state refusal, resizing and actual rewind checks pass. Generated
game C is identical. Real20ms co-op replay passes in candidate and a justified
repeated reference, with matching last frame850 records and full-domain digests.
The first reference forks; preserve it as **beads-2dw.1.111**. Full850state bytes
and differing final drain frames were not compared. No NET speedup is claimed.
Earlier NES22 stock/UI-OFF rejection is unchanged; it is a different package.

The older global O2 IPO defect **beads-2dw.1.106** remains unresolved. Its exact
candidate binary is absent. Current O3 target qualification does not diagnose
the old defect or qualify other compilers, PGO/layout policies or games. This
is a scoped decision. No merge or release yet: final fresh supported CLI and
five-title builds, combined validation and owner approval remain separate.
See [raw identities, samples, contracts and limits](2026-10-31-compiler-policy-results.json).

Implementation: [SMB1 PR16](https://github.com/mstan/SuperMarioBrosNESRecomp/pull/16),
head `35df4bc760c8a27e0e55a6ae65fd1308ae335e87`. Raw evidence is archived
with CRC/SHA verification; archive index is retained in the JSON report.
