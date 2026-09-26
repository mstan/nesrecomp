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
import re
import subprocess
import sys


def run(cmd):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    except OSError as e:
        return None, str(e)
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


def run_error(code, output, hash_file, args):
    """Require a completed run before accepting equality of its trace."""
    completed = re.search(r'^mode=\S+ frames=(\d+) cycles=\d+.*$', output, re.MULTILINE)
    if completed is None or 'TIMEOUT' in completed.group(0):
        return f'run did not complete (exit {code})'
    frames = int(completed.group(1))
    if frames == 0:
        return 'run produced no frames'
    if args.acccoin:
        score = re.search(r'^AccuracyCoin: (\d+)/(\d+) passed, (\d+) failed, '
                          r'(\d+) skipped, (\d+) not run$', output, re.MULTILINE)
        if score is None:
            return 'missing AccuracyCoin result'
        passed, total, failed, skipped, not_run = map(int, score.groups())
        if not total or not_run or passed + failed + skipped != total:
            return 'AccuracyCoin suite did not finish'
        # Exit 1 denotes failed hardware tests, including the documented
        # alignment-dependent oracle failures. They may still match the oracle.
        expected_code = 1 if failed else 0
    else:
        expected_code = 0
        if frames != (args.frames if args.frames is not None else 600):
            return f'run stopped early after {frames} frames'
    if code != expected_code:
        return f'unexpected exit {code} (expected {expected_code})'
    try:
        with open(hash_file) as f:
            lines = f.readlines()
    except OSError as e:
        return f'cannot read trace: {e}'
    if len(lines) != frames or any(not line.startswith(f'{i} trace=') for i, line in enumerate(lines)):
        return f'incomplete trace: expected {frames} consecutive frames, got {len(lines)} lines'
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
    if args.frames is not None and args.frames <= 0:
        ap.error('--frames must be positive')

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
            # A failed launch/load must never reuse a previous successful run.
            if os.path.exists(hash_file):
                os.remove(hash_file)
            cmd = [exe] + common + ['--align', str(align), '--hash-out', hash_file] + extra
            runs[(align, mode)] = (cmd, hash_file)

    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run, cmd): key for key, (cmd, _) in runs.items()}
        for f in concurrent.futures.as_completed(futures):
            results[futures[f]] = f.result()

    errors = {}
    for key, (code, output) in results.items():
        hash_file = runs[key][1]
        with open(hash_file + '.log', 'w') as f:
            f.write(output)
        errors[key] = run_error(code, output, hash_file, args)

    ok = not any(errors.values())
    for align in args.align:
        print(f'alignment {align}:')
        hashes = {mode: runs[(align, mode)][1] for mode in ['oracle', 'native', 'interp'] if (align, mode) in runs}
        for mode in hashes:
            code, output = results[(align, mode)]
            print(f'  {mode}:')
            print(summary(output))
            if errors[(align, mode)]:
                print(f'    ERROR: {errors[(align, mode)]}')
                print(f'    see {hashes[mode]}.log')
                continue
            checks = [] if mode == 'oracle' else [('the oracle', 'oracle', False)]
            if mode == 'interp':
                checks.append(('native', 'native', True))
            for label, other, with_hw in checks:
                if errors[(align, other)]:
                    continue
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
    print('ALL MATCH' if ok else 'MISMATCH OR RUN FAILURE')
    if ok and args.acccoin:
        print('Trace comparison only; hardware test scores are reported above.')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
