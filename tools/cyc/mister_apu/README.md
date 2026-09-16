# tools/cyc/mister_apu — NESRecomp's APU against NES_MiSTer's

`cyc_mister_apu` runs a program on NESRecomp's cycle-accurate machine
(interpreter) and, after every CPU or DMA cycle, drives
[NES_MiSTer](https://github.com/MiSTer-devel/NES_MiSTer)'s APU and DMA
controller with the CPU side of that cycle, through one CPU cycle of MiSTer's
master clock. MiSTer's APU only listens. Channel outputs, the IRQ output, DMC
DMA reads, sprite DMA bus ownership and every `$4015` read are compared; see
`runner/cyc/README.md` ("Against NES_MiSTer's APU") for what the results mean.

Nothing from NES_MiSTer is in this directory. `build.sh` Verilates
`rtl/apu.sv` and `rtl/regs_savestates.sv` from a checkout you provide and
extracts `DmaController` from `rtl/nes.v` into the build directory (the rest
of `nes.v`, and MiSTer's CPU, T65, are VHDL, which Verilator does not read).
`ereg_stub.sv` stands in for the VHDL savestate register the APU instantiates.

## Files

| File | |
|------|-|
| `apu_harness.sv` | MiSTer's `APU` and `DmaController`, wired as in `nes.v`, with the CPU side as ports |
| `ereg_stub.sv` | SystemVerilog `eReg_SavestateV` |
| `cosim.cpp` | the co-simulation driver and comparison |
| `build.sh` | builds `cyc_mister_apu.exe` with Verilator and MSYS2 UCRT64 g++ |

## Build and run

From an MSYS2 UCRT64 shell (`pacman -S mingw-w64-ucrt-x86_64-verilator`):

```bash
tools/cyc/mister_apu/build.sh /c/path/to/NES_MiSTer build/mister_apu

python tools/cyc/make_apu_tone_rom.py tones.nes
build/mister_apu/cyc_mister_apu tones.nes --frames 720 --dmc-timer 1024
build/mister_apu/cyc_mister_apu AccuracyCoin.nes --acccoin --dmc-timer 1024
```

Options:

- `--phase 0|1`: MiSTer's get/put cycle at power-on relative to NESRecomp's.
  0 matches (with 1, frame counter reads disagree).
- `--dmc-timer N`: start NESRecomp's DMC timer at N instead of 1022. MiSTer's
  netlist-derived timer is at NESRecomp's 1024 on the first cycle; the console
  leaves this phase undefined.
- `--noise-back N --noise-timer T`: start NESRecomp's noise LFSR N clocks
  earlier and its timer at T (MiSTer's noise timer starts ~4094 cycles after
  power-on, 1024 clocks behind).
- `--nes-v-conflicts`: feed MiSTer's DMC the external bus when a DMC DMA
  reads `$xx15` while the CPU holds `$4000-$401F`, as `nes.v` does. By default
  it gets MiSTer's own `$4015` value, since the 2A03 "read[s] $4015 and
  ignore[s] the DMA value on the external data bus" (nesdev wiki, DMA). A DMA
  read hitting `$4016`/`$4017` always gives MiSTer's DMC the controller port
  value NESRecomp's machine read (the harness has no controllers).
- `--wav-ours FILE --wav-mister FILE`: both APUs' channel levels through the
  same mixer, 48 kHz.
- `--show FIRST LAST`: print both sides cycle by cycle over a range, with each
  DMC's address, bytes left, sample buffer (`+` = holds a byte), shift
  register, bit counter and silence flag.

A full AccuracyCoin run takes about 4 minutes.
