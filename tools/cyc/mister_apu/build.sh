#!/usr/bin/env bash
# build.sh - build cyc_mister_apu, NESRecomp's machine co-simulated with
# NES_MiSTer's APU (see README.md). Run from an MSYS2 UCRT64 shell with
# verilator and g++:
#
#   tools/cyc/mister_apu/build.sh <NES_MiSTer checkout> <build dir>
set -euo pipefail

MISTER=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
CYC="$HERE/../../../runner/cyc"

# DmaController lives in rtl/nes.v next to the NES top level, whose CPU (T65)
# is VHDL; take just that module.
awk '/^module DmaController/{p=1} p{print} p&&/^endmodule/{exit}' "$MISTER/rtl/nes.v" > "$OUT/dma_controller.v"

rm -rf "$OUT/obj"
verilator --cc -O3 --x-assign fast --x-initial fast \
    -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTH -Wno-CASEINCOMPLETE -Wno-UNOPTFLAT \
    --top-module apu_harness --Mdir "$OUT/obj" \
    "$MISTER/rtl/regs_savestates.sv" "$HERE/ereg_stub.sv" "$MISTER/rtl/apu.sv" \
    "$OUT/dma_controller.v" "$HERE/apu_harness.sv"

# The Verilated model and Verilator's runtime are compiled here with one set of
# flags (MSYS2's prebuilt runtime archive does not link against its own GCC's
# libstdc++).
VINC=$(verilator --getenv VERILATOR_ROOT)/include
CXXFLAGS="-std=gnu++20 -O2 -I$OUT/obj -I$VINC -I$VINC/vltstd -I$CYC"
objs=()
for f in "$OUT"/obj/Vapu_harness*.cpp "$VINC/verilated.cpp" "$VINC/verilated_threads.cpp" "$HERE/cosim.cpp"; do
    o="$OUT/$(basename "${f%.cpp}").o"
    g++ $CXXFLAGS -c "$f" -o "$o"
    objs+=("$o")
done
for f in cpu6502.c cpu6502_interp.c hw_machine.c hw_mapper.c hw_ppu.c hw_apu.c hw_palette.c cyc_trace.c \
         cyc_run.c cyc_native_none.c cyc_accuracycoin.c; do
    o="$OUT/${f%.c}.o"
    gcc -std=c11 -O2 -I"$CYC" -c "$CYC/$f" -o "$o"
    objs+=("$o")
done
g++ "${objs[@]}" -o "$OUT/cyc_mister_apu.exe" -static -lpthread
echo "built $OUT/cyc_mister_apu.exe"
