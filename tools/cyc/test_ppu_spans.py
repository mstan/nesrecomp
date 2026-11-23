"""Compare native-span machine state, pixels and PCM with the general PPU."""
import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', required=True)
    parser.add_argument('--reference', required=True)
    args = parser.parse_args()
    outputs = []
    for exe in (args.reference, args.candidate):
        result = subprocess.run([exe], capture_output=True,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        if result.returncode:
            raise AssertionError((exe, result.returncode, result.stderr.decode(errors='replace')))
        outputs.append(result.stdout.splitlines())
    for index, (before, after) in enumerate(zip(*outputs)):
        if before != after:
            raise AssertionError(f'first divergence at row {index}: {before!r} != {after!r}')
    assert len(outputs[0]) == len(outputs[1]), 'different checkpoint counts'
    print('PASS: all nine NTSC/PAL alignment cases, delayed register traffic, DMA, '
          'Zapper, mid-span replay, full-state checkpoints, pictures and PCM match')


if __name__ == '__main__':
    main()
