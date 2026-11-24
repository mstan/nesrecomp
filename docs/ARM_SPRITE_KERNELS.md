# AArch64 sprite kernels

The HLE PPU's sprite mask and shift operations (`runner/cyc/ppu_hle_kernel.h`)
use eight-byte AArch64 NEON vectors when `__aarch64__` and `__ARM_NEON` are
defined. The x86-64 SSE2 and portable implementations are unchanged.
`NESRECOMP_PPU_HLE_FORCE_PORTABLE` still forces the scalar reference, including
on ARM. `arm_neon.h` is included only on AArch64.

The kernel loads exactly eight lanes, tests counters before decrementing,
uses unsigned saturating counter subtraction and shifts only ready sprite
planes. AArch64 has no `PMOVMSKB`, so a weighted horizontal add packs the eight
high bits into the existing priority mask; each weight occurs once, so the sum
cannot overflow. The HD recorder's shift mask keeps the same meaning.
No guest clock, bus access, mapper behavior, save field or game routine changes.

## Tests

- `cyc_ppu_kernel_test` compares native and scalar operations over every planar
  row, all 256 simultaneous lane masks (new: catches horizontal-pack mistakes
  that sparse random lanes can miss), all byte-valued counters at every lane and
  250,000 random configurations, including rendering-off and skipped-dot cases.
  It now prints which native path was compiled (`NEON=` / `SSE2=`).
- `cyc_ppu_span_reference` now forces portable kernels, so the complete-machine
  `cyc_ppu_spans` comparison also checks SIMD against scalar processing.
