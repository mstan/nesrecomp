"""Compare memory-only native service with per-access isolation at every boundary."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    outputs = []
    for mode in ('reference', 'candidate'):
        command = [str(args.probe.resolve())]
        if mode == 'reference':
            command.append('--reference')
        result = subprocess.run(command, capture_output=True, timeout=180,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        (args.out / (mode + '.stdout')).write_bytes(result.stdout)
        (args.out / (mode + '.stderr')).write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError(f'{mode} failed; see {args.out / (mode + ".stderr")}')
        outputs.append(result.stdout.splitlines())
    before, after = outputs
    if before != after:
        index = next((i for i, (a, b) in enumerate(zip(before, after)) if a != b),
                     min(len(before), len(after)))
        raise AssertionError(f'First differing service checkpoint {index}; see {args.out}')
    if len(before) != 5120:
        raise AssertionError(f'Expected 5120 checkpoints, got {len(before)}')
    data = dict(cases=len(before),
                output_sha256=hashlib.sha256(b'\n'.join(before)).hexdigest(),
                probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest())
    (args.out / 'results.json').write_text(json.dumps(data, indent=2) + '\n',
                                          encoding='utf-8', newline='\n')
    print('PASS: isolated native service, budget boundaries, observers and authoritative restoration')


if __name__ == '__main__':
    main()
