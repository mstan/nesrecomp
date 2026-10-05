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

The cycle runtime supports 34 mapper IDs; see [board coverage and limits](MAPPERS.md).
Existing game checkouts can use the [cycle-project CMake integration](PROJECTS.md)
to opt into this backend and its cartridge hardware.
The [cartridge review packet](CARTRIDGE_REVIEW.md) links the draft PRs,
reproduction commands and remaining hardware-validation requirements.
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
| FDS RAM Adapter, drive, sound unit | `hw_fds.c`, `hw_fds_audio.c` | NESRecomp's own |
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

Code in RAM that no compiled view covers (see [Code in RAM](#code-in-ram)),
open bus execution, instructions that wrap past `$FFFF`, and addresses
discovery did not reach run on `cpu6502_interp.c`. It is the output
of the same template functions with the operand bytes read at run time instead
of folded:

```bash
NESRecomp --emit-cycle-interpreter runner/cyc/cpu6502_interp.c
```

Recompiled and interpreted execution therefore cannot disagree about what an
instruction does; `--interp-only` runs check the folding and control flow.

### Banked mappers

Folding a ROM byte into a constant assumes the byte at that CPU address is
known. On a banked cartridge it is not: `$8000-$FFFF` is eight 4KB slots whose
contents the game changes at run time, so the same address holds different
instructions at different moments. 4KB is the finest granularity any supported
mapper switches, which makes the unit of compilation a **(bank, slot) pair**
rather than an address:

- one C function per 1KB of a (bank, slot) pair, one translation unit per PRG
  bank (`<prefix>_cyc_bNN.c`), and a dispatch table in `<prefix>_cyc.c`;
- entry goes through that table, which asks the cartridge which bank is at
  `cpu.pc` right now (`hw_prg_bank4()`, one indexed load into `hw_cart.prg_off`)
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
`4k:BB:AAAA count`. Old `BB:AAAA` seeds retain their 8 KiB physical
bank meaning and are converted using the address half. A line without a bank still works and means the bank the
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

### Code in RAM

A Famicom Disk System game is code in RAM: the BIOS loads its files from the
disk into PRG RAM (`$6000-$DFFF`) with ordinary CPU stores, often several
different files at the same address over a session. The disk image is a build
input, though, so the recompiler knows every byte those files can put there.
It compiles code in RAM from **images** of RAM:

- every PRG file of every side of the disk at its load address, hidden files
  and files that overlap each other included (a file's part below `$0800` is a
  CPU RAM image);
- every snapshot a run captured (below), for code no file holds as it ran:
  routines the program copies or generates, and disk code whose compiled view
  was not valid when it ran.

An image is discovered like ROM, from the game's vectors (`$DFF6/$DFF8/$DFFA`
NMI as `$0100` selects, `$DFFC` reset, `$DFFE` IRQ), JMP/JSR targets (a target
that leaves one image seeds every image covering it, since any of them may be
resident then; one in the BIOS seeds the BIOS), and captured instructions. It
is then cut into **views**: the instructions that local control flow (fall-
through, branches, JMP, JSR returns, not calls) connects inside one 1KB chunk.
A view folds **only its own instructions' bytes** — its *dependency* — and
nothing else: dummy reads of the next byte, pointer reads and all data are read
at run time. Views equal in instructions and bytes (the same code in two files)
merge. Each compiles to one C function in `<prefix>_cyc_rNN.c`, with its
dependency as `(address, length)` runs and the bytes they must hold
(`CycRamView`, `cyc_recomp.h`); `<prefix>_cyc_views.txt` names the image each
came from.

At run time (`cyc_ramview.c`) a view is entered only while RAM holds its
dependency:

- dispatch at a RAM address looks up the views with an instruction starting
  there; a view already known to be valid is entered at once, an unknown one is
  compared with RAM first (matching makes it valid, differing makes it
  invalid), and if none holds, the interpreter runs the instruction — correct
  by construction;
- a **write watch** marks every RAM byte some view folds (`hw_code_watch`). All
  RAM stores go through `hw_bus_write` and `fds_cpu_write` (the DMAs only
  read), and a store that changes a watched byte demotes every view folding it
  to unknown. A store to a byte no view folds — a variable next to code — costs
  one table load and changes nothing, so code and data can share a page;
- **a store that can reach a byte some view folds ends the block** if it
  invalidated a view: compiled RAM code tests `cyc_ram_code_dirty` after every
  store whose address range can reach the program's folded bytes (the
  compiler knows them all; stores to the stack page or to variables no view
  folds carry no test) and returns to the scheduler, which validates the next
  instruction afresh. This is the RAM form of "a write that can reach a bank
  register ends the block". An instruction's operands are fetched before its
  stores (a JSR pushes between its operand fetches, so the stack page is never
  compiled), and interrupts and DMA never write RAM, so no folded instruction
  runs after its bytes change.

Code that rewrites itself is compiled from the evidence a run records. The
capture keeps the values stores wrote over folded bytes that no compiled view
holds there. A value that a disk file holds there is a file loading over
another, which content-keyed views already handle. Any other value is the
program rewriting its code: an instruction whose **operand** bytes are rewritten
(or that ran with operands no file has there) is compiled to read them at run
time — the interpreter's template with its address and opcode constant, which
returns to the scheduler when done — so its view never depends on them; an
instruction whose **opcode** is rewritten is compiled alone, one view per
content, so the code around it keeps a valid view whichever instruction it
holds.

**The capture loop** (psxrecomp's overlay model). The interpreter records the
RAM code it ran with no valid view: each instruction as it ran (address and
bytes), and per 1KB chunk a snapshot taken when such code ran, one per distinct
code layout; the watch adds the rewritten bytes. `--capture-log FILE` writes it
(merged across runs), and `[game] cycle_capture_file` (or
`NESRecomp --cycle-capture-file`, `CAPTURE_FILE`/`NESRECOMP_CYCLE_CAPTURES` in
a [project build](PROJECTS.md)) feeds it back as a build input, like the seed
file:

```bash
FdsGame game.fds --frames 3000 --input route.txt --capture-log captures.txt
NESRecomp game.fds --game game.toml --cycle-capture-file captures.txt
```

A captured instruction seeds every disk file that holds it as it ran, and each
snapshot is compiled from what ran in it only (its data is not decoded), so a
capture never adds guesses. CPU RAM `$0000-$07FF` (not the stack page) works
the same way for any board; PRG RAM views need the FDS's unbanked RAM.

Measured (alignment 0; each row also native = `--interp-only` = `cyc_interp`
on `--hash-out` at all four alignments; "after" = compiled with the capture of
one run of the same route; the 7 cycles left are the power-on reset):

| Program, route | Frames | Native before (disk files only) | Native after one capture | RAM views |
|----------------|--------|--------------------------------|--------------------------|-----------|
| SMB2J, boot to title | 800 | 97.5% | 100.0% | 290 |
| Otocky, side B at 1258 | 3000 | 100.0% | 100.0% (no capture needed) | 207 |
| Nazo no Murasame-jou, title, menu, name entry | 6000 | 87.2% | 100.0% | 263 |
| Esper Dream, title, side B, name entry | 7200 | 99.1% | 100.0% | 727 |
| Dead Zone, title, menu, name entry | 6000 | 100.0% (716 instructions interpreted) | 100.0% | 479 |
| AccuracyCoin (CPU RAM) | 4,324 | 100.0% (29,273 cycles interpreted) | 100.0% (2,129) | 280 |

Before RAM views the FDS rows were 10.3% (SMB2J) to 68.1% of their cycles on
the interpreter. AccuracyCoin with its captured RAM code still passes 144/144
and matches the oracle on every frame at all four alignments. Speed is the
hardware model's either way: SMB2J 3,000 frames went from 315 to 326 fps native
(311 to 314 `--interp-only`), Otocky from 343 to 364 (334 to 337). The write
watch costs nothing measurable. `tools/cyc/test_cyc_ramviews.py` (CTest
`cyc_ramview_test`) checks a synthetic disk whose code rewrites itself every
way above (operands, the next opcode, overlays over each other, a copy over its
own tail, an NMI patching the loop it interrupts, stores through every
addressing mode, a variable beside code), before and after a capture.

### Reading a coverage number

A run says where its cycles went:

```
mode=native frames=800 cycles=23822067 native_cycles=23223334 (97.5%)
  native: ROM 21372061 (89.7%)  RAM views 1851273 (7.8%)
  interpreted: ROM 0 (0.0%, add to cycle_seed_file)  RAM 0 (0.0%, CPU RAM code no view covers: --capture-log)  PRG RAM 598726 (2.5%, disk code no view covers: --capture-log)
  ram views: 24 compiled (24 usable here), 2541 entries, 14 validated, 0 rejected, 0 invalidated by stores, 0 block exits after code stores, 189922 RAM instructions interpreted
```

(SMB2J's boot, compiled from its disk alone.) The interpreted cycles are acted
on by where they started. ROM cycles are entry points discovery did not reach:
`--miss-log` lists them and seeding them compiles them. RAM cycles (CPU RAM,
and the FDS's PRG RAM) are code that no compiled view held as it ran: disk code
reached only through RTS or JMP-indirect dispatch, code the program copied or
generated, code whose view a store had just made stale. `--capture-log` records
it and compiling with that file compiles it (see [Code in RAM](#code-in-ram)),
so both numbers should reach 0: the same SMB2J boot after one capture run is
100.0% native (7 cycles are the power-on reset sequence), 10.3% of it RAM views.
The `ram views` line counts entries into views, views compared with RAM
(validated or rejected), views a store invalidated, and blocks that returned
after such a store; the event ring has each of these (`view.*`,
`ram.interp`, and a per-frame `view.frame`).

`--miss-log` still lists the RAM addresses that started an instruction on the
interpreter, as comments, with the number of distinct opcode bytes seen at
each: 1 means only operands change there, more means the byte is an opcode on
one pass and an operand on another. Mapperless (Demo), an NROM demo whose
effects run from patched zero-page routines, is 100% of its ROM cycles native
and 56.7% overall without a capture file — over 45,000 frames, 364 of its 870
RAM instruction addresses saw more than one opcode.

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
Everything a mapper can do is expressed as four things — where each 4KB PRG
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

### Famicom Disk System (`hw_fds.c`, `hw_fds_audio.c`, `cyc_ring.c`)

The RAM Adapter is board 20 ([MAPPERS.md](MAPPERS.md#fds-ram-adapter-mapper-20)):
the recompiler compiles the BIOS (`disksys.rom`) as the program's fixed ROM at
$E000-$FFFF, and the host loads it with a disk image instead of an iNES file:

```bash
NESRecomp game.fds --fds-bios bios/disksys.rom --cycle-accurate   # or game.toml [fds] image/bios
FdsGame game.fds --fds-bios bios/disksys.rom                        # defaults: game.toml's media
FdsGame --fds-boot-disk none --frames 1200 --screenshot nodisk.png  # the BIOS's insert-disk screen
FdsGame otocky.fds --fds-event 1199:eject --fds-event 1258:insert=B --frames 3000
```

Only the BIOS its `bios/disksys.toml` (or, for a compiled program, the one it
was compiled from) identifies is accepted: 8192 bytes, CRC32 5E607DCF. The
host takes the first of these that exists (`cyc_fds_bios.h`): `--fds-bios`,
the window's config.ini `[FDS] Bios`, `game.toml` `[fds] bios`,
`bios/disksys.rom` beside the image, `bios/disksys.rom` in the current
directory. recomp-ui's launcher runs the same lookup for the selected disk:
when it finds nothing usable it says "FDS BIOS (disksys.rom) required" with a
Select BIOS... button, checks the chosen file's identity (a wrong one is
refused with its size and CRC), saves it as `[FDS] Bios`, and keeps PLAY
disabled until a BIOS is present; Settings, SYSTEM changes or clears it. A
cartridge never sees any of it. A windowed start without the launcher shows a
missing BIOS in a message box; headless runs print it. The
BIOS passes arguments inline after its JSRs (`$E844`, `$E3E7`); discovery
knows its routines that do (`FDS_BIOS_INLINE_JSR` in `cyc_codegen.c`, and
`[game] cycle_inline_jsr` for any program), so the BIOS runs 100% native with
no seed file. Code the BIOS loads from the disk into PRG RAM runs as compiled
RAM views ([Code in RAM](#code-in-ram)); what no view covers runs on the
interpreter, counted as "PRG RAM" and recorded by `--capture-log`. Disk events (`--fds-event`, or `F DISK_EJECT`,
`F DISK_SELECT SIDE`, `F DISK_INSERT [SIDE]` lines in an `--input` file) apply
before frame F runs, where nesref applies its script's disk commands. The
player turns the disk with the bindable Disk action ([The window](#the-window));
see also [Disk saves and sides](#disk-saves-and-sides).

The drive and the board record every event from power-on into an always-on
ring (`cyc_ring.h`): register accesses (a polling loop folds into one event
with a repeat count), IRQ edges and acknowledges, each clocked byte with its
position, motor, rewind, ready and end-of-side transitions, CRC checks and side
changes. `--ring-out FILE [--ring-frames A:B]` writes it at exit, and
`NESRECOMP_CYC_RING_DUMP=FILE` does for any host; nothing needs arming.

Against nesref (Mesen), `tools/cyc/fds_oracle_gates.py` compares CPU RAM,
PRG RAM, nametables, CHR RAM and the picture frame by frame; `--frame-log FILE
--frame-log-at mesen` gives it cyc's memories at the point where Mesen ends a
frame (scanline 240 dot 0, one scanline before cyc's VBlank frame end), so a
frame that ends mid-loop is compared at the same instant. Measured (interpreter
and native builds, alignment 0, `--ram-init zeros`):

| Run | Frames | Identical frames | Notes |
|-----|--------|------------------|-------|
| BIOS, no disk | 1-1200 | CPU RAM, PRG RAM, CHR RAM 1200; nametables 1195; picture 1194 | "PLEASE SET DISK CARD" from 155 |
| SMB2J, disk in at power-on | 1-800 | picture 794, PRG RAM 799, nametables 793, CHR RAM 797, CPU RAM 731 | title first at frame 751 in both |
| Otocky, eject at 1199, side B at 1258 | every 10th, 1180-3000 | picture, PRG RAM, nametables, CHR RAM 183/183; CPU RAM 182 | OTOCKY SELECT at 1800 |

The no-disk run also matches on every one of frames 1-1200 except the
pictures of frames 1-6 and the nametables of 1-5: CIRAM and palette RAM
power-on contents, which the hardware leaves undefined and Mesen zeroes. The
SMB2J differences past frame 6 are one or two bytes each, caught mid-update or
left on the stack by an NMI: at matched frame points the two CPU cycle counts
differ by -5 to +3 cycles (+2 in 518 of 770 frames), so the snapshot or the
NMI lands one instruction apart (in the title's 6-cycle idle loop the pushed
return address alternates between $605B and $605D). Drive byte clocks keep
that same +2 offset from Mesen's counter. In the SMB2J boot the BIOS is 89.7% of
the CPU cycles and disk-loaded code the other 10.3%, all native once compiled
with one capture ([Code in RAM](#code-in-ram)); the gate counts above are the
same with and without RAM views.

#### Disk saves and sides

FDS games save by writing their disk. The drive writes into the in-memory
disk as the BIOS clocks bytes out; the host keeps the result in a disk save
file and never writes the image:

```bash
FdsGame game.fds --save-file game.fdssave          # headless: only with --save-file
FdsGame game.fds                                   # windowed: <exe dir>/saves/<image stem>.fdssave
FdsGame game.fds --no-save                         # windowed, nothing kept
FdsGame game.fds --fds-import-ips game.ips         # start from a Mesen/nesref save
FdsGame game.fds --save-file s.fdssave --fds-export-ips game.ips   # also write Mesen's form
```

The save (`common/nes_fds_save.h`) holds the whole drive stream of every side
that differs from what the loader builds from the image, byte for byte as the
BIOS wrote it (gaps, $80 marks, blocks, CRCs), plus the disk's identity (CRC-32
and FNV-1a of the side data, so a headered image and its raw twin share a
save). Whole sides rather than a diff: a diff of the rebuilt stream is only
valid against the loader and options that built it, while a whole side
reloads unchanged under any of them. A save that is damaged, from a newer
version or for another disk is refused at start (exit 2) and left as it is.
It is rewritten atomically (a flushed temporary file replaces it) once the
drive has been idle for 60 frames after a change, which is right after the
BIOS finishes a save; at the latest 3600 frames after the first unsaved
change; when the disk is ejected; and at exit. Frames, not wall time, so a
run saves identically at any speed.

Mesen saves an IPS patch of the image file instead (`<stem>.ips`, gaps and CRCs
dropped by its `RebuildFdsFile`). `--fds-export-ips` writes that form with
the same algorithm; for Nazo no Murasame-jou's name save it is byte for byte
the `.ips` nesref writes for the same input, and booting either machine on its
own save reads the same data back (`tools/cyc/fds_save_compare.py`: PRG RAM,
nametables, CHR RAM and picture identical on the compared frames). Writes land
under the head, not two bytes behind as in Mesen 0.9.9 ([MAPPERS.md](MAPPERS.md#fds-ram-adapter-mapper-20)).

The player's Disk action (`cyc_disk_action.h`, [The window](#the-window))
shows the drive and turns the disk; a dev build (`NESRECOMP_DEV_UI`) also has
the drive bar under the picture (F4 hides it: the side in the drive or EMPTY
with the side F1 will insert, the drive lamp and MOTOR while it turns, and the
save's state: SAVE LOADED, UNSAVED, SAVED F<frame>), F1 (eject / insert) and
F3 (next side while the drive is empty). None of it is part of the emulated
frame, so screenshots and every comparison see only the machine's picture.
Scripts use `--fds-event` or `DISK_` lines (`F DISK_ACTION` presses the Disk
action). Automatic side changes are the optional
[HLE tier](#the-hle-tier-hw_fds_hlec-commonnes_fds_hleh).

Ring events: `fds.wrun` (a write run: side, first position, bytes stored and
changed), `fds.wblock` (a block written the BIOS way: code, mark position,
length), `fds.save` (reason idle/eject/exit/timeout, sides, bytes) and
`fds.load`. `tools/cyc/test_cyc_fds_saves.py` (CTest `cyc_fds_saves`) checks it all
across processes on a synthetic saving program.

#### The sound unit (`hw_fds_audio.c`)

The RAM Adapter's wavetable channel: a 64-step, 6-bit wavetable ($4040-$407F,
writable and held while $4089.7 is set), a 12-bit pitch ($4082/$4083), the
volume envelope ($4080) and the modulator: a 64-step table of 3-bit steps
($4088, two steps a write, only while $4087.7 stops the unit), a 7-bit
signed counter ($4085) that wraps, its own envelope ($4084) and pitch
($4086/$4087), and the pitch adjustment Mesen took from the nesdev wiki
(the "strange" rounding, the -64..191 wrap and the round-to-nearest of the
second product). $408A sets both envelopes' speed (8 x (speed + 1) x $408A
cycles a tick, $E8 at power-on), $4089.0-1 the master volume (36/24/17/14 of
36), $4090/$4092 read the gains back and $4040-$407F the sample at the wave
position while writes are disabled. It runs on every CPU cycle, since the
CPU can read all of that back, whether or not audio is being output.

The reference is nesref's core, libretro/Mesen master 0102910: its
`FdsAudio.h` differs from the 0.9.9 release in four places, each visible in
nesref (wave read-back at the position, the mod output applying while the
unit is stopped, $4084/$4085 recomputing it, $E8 at power-on). `--fds-profile
mesen2` follows Mesen2 b9fa69d (pitch writes recompute the mod output, the
wave runs on and the output holds while writes are enabled); `hardware` is
Mesen2's synthesis with the nesdev output stage. `hw_fds_audio.c` has the
table.

Output: Mesen mixes the 6-bit output unfiltered at 20 per step against
477600 / (8128 / n + 100) for the pulses; the runtime's mix is the same curve
at 1/5000 of that scale, so the FDS is 20/5000 per step, in phase with the
2A03 (full volume = 1.69 full pulses). The `hardware` profile uses the
nesdev level (2.4 pulses) and its ~2 kHz one-pole low-pass instead; that
column is a judgment call, not oracle-verified. That low-pass is the RAM
Adapter's own: nesdev's FDS audio page gives it for the FDS signal ("This
output signal is affected by a filter"), lidnariq derived it (1.36-1.75 kHz)
from the adapter board's resistors and capacitors (nesdev forum t=10233),
rainwarrior fitted ~2 kHz to Famicom + FDS recordings, and NSFPlay applies it
(2000 Hz) to the FDS channel alone. So it filters the FDS sound before the
adapter mixes it with the 2A03's and is not part of the console output stage;
the FDS defaults to the `famicom` output stage (APU section below) in every
profile. Neither Mesen models the
newer nesdev findings ($4083.7 speeding the envelopes up, $4087.6, 16-cycle
wave/mod ticks, $4091), and no profile does.

The ring records envelope gain changes (`fds.env`, folded per frame) and, per
frame, the wave and mod table steps (`fds.audio`); a step per event would be
thousands a frame and evict the drive's events. `--frame-log` records the
sound unit's state (`cyc_fds_audio_state`, Mesen's field order) with each
frame.

Checks:

- `cyc_fds_board_test`: envelope timing to the cycle, the gain limits, the
  master speeds, $4083 bits 6 and 7, read-back, master volume, the mod table
  and counter wrap, and the pitch adjustment for values nesref's savestates
  held and for each branch of the formula; the wave in all three profiles.
- `cyc_fds_audio_test` (`tools/cyc/test_cyc_fds_audio.py`): synthetic FDS
  programs, compiled from their disks, run native, `--interp-only` and on
  `cyc_interp` at all four alignments with identical `--hash-out`; every
  sound register read and every frame's sound state must match
  `tools/cyc/fds_audio_model.py`, a model of Mesen's FdsAudio written apart
  from `hw_fds_audio.c`, and the ring summaries must count what the model
  counts; the PCM must hold the pulse and FDS tones at their pitch with
  Mesen's level ratio (and 2.4x through the low-pass in `hardware`).
- `tools/cyc/fds_audio_gates.py` (owner files, not CI): a route on a cyc build
  and on nesref, compared by model replay, by the FdsAudio snapshot in
  Mesen's savestates frame by frame, and in PCM with the A/B analyzer
  (`tools/nes_audio_ab.py`), also with the output stage cyc used (`--console`)
  applied to nesref's PCM so synthesis and output stage separate.

#### The HLE tier (`hw_fds_hle.c`, `common/nes_fds_hle.h`)

LLE (the real BIOS against the modelled drive) is the reference; the HLE tier
is opt-in conveniences on top of it, in the shape of psxrecomp's
`psx_bios_hle_plan()`: one pure function, `nes_fds_hle_plan()`, decides every
axis from what was asked for and what the BIOS and disk support, every call
site uses its answer, and an axis a BIOS or disk cannot support is refused
with the reason (never forced). Off by default. With every axis off the
machine runs exactly as without the tier: `--hash-out` of all owner routes at
all four alignments is byte-identical to the build before it.

```bash
FdsGame game.fds --fds-hle auto-swap,fast-load   # or all / off / no-auto-swap / no-fast-load
NESRECOMP_FDS_HLE=fast-load FdsGame game.fds     # the environment
# game.toml: [fds] hle = "auto-swap,fast-load"   # the program's default
FdsGame game.fds --fds-hle fast-load --realtime --frames 2000   # paced like the window; reports load time
```

Precedence per axis: live toggle (a dev build's F6 auto swap, F7 fast load,
F6 + n for axis n) > `--fds-hle` > `NESRECOMP_FDS_HLE` > the player's saved
setting (the runtime menu's Disk Drive rows, config.ini `[FDS]`; windows only)
> `[fds] hle` > off. The host prints the plan (`fds hle: auto-swap on (cli),
fast-load REFUSED: ...`) whenever any source asked for anything; a dev build's
drive bar shows it on its second line (`HLE SWAP FAST`, `LOADING`, `AUTO SWAP
TO DISK 1 SIDE B`). Hosts list the axes from `nes_fds_hle_axes()` (word,
settings key, label, help, field offsets), so a new axis is one row there.

**Auto swap** needs the BIOS's disk-ID check anchor: the routine every
ID-checking BIOS call runs ($E445 in disksys.rom; LoadFiles, WriteFile,
AppendFile, CheckFileCount, AdjustFileCount and SetFileCount all reach it with
the pointer to the caller's 10-byte disk ID at $00). It is built in for
5E607DCF (and checked against the code there); another BIOS declares
`hle_id_check` / `hle_id_pointer` in its identity file. Without it, or on a
one-sided image, auto swap is refused. The signal is the request itself, read
where the RAM Adapter sees it: the opcode fetch of the anchor (a read of it
followed on the next cycle by the read of the byte after it, so data reads and
interrupts do not count), then the 10 ID bytes matched against every side's
disk header ($FF matches anything, as the BIOS compares). This observation is
always on (ring `fds.idreq` + `fds.idbytes`, whatever the plan). With auto
swap on:

- a request matching exactly one side that is not in the drive: eject at the
  end of the frame, 3 frames empty, insert that side. The BIOS is then still
  in its wait before it starts the motor (~40 frames: Otocky's request at
  cycle 21,623,595 reached the BIOS's $4032 disk test 1,185,167 cycles, 39.8 frames, later),
  so the call finds its side and never fails;
- a request the drive satisfies, one matching several sides (`ambiguous`) or
  none (`nomatch`) changes nothing;
- a game that shows "set side B" and watches $4032 for the disk to come out
  makes no request until it has gone out and back in. After a disk access, 20
  frames of $4032 polling with the drive idle (gaps up to 30 frames) count as
  waiting: eject, 10 frames empty, put the same side back (a bump, harmless if
  the program was not waiting); its next request then swaps as above. If it
  keeps waiting with no request in between (a game that reads the header
  itself), the next round inserts the next side. This path is inferred, like
  Mesen's own auto-insert (FDS.cpp: > 20 $4032 reads, eject, 77 frames, insert
  disk 1 side A, switch the side at $E445); a program that polls $4032 during
  play without wanting a swap gets one bump per disk access.

A host eject or insert takes the drive back from the tier until the next
request. Owner titles (native, auto swap, no disk events): Otocky, Esper Dream
and Nazo no Murasame-jou are all "watch" titles (no BIOS call until the disk
is swapped); each had side B in 36-41 frames after it began to poll (Otocky
f=691 -> 729, Esper 1008 -> 1049, Murasame 1484 -> 1520), against 94-105
frames with Mesen's auto-insert through nesref (side B at Otocky ~796, Esper
~1102, Murasame ~1585; sampled every 2-10 frames). Every request was unambiguous; in the owner's library
(114 images, 198 sides) no two sides of an image share a disk ID and every
side has a header, so exact requests are never ambiguous there. No library
image has more than two sides: multi-disk swaps are fixture-checked only.

**Fast load** needs only the drive. The machine is the same LLE machine frame
for frame; only the host's pacing changes: load frames (the drive clocking
bytes with the transfer released, data through $4031/$4024, the head rewinding
or spinning up, the BIOS between its ID check and starting the motor, an auto
swap in progress) run back to back without audio, and the window shows about
60 of them a second. Load spans are always recorded (ring `fds.span`, marked
`fast` when fast load ran them). A drive the BIOS leaves turning to the end of
the side is not loading. Nothing the program can see changes: with fast load
alone, and with auto swap + fast load against auto swap alone, `--hash-out`
is identical on every owner route and fixture, so the state after each load is
LLE's exactly (frame counters included, since the same frames run). Wall-clock
time of the load frames, `--realtime`, auto swap on, native builds:

| title (route) | frames | load frames | paced | fast load |
|---|---|---|---|---|
| SMB2J (boot) | 800 | 334 | 5.53 s | 0.58 s |
| Otocky (boot, side B) | 2000 | 631 | 10.43 s | 1.20 s |
| Esper Dream (boot, side B) | 2600 | 694 | 11.48 s | 2.00 s |
| Nazo no Murasame-jou (boot, side B) | 3000 | 617 | 10.20 s | 1.00 s |
| Dead Zone (two loads) | 1600 | 750 | 12.40 s | 2.14 s | A deeper
HLE (servicing the BIOS's file calls directly) would skip those frames and so
change what the program counts during a load; it is not done.

Ring: `fds.idreq`, `fds.idbytes`, `fds.span`, `fds.hle` (config, keep, swap,
ambiguous, nomatch, wait, eject, insert, cancel, with the evidence: the match
mask, the side in the drive, the poll frames). Tests: `cyc_fds_hle_plan_test`
(the plan over every request x capability, precedence, parsing, ID matching)
and `cyc_fds_hle_test` (`tools/cyc/test_cyc_fds_hle.py`: a synthetic
BIOS-and-game program, `tools/cyc/fds_hle_fixtures.py`, with 2- and 4-sided
disks: a program that asks the BIOS, one that watches, one that never asks, a
poller, short polls, ambiguous / unmatched / multi-disk / satisfied requests,
a self-checking game; native = `--interp-only` = `cyc_interp`, fast load
identity, config precedence, refusals, host disk changes, `--realtime`).

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
(`cyc_audio_enable`/`cyc_audio_read`) through a console output stage; the SDL
host plays it and `--wav-out FILE` records it.

The output stage has two models (`cyc_set_console`, `--console`, game.toml
`[game] console = "nes" | "famicom"`):

- `nes`: the front-loader's 90 Hz and 440 Hz first-order high-pass and 14 kHz
  low-pass (nesdev APU Mixer: blargg's measurements of the RCA output, matched
  by lidnariq to its 150 ohm / 10 uF output coupling and the 47 kohm / 220 pF
  around the inverter amplifier). Discretized as RC sections at the output
  rate, so at 48 kHz the low-pass is -5.2 dB at 14 kHz rather than -3 dB.
- `famicom`: a 37 Hz first-order high-pass, the only stage nesdev's APU Mixer
  page gives for the Famicom ("followed by the unknown (and varying)
  properties of the RF modulator and demodulator"). Like `nes`, it stops at
  the console's audio output; the RF modulator and the TV's demodulator and
  FM de-emphasis are left out, as the TV behind the NES's RCA jack is. A
  judgment call, not oracle-verified: Mesen applies no output stage.

The default follows the board: `famicom` for boards made only for the Famicom
that carry expansion audio (the Disk System, Namco 163, VRC6, VRC7: the NES
cartridge slot has no audio return, so their sound was only heard on a
Famicom), `nes` for the rest, including MMC5 and FME-7/5B, which also have NES
boards. The cartridge's audio is mixed before the stage (on the Famicom the
2A03's audio leaves on cartridge pin 46 and returns mixed on pin 45). The FDS
RAM Adapter's ~2 kHz low-pass is the adapter's, on the 2C33's sound only, not
the console's; it stays with `--fds-profile hardware`.

`cyc_console_audio_test` checks each model's response to sines and a step
against its RC sections and the board defaults; `cyc_console_audio_pcm`
(`tools/cyc/test_cyc_console_audio.py`) checks the rendered defaults, pins
the nes and famicom renders of synthetic programs by SHA-256 (the nes ones are
those of the build before the models existed) and compares their spectra with
the models. `tools/cyc/console_output.py` is the models in Python.

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

- **The Famicom's audio past its 37 Hz high-pass.** The `famicom` output
  stage stops at the console's audio output. A stock HVC-001 is heard through
  its RF modulator and a TV, whose FM de-emphasis is a ~2.1 kHz first-order
  low-pass unless the modulator pre-emphasizes (lidnariq, nesdev forum
  t=13419; the NES modulator appears not to), and the AV Famicom (HVC-101)
  has its own audio circuit; neither is measured or modelled. Which board a
  Famicom-only game ran on is decided by mapper number, so a Japanese title
  on a board that also had NES releases (MMC5, FME-7/5B, and every board
  without expansion audio) defaults to `nes`: the header does not say which
  console a cartridge was sold for.
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

## The window

`cyc_sdl.c` (SDL2), when a build has it. Everything the player presses is an
action with a keyboard and a controller binding (`cyc_input.h`): each player's
A B Select Start and D-pad, read from the keyboard or a game controller as
config.ini `[Input] PlayerNSource` says, and the host shortcuts, read from
the keyboard and every controller. Defaults (the nesrecomp NES layout):

| Action | Keyboard | Controller |
|---|---|---|
| A, B, Select, Start | Z, X, Backslash, Return (player 1) | A, X, Back, Start |
| D-pad | arrows | D-pad, left stick |
| Disk (FDS only) | D | LB |
| Menu | Escape | RB |
| Fast-forward (held) | Tab | RT |
| Screenshot (the picture, `cyc_shot_NNNN.png`) | F12 | - |
| Save state / load state (`saves/<image>.state`) | F8 / F9 | - |
| Fullscreen | F11 | - |

A key or button bound to a shortcut belongs to the shortcut while it is
held. config.ini beside the executable (`cyc_settings.h`; `--config FILE`)
keeps the bindings (`[Keyboard.PlayerN]`, `[Gamepad.PlayerN]`,
`[Keyboard.Shortcuts]`, `[Gamepad.Shortcuts]`: SDL key and controller names,
`lefttrigger+`, `back+start` chords), the display and audio settings and the
FDS HLE choices. Headless runs never read it.

**Disk** (`cyc_disk_action.h`): the first press shows a toast with the drive's
state (disk and side, motor, disk save) and the binding; each press while the
toast is up turns the disk: eject, the drive empty for 30 frames, the next
side in (disk 1 A, 1 B, 2 A, ... and around; a one-sided disk goes back in).
A press while the drive is still empty moves on one more side and restarts
the hold; an eject waits for a disk write to finish. After the toast times out
(3 s) the next press only shows it again. The drive changes through the same
eject and insert as `--fds-event`; headless, `F DISK_ACTION` in an `--input`
file presses it, with emulated time as the toast's clock.
`tools/cyc/test_cyc_disk_action.py` (CTest `cyc_disk_action_run`) checks a
swap byte-for-byte against the same `--fds-event`s at every alignment.

**recomp-ui** (`cyc_ui.h`): a game built with it (project.cmake finds the
game's `recomp-ui/` submodule, or `-DNESRECOMP_RECOMP_UI=<checkout>`) opens
recomp-ui's launcher first (the image, settings, and every binding, keyboard
and controller, on its Controls page; `NESRECOMP_NO_LAUNCHER=1` skips it) and
has recomp-ui's runtime menu on the Menu shortcut: display and audio, a Disk
Drive section for a disk image only (the side in the drive, swap, eject /
insert, and one row per HLE axis), the shortcut list, the game's own rows
(`cyc_host_extras.h`) and Quit. The game pauses while the menu is open. The
toast is recomp-ui's runtime toast; without recomp-ui the window draws it
itself (`cyc_overlay.h`). Both are drawn after the picture, never into it.

**Production and dev builds**: `NESRECOMP_DEV_UI` (CMake, default OFF) adds
the drive bar, the dev keys (F1 eject / insert, F2 recompiled code /
interpreter, F3 next side, F4 the bar, F6 + n HLE axis n) and the coverage in
the title bar. Production builds have none of them.

**Zapper games** pass `ZAPPER_PORT 2` to `nesrecomp_add_cycle_game` (port 1 is
also supported). This requires recomp-ui's `RECOMP_LAUNCHER_HAS_ZAPPER_SETTINGS`
interface. The mouse aims within the hardware 256x240 picture, with letterbox
bars treated as offscreen, and the left button pulls the trigger. With a wide
game compositor (`cyc_render.h`) the native picture sits at the origin the
compositor declares (`cyc_render_set_native_origin`, for a camera anchored at
an area's edge; centered otherwise): the aim maps through it, and margins are
offscreen to the gun (no light, so the game's own hit test decides) while the
crosshair keeps following the mouse across them. The runtime
Zapper section and launcher offer Mouse aiming and Crosshair. Both persist in
`config.ini [Zapper]`; missing choices are imported from an existing
`keybinds.ini [zapper]` without rewriting that legacy file. Closing the menu
waits for the mouse button to be released before a gameplay trigger pull.

Light detection uses pixels as the PPU draws them, a small aperture and a
20-scanline decay. This is an optical approximation informed by
[NESdev's Zapper research](https://www.nesdev.org/wiki/Zapper) and
[Mesen2's input implementation](https://github.com/SourMesen/Mesen2/blob/master/Core/NES/Input/Zapper.h).
Presentation crosshairs never enter the hardware picture or charge the sensor.
Zapper states use version 3 with a required `ZAPP` section for aim, trigger and
sensor history; matching older states remain loadable with no attached gun.
Ordinary states retain version 2. `cyc_zapper_test` covers port polarity, beam
locality, dark-screen rejection, decay, offscreen aim and state/snapshot safety.

For deterministic headless routes, `--zapper-port 1|2` attaches the gun and
`--zapper-input FILE` reads ordered `FRAME X Y TRIGGER` lines (trigger 0/1;
offscreen `-1 -1`). `--zapper-port 0` restores the ordinary controller ports.
The TCP `zapper` command supports aim, trigger, returning control to the mouse,
and window-coordinate mapping; its reply includes the presented width and the
native origin that mapping used. TriCNES has no Zapper oracle: compare gun routes
between native and interpreter execution, validate actual hits/misses, and use
the oracle separately for the unattached-controller machine.

**Games** add to the window with `nesrecomp_add_cycle_game(... HOST_EXTRAS
<sources>)` defining `cyc_host_extras()` (`cyc_host_extras.h`): a presented
picture of their own size (widescreen), view modes for the menu's View mode
row, extra menu rows, their own config.ini `[Game]` keys, per-frame work
(`power_on`, `frame_begin`, `frame_end`), developer command-line options and
TCP commands. A game built with `MODS` has the launcher's Mods screen and the
runtime menu's Mods rows ([Game mods](#game-mods)).

**TCP** (`cyc_tcp.h`; `--tcp PORT`, `NESRECOMP_CYC_TCP`, or `debug.ini` beside
the executable for port 4370): JSON over newline as in [TCP.md](../../TCP.md).
`key` and `pad` (an SDL virtual controller) hold input through the bindings,
`action` holds an action, `disk`, `menu`, `hle`, `screenshot` (layer
`picture`: the game alone; `ui`: everything the window drew), `state`,
`read_ram`, `ring_dump`, `save_state` / `load_state` (path), `video` (the
presented width and mode, optionally setting the mode), `window_size` (w, h:
Fit follows the new drawable), `mod_stats` (hook sites, isolated calls,
compositor counts), `quit`, and the game's own (`tcp_setup`). With `--hidden` and `SDL_VIDEODRIVER=dummy`
nothing reaches a desktop; `tools/cyc/test_cyc_window.py` (CTest
`cyc_window_run`) drives production and dev windows that way.

## Game mods

A game's mods are trusted C in the game's `HOST_EXTRAS`, selected and
configured through the mod runtime (`runner/src/mod_runtime.cpp`; packages and
manifests as in [docs/MOD_PACKAGES.md](../../docs/MOD_PACKAGES.md)). The
building blocks, all inert in a program that uses none of them (the stock
machine and its hashes are unchanged):

**Hook sites** (`cyc_hooks.h`). `game.toml` `[[mod_function_hook]]` entries
with an `id` and a content key (`bytes`, or `length` + `crc32`) become
instruction boundaries in the generated code that return to the scheduler
while a hook is enabled. The scheduler runs the enabled callbacks whose key
memory holds, before the instruction, on every path: compiled ROM banks,
compiled RAM views and the interpreter. A nonzero callback returns from the
routine as its RTS would. A RAM site no disk file holds, or one without a key,
fails code generation; a plugin naming a site the program lacks fails at
start. Counts per site (`cyc_hooks_stats`) and a `MOD_HOOK` ring event per
frame a site fired.

**Isolated guest calls** (`cyc_mod.h`). `cyc_mod_peek` / `cyc_mod_poke` read
and write CPU RAM, PRG RAM and cartridge RAM. Between `cyc_mod_isolate_begin`
and `cyc_mod_isolate_end`, `cyc_mod_call(routine, regs)` runs the program's
own routine (a JSR to a sentinel return) on a machine whose cycles clock
nothing: no PPU dots, APU, mapper counters or drive, no ring or trace events,
hooks suspended by default. The end restores the whole machine from a snapshot (CPU,
RAM, cartridge and PRG RAM, CHR RAM, PPU, APU, mapper and expansion sound, FDS
media and HLE, RAM view validity), so a mod can decode, simulate and draw with
the game's own code without the game noticing. A budget (default 2,000,000
CPU cycles per call), a stack check and a jam check fail a call loudly
(`MOD_FAIL` ring event). `cyc_mod_call_commit` keeps a routine's memory effects
instead, and refuses a routine that stores to a device register.

Trusted game ports can opt into return observation and hooks inside one
isolated scope. Input/event callbacks and the voxel presentation adapter are
described in [CYCLE-MOD-ADAPTERS.md](../../docs/CYCLE-MOD-ADAPTERS.md).

**Save states** (`cyc_state.h`). The whole machine, the host's section and
every registered mod record (`runner/include/mod_savestate.h`), identified by
the program, the layout of each section and, for a disk, the image's base
data and FDS options; a state from another program or layout is refused.
Window: F8 / F9 and the menu's rows (`saves/<image>.state`); TCP `save_state`
/ `load_state`; headless `--save-state FRAME:FILE` (after that frame,
repeatable) and `--load-state FILE` (continues at the next frame). A state
loaded in a new process continues exactly as the uninterrupted run.

**Presented width** (`cyc_video.h`). The function-level runtime's modes from
the same geometry (`common/nes_video_geometry.h`): stock 256, 16:9 426, 21:9
560, 32:9 854, Fit 256..854 following the window's drawable aspect (clamped to
[16:15, 32:9], even widths, square pixels). A change queues until the window
has presented, then applies (`VIDEO` ring event). Headless, `--present-size
WxH` gives Fit a drawable.

**Custom renderer** (`cyc_render.h`). With a width over 256 the game's
compositor paints the picture from what the frame used: the native 256x240
picture and its background opacity, each line's scroll, PPUCTRL and PPUMASK
as captured at dot 1 of the line, OAM, CHR, nametables and palette, with tile
and sprite helpers. Without a compositor, or when it declines a frame, the
native picture is pillarboxed. A picture is composed once per frame, on the
first present or in the game's `frame_end`.

**Runtime.** `nesrecomp_add_cycle_game(... MODS GAME_ID <id>)` builds the mod
runtime in (the catalog is `mods/` beside the executable; headless
`--mods-root DIR`): the launcher's Mods screen, the runtime menu's rows for
each installed feature and its options (applied at once), activation plugins
and reset callbacks at start, and mod records in save states. A game's
`options` (`cyc_host_extras.h`) are developer overrides on the command line,
applied after the saved selection.

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

- NTSC and PAL have separate CPU/PPU/APU timing and save-state clock phase.
  Dendy timing is not implemented. Mapper support covers the board
  configurations above, not every variant sharing a mapper number. Cartridge
  battery RAM is persisted unless `--no-save` is active.

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
- Code in RAM is compiled only from images the compiler has seen: disk files,
  and snapshots a capture run recorded. Code a run has not yet executed with
  no view (a new route, new generated code) runs on the interpreter until it is
  captured; correctness is unaffected either way. Cartridge work RAM
  (`$6000-$7FFF`, banked on several boards) has no views; CPU RAM and the FDS
  PRG RAM do.
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
