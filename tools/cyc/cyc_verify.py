#!/usr/bin/env python3
"""
cyc_verify.py - compare NESRecomp's cycle-accurate build against the oracle.

Runs the recompiled executable (and optionally its --interp-only mode) and
the TriCNES oracle on the same ROM at each CPU/PPU clock alignment, in
parallel, then compares their --hash-out files line by line:

  - against the oracle: the observable per-cycle trace, memory and picture
    (color indices), the CPU registers and the cycle count after every frame;
  - native against --interp-only: all of that plus the hardware internals
    hash, since both run NESRecomp's hardware.

Prints each run's summary line and, for any mismatch, the first differing
frame (rerun both with --trace-frame N --trace-out FILE and diff to see the
cycle).

  python cyc_verify.py --exe build/Release/AccuracyCoinRecomp.exe \\
      --oracle build/Release/cyc_oracle.exe --rom AccuracyCoin.nes --acccoin \\
      --align 0 1 2 3 --interp --out verify/

  python cyc_verify.py --exe build/Release/cyc_stress.exe \\
      --oracle build/Release/cyc_oracle.exe --rom cyc_stress.nes --frames 12000

Exit status 0 when every run matches.
"""
import argparse
import concurrent.futures
import os
import subprocess
import sys


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def summary(output):
    keep = [l for l in output.splitlines()
            if l.startswith('mode=') or l.startswith('AccuracyCoin:') or '[FAIL]' in l]
    return '\n'.join('    ' + l.strip() for l in keep)


def first_difference(a, b, with_hw):
    with open(a) as fa, open(b) as fb:
        la, lb = fa.readlines(), fb.readlines()
    if not with_hw:
        la = [l.split(' hw=')[0] for l in la]
        lb = [l.split(' hw=')[0] for l in lb]
    for i, (x, y) in enumerate(zip(la, lb)):
        if x != y:
            return i, x.rstrip(), y.rstrip()
    if len(la) != len(lb):
        n = min(len(la), len(lb))
        return n, f'{len(la)} frames', f'{len(lb)} frames'
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', required=True, help='recompiled host executable')
    ap.add_argument('--oracle', required=True, help='cyc_oracle executable')
    ap.add_argument('--rom', required=True)
    ap.add_argument('--acccoin', action='store_true', help='drive AccuracyCoin to completion')
    ap.add_argument('--input', help='button schedule to drive both runs with (cyc_host.c --input); '
                                    'needed for a game whose interesting behavior is past a title screen')
    ap.add_argument('--ram-init', choices=['pattern', 'zeros', 'ones'],
                    help='CPU RAM at power-on, for both runs')
    ap.add_argument('--frames', type=int, help='frames to run (a limit with --acccoin)')
    ap.add_argument('--align', type=int, nargs='+', default=[0, 1, 2, 3])
    ap.add_argument('--interp', action='store_true', help='also check the --interp-only mode')
    ap.add_argument('--out', default='cyc_verify_out')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    common = [args.rom]
    if args.acccoin:
        common.append('--acccoin')
    if args.frames is not None:
        common += ['--frames', str(args.frames)]
    if args.input:
        common += ['--input', os.path.abspath(args.input)]
    if args.ram_init:
        common += ['--ram-init', args.ram_init]
    name = os.path.splitext(os.path.basename(args.rom))[0]

    runs = {}
    for align in args.align:
        modes = [('oracle', args.oracle, []), ('native', args.exe, [])]
        if args.interp:
            modes.append(('interp', args.exe, ['--interp-only']))
        for mode, exe, extra in modes:
            hash_file = os.path.join(args.out, f'{name}_a{align}_{mode}.txt')
            cmd = [exe] + common + ['--align', str(align), '--hash-out', hash_file] + extra
            runs[(align, mode)] = (cmd, hash_file)

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run, cmd): key for key, (cmd, _) in runs.items()}
        for f in concurrent.futures.as_completed(futures):
            results[futures[f]] = f.result()

    ok = True
    for align in args.align:
        print(f'alignment {align}:')
        hashes = {mode: runs[(align, mode)][1] for mode in ['oracle', 'native', 'interp'] if (align, mode) in runs}
        for mode in hashes:
            code, output = results[(align, mode)]
            print(f'  {mode}:')
            print(summary(output))
            checks = [] if mode == 'oracle' else [('the oracle', 'oracle', False)]
            if mode == 'interp':
                checks.append(('native', 'native', True))
            for label, other, with_hw in checks:
                diff = first_difference(hashes[other], hashes[mode], with_hw)
                what = ' (hardware internals included)' if with_hw else ''
                if diff is None:
                    print(f'    matches {label} on every frame{what}')
                else:
                    ok = False
                    frame, x, y = diff
                    print(f'    DIFFERS from {label} at frame {frame}{what}:')
                    print(f'      {other}: {x}')
                    print(f'      {mode}:  {y}')
    print('ALL MATCH' if ok else 'MISMATCH')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
