#!/usr/bin/env python3
"""Reject unsupported NES 2.0 variants consistently in compiler and both hosts."""
import argparse
from pathlib import Path
import subprocess

from mapper_fixtures import mapper_fixtures


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--recompiler', type=Path, required=True)
    ap.add_argument('--interp', type=Path, required=True)
    ap.add_argument('--oracle', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    checked = set()
    count = 0
    for _, image, _, _ in mapper_fixtures():
        mapper = (image[6] >> 4) | (image[7] & 0xf0)
        if mapper in checked:
            continue
        checked.add(mapper)
        for variant, byte8 in [('unspecified', 0), ('submapper', 0x10), ('extended_id', 1)]:
            case = out / f'mapper{mapper}_{variant}'
            case.mkdir(exist_ok=True)
            rom = bytearray(image)
            rom[7] |= 8
            rom[8] = byte8
            (case / 'test.nes').write_bytes(rom)
            (case / 'game.toml').write_text('[game]\noutput_prefix="test"\ncycle_accurate=true\n')
            for name, exe, extra in [
                ('compiler', args.recompiler, ['--game', 'game.toml']),
                ('interp', args.interp, ['--frames', '1']),
                ('oracle', args.oracle, ['--frames', '1']),
            ]:
                p = subprocess.run([str(exe.resolve()), 'test.nes', *extra], cwd=case,
                                   capture_output=True, text=True, timeout=30,
                                   creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                (case / f'{name}.log').write_text(p.stdout + p.stderr)
                if p.returncode == 0 or not any(word in p.stderr for word in ('NES 2.0', 'cannot load')):
                    raise AssertionError(f'{case.name}: {name} did not reject unsupported header cleanly')
                count += 1
    print(f'{count} unsupported-header checks passed ({len(checked)} mapper IDs)')


if __name__ == '__main__':
    main()
