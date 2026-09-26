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
`tools/cyc/mapper_fixtures.py`. Oracle parity checks integration with two CPU/PPU
models; it does not independently establish the mapper specification.

| ID | Board / reference | Behavior and limits |
|---:|---|---|
| 75 | [VRC1](https://www.nesdev.org/wiki/VRC1) | Three 8 KiB PRG windows; split CHR high/low bits; H/V control. Vs. System excluded. |
| 71 | [Camerica](https://www.nesdev.org/wiki/INES_Mapper_071) | 16 KiB PRG; iNES Fire Hawk mirroring heuristic only at $9000-$9FFF; no bus conflicts. |
| 34 | [BNROM / NINA-001](https://www.nesdev.org/wiki/INES_Mapper_034) | 0–8 KiB CHR ROM selects BNROM (AND conflicts); larger CHR ROM selects NINA-001 (WRAM writes also reach bank registers). |
| 13 | [CPROM](https://www.nesdev.org/wiki/CPROM) | Fixed PRG; 16 KiB CHR RAM, upper 4 KiB switchable; vertical mirroring; AND conflicts. |
| 11 | [Color Dreams](https://www.nesdev.org/wiki/Color_Dreams) | 32 KiB PRG / 8 KiB CHR; AND bus conflicts. Conflict-free prototypes excluded. |

New boards initially accept legacy iNES images only. NES 2.0 variants are rejected
for these IDs until their submapper and RAM-size metadata are implemented. Fixed
mirroring follows the iNES header. Four-screen boards are rejected. Power-on bank
registers use deterministic zero values where hardware does not guarantee a state.
Battery RAM persistence and analog CIC defeat circuits are outside this host.

Run the cartridge contracts with `ctest` in a `runner/cyc` build. Run execution
checks with `tools/cyc/test_cyc_runtime.py`; every fixture runs compiled code, the
same binary's interpreter, the standalone interpreter, and the independent oracle
at all four CPU/PPU alignments. Retained logs contain the frame and bus hashes.
