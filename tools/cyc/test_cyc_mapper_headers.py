#!/usr/bin/env python3
"""Accept known NES 2.0 boards and reject unknown IDs/variants in all loaders."""
import argparse
from pathlib import Path
import subprocess
from itertools import chain

from mapper_fixtures import mapper_fixtures
from latch_fixtures import latch_fixtures
from fineprg_fixtures import fineprg_fixtures
from vrc_fixtures import vrc_fixtures
from expansion_fixtures import expansion_fixtures
from vrc7_fixtures import vrc7_fixtures
from bandai_fixtures import bandai_fixtures
from mmc5_fixtures import mmc5_fixtures
from mmc1_fixtures import mmc1_fixtures
from mapper40_fixtures import mapper40_fixtures


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
    for _, image, _, _ in chain(mapper_fixtures(), latch_fixtures(), fineprg_fixtures(), vrc_fixtures(), expansion_fixtures(), vrc7_fixtures(), bandai_fixtures(), mmc5_fixtures(), mmc1_fixtures(), mapper40_fixtures()):
        mapper = (image[6] >> 4) | (image[7] & 0xf0)
        if mapper in checked:
            continue
        checked.add(mapper)
        for variant, byte8, accepted in [('unspecified', 0, True), ('submapper', 0xf0, False), ('extended_id', 1, False)]:
            case = out / f'mapper{mapper}_{variant}'
            case.mkdir(exist_ok=True)
            rom = bytearray(image)
            rom[7] |= 8
            rom[8] = byte8
            if not rom[5]:
                rom[11] = 8 if mapper == 13 else 7
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
                if (p.returncode == 0) != accepted:
                    raise AssertionError(f'{case.name}: {name} header acceptance was {p.returncode}, expected {accepted}')
                count += 1
    print(f'{count} header acceptance/rejection checks passed ({len(checked)} mapper IDs)')


if __name__ == '__main__':
    main()
