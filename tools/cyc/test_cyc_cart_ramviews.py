#!/usr/bin/env python3
"""Synthetic SNROM copied code: native/interpreter bus, memory and PCM parity.

No copyrighted ROM or FDS workload. Build fresh generated code from declared
ROM-to-SRAM copies, then exercise self-modification and changing read gates.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import sys

import nested_build
from test_cyc_ramviews import ram_from_mem, stats


def fixture(kind):
    prg = bytearray([0xEA] * 32768)
    code = bytearray()
    def put(*b): code.extend(b)
    def serial(addr, value):
        for bit in range(5): put(0xA9, (value >> bit) & 1, 0x8D, addr & 255, addr >> 8)
    put(0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA9, 0x80, 0x8D, 0, 0x80)
    put(0xA9, 0, 0x85, 0x31, 0x85, 0x32, 0x85, 0x33, 0x85, 0x40)
    serial(0x8000, 0x1C if kind == 'a12' else 0x0C)
    put(0xA2, 0)
    copy = len(code)
    put(0xBD, 0, 0xA0, 0x9D, 0, 0x60, 0xE8, 0xD0, ((copy - len(code) - 9) & 255))
    if kind == 'a12': serial(0xC000, 0x10)
    # NTSC power-on PPU writes can be ignored; wait over two frames first.
    put(0xA0, 0x60, 0xA2, 0)
    delay = len(code)
    put(0xCA, 0xD0, 0xFD, 0x88, 0xD0, ((delay - len(code) - 6) & 255))
    put(0xA9, 0x90, 0x8D, 0, 0x20, 0xA9, 0x1E, 0x8D, 1, 0x20)
    put(0xA9, 0xC0, 0x8D, 0x17, 0x40)
    if kind == 'selfmod':
        put(0xA9, 0x4F, 0x8D, 0x10, 0x40, 0xA9, 8, 0x8D, 0x12, 0x40,
            0xA9, 1, 0x8D, 0x13, 0x40, 0xA9, 0x10, 0x8D, 0x15, 0x40)
    loop = 0x8000 + len(code)
    if kind in ('selfmod','stable'): put(0xA9, 0x22, 0x8D, 9, 0x60)
    put(0x20, 0, 0x60, 0xE6, 0x32, 0x4C, loop & 255, loop >> 8)
    prg[:len(code)] = code
    if kind == 'gate':
        ram = bytearray()
        for bit in range(5): ram.extend((0xA9, int(bit == 4), 0x8D, 0, 0xA0))
        ram.extend((0xA9, 0x55, 0x85, 0x40, 0x60))
    else:
        ram = bytes((0xEE, 9, 0x60, 0xA9, 2, 0x8D, 0x14, 0x40,
                     0xA9, 0x22, 0x85, 0x31, 0xAD, 0x16, 0x40, 0x60))
    prg[0x2000:0x2100] = bytes(ram).ljust(256, b'\xEA')
    prg[0x4100:0x4105] = b'\xE6\x33\x40\xEA\x02'  # NMI: INC $33; RTI, IRQ: jam
    prg[0x4200:0x4300] = bytes(range(256))
    prg[0x4300:0x4306] = bytes((0xA9, 0x23, 0x8D, 9, 0x60, 0x60))
    prg[-6:] = bytes((0, 0xC1, 0, 0x80, 4, 0xC1))
    header = bytearray(b'NES\x1a' + bytes((2, 0, 0x12, 0, 2 if kind == 'banked' else 1)) + bytes(7))
    return header + prg


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--recompiler', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2] / 'runner/cyc')
    ap.add_argument('--ppu', choices=('HLE', 'LLE'), default='HLE')
    ap.add_argument('--parallel', type=int, default=1)
    nested_build.add_arguments(ap)
    args = ap.parse_args()
    tc = nested_build.Toolchain.from_args(args)
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    source = args.source.resolve()
    def run(cmd, cwd, log):
        p = subprocess.run([str(x) for x in cmd], cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=300,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        log.write_text(p.stdout, encoding='utf-8')
        if p.returncode: raise RuntimeError(f'exit {p.returncode}; see {log}')
        return p.stdout
    cmake = ['cmake_minimum_required(VERSION 3.20)', 'project(cart_ramviews C)',
             'set(CMAKE_C_STANDARD 11)', 'add_compile_definitions(NESRECOMP_NATIVE_INTERVALS=1)',
             f'include("{source.as_posix()}/cyc.cmake")',
             'set(RUNTIME_SOURCES ${NESRECOMP_CYC_SOURCES})',
             'list(REMOVE_ITEM RUNTIME_SOURCES "${NESRECOMP_CYC_DIR}/cyc_host.c")',
             'add_library(runtime OBJECT ${RUNTIME_SOURCES})',
             'target_include_directories(runtime PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})',
             'target_compile_definitions(runtime PRIVATE CYC_HOST_EXTRAS_STRONG)']
    cases = ('selfmod', 'stable', 'gate', 'a12', 'banked')
    for kind in cases:
        folder = out / kind; folder.mkdir(exist_ok=True)
        (folder / 'test.nes').write_bytes(fixture(kind))
        (folder / 'game.toml').write_text('[game]\noutput_prefix="cart"\ncycle_accurate=true\n'
                                          '\n[[sram_map]]\nsram_start=0x6000\nrom_start=0xA000\nbank=0\nsize=0x100\n')
        run([args.recompiler.resolve(), 'test.nes', '--game', 'game.toml'], folder, folder / 'codegen.log')
        cmake += [f'file(GLOB GEN_{kind} CONFIGURE_DEPENDS "{kind}/generated/*_cyc*.c")',
                  f'add_executable({kind} ${{GEN_{kind}}} "${{NESRECOMP_CYC_DIR}}/cyc_host.c" $<TARGET_OBJECTS:runtime>)',
                  f'target_include_directories({kind} PRIVATE ${{NESRECOMP_CYC_INCLUDE_DIRS}})',
                  f'target_link_libraries({kind} PRIVATE ${{NESRECOMP_CYC_LIBRARIES}})']
    cmake += [f'add_executable(guards "{source.as_posix()}/tests/cart_ramview_probe.c" ${{GEN_selfmod}} $<TARGET_OBJECTS:runtime>)',
              'target_include_directories(guards PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})',
              'target_link_libraries(guards PRIVATE ${NESRECOMP_CYC_LIBRARIES})']
    (out / 'CMakeLists.txt').write_text('\n'.join(cmake) + '\n')
    tc.build(out, out / 'build', out, parallel=args.parallel, defines=(f'-DNESRECOMP_PPU_IMPL={args.ppu}',))
    report = {}
    for kind in cases:
        folder = out / kind; exe = tc.executable(out / 'build', kind)
        rows = []
        for align in range(4):
            traces = {}; pcm = {}
            for mode in ('native', 'interp'):
                base = folder / f'a{align}-{mode}'
                cmd = [exe, folder / 'test.nes', '--frames', '40', '--align', align,
                       '--hash-out', base.with_suffix('.hash'), '--mem-frame', '39',
                       '--mem-out', base.with_suffix('.mem'), '--wav-out', base.with_suffix('.wav'), '--no-save']
                if mode == 'interp': cmd += ['--interp-only']
                log = run(cmd, folder, base.with_suffix('.log'))
                traces[mode] = base.with_suffix('.hash').read_bytes()
                pcm[mode] = hashlib.sha256(base.with_suffix('.wav').read_bytes()).hexdigest()
                if mode == 'native': rows.append(stats(log if '\n  native:' in log else log+'\nnative: ROM 0 (0.0%)  RAM views 0'))
                ram = ram_from_mem(base.with_suffix('.mem'))
                if kind in ('selfmod','stable'):
                    assert ram[0x31] == 0x23, (kind, align, mode, ram[0x31])
                    assert ram[0x33], 'NMI handler never ran'
                if kind == 'gate': assert not ram[0x40], 'code ran beyond the closed read gate'
            assert traces['native'] == traces['interp'], (kind, align, 'bus/memory/hardware parity')
            assert pcm['native'] == pcm['interp'], (kind, align, 'PCM parity')
        if kind in ('selfmod','stable'):
            for row in rows:
                assert all(row[k] > 0 for k in ('native_ram','entries','invalidated','exits','rejected')), row
        if kind in ('a12', 'banked'): assert all(not row['entries'] for row in rows), rows
        if kind == 'banked': assert all(not row['usable'] for row in rows), rows
        if kind == 'gate': assert all(row['entries'] for row in rows), rows
        report[kind] = rows
        print(kind, 'four alignments: bus/memory/hardware and PCM identical', rows[0], flush=True)
    # Trace hashing intentionally uses the coupled clock. Also compare the
    # unobserved deadline executor with interpreter output at every alignment.
    folder = out / 'stable'; exe = tc.executable(out / 'build', 'stable')
    for align in range(4):
        hashes = {}
        for mode in ('native', 'interp'):
            base = folder / f'fast-a{align}-{mode}'
            cmd = [exe, folder / 'test.nes', '--frames', '40', '--align', align, '--no-save',
                   '--state-out', base.with_suffix('.hw'), '--state-frame', '39',
                   '--mem-out', base.with_suffix('.mem'), '--mem-frame', '39',
                   '--screenshot', base.with_suffix('.png'), '--wav-out', base.with_suffix('.wav')]
            if mode == 'interp': cmd += ['--interp-only']
            log = run(cmd, folder, base.with_suffix('.log'))
            hashes[mode] = [hashlib.sha256(base.with_suffix(ext).read_bytes()).hexdigest() for ext in ('.hw','.mem','.png','.wav')]
            if mode == 'native':
                m = re.search(r'native_intervals: cycles=(\d+)', log)
                assert m and int(m[1]) > 0, 'SRAM deadline cursor was never used'
        assert hashes['native'] == hashes['interp'], (align, 'untraced SRAM deadline parity')
    print('untraced SRAM deadline cursor: four alignments, hardware/memory/raster/PCM identical', flush=True)
    run([tc.executable(out / 'build', 'guards'), out / 'selfmod/test.nes'], out, out / 'guards.log')
    print((out / 'guards.log').read_text(), flush=True)
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__': main()
