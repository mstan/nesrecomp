# runner/cyc — cycle-accurate static recompilation

`NESRecomp --cycle-accurate` (or `[game] cycle_accurate = true`) recompiles a
program so that every CPU cycle of every ROM instruction is spelled out in C,
and runs it on a cycle-accurate model of the rest of the console. It exists for
software that measures the hardware itself, such as AccuracyCoin, which the
regular function-level output (`*_full.c`, whole-instruction timing, NMI as a
frame driver) cannot run.

Result: **AccuracyCoin passes 144/144 with 100.0% of its ROM code's CPU cycles
executed by recompiled code, on NESRecomp's own CPU, PPU, APU and DMA
implementations**. The run matches the TriCNES oracle on every bus access of
every cycle and on every pixel, and its APU matches NES_MiSTer's HDL APU
channel for channel (see [Verification](#verification)).

The cycle runtime supports 22 mapper IDs; see [board coverage and limits](MAPPERS.md).
**Super Mario Bros. 3** (MMC3, 256KB PRG) runs at 100.0% native
and matches the oracle on every frame of a 3,000-frame scripted playthrough at
all four CPU/PPU alignments, and Mesen's picture exactly on most frames.
**Mega Man 2** (MMC1), **Mega Man** (UxROM) and **Donkey Kong Original
Edition** (CNROM) do the same over a minute of play each; see
[Banked mappers](#banked-mappers) for how a block can fold ROM bytes when a
game moves them, and [Against Mesen's picture](#against-mesens-picture) for
the few pixels that still differ.

## What is NESRecomp's and what is not

| Layer | Files | Status |
|-------|-------|--------|
| 6502 CPU | `cpu6502.h`, `cpu6502.c` | NESRecomp's own |
| Recompiled code | `recompiler/src/cyc_codegen.c` → `generated/<prefix>_cyc.c` | NESRecomp's own |
| Fallback interpreter | `cpu6502_interp.c`, generated from the same templates | NESRecomp's own |
| CPU ↔ hardware interface | `hw.h` | NESRecomp's own |
| Master clock, CPU bus, cartridge loading | `hw_machine.c` | NESRecomp's own |
| Mappers ([coverage](MAPPERS.md)) | `hw_mapper.h`, `hw_mapper.c` | NESRecomp's own |
| PPU (2C02) | `hw_ppu.c` | NESRecomp's own |
| APU, DMAs, controller ports, audio | `hw_apu.c` | NESRecomp's own |
| Palette | `hw_palette.c` (NTSC signal model) | NESRecomp's own |
| Oracle | `tric_core.cpp` (`cyc_oracle`) | TriCNES, test-only |
| APU co-simulation | `tools/cyc/mister_apu` + a NES_MiSTer checkout | NES_MiSTer's `apu.sv`, test-only |
| Picture cross-check | `tools/cyc/mesen_shot` + a MesenCE checkout | Mesen's core, test-only |
| Comparison | `cyc_trace.h`, `tools/cyc/cyc_verify.py` | NESRecomp's own |

No runtime build contains TriCNES or NES_MiSTer code. The machine a recompiled
game runs on is `cpu6502.c` + `hw_*.c`, plain C11.

What the hardware does below the register level (the `$2007` access latch
chain, the per-alignment delays of `$2001`/`$2005`/`$2006`, OAM and palette
corruption, OAM address stepping during sprite evaluation, DMA scheduling,
`$4015` delays) is not in Nintendo's or nesdev's documentation; it was measured
on consoles by the author of AccuracyCoin and TriCNES, and `hw_ppu.c` and
`hw_apu.c` implement that research (their comments say where). The code, its
structure and its fast paths are NESRecomp's.

## The CPU

`cpu6502.h` holds the registers and the four things a CPU cycle can be:

```c
uint8_t cpu_read(uint16_t addr, unsigned flags);
uint8_t cpu_read_rom(uint16_t addr, uint8_t value, unsigned flags);  /* folded PRG ROM byte */
void    cpu_write(uint16_t addr, uint8_t value, unsigned flags);
bool    cpu_fetch_rom(uint16_t pc, uint8_t opcode);                   /* opcode fetch */
```

`flags` marks the cycles that poll the interrupt lines (`CYC_POLL`, or
`CYC_POLL_BRANCH` for a taken branch's page-crossing cycle) and the cycle that
completes the instruction (`CYC_DONE`). Everything else about an instruction
is the order of these calls. Operands, latches and effective addresses are C
locals; nothing records how far through an instruction the CPU is, because
recompiled code and the interpreter only hand over between instructions.
Interrupt entry (NMI, IRQ, reset, BRK) and the jam opcodes are sequences in
`cpu6502.c` that both call.

The NMI input goes through an edge detector in the CPU (`cpu_nmi_input`),
sampled once per cycle including DMA cycles; a rising edge stays latched until
the NMI vector is taken, so the next poll sees it even if `/NMI` was released
in between. IRQ is level-sensitive and sampled by the hardware each cycle; the
2A03 drives it as the frame interrupt OR the DMC interrupt, so acknowledging
one source leaves the other asserted.

### hw.h

The interface to the rest of the machine follows the CPU's pins rather than an
emulator's variables:

```c
void    hw_cycle_start(uint16_t addr, HwCycleKind kind);   /* address and R/W for this cycle */
uint8_t hw_read(uint16_t addr);                            /* or hw_read_rom / hw_write */
void    hw_cycle_finish(bool instruction_done);
bool    hw_irq_line(void);
void    cpu_nmi_input(bool asserted);                      /* called by the hardware */
extern int  hw_dma_stalls;                                 /* CPU cycles DMAs took in this start */
extern bool hw_frame_done;
```

A DMA pauses the 2A03's CPU through RDY, which the 6502 honors only on read
cycles, holding the address it is about to read. So `hw_cycle_start` runs DMA
cycles first only for reads, and the halted cycles see the pending address.
The SHA/SHS/SHX/SHY opcodes check `hw_dma_stalls` on their carry fix-up cycle,
where a DMA changes their result.

## Generated code

One labeled block per ROM instruction, bracketed by the frame check and the
opcode fetch. `STA $04` at `$F1A7`:

```c
L_F1A7: /* 85 STA zp */
    if (hw_frame_done) { cpu.pc = 0xF1A7; return; }
    if (cpu_fetch_rom(0xF1A7, 0x85)) { cpu.pc = 0xF1A7; cpu_interrupt(false); return; }
    {
        cpu_read_rom(0xF1A8, 0x04, 0);
        uint16_t ea = 0x04;
        cpu_write(ea, cpu.a, CYC_POLL | CYC_DONE);
        goto L_F1A9;
    }
```

`LDA $0200,X` reads the uncorrected address only when indexing carries:

```c
        cpu_read_rom(0xF1A8, 0x00, 0);
        cpu_read_rom(0xF1A9, 0x02, 0);
        uint16_t base = 0x0200, ea = (uint16_t)(base + cpu.x);
        if ((ea ^ base) & 0xFF00) cpu_read((uint16_t)(ea - 0x100), 0);
        uint8_t v = cpu_read(ea, CYC_POLL | CYC_DONE);
        cpu.a = v; cpu_nz(v);
```

Static jumps, branches and JSRs become `goto`s within a 1KB chunk and returns
to the scheduler (`cyc_run.c`) across chunks. `cpu.pc` is only written when
control leaves compiled code.

### The interpreter

Code in RAM, open bus execution, instructions that wrap past `$FFFF`, and
addresses discovery did not reach run on `cpu6502_interp.c`. It is the output
of the same template functions with the operand bytes read at run time instead
of folded:

```bash
NESRecomp --emit-cycle-interpreter runner/cyc/cpu6502_interp.c
```

Recompiled and interpreted execution therefore cannot disagree about what an
instruction does; `--interp-only` runs check the folding and control flow.

### Banked mappers

Folding a ROM byte into a constant assumes the byte at that CPU address is
known. On a banked cartridge it is not: `$8000-$FFFF` is four 8KB slots whose
contents the game changes at run time, so the same address holds different
instructions at different moments. 8KB is the finest granularity any supported
mapper switches, which makes the unit of compilation a **(bank, slot) pair**
rather than an address:

- one C function per 1KB of a (bank, slot) pair, one translation unit per PRG
  bank (`<prefix>_cyc_bNN.c`), and a dispatch table in `<prefix>_cyc.c`;
- entry goes through that table, which asks the cartridge which bank is at
  `cpu.pc` right now (`hw_prg_bank()`, one indexed load into `hw_cart.prg_off`)
  and only enters a block generated for that bank. A block whose bank is not
  mapped is simply never entered, so a wrong guess costs output, never
  correctness;
- **a write that can reach a PRG bank register ends the block.** The compiler
  uses the board's lowest register address, including `$4100` on NINA-003/006
  and HES, `$6000` on Jaleco JF-11/14, and `$7FFD` on NINA-001. It sets
  `cpu.pc` and returns so the scheduler re-dispatches on the new mapping.
  Absolute, indexed, and indirect writes all obey this rule; writes proven
  below the register aperture continue in place;
- control flow that leaves the slot it started in also returns to the
  scheduler, unless the board fixes that slot's bank — `$E000-$FFFF` on MMC3,
  the last 16KB on UxROM. Both are cheap: a return to the scheduler is a
  function return and one table lookup, and the hardware model, not dispatch,
  dominates the clock (see below);
- an instruction whose bytes cross a slot boundary is left to the interpreter,
  like one that wraps past `$FFFF`: its later bytes come from a bank chosen at
  run time, so neither its operands nor its length can be folded.

Static discovery therefore reaches only what the fixed slots lead to — for
SMB3, 534 instructions from the vectors. Everything behind a bank switch comes
from a run, exactly as RTS and JMP-indirect targets already did:

```bash
SMB3Recomp rom.nes --frames 3000 --input into_level.txt --miss-log cyc_seeds.txt
NESRecomp rom.nes --game game.toml       # 20,300 instructions, 17 of 32 banks
```

`--miss-log` records the bank each instruction ran in, so its lines are
`BB:AAAA count`. A line without a bank still works and means the bank the
power-on configuration has at that address, which is every bank on NROM, so
seed files written before mappers existed are still valid.

### Discovery and seeds

Discovery walks static control flow from the vectors, `[functions]`/`[[extra_func]]`
entries and `[game] cycle_seed_file`. Compiling an address that never executes
is harmless: a block decodes the same constant ROM bytes the interpreter
would. Seeds therefore don't need to be proven instruction starts.
Entry points reached only through RTS or JMP-indirect dispatch come from a run:

```bash
AccuracyCoinRecomp AccuracyCoin.nes --acccoin --miss-log cyc_seeds.txt   # merges into the file
NESRecomp AccuracyCoin.nes --game game.toml                              # regenerate
```

A frame ends at the first instruction boundary at or after the PPU reports
VBlank, in recompiled, interpreted and oracle runs alike.

### Reading a coverage number

A run that is not 100% native says where the rest of its cycles went:

```
mode=native frames=20000 cycles=595608176 native_cycles=337726733 (56.7%)
  interpreted: ROM 0 (0.0%, add to cycle_seed_file)  RAM 321881443 (54.2%, written at run time, not compilable)
```

The two are acted on differently. ROM cycles are entry points discovery did not
reach: `--miss-log` lists them and seeding them compiles them, so that number
should reach 0. RAM cycles are instructions the program wrote at run time —
code that is not in the ROM image, and that a static recompiler therefore
cannot compile at any effort. `--miss-log` also lists the RAM addresses that
started an instruction, as comments (the recompiler skips them), with the
number of distinct opcode bytes ever seen at each: 1 means only operands change
there, more means the byte is an opcode on one pass and an operand on another.
Mapperless (Demo), an NROM demo whose effects run from patched zero-page
routines, is 100% of its ROM cycles native and 56.7% overall for that reason —
over 45,000 frames, 364 of its 870 RAM instruction addresses saw more than one
opcode, so there is no fixed instruction stream there to compile.

**What the percentage is and is not.** It says how much of the program's work
was performed by compiled code rather than interpreted — provenance, not
speed. Both paths come from the same templates, and the hardware model, not
CPU dispatch, dominates the run: AccuracyCoin takes 10.1 s at 100% native and
10.6 s with `--interp-only`, and Mapperless (Demo) 92 s at 56.7% against 94 s
at 0%. What actually moves the clock is how many dots take the general PPU
path — AccuracyCoin runs at ~430 fps with rendering mostly off and this demo
at ~215 fps with it on.

## The hardware

### Mappers (`hw_mapper.h`, `hw_mapper.c`)

Unlike the 2A03 and 2C02 timing, mapper behavior is documented: these are small
synchronous chips whose registers and bank arithmetic the nesdev wiki describes
completely, and each section of `hw_mapper.c` says which page it follows.
Everything a mapper can do is expressed as four things — where each 8KB PRG
slot and each 1KB CHR page reads from, how it drives CIRAM A10 (the nametable
arrangement), and whether it asserts `/IRQ` — so a mapper is a rule for filling
two small tables. That is also what makes a PRG read one indexed load and what
the recompiler's dispatch reads.

`/IRQ` is one open-drain line shared with the 2A03, so a cartridge interrupt is
ORed with the frame and DMC interrupts (`apu_sample_irq`): a handler that
acknowledges the APU still sees the line held by the cartridge.

The one part that is a timing question rather than a lookup is **MMC3's IRQ
counter**, which has no scanline input and instead counts rising edges of the
PPU's A12. A12 also toggles every few dots inside a tile fetch, so the chip
low-pass filters it: an edge counts only after A12 has been low for at least
three CPU cycles (nesdev wiki, MMC3; Mesen filters on the same three CPU
cycles, NES_MiSTer's `MMC3.sv` on 16 PPU cycles — a rendering PPU produces gaps
of either ~4 dots or ~64, so any threshold between them behaves alike). A12 is
a direct pin rather than one multiplexed through the cartridge's address latch,
so `hw_ppu.c` offers the mapper every address the PPU drives, once per dot in
its first half: a fetch puts its address out with ALE, and the second half only
replaces the latched low byte with the data read. (Over SMB3's 3,000-frame run
every edge the counter took was seen there.) A cartridge that does not watch
the bus costs one predictable branch.

Where the counter is sampled matters to within a tick. At alignments 2 and 3 a
dot falls one tick before the CPU samples `/IRQ`, so sampling A12 anywhere later
than the end of that dot moves an IRQ to the next poll. The oracle's mapper hook
had exactly that problem at first — it sat in TriCNES's half-dot handler — and
SMB3 disagreed with it at those two alignments until it moved to the end of
`_EmulatePPU()`.

### Timing model (`hw_internal.h`, `hw_machine.c`)

The NTSC master clock runs 12 ticks per CPU cycle and 4 per PPU dot. Within a
CPU cycle, ticks are numbered 0-11: the CPU's access is at tick 0, the NMI
input is sampled at tick 4 and IRQ at tick 7. A PPU dot is clocked where
`(alignment + tick) % 4 == 0` and its second half two ticks later; the
alignment (0-3) is fixed at power-on. The APU is clocked on tick 0, after the
CPU's access and the PPU. Some register accesses run part of their cycle's
ticks themselves (a `$2002` read samples VBlank, runs 7 ticks, then samples
the sprite flags), which is how the 2C02 sees them. The 11 ticks between
accesses are unrolled per alignment.

### PPU (`hw_ppu.c`)

Modeled at the level of the VRAM bus: addresses go out on AD0-7 and are held by
the octal latch while ALE is high, the value read comes back on the same pins,
and the pattern address register is shared between background tiles and
sprite loading. That is the level AccuracyCoin's background and sprite
evaluation pages measure. Output is a 9-bit color index per pixel
(color | emphasis << 6); `hw_palette.c` turns indices into ARGB by decoding a
model of the 2C02's composite signal (nesdev's measured voltage levels), so
the displayed palette is not taken from any emulator.

Two fast paths keep it quick without a second implementation to maintain:
dots with rendering off and nothing in progress (most of AccuracyCoin) take a
blank-dot function that is the general one with the ruled-out branches
removed, classified once and reclassified on any register access; and the
`$2007` latch chain skips its latches while at rest.

### APU, DMAs, controllers (`hw_apu.c`)

What the CPU can see — `$4015`, the IRQ line, when DMAs take cycles and what
they read, the controller shift registers — follows the measured behavior and
is checked against the oracle. The tone generators (pulse duty and sweep,
envelopes, triangle linear counter, noise LFSR, DMC output unit) and the mixer
follow the nesdev descriptions, run only when audio is enabled, and are
checked against NES_MiSTer's APU. Audio is 16-bit mono at the host's rate
(`cyc_audio_enable`/`cyc_audio_read`), with the console's 90 Hz and 440 Hz
high-pass and 14 kHz low-pass stages; the SDL host plays it and
`--wav-out FILE` records it.

## Verification

### Against the TriCNES oracle

The oracle and a recompiled build run the same ROM and write `--hash-out`
files that must be identical line for line. Each frame's line holds:

- `trace`: a running hash of the observable trace (`cyc_trace.h`): every CPU
  cycle's bus access (address, value, read/write), the data bus left behind and
  whether the cycle completed an instruction; every DMA cycle's own access; and
  PC, A, X, Y, S and P at every instruction start;
- `mem`: CPU RAM, CIRAM, OAM, palette RAM (6 bits), CHR RAM, the picture as
  color indices, and the cycle count;
- the CPU registers and the interrupts the next opcode fetch will take;
- `hw`: the hardware internals, compared only between runs of the same
  implementation (native against `--interp-only`), since TriCNES's internals
  are organized differently.

Internal state is never compared across implementations: two correct machines
need not agree on how they represent work in progress, only on what they do.

When a frame differs, `--trace-frame N --trace-out FILE` prints its trace in
both runs and a text diff shows the cycle; `--mem-frame N --mem-out FILE`
dumps what `mem` covers and `--state-frame N --state-out FILE` the `hw`
fields by name.

```bash
python tools/cyc/cyc_verify.py --exe build/Release/AccuracyCoinRecomp.exe \
    --oracle build/Release/cyc_oracle.exe --rom AccuracyCoin.nes --acccoin \
    --align 0 1 2 3 --interp --out verify/
```

Check AccuracyCoin and the stress ROM at all four CPU/PPU alignments: several
PPU paths depend on the alignment, and a clocking mistake can be invisible at
one of them.

### Oracle corrections

The oracle is a reference, not the definition of correct. Where TriCNES departs
from documented hardware behavior, the oracle is corrected (comments
`ORACLE FIX` and `PORT FIX` in `tric_core.cpp`; `CYC_ORACLE_UNFIXED=RDY,BUS,BACKDROP,IRQ`
restores the original rules) and NESRecomp implements the documented behavior.

**RDY (DMA halts).** TriCNES lets a DMA take a CPU cycle whenever its
`CPU_Read` flag is set, which stays set during the stack writes of PHA, PHP
and interrupt entry, and its halted cycles re-read an `addressBus` variable
that on many cycles still holds the previous address. The 6502 datasheet
describes RDY as ignored on write cycles, with the halted CPU holding the
address it is fetching. The oracle probes the CPU's next access on a copy of
its state and applies that rule. The difference is visible: AccuracyCoin
executes `STA $4014` across `$3FFE-$4000`, and a DMA halting the read of
`$4000` made TriCNES re-read `$3FFF`, a `$2007` mirror that advances the PPU
address. `CYC_ORACLE_ADDRESS_REPORT=1` lists where TriCNES's `addressBus`
lagged.

**Data bus after `$4003`/`$4007`/`$400B` writes.** TriCNES computes the
timer's high bits with `Input &= 0x7` before `dataBus = Input`, leaving only
bits 0-2 on the bus. The CPU drives the whole byte for a write; the difference
shows up as open bus on the next read of an undriven address. Found by
AccuracyCoin itself once the hardware was no longer TriCNES's.

**Odd-frame scanline 0 (`BACKDROP`).** On odd frames with rendering on,
TriCNES shifts scanline 0 one pixel left and paints x=255 from `PaletteRAM[0]`.
What an odd frame skips is the last tick of the *pre-render* scanline: the PPU
"jumps directly from (339, 261) to (0, 0)" (nesdev wiki, PPU frame timing), and
NES_MiSTer's `ppu.sv` skips "the *last* cycle of odd frames", armed on the
pre-render line. Scanline 0 still runs its own dots and emits 256 pixels, and
dropping its dot 0 does not displace the color pipeline either, because that
dot only shifts the pipeline and never computes a pixel. Mesen agrees: SMB3's
title screen, whose scanline 0 is not a flat color, differed from Mesen in 48
pixels of that row on every other frame until the shift was removed, and now
matches. (An earlier correction here kept the shift and only fixed the color
of the x=255 pixel — TriCNES used the stored 8-bit palette byte without the
greyscale mask, found by the stress ROM; with no shift that pixel no longer
exists.)

**IRQ line.** The 2A03's IRQ output is the frame interrupt flag (unless
inhibited) OR the DMC interrupt flag (nesdev wiki, APU: reading `$4015` clears
the frame flag "but not the DMC interrupt flag"; NES_MiSTer's `apu.sv` has
`IRQ = frame_irq || DmcIrq`). TriCNES keeps a single level detector that both
sources set and every acknowledgment clears, so a `$4010`/`$4015` write dipped
the line for one sample under a pending frame IRQ, and a `$4015` read or an
inhibiting `$4017` write dropped a pending DMC IRQ from the line while `$4015`
bit 7 still read 1. The visible difference: an IRQ handler that reads `$4015`
and returns without acknowledging the DMC is re-entered on hardware and was not
on TriCNES. The frame interrupt keeps its measured timing (flag at 29828, IRQ
from 29829). AccuracyCoin does not observe the difference (its trace is
identical either way); the stress ROM does, from its first frame.

**OAM2 address overflow (port fix).** When rendering is disabled mid sprite
evaluation, TriCNES rounds the secondary OAM address up to a multiple of 4
without wrapping; from `$1D-$1F` it indexes past the 32-byte table (upstream
throws). The counter now overflows as `IncrementOAM2Address()` does.

**The cartridge (port addition).** TriCNES's port kept only an NROM cartridge.
`tric_prelude.inc` now implements the same mappers as `hw_mapper.c`,
independently of it, and TriCNES calls them where a cartridge sees the
connector: PRG and work RAM through `FetchCPU`/`StoreCPU`, CHR through
`FetchPatternAddress`, CIRAM A10 through `Connector_CheckCIRAM`, and A12 once per
PPU clock at the end of `_EmulatePPU()`. The mapper's `/IRQ` is ORed into
`IRQLine`. The bank arithmetic comes from the same nesdev pages as the runtime's,
so the part this checks independently is the integration — above all when the
PPU drives each address relative to when the CPU samples `/IRQ`.

### Results

NESRecomp's machine (`native`, and `--interp-only`) against the corrected
oracle, compared on every frame:

| Program | Mapper | Alignment | Frames | Native | Tests | vs oracle |
|---------|--------|-----------|--------|--------|-------|-----------|
| AccuracyCoin | NROM | 0 | 4,324 | 100.0% | 144/144 | native and interp identical |
| AccuracyCoin | NROM | 1 | 4,227 | 100.0% | 143/144 | native and interp identical |
| AccuracyCoin | NROM | 2 | 4,221 | 100.0% | 141/144 | native and interp identical |
| AccuracyCoin | NROM | 3 | 4,260 | 100.0% | 143/144 | native and interp identical |
| Stress ROM | NROM | 0-3 | 12,000 each | 100.0% | — | native and interp identical |
| Mapperless (Demo) | NROM | 0-3 | 20,000 each | 56.7% | — | native and interp identical |
| Super Mario Bros. 3 | MMC3 | 0-3 | 3,000 each | 100.0% | — | native and interp identical |
| Mega Man 2 | MMC1 | 0-3 | 3,600 each | 100.0% | — | native and interp identical |
| Mega Man | UxROM | 0-3 | 3,600 each | 100.0% | — | native and interp identical |
| Donkey Kong Original Edition | CNROM | 0-3 | 3,600 each | 100.0% | — | native and interp identical |

At alignments 1-3 the oracle itself fails the same AccuracyCoin tests ($2002
flag timing, OAM corruption, frozen OAM2 increment). Trace agreement at these
alignments establishes parity with the oracle, not correctness on hardware;
the shared failures remain accuracy limitations.

The SMB3 run is a scripted playthrough (`--input`, 3,000 frames: title screen,
world 1 map, into 1-1, play, death, back to the map), so it covers the map and
level engines, the status bar split and the CHR bank switches that animate
tiles — 20,300 instructions across 17 of the ROM's 32 banks. Native and
`--interp-only` are identical at every alignment, so the banked code generation
and its dispatch agree with the interpreter exactly, and both agree with the
oracle's independently wired cartridge on every bus access and pixel.

The other three banked runs follow the same pattern: an `--input` schedule
from power-on through the title screens into a stage, then a minute of
running, jumping and shooting, one `--miss-log` seeding pass, and the check at
all four alignments. Mega Man 2 (MMC1's serial register, 256KB PRG) compiles
7,474 instructions across 6 of its 32 banks from 10 reachable statically;
Mega Man (UxROM) 8,195 across 6 of 16; Donkey Kong Original Edition (CNROM)
6,844 with no seeding, and switches its CHR bank between the title screen and
play. AxROM and GxROM have not been run with a game.

### Against NES_MiSTer's APU

TriCNES has no audio, so the tone generators have a second reference:
NES_MiSTer's `rtl/apu.sv` (SystemVerilog), compiled with Verilator into
`cyc_mister_apu`. NESRecomp's machine runs the program; after every CPU or DMA
cycle, the CPU side of that cycle (address, R/W, data, the bus value for reads)
drives MiSTer's APU and its DMA controller through one CPU cycle of their
master clock, as MiSTer's `nes.v` wires them. MiSTer's APU only listens. Each
signal is compared as a sequence of changes: same values in the same order,
with the cycle offset of each change tallied.

```bash
# MSYS2 UCRT64 shell with verilator and g++
tools/cyc/mister_apu/build.sh <NES_MiSTer checkout> build/mister_apu
build/mister_apu/cyc_mister_apu tones.nes --frames 720 --dmc-timer 1024
```

The DMC timer and noise timer run freely from power-on, where the console
leaves them at no particular phase; TriCNES and MiSTer start them at different
points. `--dmc-timer`/`--noise-back`/`--noise-timer` start NESRecomp's where
MiSTer's are, so their outputs can be compared directly.

On `tools/cyc/make_apu_tone_rom.py`'s program (each channel, the DMC, an
envelope, a length counter, a sweep):

| Signal | Changes | Offset (MiSTer − NESRecomp) | Mismatches |
|--------|---------|------------------------------|------------|
| Pulse 1 | 6,278 | −1 cycle, every change | 0 |
| Pulse 2 | 1,760 | −1 cycle (one change at 0) | 0 |
| Triangle | 6,606 | −1 cycle, every change | 0 |
| Noise | 14,028 | 0, every change | 0 |
| DMC output level | 33,105 | 0, every change | 0 |
| DMC DMA reads | 8,276 | 0, every read | 0 |

(`--dmc-timer 1024 --noise-back 1023 --noise-timer 1`.) Mixed through the
same mixer at 48 kHz, the two recordings differ by 0.9% RMS, the one-cycle
edge offsets.

The one-cycle offsets are where each model registers its outputs. Three things
in NESRecomp's APU were changed to agree with MiSTer and nesdev: the pulse duty
waveforms now start where a `$4003`/`$4007` write leaves the sequencer; the
triangle holds its level at periods 0-1 instead of jumping to mid-scale; and the
DMC output unit keeps its own "buffer holds a byte" flag, so a sample's first
output cycle is silent and a non-looping sample's last byte plays. None of
these is visible to the CPU.

The CPU-visible parts, which NESRecomp takes from TriCNES's measurements, also
agree with MiSTer's independent model:

| Program | `$4015` reads | Sprite DMA | DMC DMA reads | DMC output level | IRQ output |
|---------|---------------|------------|---------------|------------------|------------|
| Stress ROM, 4,000 frames | 385,677, none differ | 55,232 changes, all on the same cycle | 551,434, all on the same cycle | 63 changes, same cycle | 33,600 changes, none unmatched; MiSTer 0-2 cycles earlier |
| AccuracyCoin | 2,566, none differ | 990, all on the same cycle | 16,788 on the same cycle; the rest in the DMA abort tests (below) | 13,992 changes, none differ | 217 changes, none unmatched; MiSTer 0-2 cycles earlier |

MiSTer's IRQ output rises up to two cycles before NESRecomp's, which follows
the frame IRQ timing AccuracyCoin measures ("Frame Counter IRQ" N and O) at the
CPU's sample point. Before the IRQ line correction (see
[Oracle corrections](#oracle-corrections)) AccuracyCoin also showed 1,418
unmatched one-cycle dips.

Where the DMC still differs, NESRecomp follows the hardware references and
MiSTer does not model the case:

- **DMA aborts.** In AccuracyCoin's "Explicit DMA Abort" and "Implicit DMA
  Abort" tests (cycles 13.41M-13.44M of the run above) NESRecomp performs the
  DMA that a `$4015` write aborts as it starts, and the one-cycle DMA of an
  implicit stop (nesdev wiki, DMA: "the DMA starts, but is aborted after a
  single cycle"); MiSTer takes neither, so its later reads pair with
  NESRecomp's one to four bytes late. Both tests compare the DMA's duration
  with answer keys measured on consoles, and NESRecomp passes them.
- **Register conflicts.** A DMC DMA reading `$xx15` while the CPU holds
  `$4000-$401F` makes the 2A03 "read $4015 and ignore the DMA value on the
  external data bus" (nesdev wiki, DMA), so the sample buffer gets `$4015`
  (bit 5 left from the internal bus) while the CPU sees the ROM byte on open
  bus afterwards. NES_MiSTer's `nes.v` gives the DMC the external bus instead.
  `cyc_mister_apu` wires the harness the nesdev way unless `--nes-v-conflicts`
  is given; with it, AccuracyCoin's DMC output level differs in 103 changes,
  all following DMA reads of `$EFD5`, `$EFF5` and `$FFD5` in the "DMC DMA Bus
  Conflicts" and "APU Register Activation" tests. (The harness has no
  controllers, so a DMA read hitting `$4016`/`$4017` gives MiSTer's DMC the
  value NESRecomp's controller port produced.)

### Against Mesen's picture

TriCNES is the oracle for the bus and the picture, but it shares this project's
reading of the hardware research. Mesen is a third, unrelated implementation.
`tools/cyc/mesen_shot` builds MesenCE's core (no .NET UI) into a headless
program that runs a ROM and writes the PPU's own output buffer — one 9-bit
color index per pixel, the same thing `cyc_mem_dump` writes — so the two
pictures are compared without a palette in between:

```bash
tools/cyc/mesen_shot/build.ps1 <MesenCE checkout> build/mesen_shot
build/mesen_shot/mesen_shot rom.nes shots 3000 9000 16500   # <frame>.png + <frame>.idx
<game> rom.nes --frames 3001 --ram-init zeros --mem-frame 3000 --mem-out mem3000.txt
python tools/cyc/cmp_picture.py mem3000.txt shots/3000.idx
```

`cmp_picture.py` reports how many pixels differ and where. Mesen's test
settings power CPU RAM up as zeros, so compare with `--ram-init zeros`; the
default is the reference console's `$F0/$0F` pattern, and a program that reads
uninitialized RAM will legitimately differ between the two.

**Frame numbers.** Host frame N and Mesen frame N are the same picture — until
a program boots differently. SMB3 reaches the same state one frame later in
Mesen than in NESRecomp (and the TriCNES oracle, which agrees with NESRecomp from
frame 0), at every CPU/PPU alignment and before any input, so there host frame
N is Mesen frame N+1. Find the offset K by comparing a frame from before the
first button press against Mesen N and N+1, then give it to mesen_shot:

```bash
build/mesen_shot/mesen_shot smb3.nes shots --input into_level.txt --frame-offset 1 1501
SMB3Recomp smb3.nes --frames 1501 --input into_level.txt --ram-init zeros --mem-frame 1500 --mem-out m.txt
python tools/cyc/cmp_picture.py m.txt shots/1501.idx
```

A game whose interesting screens are past a title screen needs the same input
in both: `mesen_shot --input FILE` takes the host's `--input` schedule, and
`--frame-offset` makes each press reach the program on the same frame of its
own. Without the offset a program like SMB3 gets its presses a frame off, and
its game drifts apart in play.

**Mesen's opt-in hardware behaviors.** Mesen ships OAM row corruption, the
sprite evaluation bug, the `$2000`/`$2006` scroll glitches and the DMC sample
duplication glitch switched off. `--hw-options oamrow,spriteeval,...` or `all`
turns them on, but they do not make Mesen a better reference here: its own
source calls its OAM row corruption a worst case rather than
alignment-dependent, and with `all` Mesen scores 139/144 on AccuracyCoin against
141/144 at its defaults (failing Frozen OAM2 Increment, ALE + Read and Hybrid
Addresses there; NESRecomp passes 144). mesen_shot therefore uses Mesen's
defaults unless asked. `--hw-options noppureset` turns off the one default-on
behavior NESRecomp does not model, the PPU ignoring register writes during its
first frame (front-loader NES only); it does not explain SMB3's boot offset.

**What still differs.**

- *SMB3*, over 13 sampled frames of the playthrough: 10 identical, and 6, 1 and
  4 pixels in the other three, all on scanline 194 at x=8-15. The status bar's
  IRQ handler turns rendering off at scanline 193 dot 12 and back on at dot
  336 — during dots 321-336, where the PPU prefetches the first two tiles of
  scanline 194 — so x=8-15 come from a half-fetched tile, and which pixels they
  are depends on how many dots a `$2001` write takes to land. The difference is
  smaller than a CPU cycle (3 pixels), so it is not when the IRQ is recognized.
  NESRecomp agrees with the oracle at all four alignments; Mesen matches none
  of them. No measurement here settles it.
- *Mapperless (Demo)*, frame 9000: 69 pixels of one sprite column (x=134-141)
  on every third scanline from 70 to 154 — a sprite present in NESRecomp and
  absent in Mesen, or on a different palette row. The demo writes `$2001`
  constantly during sprite loading (dots 285-320), with the same value it
  already holds in that frame. Ruled out: `$2004` writes during rendering (both
  models drop the value and bump the address by 4), the frozen OAM2 increment
  (switching it off in NESRecomp changes nothing), and every Mesen hardware
  option. Frames 3000, 13500, 16500 and 20000 are identical. Open.

### Other checks

**Stress ROM.** `tests/stress/` + `tools/cyc/make_cyc_stress_rom.py`: every
opcode template under NMIs, IRQs, DMC and OAM DMAs, plus a per-pass PPU
register workout (8x16 sprites, mid-frame mask toggles, scrolling, `$2007`,
`$2003/$2004`) over pseudo-random pattern tables, and an interrupt workout on
the shared IRQ line: `$4015`/`$4010` writes polling right after CLI with a frame
IRQ possibly pending, a one-shot DMC sample whose IRQ the handler leaves
unacknowledged for up to three entries, `$4017` inhibiting frame IRQs with the
DMC IRQ pending, and `$4015` writes landing on every point of the DMC schedule.
AccuracyCoin alone has missed bugs the stress ROM caught and the reverse; check
both.

**Controller ports under a player's hands.** `--acccoin` runs the suite with
no buttons held, so it never exercises a button moving while a test reads the
port. `--spam PAGE ROW` plays one test over and over the way a player does:
walk to the page and row, then mash A while tapping Down and Up between that
row and the one below it. It reports every result the ROM writes, so the
oracle and a recompiled build can be compared on it:

```
AccuracyCoinRecomp AccuracyCoin.nes --spam 13 7 --spam-seed 1 --frames 4000
cyc_oracle         AccuracyCoin.nes --spam 13 7 --spam-seed 1 --frames 4000
```

Page 13 rows 7 and 8 are "Controller Strobing" and "Controller Clocking".
Both fail often here, and are *meant* to: they assert that nothing but A is
pressed, so a d-pad bit latched by the strobe shows up in the shift register
and breaks them (strobing error 2 or 3, clocking error 2 — clocking's test 1
only checks that reads past the eighth return 1, which no button can disturb).
`--spam-no-dpad` mashes A alone and passes every run. The oracle produces the
same results frame for frame.

**APU tones against the documentation.** `tools/cyc/check_apu_tones.py`
checks a `--wav-out` recording of the tone program: pulse and triangle pitch
from their period registers, broadband noise, DMC pitch from its rate, envelope
decay time, length counter duration, and a rising sweep.

**Helpers.** `cyc_helper_test` checks `cpu_adc`, `cpu_sbc`, `cpu_cmp`,
`cpu_bit`, the shifts and the status register for every input. An earlier
build found MSVC 19.44 `/O2` miscompiling an out-of-line shift helper; run it
after changing compilers.

## Open questions

- **`$2001` landing inside a tile prefetch, and the Mapperless sprite column.**
  The two picture differences from Mesen still open (see
  [Against Mesen's picture](#against-mesens-picture)). Both involve `$2001`
  writes on rendering-critical dots, NESRecomp agrees with the oracle on both,
  and AccuracyCoin has no test that decides either. A PPU co-simulation against
  NES_MiSTer's `rtl/ppu.sv`, in the pattern of `tools/cyc/mister_apu`, would be
  a fourth, HDL reference for them.
- **CPU revision.** The DMC follows the early RP2A03G, like TriCNES
  (AccuracyCoin "Implicit DMA Abort" reports behavior 2). RP2A03H and late
  RP2A03G CPUs fetch an unexpected extra byte when a sample is stopped
  implicitly on the APU cycle a reload DMA would schedule (nesdev wiki, DMA);
  AccuracyCoin accepts either and could check a switch for it.
- **SMB3 boots a frame later in Mesen** than in NESRecomp and TriCNES, at every
  alignment and without input. Not a picture question (the frames match once
  offset) and not Mesen's PPU warm-up; which one a console does is unmeasured
  here.

## Roadmap

1. **More mappers.** [Coverage and validation](MAPPERS.md) list the current boards; the
   layer they plug into (`hw_mapper.h`) is four tables and an IRQ line, and
   `cyc_codegen.c` needs only each new mapper's bank granularity and which
   slots its board fixes. MMC2/MMC4 and MMC5 are the interesting next ones —
   MMC2/MMC4 need a qualified PPU-read latch hook that switches after the
   triggering fetch; merely watching A12 is insufficient. MMC5 needs more
   than these tables express. NES 2.0 variants and four-screen memory also
   need explicit implementation and tests.
2. **Main runner integration**: save states and the launcher (input is now the
   `--input` schedule headless and the keyboard under SDL).

## Building

Game projects include `cyc.cmake` (see `tests/accuracycoin/CMakeLists.txt`);
link `NESRECOMP_CYC_LIBRARIES` as well as using its source and include lists
(the C runtime needs `libm` on Unix).
`nesrecomp_cyc_add_oracle(cyc_oracle)` adds the oracle. AccuracyCoin itself is
set up in `tests/accuracycoin/` (see its README for where the ROM comes from).
This directory also builds standalone:

```bash
cmake -S runner/cyc -B build/cyc && cmake --build build/cyc --config Release
build/cyc/Release/cyc_oracle  <rom.nes> ...   # TriCNES
build/cyc/Release/cyc_interp  <rom.nes> ...   # NESRecomp's machine, interpreter only, any NROM ROM
build/cyc/Release/cyc_helper_test
```

The stress test:

```bash
cd runner/cyc/tests/stress
python ../../../../tools/cyc/make_cyc_stress_rom.py cyc_stress.nes
NESRecomp cyc_stress.nes --game game.toml
cmake -S . -B build && cmake --build build --config Release
python ../../../../tools/cyc/cyc_verify.py --exe build/Release/cyc_stress.exe \
    --oracle build/Release/cyc_oracle.exe --rom cyc_stress.nes --frames 12000 --interp
```

ROM-free regressions for the verifier, ordinary-ROM startup and MMC1 banked
reads (run from the repository root after building the recompiler and the
standalone runtime):

```bash
python -m unittest discover -s tools/cyc
python tools/cyc/test_cyc_runtime.py --recompiler build/compiler/Release/NESRecomp.exe \
    --interp build/cyc/Release/cyc_interp.exe --oracle build/cyc/Release/cyc_oracle.exe \
    --out build/cyc-regressions
```

Use the corresponding executable paths without `Release/` and `.exe` for
single-configuration Unix builds. `cyc_verify.py` rejects failed launches,
missing or incomplete traces, and unfinished AccuracyCoin runs. A completed
AccuracyCoin run with failed tests can still match the oracle; `ALL MATCH`
describes trace equality, while the printed test scores describe accuracy.

## Limits

- The model targets NTSC; PAL/Dendy timing is not implemented. Mapper support
  covers the board configurations above, not every variant sharing an iNES
  mapper number: MMC1 outer PRG/WRAM banking and the original discrete
  mappers' bus conflicts are not modeled. The new discrete boards listed in
  [MAPPERS.md](MAPPERS.md) do model AND conflicts where specified.
  Battery-backed RAM is not persisted by this host.

- Mappers 0, 1, 2, 3, 4, 7 and 66. NROM, MMC1, UxROM, CNROM and MMC3 have
  each run a game against the oracle (see [Results](#results)); AxROM and
  GxROM are implemented from the nesdev descriptions on the same layer and in
  both the runtime and the oracle, but no game has been put through them yet —
  treat them as untested. The 15 additions have synthetic CPU/PPU contracts
  and native/interpreter/oracle parity, not commercial-game compatibility
  certification. Their board and header limits are listed in [MAPPERS.md](MAPPERS.md).
- Four-screen boards (iNES flag 6 bit 3), including mapper-206 Gauntlet, are
  rejected because cartridge nametable RAM is not implemented.
- A separate host from the main runner (`runner/src`).
- Code the program writes at run time runs on the interpreter, not as compiled
  code, and no amount of seeding changes that: the instructions are not in the
  ROM to compile. Correctness is unaffected — the interpreter comes from the
  same templates — but a program built around self-modifying code will not
  reach a high native share. Mapperless (Demo) is the worked example.
- Speed: a headless AccuracyCoin run takes about 9.6 s (~450 fps, ~7.5× real
  time) on a Ryzen 7 5700; 3,000 frames of its rendering menu take 11.8 s
  (~250 fps). SMB3's 3,000-frame playthrough takes 14.1 s (~213 fps, ~3.5×
  real time) against 14.6 s with `--interp-only`, another instance of the
  point below: banking and dispatch cost nothing measurable, and dots with
  rendering on take the general PPU path.

  That path, not CPU dispatch, is what costs: forcing every dot down
  `blank_dot()` (wrong picture, but frames still end) takes Mapperless (Demo)
  from 216 to 433 fps, so `general_dot()` is about half of the run time and 2×
  is the ceiling on optimising it. Compiled code against `--interp-only` is
  worth ~5% by comparison. Any work there has to keep AccuracyCoin at 144/144
  and both ROMs matching the oracle at all four alignments.
