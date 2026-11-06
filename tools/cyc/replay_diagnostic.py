#!/usr/bin/env python3
"""Reproduce an always-on diagnostic replay of the cycle host's window, headless.

The window records rolling segments (runner/cyc/cyc_replay.h): a machine
checkpoint (<stem>-NNNNNN.cycreplay.state, an ordinary save state), then per
frame the inputs the machine consumed and its state hashes afterwards. This
tool plays a segment back in the headless host of the SAME build with the
host's own options (--load-state, --input, --zapper-input, --hash-out,
--replay-exact) and compares every frame's hashes, stopping at the first
mismatch.

  replay_diagnostic.py CAPTURE --exe GAME.exe --rom ROM [options]

CAPTURE is one segment (.cycreplay), or a folder / a session stem, in which
case the newest session's segments are replayed one after another (each from
its own checkpoint). Without --exe/--rom, --check only inspects the files.

  --check               inspect only: segment table, frame counts, unfinished frame
  --segment N           only that segment of the session
  --until N             stop after N frames of each segment
  --screenshot-every K  save a PNG of every Kth frame under <work>/shots
  --work-dir DIR        a NEW directory for the run (default: replay-<time> beside CAPTURE)
  --keep-going          after a divergence continue with the next segment
  --timeout S           seconds the host may run per segment (default 900)
  --host-arg ARG        an extra host argument (repeatable), e.g. the FDS BIOS:
                        --host-arg=--fds-bios --host-arg FILE

Exit status: 0 every compared frame matched; 1 a state mismatch (the first one
is printed, and replay-divergence.cycstate holds the machine right after that
frame); 2 bad or unsupported capture, wrong program, usage; 3 the capture's
final input has no checkpoint (the frame hung or crashed in the original
run; playback executed it so it can recur under a debugger); 4 the host
failed or timed out.

Run with the Python that has no site packages needed: python -I replay_diagnostic.py ...
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
BUTTONS = [('A', 0x80), ('B', 0x40), ('SELECT', 0x20), ('START', 0x10),
           ('UP', 0x08), ('DOWN', 0x04), ('LEFT', 0x02), ('RIGHT', 0x01)]
SEGMENT_RE = re.compile(r'^(?P<stem>.+-replay-\d{8}-\d{6}-\d+)-(?P<n>\d{6})\.cycreplay$')


class CaptureError(Exception):
    pass


def buttons_text(value):
    names = [name for name, bit in BUTTONS if value & bit]
    return '+'.join(names) if names else '-'


class Segment:
    """One parsed .cycreplay file."""

    def __init__(self, path):
        self.path = Path(path)
        self.header = {}
        self.options = []
        self.inputs = []        # (frame, [b0,b1,l0,l1,l2,l3,f0,f1])
        self.pokes = []         # (frame, addr, value)
        self.drive = []         # (frame, 'E'|'I', side)
        self.zapper = []        # (frame, x, y, trigger)
        self.hashes = {}        # frame -> dict
        self.unfinished = None  # frame of an input with no hash line
        self.parse()

    def parse(self):
        try:
            text = self.path.read_text(encoding='ascii', errors='replace')
        except OSError as e:
            raise CaptureError(f'cannot read {self.path}: {e}')
        lines = text.split('\n')
        # the writer flushes whole lines but a killed process can leave a partial last one
        if lines:
            lines.pop()                      # the terminator after the last whole line, or a partial line
        if not lines or lines[0] != 'CYCREPLAY 1':
            raise CaptureError(f'{self.path.name}: not a CYCREPLAY 1 file')
        i = 1
        while i < len(lines) and lines[i] != 'BEGIN':
            key, _, value = lines[i].partition(' ')
            if key == 'OPTION':
                self.options.append(value)
            else:
                self.header[key] = value
            i += 1
        if i >= len(lines):
            raise CaptureError(f'{self.path.name}: truncated header')
        for key in ('SEGMENT', 'FIRST_FRAME', 'PROGRAM', 'REGION', 'FDS'):
            if key not in self.header:
                raise CaptureError(f'{self.path.name}: header lacks {key}')
        self.number = int(self.header['SEGMENT'])
        self.first_frame = int(self.header['FIRST_FRAME'])
        expect = self.first_frame
        open_frame = None
        for raw in lines[i + 1:]:
            f = raw.split()
            if not f:
                raise CaptureError(f'{self.path.name}: blank record')
            kind = f[0]
            try:
                if kind == 'P' and len(f) == 4:
                    self.pokes.append((int(f[1]), int(f[2], 16), int(f[3], 16)))
                elif kind == 'D' and len(f) in (3, 4) and f[2] in 'EI':
                    self.drive.append((int(f[1]), f[2], int(f[3]) if len(f) == 4 else 0))
                elif kind == 'Z' and len(f) == 5:
                    self.zapper.append((int(f[1]), int(f[2]), int(f[3]), int(f[4])))
                elif kind == 'I' and len(f) == 10:
                    if open_frame is not None:
                        raise CaptureError(f'{self.path.name}: input for frame {f[1]} while frame {open_frame} is open')
                    frame = int(f[1])
                    if frame != expect:
                        raise CaptureError(f'{self.path.name}: frame {frame} follows {expect - 1}')
                    self.inputs.append((frame, [int(x, 16) for x in f[2:]]))
                    open_frame = frame
                elif kind == 'H' and len(f) == 11:
                    frame = int(f[1])
                    if open_frame != frame:
                        raise CaptureError(f'{self.path.name}: hash for frame {frame} without its input')
                    self.hashes[frame] = {'mem': int(f[2], 16), 'hw': int(f[3], 16), 'pc': int(f[4], 16),
                                          'a': int(f[5], 16), 'x': int(f[6], 16), 'y': int(f[7], 16),
                                          's': int(f[8], 16), 'p': int(f[9], 16), 'cycles': int(f[10])}
                    open_frame = None
                    expect = frame + 1
                else:
                    raise CaptureError(f'{self.path.name}: invalid record: {raw}')
            except ValueError:
                raise CaptureError(f'{self.path.name}: unreadable record: {raw}')
        self.unfinished = open_frame

    @property
    def state_path(self):
        return self.path.with_name(self.path.name + '.state')

    @property
    def completed(self):
        return len(self.hashes)

    @property
    def last_frame(self):
        return self.inputs[-1][0] if self.inputs else self.first_frame - 1


def find_segments(target, only=None):
    """Segments of one capture, in order. target: a .cycreplay, a session stem or a folder."""
    target = Path(target)
    if target.is_file():
        m = SEGMENT_RE.match(target.name)
        if not m:
            raise CaptureError(f'{target.name} is not a replay segment name')
        if only is None:
            return [target]
        folder, stem = target.parent, m.group('stem')
    elif target.is_dir():
        folder, stem = target, None
    else:
        folder, stem = target.parent, target.name
    found = {}
    for p in folder.glob('*.cycreplay'):
        m = SEGMENT_RE.match(p.name)
        if not m or (stem and m.group('stem') != stem):
            continue
        found.setdefault(m.group('stem'), []).append((int(m.group('n')), p))
    if not found:
        raise CaptureError(f'no replay segments under {target}')
    if stem is None:
        # the session whose newest segment was written last
        stem = max(found, key=lambda s: max(p.stat().st_mtime for _, p in found[s]))
    chosen = sorted(found[stem])
    if only is not None:
        chosen = [(n, p) for n, p in chosen if n == only]
        if not chosen:
            raise CaptureError(f'session {stem} has no segment {only}')
    return [p for _, p in chosen]


def write_input_files(seg, until, work):
    """The headless host's --input / --zapper-input files for one segment."""
    names = ['', '2:', '3:', '4:']
    held = [0, 0, 0, 0]
    logical_set = [False, False]
    lines = []
    for frame, v in seg.inputs[:until]:
        b0, b1, l0, l1, l2, l3, _f0, _f1 = v
        for port, value in ((0, b0), (1, b1), (2, l2), (3, l3)):
            if held[port] != value:
                held[port] = value
                lines.append(f'{frame} {names[port]}{buttons_text(value)}')
        for k, (phys, logical) in enumerate(((b0, l0), (b1, l1))):
            if logical != phys:
                lines.append(f'{frame} L{k + 1}:{buttons_text(logical)}')
                logical_set[k] = True
            elif logical_set[k]:
                lines.append(f'{frame} L{k + 1}:=')
                logical_set[k] = False
    last = seg.inputs[min(until, len(seg.inputs)) - 1][0] if seg.inputs else seg.first_frame
    for frame, addr, value in seg.pokes:
        if frame <= last:
            lines.append(f'{frame} POKE {addr:04X} {value:02X}')
    for frame, action, side in seg.drive:
        if frame <= last:
            lines.append(f'{frame} DISK_EJECT' if action == 'E' else f'{frame} DISK_INSERT {side}')
    input_path = work / f'segment-{seg.number:06d}.input'
    input_path.write_text('# generated from ' + seg.path.name + '\n' + '\n'.join(lines) + '\n', newline='\n')
    zapper_path = None
    if seg.zapper:
        zapper_path = work / f'segment-{seg.number:06d}.zapper'
        zapper_path.write_text('\n'.join(f'{f} {x} {y} {t}' for f, x, y, t in seg.zapper if f <= last) + '\n', newline='\n')
    return input_path, zapper_path


HASH_RE = re.compile(r'^(\d+) trace=\S+ mem=(\S+) PC=(\S+) A=(\S+) X=(\S+) Y=(\S+) S=(\S+) P=(\S+) int=\S+ '
                     r'cycles=(\d+) hw=(\S+)')


def read_host_hashes(path):
    out = {}
    try:
        for raw in Path(path).read_text(errors='replace').splitlines():
            m = HASH_RE.match(raw)
            if m:
                g = m.groups()
                out[int(g[0])] = {'mem': int(g[1], 16), 'pc': int(g[2], 16), 'a': int(g[3], 16), 'x': int(g[4], 16),
                                  'y': int(g[5], 16), 's': int(g[6], 16), 'p': int(g[7], 16), 'cycles': int(g[8]),
                                  'hw': int(g[9], 16)}
    except OSError:
        pass
    return out


def stage_mods(seg, exe, work):
    root = work / f'mods-{seg.number:06d}'
    if root.exists():
        return root
    source = Path(exe).parent / 'mods'
    if source.is_dir():
        shutil.copytree(source, root, ignore=shutil.ignore_patterns('state.toml'))
    else:
        root.mkdir()
    sidecar = seg.path.with_name(seg.path.name + '.mods.toml')
    if sidecar.is_file():
        shutil.copy2(sidecar, root / 'state.toml')
    return root


def staged_name(seg):
    return f'segment-{seg.number:06d}.state'


def host_command(seg, args, work, frames_end, extra):
    cmd = [str(args.exe), str(args.rom), '--load-state', staged_name(seg), '--no-save', '--replay-exact',
           '--frames', str(frames_end)]
    if seg.header.get('HLE'):
        fields = dict(kv.split('=') for kv in seg.header['HLE'].split())
        axes = [n for n, k in (('auto-swap', 'auto_swap'), ('fast-load', 'fast_load')) if fields.get(k) == '1']
        cmd += ['--fds-hle', ','.join(axes) if axes else 'off']
    if seg.header.get('MODS') == '1':
        cmd += ['--mods-root', stage_mods(seg, args.exe, work).name]
    for option in seg.options:
        cmd += option.split(' ', 1)
    return cmd + list(args.host_arg) + extra


def run_host(cmd, work, timeout, log_name):
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', NESRECOMP_REPLAY_RECORD='0',
               NESRECOMP_DIAGNOSTICS_DIR=str(work))
    log = work / log_name
    with log.open('wb') as out:
        try:
            result = subprocess.run(cmd, cwd=work, env=env, stdout=out, stderr=subprocess.STDOUT, timeout=timeout,
                                    creationflags=NO_WINDOW)
        except subprocess.TimeoutExpired:
            return None, log
    return result.returncode, log


def diff_fields(rec, got):
    names = ('mem', 'hw', 'pc', 'a', 'x', 'y', 's', 'p', 'cycles')
    return [f'{n}: recorded {rec[n]:X} replayed {got[n]:X}' if n != 'cycles' else
            f'cycles: recorded {rec[n]} replayed {got[n]}' for n in names if rec[n] != got[n]]


def replay_segment(seg, args, work):
    """Returns (status, message) with status in ok, diverged, incomplete, error, host."""
    if not seg.state_path.is_file():
        return 'error', f'segment {seg.number}: missing checkpoint {seg.state_path.name}'
    if not seg.inputs:
        return 'error', f'segment {seg.number}: no frames were recorded'
    count = len(seg.inputs) if args.until is None else min(args.until, len(seg.inputs))
    if count <= 0:
        return 'error', '--until must be positive'
    end = seg.inputs[count - 1][0] + 1
    input_path, zapper_path = write_input_files(seg, count, work)
    # The host runs in the work directory and gets only plain relative names, so a capture under a
    # folder with non-ASCII letters (the host's argv is ANSI on Windows) still replays.
    shutil.copy2(seg.state_path, work / staged_name(seg))
    extra = ['--input', input_path.name, '--hash-out', f'segment-{seg.number:06d}.hashes']
    if zapper_path:
        extra += ['--zapper-input', zapper_path.name]
    if args.screenshot_every:
        shots = work / 'shots'
        shots.mkdir(exist_ok=True)
        extra += ['--screenshot', f'shots/segment-{seg.number:06d}.png', '--shot-every', str(args.screenshot_every)]
    cmd = host_command(seg, args, work, end, extra)
    code, log = run_host(cmd, work, args.timeout, f'segment-{seg.number:06d}.log')
    got = read_host_hashes(work / f'segment-{seg.number:06d}.hashes')
    tail = log.read_text(errors='replace')[-1500:] if log.is_file() else ''
    if code is None:
        # the host never finished: it may have hung on the frame after the last good one
        done = max(got) if got else seg.first_frame - 1
        return 'host', (f'segment {seg.number}: the host did not finish within {args.timeout}s '
                        f'(last frame completed: {done}). See {log}')
    for frame, _ in seg.inputs[:count]:
        rec = seg.hashes.get(frame)
        if rec is None:
            break                                    # the unfinished final input
        have = got.get(frame)
        if have is None:
            return 'host', (f'segment {seg.number}: the host stopped (exit {code}) before frame {frame}; '
                            f'refusal or crash. Tail of {log}:\n{tail}')
        bad = diff_fields(rec, have)
        if bad:
            state = work / f'replay-divergence-{seg.number:06d}.cycstate'
            div_extra = ['--input', input_path.name, '--save-state', f'{frame}:{state.name}']
            if zapper_path:
                div_extra += ['--zapper-input', zapper_path.name]
            run_host(host_command(seg, args, work, frame + 1, div_extra), work, args.timeout,
                     f'segment-{seg.number:06d}-divergence.log')
            saved = f'; the machine right after it is saved as {state}' if state.is_file() else ''
            return 'diverged', (f'segment {seg.number}: FIRST DIVERGENCE at frame {frame} (frame {frame - seg.first_frame + 1} '
                                f'of the segment): ' + '; '.join(bad) + saved)
    if code != 0 and len(got) < count:
        return 'host', f'segment {seg.number}: the host exited {code}. Tail of {log}:\n{tail}'
    if seg.unfinished is not None and (args.until is None or count == len(seg.inputs)):
        return 'incomplete', (f'segment {seg.number}: all {seg.completed} recorded frames matched, but frame '
                              f'{seg.unfinished} has an input and no checkpoint: it hung or crashed in the original run. '
                              f'Playback executed it (host exit {code}); rerun it under a debugger.')
    return 'ok', f'segment {seg.number}: frames {seg.first_frame}-{end - 1} matched ({count} frames)'


def describe(seg):
    first, last = seg.first_frame, seg.last_frame
    extras = []
    if seg.pokes:
        extras.append(f'{len(seg.pokes)} cheat writes')
    if seg.drive:
        extras.append(f'{len(seg.drive)} drive changes')
    if seg.zapper:
        extras.append('zapper')
    if seg.unfinished is not None:
        extras.append(f'UNFINISHED frame {seg.unfinished}')
    return (f'segment {seg.number}: frames {first}-{last} ({seg.completed} completed), start: {seg.header.get("WHY", "?")}'
            + (', ' + ', '.join(extras) if extras else ''))


def main():
    for stream in (sys.stdout, sys.stderr):           # a capture folder may have letters the console code page lacks
        stream.reconfigure(errors='backslashreplace')
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture', type=Path)
    ap.add_argument('--exe', type=Path, help='the game executable that recorded it (a headless-capable cycle host)')
    ap.add_argument('--rom', type=Path, help='the ROM / disk image the program was built for')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--segment', type=int)
    ap.add_argument('--until', type=int)
    ap.add_argument('--screenshot-every', type=int, default=0)
    ap.add_argument('--work-dir', type=Path)
    ap.add_argument('--keep-going', action='store_true')
    ap.add_argument('--timeout', type=int, default=900)
    ap.add_argument('--host-arg', action='append', default=[],
                    help='an extra argument for the host (repeatable), e.g. --host-arg=--fds-bios --host-arg FILE')
    args = ap.parse_args()
    if args.until is not None and args.until <= 0:
        ap.error('--until must be positive')
    args.capture = args.capture.resolve()      # the host runs in another directory
    try:
        paths = find_segments(args.capture, args.segment)
        segments = [Segment(p) for p in paths]
    except CaptureError as e:
        print(e, file=sys.stderr)
        return 2
    print(f'{len(segments)} segment(s) from {paths[0].parent}')
    for seg in segments:
        print(' ', describe(seg))
        if not seg.state_path.is_file():
            print(f'  missing checkpoint {seg.state_path.name}', file=sys.stderr)
            if args.check:
                return 2
    if args.check:
        return 3 if any(s.unfinished is not None for s in segments) else 0
    if not args.exe or not args.rom:
        ap.error('replaying requires --exe and --rom (or use --check)')
    args.exe, args.rom = args.exe.resolve(strict=True), args.rom.resolve(strict=True)
    work = args.work_dir.resolve() if args.work_dir else paths[0].parent / time.strftime('replay-%Y%m%d-%H%M%S')
    try:
        work.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        print(f'--work-dir {work} exists; give a new directory', file=sys.stderr)
        return 2
    print(f'reproduction directory: {work}', flush=True)
    results = set()
    for seg in segments:
        status, message = replay_segment(seg, args, work)
        print(f'{status.upper():10s} {message}', flush=True)
        results.add(status)
        if status in ('diverged', 'error', 'host') and not args.keep_going:
            break
    for status, code in (('host', 4), ('error', 2), ('diverged', 1), ('incomplete', 3)):
        if status in results:
            return code
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
