"""Compare active native device intervals with the coupled master clock."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from prepare_project import native_interval_board


def check_build_time_boards():
    with tempfile.TemporaryDirectory(prefix='nes-native-board-') as folder:
        image = Path(folder) / 'fixture.nes'
        for mapper, expected in ((0, '1'), (1, '1'), (4, '1'), (66, '1'),
                                 (48, '0'), (256, '0'), (257, '0'), (260, '0'), (322, '0')):
            header = bytearray(16)
            header[:4] = b'NES\x1a'
            header[6] = (mapper & 15) << 4
            header[7] = (mapper & 0xf0) | 0x08
            header[8] = mapper >> 8
            image.write_bytes(header)
            assert native_interval_board(image, False) == expected, mapper
        image.write_bytes(b'not an iNES image')
        assert native_interval_board(image, False) == '0'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True)
    args = parser.parse_args()
    check_build_time_boards()
    for modes in (('--reference', ''), ('--cpu-reference', '--cpu')):
        outputs = []
        for mode in modes:
            result = subprocess.run([args.probe, *([mode] if mode else [])], capture_output=True,
                                    timeout=120,
                                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            if result.returncode:
                raise AssertionError((mode, result.returncode, result.stderr.decode(errors='replace')))
            outputs.append(result.stdout.splitlines())
        for index, (before, after) in enumerate(zip(*outputs)):
            if before != after:
                raise AssertionError(f'{modes}: first divergence at row {index}: {before!r} != {after!r}')
        assert len(outputs[0]) == len(outputs[1]), 'different checkpoint counts'
    print('PASS: active NROM/MMC1/MMC3/GxROM intervals, four NTSC alignments, device/bus/memory/'
          'picture checkpoints and PCM match; DMA, IRQ, delayed-register edges, '
          'trace and PAL refusal, CPU RAM/folded-ROM cursor and snapshot replay, '
          'settled VBlank, held-high NMI and idle bus-decay deadlines exercised')


if __name__ == '__main__':
    main()
