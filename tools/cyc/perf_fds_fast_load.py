#!/usr/bin/env python3
"""Fast load's wall-clock effect: a local perf check, not CTest (wall-clock time
depends on machine load; CTest cyc_fds_hle_test checks the deterministic part:
which frames the host runs unpaced, and that the machine is unchanged).

Runs a program twice with --realtime (paced at 60.0988 Hz like the window):
the HLE list without fast load, then with it, and compares the host's reported
wall-clock time of the load frames. Paced loads should take about their 60 fps
time; fast loads well under half of the paced time.

  python tools/cyc/perf_fds_fast_load.py EXE IMAGE --bios disksys.rom [--frames 800]
      [--hle auto-swap] [--min-paced 0.9] [--max-fast 0.5]
  python tools/cyc/perf_fds_fast_load.py build/cyc/cyc_interp.exe --fixtures OUT
      (the synthetic HLE fixtures: tools/cyc/fds_hle_fixtures.py, disk4_7)
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def run(exe, image, bios, frames, hle):
    cmd = [str(exe)] + ([str(image)] if image else []) + (['--fds-bios', str(bios)] if bios else [])
    cmd += ['--frames', str(frames), '--realtime', '--fds-hle', hle]
    p = subprocess.run(cmd, capture_output=True, text=True, creationflags=NO_WINDOW)
    m = re.search(r'(\d+) load frames \((\d+) unpaced\) \(([\d.]+) s at 60 fps\) took ([\d.]+) s', p.stdout)
    if p.returncode or not m:
        raise SystemExit(f'{cmd}: exit {p.returncode}\n{p.stdout}{p.stderr}')
    return int(m.group(1)), float(m.group(3)), float(m.group(4))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('exe', type=Path)
    ap.add_argument('image', type=Path, nargs='?')
    ap.add_argument('--bios', type=Path)
    ap.add_argument('--fixtures', type=Path, help='write the synthetic HLE fixtures here and use disk4_7')
    ap.add_argument('--frames', type=int, default=240)
    ap.add_argument('--hle', default='auto-swap', help='the HLE list both runs use (fast-load is added)')
    ap.add_argument('--min-paced', type=float, default=0.9)
    ap.add_argument('--max-fast', type=float, default=0.5)
    args = ap.parse_args()
    if args.fixtures:
        subprocess.run([sys.executable, str(HERE / 'fds_hle_fixtures.py'), '--out', str(args.fixtures)], check=True)
        args.image, args.bios = args.fixtures / 'disk4_7.fds', args.fixtures / 'bios' / 'hle.rom'
    frames, nominal, paced = run(args.exe, args.image, args.bios, args.frames, args.hle)
    frames2, _, fast = run(args.exe, args.image, args.bios, args.frames, args.hle + ',fast-load')
    print(f'{frames} load frames ({nominal:.2f} s at 60 fps): paced {paced:.2f} s, fast load {fast:.2f} s '
          f'({100 * fast / paced if paced else 0:.0f}% of paced)')
    ok = frames == frames2 and paced >= args.min_paced * nominal and fast < args.max_fast * paced
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
