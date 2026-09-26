#!/usr/bin/env python3
"""Exercise an existing project's cycle opt-in and automatic regeneration."""
import argparse
from pathlib import Path
import re
import subprocess
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--cmake', default='cmake')
    ap.add_argument('--generator')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.out.resolve()
    project = out / 'existing project'
    data = project / 'game data'
    data.mkdir(parents=True, exist_ok=True)
    binary = out / 'build'
    prg = bytearray([255]) * 32768
    prg[:13] = bytes([0xa9,0,0x85,0,0xa9,0x82,0x85,1,0x6c,0,0,0xea,0xea])
    prg[0x200:0x205] = bytes([0xa9,0x42,0x4c,0,0x82])
    prg[-6:] = bytes([0,0x80]) * 3
    header = b'NES\x1a' + bytes([2,0]) + bytes(10)
    rom = data / 'test #1.nes'
    rom.write_bytes(header + prg)
    seeds = data / 'seeds.txt'
    seeds.write_text('', encoding='utf-8')
    (data / 'game.toml').write_text('[game]\ncycle_seed_file="seeds.txt"\n', newline='\n')
    (project / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\nproject(ExistingGame C)\n'
        f'include("{root.as_posix()}/runner/cyc/project.cmake")\n'
        'nesrecomp_add_cycle_game(existing_game ROM "game data/test #1.nes" '
        'GAME_CONFIG "game data/game.toml" HEADLESS)\n', newline='\n')
    def run(argv, log, success=True):
        p = subprocess.run([str(v) for v in argv], capture_output=True, text=True,
                           timeout=900, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        text = p.stdout + p.stderr
        (out / log).write_text(text, encoding='utf-8', newline='\n')
        assert (p.returncode == 0) == success, (argv, p.returncode, text[-4000:])
        return text
    configure = [args.cmake, '-S', project, '-B', binary, '-DCMAKE_BUILD_TYPE=Release']
    if args.generator:
        configure += ['-G', args.generator]
    run(configure, 'configure.log')
    build = [args.cmake, '--build', binary, '--config', 'Release', '--parallel', '2']
    suffix = '.exe' if hasattr(subprocess, 'CREATE_NO_WINDOW') else ''
    def check(stage, value, native):
        run(build, f'{stage}-build.log')
        exe = binary / 'Release' / ('existing_game' + suffix)
        if not exe.exists():
            exe = binary / ('existing_game' + suffix)
        trace = out / (stage + '.txt')
        output = run([exe, rom, '--frames', '3', '--hash-out', trace], stage + '.log')
        lines = trace.read_text().splitlines()
        assert len(lines) == 3 and all(f'A={value:02X}' in line for line in lines), lines
        percent = float(re.search(r'native_cycles=\d+ \(([0-9.]+)%\)', output).group(1))
        assert (percent == 100) == native, output
        return exe
    check('unprofiled', 0x42, False)
    time.sleep(1)
    seeds.write_text('00:8200\n', newline='\n')
    check('profiled', 0x42, True)
    time.sleep(1)
    prg[0x201] = 0x43
    rom.write_bytes(header + prg)
    exe = check('changed-rom', 0x43, True)
    time.sleep(1)
    (data / 'alternate.txt').write_text('', newline='\n')
    (data / 'game.toml').write_text('[game]\ncycle_seed_file="alternate.txt"\n', newline='\n')
    check('changed-config', 0x43, False)
    time.sleep(1)
    (data / 'alternate.txt').write_text('00:8200\n', newline='\n')
    check('changed-config-seed', 0x43, True)
    # The compiled ROM identity must reject running a different image.
    other = data / 'different.nes'
    prg[0x201] = 0x44
    other.write_bytes(header + prg)
    run([exe, other, '--frames', '3'], 'wrong-rom.log', success=False)
    # Broken seed references fail configure before exposing an unprofiled build.
    (data / 'game.toml').write_text('[game]\ncycle_seed_file="missing.txt"\n', newline='\n')
    run(configure, 'missing-seed.log', success=False)
    assert not (project / 'generated').exists()
    print('Cycle project: opt-in, native coverage, ROM/config/seed rebuilds, identity rejection, missing-seed rejection and source-tree isolation passed')


if __name__ == '__main__':
    main()
