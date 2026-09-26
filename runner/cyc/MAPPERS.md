# Cycle runtime mapper coverage

This applies to `cycle_accurate=true`, `cyc_interp`, and `cyc_oracle`.
The legacy runner has a separate cartridge implementation.

Start with the [NESdev mapper index](https://www.nesdev.org/wiki/List_of_mappers),
then the board's register description and
[NES 2.0 submapper assignments](https://www.nesdev.org/wiki/NES_2.0_submappers).
The submapper notes matter: a mapper ID can describe electrically different boards.
Register behavior can be cross-checked against the
[Mesen cartridge implementations](https://github.com/SourMesen/Mesen2/tree/master/Core/NES/Mappers).
Those are a cross-check, not a replacement for hardware documentation.

The original IDs are 0, 1, 2, 3, 4, 7, and 66. The additions below have explicit
bank/decode contracts in `mapper_test.c` and generated ROM execution checks in
`tools/cyc/mapper_fixtures.py` and `mapper_ppu_fixtures.py`.
Oracle parity checks integration with two CPU/PPU
models; it does not independently establish the mapper specification.

| ID | Board / reference | Behavior and limits |
|---:|---|---|
| 21 | VRC4a/c | Submappers 1/2 select address wiring; PRG swap, 9-bit CHR, WRAM gate and CPU/divider IRQ. |
| 22 | VRC2a | Swapped address lines, shifted 8-bit CHR, one-bit latch, no IRQ. |
| 23 | VRC2b / VRC4e/f | Submappers 1/2 select VRC4 wiring; submapper 3 selects VRC2. |
| 25 | VRC2c / VRC4b/d | Submappers 1/2 select VRC4 wiring; submapper 3 selects VRC2. |
| 31 | [NSF cartridge](https://www.nesdev.org/wiki/INES_Mapper_031) | Eight independent 4 KiB PRG windows, $5000-$5FFF register aliases, $F000 power-on bank $FF, fixed CHR and H/V wiring. |
| 9 | [MMC2](https://www.nesdev.org/wiki/MMC2) | Switchable 8 KiB PRG plus fixed last 24 KiB; two pairs of 4 KiB CHR banks. Read latches commit when /RD is released. |
| 10 | [MMC4](https://www.nesdev.org/wiki/MMC4) | Switchable 16 KiB PRG plus fixed last 16 KiB, 8 KiB WRAM, and MMC2-style latches with eight-address trigger ranges on both CHR halves. |
| 232 | [Quattro](https://www.nesdev.org/wiki/INES_Mapper_071) | Outer 64 KiB plus inner 16 KiB banking; NES 2.0 submapper 1 swaps the outer bits for Aladdin. |
| 184 | [Sunsoft-1](https://www.nesdev.org/wiki/Sunsoft_1) | Fixed PRG; two 4 KiB CHR windows; upper window forces bank bit 2. |
| 180 | [UxROM variant](https://www.nesdev.org/wiki/UxROM) | First 16 KiB fixed, upper 16 KiB switchable; AND bus conflicts. |
| 140 | [JF-11/14](https://www.nesdev.org/wiki/INES_Mapper_140) | 32 KiB PRG / 8 KiB CHR; $6000-$7FFF decode; no conflicts. |
| 113 | [HES](https://www.nesdev.org/wiki/INES_Mapper_113) | 32 KiB PRG / 8 KiB CHR, split CHR select bits and H/V control; $4100 decode. |
| 94 | [UxROM variant](https://www.nesdev.org/wiki/UxROM) | PRG bank uses bits 2-4, last 16 KiB fixed; AND bus conflicts. |
| 87 | [J87](https://www.nesdev.org/wiki/INES_Mapper_087) | Fixed PRG; reversed CHR select bits; $6000-$7FFF decode. |
| 79 | [NINA-003/006](https://www.nesdev.org/wiki/NINA-003-006) | 32 KiB PRG / 8 KiB CHR; partial $4100-$5FFF decode. |
| 76 | [Namco 109](https://www.nesdev.org/wiki/INES_Mapper_076) | Two 8 KiB PRG windows; four 2 KiB CHR windows; fixed mirroring. |
| 206 | [DxROM](https://www.nesdev.org/wiki/INES_Mapper_206) | Fixed MMC3-style orientation, no IRQ; four-screen memory and header-declared Popils WRAM; submapper 1 fixes 32 KiB PRG. Namco 108 spurious writes remain a separate chip-revision task. |
| 75 | [VRC1](https://www.nesdev.org/wiki/VRC1) | Three 8 KiB PRG windows; split CHR high/low bits; H/V control. Vs. System excluded. |
| 71 | [Camerica](https://www.nesdev.org/wiki/INES_Mapper_071) | 16 KiB PRG; NES 2.0 submapper 0 fixes H/V, submapper 1 enables Fire Hawk mirroring at $8000-$9FFF. iNES retains the $9000 heuristic. |
| 34 | [BNROM / NINA-001](https://www.nesdev.org/wiki/INES_Mapper_034) | Submapper 1 selects NINA, 2 selects BNROM; submapper 0/iNES uses CHR size. NINA WRAM writes also reach bank registers. BNROM has AND conflicts. |
| 13 | [CPROM](https://www.nesdev.org/wiki/CPROM) | Fixed PRG; 16 KiB CHR RAM, upper 4 KiB switchable; vertical mirroring; AND conflicts. |
| 11 | [Color Dreams](https://www.nesdev.org/wiki/Color_Dreams) | 32 KiB PRG / 8 KiB CHR; AND bus conflicts. Conflict-free prototypes excluded. |

[NES 2.0](https://www.nesdev.org/wiki/NES_2.0) decoding now preserves the 12-bit
mapper, submapper, extended/exponent ROM lengths, RAM/NVRAM sizes, trainer offset,
timing, and console type. Unknown submappers, unsupported console/timing models,
and mixed CHR ROM/RAM boards are rejected explicitly. NTSC and multi-region
headers run the NTSC machine. File allocations are bounded to 64 MiB per ROM,
128 KiB PRG RAM, and 1 MiB CHR RAM. RAM sizes do not by themselves add banking
registers: extended MMC1 RAM/outer-bank wiring is separate follow-up work.

Four-screen cartridges have four independent nametables, included in memory
hashes and dumps. RAM below a CPU/PPU window size mirrors within that window.
Submapper 5 of MMC1 fixes the 32 KiB PRG map; submappers 1/2 of UxROM, CNROM,
and AxROM select no/AND bus conflicts. Generated programs now check cartridge
metadata as well as PRG bytes: regenerate older cycle output when updating.
Power-on mappings remain deterministic where hardware does not specify a state.
NVRAM bytes survive machine power-on within a loaded cartridge; host save-file
persistence is part of the EEPROM/persistence follow-up.

Run the cartridge contracts with `ctest` in a `runner/cyc` build. Run execution
checks with `tools/cyc/test_cyc_runtime.py`; every fixture runs compiled code, the
same binary's interpreter, the standalone interpreter, and the independent oracle
at all four CPU/PPU alignments. Retained logs contain the frame and bus hashes.

Validation of this expansion: 417 cartridge assertions and 41 synthetic programs
(656 executions across the four modes and alignments on each of Windows and
Linux), all with 100% native CPU execution, plus 135 rejection
checks from `test_cyc_mapper_headers.py`. PPU programs assert physical CHR-page
bytes through `$2007`, verify banked CHR RAM, mirroring and overlapping WRAM,
then enable background rendering for frame-hash comparison. These tests establish
specific board contracts; commercial games on the new IDs have not been tested.
AccuracyCoin and the 3,000-frame SMB3 route also match the pre-expansion traces
exactly at all four alignments. AccuracyCoin retains its existing alignment
scores of 144/144, 143/144, 141/144 and 143/144; this change adds no new failures.

The remaining draft stack includes MMC5, VRC expansion audio chips,
Bandai EEPROM, and extended board wiring.
Each needs its missing hardware primitive and suitable regression ROMs before
being added to the supported list. The legacy runner still needs separate work.

The metadata/variant draft adds `cart_header_test.c` and
`tools/cyc/cart_variant_fixtures.py`. Contracts cover Aladdin, fixed Namco PRG,
explicit NINA/BNROM selection, exponent lengths, Camerica variants, Popils RAM,
small RAM mirroring, and four-screen reads/writes through the actual PPU bus.
The full execution harness and header acceptance/rejection harness retain their
per-case logs under the selected output directory.

MMC2/MMC4 latch tests exhaust all 8,192 pattern addresses for each chip, verify the old byte remains
stable throughout the read pulse, and keep peeks/writes from triggering latches.
`latch_fixtures.py` exercises the actual `$2007` state machine and rendered FD/FE
tiles, with native/interpreter/oracle parity at all four alignments. This caught
and corrected an early latch update: `$2007` samples more than once during /RD,
so committing on the first sample returned the new bank too soon. Commercial
MMC2/MMC4 games and a physical cartridge are not part of this validation.

PRG translation and generated views now use 4 KiB banks. Mapper 31 fixtures
replace each of the eight executing windows, use register aliases, read across
independently banked halves of an old 8 KiB window, and load an exponent-encoded
4 KiB ROM. Old `BB:AAAA` seed files retain their physical 8 KiB meaning; new miss
logs use `4k:BB:AAAA`. `test_cyc_seed_units.py` checks log merging and recompiling
from those identities. The PRG table's hardware-state hash layout changes with
its granularity; bus, memory, and picture traces remain comparable.

VRC board contracts follow [VRC2/VRC4](https://www.nesdev.org/wiki/VRC2_and_VRC4),
[VRC IRQ](https://www.nesdev.org/wiki/VRC_IRQ), and
[VRC3](https://www.nesdev.org/wiki/VRC3). The IRQ hook runs once per completed
CPU cycle, including DMA. Tests assert the VRC4 divider's 114/114/113 sequence,
reload/acknowledge behavior, CHR high bits, all PCB address decodes, WRAM gating,
and VRC2's partially driven read bus. CPU programs exercise rendering and IRQs
through OAM DMA. The full five-ID matrix has 33 programs (528 executions on
Windows), all passing at every alignment with 100% native execution. Commercial
VRC games and physical hardware have not been tested.

iNES mapper 23/25 cannot unambiguously distinguish VRC2 from VRC4. Submapper 0
retains the historical VRC4 union of address decodes; use NES 2.0 submapper 3 for
the VRC2 bit latch and 8-bit CHR behavior (including Wai Wai World). VRC4 boards
with explicit 2 KiB WRAM mirror it only through $6000-$6FFF.
