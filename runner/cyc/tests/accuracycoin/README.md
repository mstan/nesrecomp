# AccuracyCoin

[AccuracyCoin](https://github.com/100thCoin/AccuracyCoin) is a 144-test NES
accuracy suite (MIT). This directory recompiles it with `--cycle-accurate` and
checks the result against the TriCNES oracle. Put `AccuracyCoin.nes` from that
repository here first (`*.nes` is ignored by git).

```bash
cd runner/cyc/tests/accuracycoin
NESRecomp AccuracyCoin.nes --game game.toml
cmake -S . -B build && cmake --build build --config Release

# headless: runs every test and prints the results table
build/Release/AccuracyCoinRecomp AccuracyCoin.nes --acccoin

# against the oracle, native and --interp-only, at every CPU/PPU alignment
python ../../../../tools/cyc/cyc_verify.py --exe build/Release/AccuracyCoinRecomp.exe \
    --oracle build/Release/cyc_oracle.exe --rom AccuracyCoin.nes --acccoin \
    --align 0 1 2 3 --interp

build/Release/cyc_helper_test
```

With SDL2 (`runner/external/SDL2`) the build also has a window: arrows, X/Z for
A/B, Enter/Right Shift for Start/Select, Tab to fast-forward, F2 to switch
between recompiled code and the interpreter live, F12 for a screenshot. Start
on the main menu runs every test.

Expected: 144/144 at alignment 0; 143, 141 and 143 at alignments 1-3, where the
oracle fails the same tests ($2002 flag timing, OAM corruption, frozen OAM2
increment). Every alignment must print `ALL MATCH`, and the run should report
100.0% native — anything less means the seed file has fallen behind the ROM.

`cyc_seeds.txt` holds the entry points only reachable through RTS and
JMP-indirect dispatch; `--miss-log cyc_seeds.txt` merges new ones into it.
