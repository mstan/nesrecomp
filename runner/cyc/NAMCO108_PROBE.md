# Namco 108 false-write diagnostic

The [primary hardware report](https://forums.nesdev.org/viewtopic.php?p=156136)
observed false mapper writes when code at $8000-$9FFF wrote internal RAM.
It measured Namco **108**; differences in 109/118/119 are conjecture in that
report. It does not establish exact M2/address propagation timing, all affected
instructions, or whether a particular ROM dump identifies the chip revision.
Mapper 206 therefore retains its normal register decode. No probabilistic or
PC-based false-write model is presented as verified hardware behavior.

Generate an original diagnostic cartridge image with:

```text
python tools/cyc/namco108_probe.py --out namco108-probe.nes
```

Use a real Namco PCB with replaceable PRG/CHR storage, retaining its mapper IC.
An ordinary flashcart or emulator tests that device's implementation instead.
The image has 128 KiB PRG and 8 KiB CHR. The control program executes at $E400;
the short write probes execute at $8100, $A100, $C100 and $E100, respectively.
Every PRG bank contains the same probe stubs so an unexpected bank switch can
still return to the control program. IRQs and rendering are off during probes.

Four visible rows correspond to those four execution windows. Each row contains
eight groups of four hex digits, for writes to these addresses in order:

| Group | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| RAM address | $0000 | $0001 | $0100 | $0101 | $0800 | $0801 | $1800 | $1801 |

Each probe selects PRG registers 6=0 and 7=1, leaves register 6 selected, then
executes `LDA #3; STA address; RTS` in the selected window. It records the bank
markers at $9F00/$BF00, writes 5 to $8001 from the control program, and records
both markers again. Normal behavior produces **0151** for every group. A false
odd-address data write may produce **3151**; a false even-address select write
may produce **0101**. These are interpretations to investigate, not assertions
that every chip must produce those patterns. The raw 128 bytes also remain at
CPU RAM $0700-$077F for a hardware debugger or capture tool.

Record the PCB marking, mapper IC marking, CPU/console revision, power/reset
procedure, photograph and captured CPU address/data/RW/M2/ROMSEL signals for
any differing group. Repeat power cycles and compare chips. The experiment
tests absolute STA only; it does not yet characterize RMW or indexed stores.
Results may justify a chip-specific model once the electrical trigger is known.

The synthetic emulator regression checks this ROM's control flow and ordinary
206 behavior at four alignments. It establishes that the diagnostic itself
runs; it is not a physical validation of the erratum.
