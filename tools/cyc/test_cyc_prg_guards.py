#!/usr/bin/env python3
"""Fresh generated code under atomic code/data patches and writable escapes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import nested_build


def fixture():
    prg = bytearray([0xEA] * 32768)
    code = bytearray((0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA9, 0))
    for addr in range(1, 6): code.extend((0x85, addr))
    code.extend((0xA0, 0x60, 0xA2, 0))
    loop = len(code)
    code.extend((0xCA, 0xD0, 0xFD, 0x88, 0xD0, (loop - len(code) - 6) & 255))
    for addr, value in ((0x2001, 0x1E), (0x4017, 0xC0), (0x4015, 1),
                        (0x4000, 0x9F), (0x4002, 0x80), (0x4003, 8)):
        code.extend((0xA9, value, 0x8D, addr & 255, addr >> 8))
    # A subroutine that starts in the last byte of a 4KB slot: LDA #imm with the
    # immediate in the next slot ($B000), then STA $06, RTS.
    code.extend((0x20, 0xFF, 0xAF))
    code.extend((0x4C, 0, 0x8F)); prg[:len(code)] = code
    prg[0x2FFF:0x3004] = bytes((0xA9, 0x11, 0x85, 0x06, 0x60))
    prg[0x0E00:0x0E02] = bytes((0, 0xC0))
    prg[0x0F00:0x0F03] = bytes((0x6C, 0, 0x8E))
    prg[0x4000:0x4005] = bytes((0xE6, 1, 0x4C, 0, 0x8F))
    prg[0x4400:0x4405] = bytes((0xE6, 2, 0x4C, 0, 0x8F))
    prg[0x4100] = 0x40
    prg[-6:] = bytes((0, 0xC1, 0, 0x80, 0, 0xC1))
    return b'NES\x1a' + bytes((2, 0, 0x12, 0, 1)) + bytes(7) + prg


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--recompiler', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2] / 'runner/cyc')
    ap.add_argument('--ppu', choices=('HLE', 'LLE'), default='HLE')
    ap.add_argument('--parallel', type=int, default=1)
    ap.add_argument('--probe', type=Path, help='Reuse the already-built probe for unchanged fixture inputs')
    nested_build.add_arguments(ap)
    args = ap.parse_args(); tc = nested_build.Toolchain.from_args(args)
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    source = args.source.resolve()
    (out / 'test.nes').write_bytes(fixture())
    (out / 'seeds.txt').write_text('4k:00:8000\n4k:00:8F00\n4k:04:C000\n4k:04:C400\n4k:04:C100\n4k:02:AFFF\n4k:03:B001\n')
    (out / 'game.toml').write_text('[game]\noutput_prefix="guard"\ncycle_accurate=true\ncycle_seed_file="seeds.txt"\n')
    def run(argv, name):
        result = subprocess.run([str(x) for x in argv], cwd=out, capture_output=True,
                                text=True, creationflags=nested_build.NO_WINDOW,
                                env=tc.environment())
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode: raise RuntimeError(f'{name} exit {result.returncode}; see {out / (name + ".log")}')
        observations = [line for line in result.stdout.splitlines() if line and line[0].isdigit()]
        assert len(observations) == 45, (name, 'complete frame observations')
        return '\n'.join(observations)
    if not args.probe:
        # Code generation has no frame observations; retain its complete log.
        generated = subprocess.run([str(args.recompiler.resolve()), 'test.nes', '--game', 'game.toml'],
                                   cwd=out, capture_output=True, text=True,
                                   creationflags=nested_build.NO_WINDOW, env=tc.environment())
        (out / 'codegen.log').write_text(generated.stdout + generated.stderr)
        if generated.returncode: raise RuntimeError(f'codegen exit {generated.returncode}')
    cmake = ['cmake_minimum_required(VERSION 3.20)', 'project(prg_guards C)',
             'set(CMAKE_C_STANDARD 11)',
             'add_compile_definitions(NESRECOMP_NATIVE_PRG_GUARDS=1 NESRECOMP_NATIVE_INTERVALS=1 CYC_HOST_EXTRAS_STRONG)',
             f'include("{source.as_posix()}/cyc.cmake")',
             'set(RUNTIME_SOURCES ${NESRECOMP_CYC_SOURCES})',
             'list(REMOVE_ITEM RUNTIME_SOURCES "${NESRECOMP_CYC_DIR}/cyc_host.c")',
             'file(GLOB GEN CONFIGURE_DEPENDS "generated/*_cyc*.c")',
             f'add_executable(probe ${{GEN}} ${{RUNTIME_SOURCES}} "{source.as_posix()}/tests/prg_guard_probe.c")',
             'target_include_directories(probe PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})',
             'target_link_libraries(probe PRIVATE ${NESRECOMP_CYC_LIBRARIES})']
    (out / 'CMakeLists.txt').write_text('\n'.join(cmake) + '\n')
    if not args.probe:
        tc.build(out, out / 'build', out, parallel=args.parallel, defines=(
            f'-DNESRECOMP_PPU_IMPL={args.ppu}', '-DNESRECOMP_APU_OPERATION=ON', '-DNESRECOMP_NATIVE_WAIT_LOOPS=ON'))
    exe = args.probe.resolve() if args.probe else tc.executable(out / 'build', 'probe')
    for case in ('code', 'escape', 'isolated'):
        result = subprocess.run([str(x) for x in (exe, out/'test.nes', 1, 0, 0, 0, case)],
                                cwd=out, capture_output=True, text=True,
                                creationflags=nested_build.NO_WINDOW, env=tc.environment())
        (out / ('coverage-' + case + '.log')).write_text(result.stdout + result.stderr)
        assert (result.returncode == 0 if case == 'isolated' else result.returncode != 0), (case, result)
        assert '[native coverage]' in result.stderr, (case, result)
        assert 'interpreter_rom_cycles=0' in result.stderr, (case, result.stderr)
        assert ('pc=$8000' if case == 'escape' else 'pc=$C400') in result.stderr, (case, result.stderr)
        if case == 'isolated': assert 'preserves rollback' in result.stdout
    report = []
    for trace in (0, 1):
        for style in (0, 1):
            for align in range(4):
                results = {}
                for native in (0, 1):
                    name = f't{trace}-d{style}-a{align}-n{native}'
                    results[native] = run([exe, out/'test.nes', native, align, trace, style], name)
                assert results[0] == results[1], (trace, style, align, 'hardware/RAM/raster/PCM/CPU/bus parity')
                report.append(dict(trace=trace, data_only=style, alignment=align,
                                   output_sha256=hashlib.sha256(results[0].encode()).hexdigest()))
            print(args.ppu, 'trace', trace, 'data-only', style, ': four alignments identical', flush=True)
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__': main()
