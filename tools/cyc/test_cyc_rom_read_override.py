"""ROM-free native/interpreter parity for folded immediate and data overrides."""
import argparse
from pathlib import Path
import re
import struct
import subprocess

from nested_build import Toolchain, add_arguments


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recompiler', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    add_arguments(parser)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    image = bytearray(16 + 16384)
    image[:5] = b'NES\x1a\x01'
    program = bytes.fromhex('A9 11 85 00 AD 10 80 85 01 4C 00 80')
    image[16:16 + len(program)] = program
    image[16 + 0x10] = 0x33
    image[-6:] = struct.pack('<HHH', 0x8000, 0x8000, 0x8000)
    (out / 'fixture.nes').write_bytes(image)
    (out / 'extras.c').write_text(r'''
#include "cyc_host_extras.h"
#include "cyc_mod.h"
#include <stdio.h>
static unsigned frame;
static void enable(void) {
    cyc_mod_set_rom_read_override(0x8001, 0x42);
    cyc_mod_set_rom_read_override(0x8010, 0x99);
}
static void power(void *ctx) { (void)ctx; frame = 0; enable(); }
static void end(void *ctx) {
    (void)ctx;
    printf("override-result %02X %02X\n", cyc_mod_peek(0), cyc_mod_peek(1));
    if (frame++ == 0) cyc_mod_clear_rom_read_overrides(); else enable();
}
static const CycHostExtras extras = {.power_on=power, .frame_end=end};
const CycHostExtras *cyc_host_extras(void) { return &extras; }
''', encoding='utf-8')
    (out / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(RomOverrideTest C CXX)
set(NESRECOMP_HEADLESS ON)
set(NESRECOMP_RECOMP_UI OFF)
include("{root.as_posix()}/runner/cyc/project.cmake")
nesrecomp_add_cycle_game(rom_override_test HEADLESS
    ROM "{(out / 'fixture.nes').as_posix()}"
    RECOMPILER "{args.recompiler.resolve().as_posix()}"
    HOST_EXTRAS "{(out / 'extras.c').as_posix()}")
''', encoding='utf-8')
    toolchain = Toolchain.from_args(args)
    (out / 'logs').mkdir(exist_ok=True)
    toolchain.build(out, out / 'build', out / 'logs', parallel=4)
    executable = toolchain.executable(out / 'build', 'rom_override_test')
    expected = [('42', '99'), ('11', '33'), ('42', '99')]
    for mode in ([], ['--interp-only']):
        result = subprocess.run(
            [str(executable), str(out / 'fixture.nes'), '--frames', '3', '--no-save', *mode],
            capture_output=True, text=True, timeout=30,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        name = 'interpreter' if mode else 'native'
        (out / f'{name}.log').write_text(result.stdout + result.stderr, encoding='utf-8')
        assert result.returncode == 0, result.stdout + result.stderr
        actual = re.findall(r'override-result ([0-9A-F]{2}) ([0-9A-F]{2})', result.stdout)
        assert actual == expected, (name, actual, expected)
        print(f'{name}: immediate/data substitutions, clear and re-enable passed', flush=True)


if __name__ == '__main__':
    main()
