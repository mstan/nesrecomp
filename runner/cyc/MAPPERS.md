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
| 155 | [MMC1A](https://www.nesdev.org/wiki/MMC1) | RAM stays enabled by the PRG register; bit 4 instead bypasses fixed-bank A17 selection. Uses the same SxROM board wiring as mapper 1. |
| 85 | [VRC7](https://www.nesdev.org/wiki/VRC7) | Three 8 KiB PRG windows, eight CHR windows, WRAM gate, VRC IRQ, and six FM channels. Submapper 1 selects A3 and omits the oscillator; submapper 2 selects A4. |
| 24 | [VRC6a](https://www.nesdev.org/wiki/VRC6) | 16+8 KiB PRG, all CHR/nametable modes, WRAM gate, CPU IRQ and two pulse/one saw audio channels. |
| 26 | [VRC6b](https://www.nesdev.org/wiki/VRC6) | VRC6 with swapped A0/A1 register wiring, including the audio ports. |
| 21 | VRC4a/c | Submappers 1/2 select address wiring; PRG swap, 9-bit CHR, WRAM gate and CPU/divider IRQ. |
| 22 | VRC2a | Swapped address lines, shifted 8-bit CHR, one-bit latch, no IRQ. |
| 23 | VRC2b / VRC4e/f | Submappers 1/2 select VRC4 wiring; submapper 3 selects VRC2. |
| 25 | VRC2c / VRC4b/d | Submappers 1/2 select VRC4 wiring; submapper 3 selects VRC2. |
| 73 | VRC3 | 16 KiB PRG switch and 8/16-bit CPU IRQ counter. |
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
registers; implemented MMC1 board wiring is described below.

Four-screen cartridges have four independent nametables, included in memory
hashes and dumps. RAM below a CPU/PPU window size mirrors within that window.
Submapper 5 of MMC1 fixes the 32 KiB PRG map; submappers 1/2 of UxROM, CNROM,
and AxROM select no/AND bus conflicts. Generated programs now check cartridge
metadata as well as PRG bytes: regenerate older cycle output when updating.
Power-on mappings remain deterministic where hardware does not specify a state.
NVRAM bytes survive machine power-on within a loaded cartridge; host save-file
persistence is available with `--save-file FILE`. Files contain raw PRG NVRAM
followed by CHR NVRAM, or the cartridge's serial EEPROM bytes. Saves are opt-in
so unattended regression runs start consistently. Invalid lengths fail before
execution; successful exits flush a temporary file and atomically replace the
save, including after the SDL host returns.

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

The extended board wiring is described below. The legacy runner still needs
separate integration work.

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

VRC6 nametable pin vectors come from BootGod's measurements reported by Quietust
on [Talk:VRC6](https://www.nesdev.org/wiki/Talk:VRC6#Raw_data). The contracts cover
all 64 banking-style values, including CHR-ROM nametables and independent CIRAM
selection. Execution fixtures read these through $2007 and enable rendering.
The audio sequencers run during DMA and when host audio is disabled; PCM mixing
adds their inverted linear DAC before the existing output filters. Nominal mixer
gain is approximate; cartridge resistor tolerances and analog response need
hardware comparison. `test_cyc_expansion_audio.py` records native/interpreter
WAVs, checks exact parity and pulse/saw frequencies, and rejects silent/clipped
output. The oracle does not synthesize expansion audio; waveform contracts and
recorded PCM tests provide that validation. Commercial VRC6 games remain to test.

VRC7 uses the pinned MIT-licensed emu2413 core in `vendor/emu2413`, with the
instrument bytes checked against the chip's dumped patch ROM. A rational clock
divider models the independent 3.579545 MHz resonator; power-on phase is
deterministic. Diagnostic test-register behavior, sound reset, ignored writes,
and all six channels have contracts. CPU fixtures cover all three submapper
choices, PRG/CHR/WRAM/mirroring, IRQs through DMA, a custom sine carrier, and the
silent VRC7b/reset cases. The PCM harness measures the 440.601 Hz carrier and
compares native/interpreter WAVs byte for byte. This is an FM model with nominal
gain, not a bit-exact capture of the chip's serial DAC or cartridge analog mixer.
Lagrange Point and Tiny Toon Adventures 2 remain commercial-game validation work.

Bandai mapper 16 supports FCG-1/2 ($6000 registers, direct IRQ counter) and
LZ93D50 ($8000 registers, reload latch). NES 2.0 submappers 4/5 choose those
decodes; ambiguous submapper 0 accepts each in its corresponding window.
Deprecated submappers 1/2/3 are rejected; use mapper 159/157/153 respectively.
LZ93D50's Xicor X24C02 EEPROM uses an A0/A1 device command, MSB-first bytes,
four-byte page wrapping, sequential reads, ACK polling, and a nominal 5 ms
programming interval. Only committed bytes are exported; power interruption
discards an unfinished write. D7 releases SDA for reads and D4 receives the
open-drain result. EEPROM capacity comes from NES 2.0 PRG NVRAM (256 bytes) or
the iNES battery bit. The bytes are not exposed as CPU work RAM.

Sources: [mapper 16](https://www.nesdev.org/wiki/INES_Mapper_016),
[FCG-2 PCB tracing](https://seesaawiki.jp/famicomcartridge/d/Bandai%20FCG-2),
[LZ93D50 PCB tracing](https://seesaawiki.jp/famicomcartridge/d/Bandai%20LZ93D50%20standard),
and the Xicor datasheets attached to the latter page (its two PDF labels are
reversed; inspect the document title). These parts have four-byte pages,
unlike many later 24Cxx devices. The EEPROM primitive is shared with the oracle;
board and IRQ logic are independent. Direct transaction vectors check every
address, sequential wrap, page rollover, NACK, and interrupted programming.
Fourteen 6502 fixtures (224 executions) exercise banks, rendering, IRQs through
DMA, and bit-banged serial traffic at all alignments. Save tests start separate
processes to increment persisted bytes, verify PRG/CHR save layout, and reject
truncated/oversized files without modifying them. Commercial Bandai games and
physical EEPROM timing have not been compared yet.

Mapper 159 uses the same LZ93D50 board with a 128-byte X24C01. Its command is
the seven-bit word address followed by R/W, with no I2C device address. NES 2.0
must declare 128-byte PRG NVRAM; iNES mapper 159 implies that chip. Byte order
is MSB first, as verified by the PCB researcher using sequential reads; older
emulators sometimes reverse both addresses and data. Mapper 159 has its own
bank, IRQ, serial and process-restart fixtures. Low-window FCG writes also
terminate compiled blocks, so switching $6008 cannot execute stale native code.

Mapper 153 (BA-JUMP2/Famicom Jump II) has fixed 8 KiB CHR RAM and 8 KiB
battery RAM, enabled by $800D bit 5. Cold battery RAM starts at $FF because
this game does not tolerate an all-zero uninitialized save. The active PPU
A11:A10 selects which of $8000-$8003 drives PRG A18, including in the otherwise
fixed $C000 window. This follows the [measured PCB wiring](https://seesaawiki.jp/famicomcartridge/d/Bandai%20BA-JUMP2),
rather than ORing four bank registers. With unequal A18 outputs, dispatch uses
the interpreter so every opcode and operand sees changes during PPU clocks or
DMA. Native dispatch resumes when the outputs agree. Its fixtures check both
256 KiB halves from both CPU windows, PPU-controlled outer banks, WRAM gating,
IRQ/DMA and persisted SRAM. Driving RAM and the unconnected SDA input together
resolves D4 low; exact analog contention on that invalid setting is unspecified.

Mapper 157 (Datach) has fixed 8 KiB CHR RAM, its own 256-byte X24C02, and an
optional cartridge 128-byte X24C01. Header NVRAM describes only the latter.
`--datach-save-file FILE` persists the main unit independently of cartridge
`--save-file FILE`, so it can be shared between games. The clock from the active
CHR register's bit 3 selects the external device; $800D bit 5 clocks the internal
one, and both share a resolved open-drain SDA wire. PPU A11:A10 selection is
modeled, so unequal clock registers can generate serial edges during rendering.

The barcode API and headless `--barcode DIGITS --barcode-frame N` provide
EAN-8, UPC-A or EAN-13 light/dark input, including checksum validation. Swipe
speed is selectable with `--barcode-module-cycles N` (default 1000 CPU cycles).
This models a chosen photodiode stimulus; actual swipe speed is user-dependent.
The [Datach PCB tracing](https://seesaawiki.jp/famicomcartridge/d/Bandai%20Datach)
and [mapper 157 register description](https://www.nesdev.org/wiki/INES_Mapper_157)
define the serial and barcode bus wiring. GS1's General Specifications section
5.2 defines the barcode symbols. The scanner contract checks a literal EAN-13
waveform; 48 process runs check its 62 transitions at three speeds and every
alignment. Runtime and oracle share the optical stimulus and EEPROM primitives,
so these expectations are checked directly instead of claiming independent
chip implementations. No commercial Datach game or physical reader was tested.

The enlarged fixture harness compiles its common runtime once as an object
library, explicitly selects Release on single-configuration generators, and
allows `--build-timeout` for slower machines. Generated game code still links
into separate executables and runs all four execution modes.

### MMC5 / ExROM (mapper 5)

All four PRG modes support ROM and protected RAM in $6000-$DFFF. Tagged bank
tables keep writable and open windows out of native ROM dispatch and ROM miss
logs. Code in cartridge RAM uses the interpreter and may modify itself; returning
to ROM resumes native execution. Writes to $5000+ leave compiled blocks before
the next opcode. Folded ROM reads retain PCM and NMI-vector side effects.

NES 2.0 metadata selects physical RAM geometry: no RAM, EKROM 8 KiB, ETROM's
two 8 KiB chips, EWROM 32 KiB, two 32 KiB chips, or a single 128 KiB chip.
Unpopulated chip selects read open bus. For mixed volatile/nonvolatile chips,
chip 0 is the battery-backed chip, including 8+32 KiB configurations. Legacy
iNES headers with no RAM count use a 64 KiB compatibility allocation because
they cannot identify the board; specify NES 2.0 for exact holes and persistence.
Battery-backed MMC5 saves append its internal 1 KiB ExRAM after PRG/CHR NVRAM;
ExRAM survives machine power-on and is not counted in header RAM sizes.
Mapper-specific four-screen and unknown submapper headers are rejected.

CHR supports all four sizes, write-time 10-bit bank packing, upper-bank latches,
8x8/8x16 bank-set selection and $2007 access. Nametables support both CIRAM
pages, ExRAM and fill mode. Extended attributes and left/right vertical splits
use actual PPU fetch sequences. The scanline IRQ detects three matching
nametable reads and clocks on the following read; frame timeout samples the
actual /RD pin, including during blanking. Fully decoded $2000/$2001 snoops,
NMI-vector acknowledgement, multiplier, MMC5A timer and GPIO registers are
implemented. GPIO has no external peripheral; undriven inputs resolve low.

The audio model has two pulse channels and an 8-bit PCM DAC, with PCM updates
from CPU reads (including opcodes and DMA) or direct writes, zero-sample IRQ,
nonzero-sample acknowledgement, envelopes and length counters. Channel clocks
run even when host audio is disabled. The oracle independently models board,
IRQ, PCM and channel status; it does not synthesize expansion audio. Nominal
audio gain and a 7,457-cycle envelope clock are used. Mixer component tolerances,
analog capture matching, letterless MMC5 silicon and unused SL split wiring
are not claimed. Undocumented low bits in mode-dependent CHR writes resolve
to zero; registers without a specified startup value have deterministic defaults.

References: [MMC5 registers and measured behavior](https://www.nesdev.org/wiki/MMC5),
[ExROM wiring](https://www.nesdev.org/wiki/ExROM),
[audio](https://www.nesdev.org/wiki/MMC5_audio), and
[AWJ's hardware CHR tests and reference pictures](https://sourceforge.net/p/fceultra/bugs/787/).
`tools/cyc/test_cyc_mmc5_public.py` checks all six published pictures, allowing
only a consistent RGB palette conversion and the reference's overscan crop.
ROM-free fixtures exercise bank handoffs, writable code, PCM/DMA, timer and
scanline IRQs, CHR selection, split rendering and expansion tones at all four
CPU/PPU alignments. The cartridge contracts enumerate all 65,536 multiplier
inputs and RAM geometry; rendered PCM is checked separately.

### MMC1 board wiring

[MMC1 board documentation](https://www.nesdev.org/wiki/MMC1) and the
[pinout](https://www.nesdev.org/wiki/MMC1_pinout) define SNROM RAM /CE,
SOROM/SXROM RAM banking, SUROM/SXROM outer PRG banking and SZROM RAM selection.
All use the currently selected CHR output, including PPU A12 changes in 4 KiB
CHR mode. A 512 KiB ROM's fixed bank stays within its selected 256 KiB half.
Native execution falls back while unequal CHR registers can change PRG during
an instruction, then resumes when the mapping is stable.

NES 2.0 sizes distinguish those boards. Deprecated submappers 1, 2 and 4 are
accepted only with matching SUROM, SOROM and SXROM geometry. Submapper 5 retains
fixed 32 KiB PRG; submapper 7 preserves the header's hardwired nametables.
Unspecified iNES RAM counts reserve 32 KiB for compatibility, so old MMC1
battery saves may need migration from 8 KiB; explicit NES 2.0 sizes keep exact
save lengths. Legacy headers with larger CHR use the SZROM RAM select pin.
Only explicit 8 KiB RAM geometry enables SNROM's additional /CE behavior.
SOROM/SZROM saves contain chip 1; chip 0 remains volatile. SXROM saves use the
physical A14:A13 order, with four consecutive 8 KiB banks.

Mapper 155 models MMC1A's PRG-register bit 4: RAM remains enabled and PRG
bit 3 drives A17 even in a fixed window. The separate SNROM CHR-controlled
/CE still applies. Deprecated mapper 1 submapper 3 selects the same revision.
Direct contracts enumerate every PRG register value in every mode, and ROM
fixtures test both fixed windows and both A17 states.

The ROM-free board tests exhaust inner/outer PRG mode combinations and PPU
A12, test hardwired mirroring, then run synthetic ROMs through all four
execution modes/alignments. Save tests restart each executable twice and
verify exact physical bytes plus rejection of invalid lengths. Submapper 6
is a Famicom Network System card on a separate card bus, not the console's
CPU/PPU bus; it remains explicitly rejected pending an FCNS machine model.
