#!/usr/bin/env python3
"""ROM-free host and bank-folding regressions (build artifacts are retained).

python tools/cyc/test_cyc_runtime.py --recompiler build/compiler/Release/NESRecomp.exe \
    --interp build/cyc/Release/cyc_interp.exe --oracle build/cyc/Release/cyc_oracle.exe \
    --out build/cyc-regressions
"""
import argparse
from pathlib import Path
import subprocess

from cyc_verify import first_difference
from mapper_fixtures import mapper_fixtures
from mapper_ppu_fixtures import ppu_fixtures
from cart_variant_fixtures import variant_fixtures
from latch_fixtures import latch_fixtures
from fineprg_fixtures import fineprg_fixtures
from vrc_fixtures import vrc_fixtures
from expansion_fixtures import expansion_fixtures
from vrc7_fixtures import vrc7_fixtures
from bandai_fixtures import bandai_fixtures
from mmc5_fixtures import mmc5_fixtures
from mmc1_fixtures import mmc1_fixtures
from mapper40_fixtures import mapper40_fixtures
from namco108_probe import fixture as namco108_fixture


def run(cmd, cwd, log, timeout=180):
    p = subprocess.run([str(x) for x in cmd], cwd=cwd, capture_output=True, text=True,
                       timeout=timeout, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    Path(log).write_text(p.stdout + p.stderr)
    if p.returncode:
        raise RuntimeError(f'exit {p.returncode}: {cmd}; see {log}')
    return p.stdout


def fixtures():
    # A halt opcode stops instruction fetch, not the PPU/APU or host frames.
    # Previously the oracle waited forever for another SYNC/instruction end.
    for opcode in (0x02,0x12,0x22,0x32,0x42,0x52,0x62,0x72,0x92,0xb2,0xd2,0xf2):
        prg = bytearray([0xff]) * 32768
        prg[:3] = bytes([0xa9,0x42,opcode])
        prg[-6:] = bytes([0,0x80]) * 3
        yield f'cpu_halt_{opcode:02x}', b'NES\x1a'+bytes([2,0])+bytes(10)+prg, '', 'fallback:A=42'

    # Valid NROM containing no $FF terminators. Parsing it as an AccuracyCoin
    # menu used to hang before the host ran even one instruction.
    prg = bytearray([0xea]) * 32768
    prg[:3] = bytes([0x4c, 0x00, 0x80])
    prg[-6:] = bytes([0x00, 0x80]) * 3
    yield 'nrom_no_ff', b'NES\x1a' + bytes([2, 0]) + bytes(10) + prg, '', 'A=00'

    # MMC1 modes 0, 1 and 2 can all replace $E000. Read a JMP-indirect pointer
    # there from compiled code in $8000. The reset bank would send it to $8100
    # (A=$11), while the actual selected bank points to $8200 (A=$22).
    for mode in range(3):
        prg = bytearray([0xff]) * 65536
        code = bytearray([0x78, 0xd8])
        for addr, value in ((0x8000, mode << 2), (0xe000, 1)):
            for bit in range(5):
                code.extend([0xa9, (value >> bit) & 1, 0x8d, addr & 255, addr >> 8])
        code.extend([0x6c, 0x00, 0xe1])
        prg[:len(code)] = code
        for addr, value in ((0x8100, 0x11), (0x8200, 0x22)):
            offset = addr - 0x8000
            prg[offset:offset+7] = bytes([0xa9, value, 0x85, 0x00, 0x4c, addr & 255, addr >> 8])
        prg[3*8192+0x100:3*8192+0x102] = bytes([0x00, 0x82])
        prg[7*8192+0x100:7*8192+0x102] = bytes([0x00, 0x81])
        prg[-6:] = bytes([0x00, 0x80]) * 3
        yield f'mmc1_mode{mode}', b'NES\x1a' + bytes([4, 0, 0x10, 0]) + bytes(8) + prg, \
            '00:8000\n00:8100\n00:8200\n', 'A=22'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--recompiler', required=True, type=Path)
    ap.add_argument('--interp', required=True, type=Path)
    ap.add_argument('--oracle', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--cmake', default='cmake')
    ap.add_argument('--generator')
    ap.add_argument('--config', default='Release')
    ap.add_argument('--build-timeout', type=int, default=900)
    ap.add_argument('--case-prefix', default='', help='Run only fixtures with this name prefix')
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    compiler, interp, oracle = (p.resolve() for p in (args.recompiler, args.interp, args.oracle))
    source = Path(__file__).resolve().parents[2] / 'runner/cyc'
    cmake = ['cmake_minimum_required(VERSION 3.20)', 'project(cyc_regressions C)',
             'set(CMAKE_C_STANDARD 11)', f'include("{source.as_posix()}/cyc.cmake")',
             'add_library(cyc_regression_runtime OBJECT ${NESRECOMP_CYC_SOURCES})',
             'target_include_directories(cyc_regression_runtime PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})',
             'target_compile_definitions(cyc_regression_runtime PRIVATE _CRT_SECURE_NO_WARNINGS)']
    cases = list(fixtures()) + list(mapper_fixtures()) + list(ppu_fixtures()) + list(variant_fixtures()) + list(latch_fixtures()) + list(fineprg_fixtures()) + list(vrc_fixtures()) + list(expansion_fixtures()) + list(vrc7_fixtures()) + list(bandai_fixtures())
    cases += list(mmc5_fixtures())
    cases += list(mmc1_fixtures())
    cases += list(mapper40_fixtures())
    cases += [namco108_fixture()]
    cases = [case for case in cases if case[0].startswith(args.case_prefix)]
    if not cases:
        ap.error('no matching fixtures')
    for name, image, seeds, _ in cases:
        case = out / name
        case.mkdir(exist_ok=True)
        (case / f'{name}.nes').write_bytes(image)
        (case / 'seeds.txt').write_text(seeds)
        (case / 'game.toml').write_text(f'[game]\noutput_prefix="{name}"\n'
                                      'cycle_accurate=true\ncycle_seed_file="seeds.txt"\n')
        run([compiler, f'{name}.nes', '--game', 'game.toml'], case, case / 'codegen.log')
        cmake += [f'file(GLOB {name}_GEN CONFIGURE_DEPENDS "{name}/generated/*_cyc*.c")',
                  f'add_executable({name} $<TARGET_OBJECTS:cyc_regression_runtime> ${{{name}_GEN}})',
                  f'target_include_directories({name} PRIVATE ${{NESRECOMP_CYC_INCLUDE_DIRS}})',
                  f'target_link_libraries({name} PRIVATE ${{NESRECOMP_CYC_LIBRARIES}})',
                  f'target_compile_definitions({name} PRIVATE _CRT_SECURE_NO_WARNINGS)']
    (out / 'CMakeLists.txt').write_text('\n'.join(cmake))
    command = [args.cmake, '-S', out, '-B', out / 'build', f'-DCMAKE_BUILD_TYPE={args.config}']
    if args.generator:
        command += ['-G', args.generator]
    run(command, out, out / 'configure.log')
    run([args.cmake, '--build', out / 'build', '--config', args.config, '--parallel', '4'],
        out, out / 'build.log', timeout=args.build_timeout)
    for name, _, _, expected in cases:
        final_only = expected.startswith('final:')
        expected = expected.removeprefix('final:')
        mixed = expected.startswith('mixed:')
        expected = expected.removeprefix('mixed:')
        fallback = expected.startswith('fallback:')
        expected = expected.removeprefix('fallback:')
        frames = 6 if final_only else 3
        case = out / name
        suffix = '.exe' if hasattr(subprocess, 'CREATE_NO_WINDOW') else ''
        native = out / 'build' / args.config / (name + suffix)
        if not native.exists():
            native = out / 'build' / (name + suffix)
        for align in range(4):
            hashes = {}
            for mode, exe, extra in [('native', native, []), ('interp', native, ['--interp-only']),
                                      ('standalone', interp, []), ('oracle', oracle, [])]:
                trace = case / f'a{align}_{mode}.txt'
                command = [exe, case / f'{name}.nes', '--frames', frames, '--align', align,
                           '--hash-out', trace] + extra
                if name == 'namco108_probe':
                    command += ['--mem-frame', frames - 1, '--mem-out', trace.with_suffix('.mem')]
                stdout = run(command, case, trace.with_suffix('.log'))
                if name == 'namco108_probe':
                    from namco108_probe import check_memory
                    check_memory(trace.with_suffix('.mem'))
                lines = trace.read_text().splitlines()
                checked_lines = lines[-1:] if final_only else lines
                if len(lines) != frames or any(expected not in line for line in checked_lines):
                    raise AssertionError(f'{name}, {mode}, alignment {align}: wrong result in {trace}')
                if mode == 'native' and mixed and 'interpreted: ROM' not in stdout:
                    raise AssertionError(f'{name}: expected PPU-driven PRG interpreter fallback')
                if mode == 'native' and fallback and 'native_cycles=0 ' in stdout:
                    raise AssertionError(f'{name}: did not exercise native code around RAM fallback')
                if mode == 'native' and not mixed and not fallback and '(100.0%)' not in stdout:
                    raise AssertionError(f'{name}: regression did not exercise native code')
                hashes[mode] = trace
            for mode in ('interp', 'standalone', 'oracle'):
                diff = first_difference(hashes['native'], hashes[mode], mode != 'oracle')
                if diff is not None:
                    raise AssertionError(f'{name}, {mode}, alignment {align}: {diff}')
        print(f'{name}: correct result and native/interpreter/oracle parity at all four alignments')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
