"""Whole stable raster job vs coupled clocks, including exact background state."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--probe', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    def run(mode, diagnostic=None):
        name = mode + ('-diagnostic' if diagnostic is not None else '')
        env = dict(os.environ)
        if diagnostic is not None:
            env['NESRECOMP_RASTER_DUMP_ROW'] = str(diagnostic)
        command = [str(args.probe.resolve())]
        if mode == 'reference':
            command.append('--reference')
        p = subprocess.run(command, capture_output=True, timeout=180, env=env,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        (out / (name + '.stdout')).write_bytes(p.stdout)
        (out / (name + '.stderr')).write_bytes(p.stderr)
        if p.returncode:
            raise RuntimeError(f'{mode} exit {p.returncode}; see {out / (name + ".stderr")}')
        return p.stdout.splitlines()

    before, after = run('reference'), run('candidate')
    if before != after:
        i = next((i for i, (a, b) in enumerate(zip(before, after)) if a != b),
                 min(len(before), len(after)))
        row = (i // 129) * 128 + i % 129 - 1
        run('reference', row)
        run('candidate', row)
        old = before[i] if i < len(before) else b'<missing>'
        new = after[i] if i < len(after) else b'<missing>'
        raise AssertionError(f'row {i}: {old!r} != {new!r}; diagnostics in {out}')
    if len(before) != 32 * 129:
        raise AssertionError(f'Expected 32 configurations with 128 cases, got {len(before)} rows')
    result = dict(
        cases=4096, rows=len(before),
        output_sha256=hashlib.sha256(b'\n'.join(before)).hexdigest(),
        probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest(),
        coverage=('NROM/MMC1/MMC3/GxROM, CHR ROM/RAM, four alignments, '
                  'mirroring/four-screen, scroll/fine X, palette/emphasis/greyscale, '
                  'OAM/overflow/sprite-zero, Zapper, decay deadlines, background records, '
                  'current-build serialized-state replay and full PCM'))
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n',
                                    encoding='utf-8', newline='\n')
    print('PASS: 4096 varied raster cases and current-build replay; hardware/raster/RAM/background/PCM identical')


if __name__ == '__main__':
    main()
