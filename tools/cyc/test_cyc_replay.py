#!/usr/bin/env python3
"""Diagnostic replay acceptance (CTest cyc_replay): record a hidden window session,
play every segment back headless, and require exact per-frame hash equality.

Nothing reaches the desktop: the window is hidden on SDL's dummy video driver and
driven over its TCP server; every file goes under --out (never %TEMP%).

  nes       a synthetic NROM program that folds both pads into RAM. The session holds
            buttons on both ports, writes RAM over TCP (a QA cheat), saves a state,
            LOADS it mid-session (a new segment), and runs past several 40-frame
            segment rollovers. Checks: segment files and header facts; every segment
            replays with identical hashes; the contiguous segments before the load
            equal a plain power-on headless run of the same inputs (recording and its
            checkpoints do not perturb the machine); a corrupted input, a corrupted
            hash, a different program and a truncated final frame are each detected
            with the documented exit status and a divergence state. The capture
            folder has non-ASCII letters in its name on purpose.
  fds       the HLE test disk of tools/cyc/fds_hle_fixtures.py: the drive is ejected and
            side B inserted over TCP (drive changes before frames), an FDS HLE setting
            changes mid-session (a new segment), then everything is replayed; without
            the drive lines the replay must diverge.
  zapper    a Zapper on port 2 aimed and fired over TCP, replayed; without the aim
            lines the replay must diverge.
  overflow  a writer that stops answering: the bounded queue fills, recording stops
            loudly (log line, ring event), the game never waits.
  off       NESRECOMP_REPLAY_RECORD=0 writes nothing.
  cap       a tiny NESRECOMP_REPLAY_HISTORY_MIB evicts the oldest segments, keeps the
            newest within the budget, and the survivors still replay.
  mods      (--window-mods: the same window built with mods, as a title is) the framework
            package "Replay capture" is staged, on by default, and records; turned off
            at run time through the Mods path (TCP mod_feature: feature_enable, commit,
            re-activation) the current segment ends whole and nothing more is written
            (even across rollovers); turned on again it opens a new segment and every
            segment still replays exactly; the selection persists in mods/state.toml
            (a later launch with it off creates no file and no folder; enabling it then
            starts a first segment); NESRECOMP_REPLAY_RECORD=0 wins over an enabled
            feature; a catalog without the package records.

python tools/cyc/test_cyc_replay.py --window build/cyc_window.exe --out build/replay_test
"""
import argparse
import atexit
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
SEGMENT_FRAMES = 40
BELOW_NORMAL = getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0)   # other sessions share this machine
MOD_PACKAGE, MOD_FEATURE = 'nesrecomp.developer.replay', 'replay-capture'
checks = 0
OPEN = []


def _kill_open():
    for p in OPEN:
        if p.poll() is None:
            p.kill()


atexit.register(_kill_open)


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        raise AssertionError(what)


spec = importlib.util.spec_from_file_location('replay_diagnostic', HERE / 'replay_diagnostic.py')
rd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rd)


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def make_nes_rom(path, variant=0):
    path.write_bytes(nes_rom_bytes(variant))


def nes_rom_bytes(variant=0):
    """NROM: NMI every frame; reads both pads and folds them into RAM $00-$13."""
    code = bytearray()

    def emit(*b):
        code.extend(b)
    emit(0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA9, 0x80, 0x8D, 0x00, 0x20)     # SEI CLD LDX #$FF TXS LDA #$80 STA $2000
    loop = 0xC000 + len(code)
    emit(0xE6, 0x14 + variant, 0x4C, loop & 255, loop >> 8)               # INC $14: JMP loop
    nmi = 0xC000 + len(code)
    emit(0xA9, 0x01, 0x8D, 0x16, 0x40, 0xA9, 0x00, 0x8D, 0x16, 0x40)      # strobe
    emit(0xA2, 0x08, 0xA9, 0x00, 0x85, 0x00, 0x85, 0x01)                   # LDX #8 LDA #0 STA $00 STA $01
    rd_at = len(code)
    emit(0xAD, 0x16, 0x40, 0x4A, 0x26, 0x00)                               # LDA $4016 LSR ROL $00
    emit(0xAD, 0x17, 0x40, 0x4A, 0x26, 0x01)                               # LDA $4017 LSR ROL $01
    emit(0xCA, 0xD0, (rd_at - (len(code) + 2)) & 255)                      # DEX BNE rd
    emit(0xA5, 0x00, 0x18, 0x65, 0x10, 0x85, 0x10)                         # LDA $00 CLC ADC $10 STA $10
    emit(0x45, 0x11, 0x85, 0x11)                                           # EOR $11 STA $11
    emit(0xA5, 0x01, 0x65, 0x13, 0x85, 0x13)                               # LDA $01 ADC $13 STA $13
    emit(0xE6, 0x12, 0x40)                                                 # INC $12 RTI
    prg = bytearray(code) + bytes(16384 - len(code))
    for off, addr in ((0x3FFA, nmi), (0x3FFC, 0xC000), (0x3FFE, 0xC000)):
        prg[off], prg[off + 1] = addr & 255, addr >> 8
    chr_rom = bytes((i * 7 + 3) & 255 for i in range(8192))
    return b'NES\x1a' + bytes([1, 1, 0, 0]) + bytes(8) + bytes(prg) + chr_rom


class Window:
    def __init__(self, exe, image, out, tag, env_extra=None, args=()):
        self.out = out / tag
        if self.out.exists():
            shutil.rmtree(self.out)
        self.out.mkdir(parents=True)
        self.rec = self.out / 'rec-é中'      # non-ASCII on purpose: paths are UTF-8 end to end
        (self.out / 'config.ini').write_text('[General]\n', newline='\n')
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', NESRECOMP_NO_LAUNCHER='1',
                   NESRECOMP_REPLAY_DIR=str(self.rec), NESRECOMP_DIAGNOSTICS_DIR=str(self.out / 'diag'),
                   NESRECOMP_REPLAY_SEGMENT_FRAMES=str(SEGMENT_FRAMES))
        env.pop('NESRECOMP_REPLAY_RECORD', None)
        env.pop('NESRECOMP_FDS_HLE', None)
        env.update(env_extra or {})
        self.port = free_port()
        self.log = open(self.out / 'stdout.log', 'w')
        self.proc = subprocess.Popen([str(exe), str(image), '--config', str(self.out / 'config.ini'), '--tcp', str(self.port),
                                      '--hidden', '--no-save', *args], cwd=self.out, env=env, stdout=self.log,
                                     stderr=subprocess.STDOUT, creationflags=NO_WINDOW | BELOW_NORMAL)
        OPEN.append(self.proc)
        self.sock, self.buf, self.next_id = None, b'', 1
        deadline = time.time() + 30
        while time.time() < deadline and self.sock is None:
            try:
                self.sock = socket.create_connection(('127.0.0.1', self.port), timeout=10)
            except OSError:
                if self.proc.poll() is not None:
                    raise RuntimeError(f'{tag}: the window exited {self.proc.returncode}; see {self.out}/stdout.log')
                time.sleep(0.1)
        check(self.sock is not None, f'{tag}: no TCP server')

    def cmd(self, _cmd, **fields):
        fields.update(cmd=_cmd, id=self.next_id)
        self.next_id += 1
        self.sock.sendall((json.dumps(fields) + '\n').encode())
        while b'\n' not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError('the TCP server closed')
            self.buf += chunk
        line, self.buf = self.buf.split(b'\n', 1)
        r = json.loads(line)
        check(r.get('id') == fields['id'], f'reply id {r}')
        check(r.get('ok'), f'{_cmd} {fields}: {r}')
        return r

    def frame(self):
        return self.cmd('state')['frame']

    def wait_frames(self, n):
        start = self.frame()
        deadline = time.time() + 120
        while self.frame() < start + n:
            check(time.time() < deadline, f'{n} frames did not run')
            time.sleep(0.01)

    def wait_until_frame(self, target):
        deadline = time.time() + 120
        while self.frame() < target:
            check(time.time() < deadline, f'frame {target} not reached')
            time.sleep(0.01)

    def hold(self, name, frames, player=1):
        """Hold a button for `frames` frames (players are 1-based, as the TCP action wants)."""
        self.cmd('action', name=name, player=player, frames=frames)
        self.wait_frames(frames + 2)

    def quit(self):
        self.cmd('quit')
        self.sock.close()
        rc = self.proc.wait(60)
        self.log.close()
        return rc


def segments(rec):
    return sorted(rec.glob('*-replay-*.cycreplay'))


def run_tool(args, work, expect):
    if '--work-dir' in args:
        stale = Path(args[args.index('--work-dir') + 1])
        if stale.exists():
            shutil.rmtree(stale)
    cmd = [sys.executable, '-I', str(HERE / 'replay_diagnostic.py'), *[str(a) for a in args]]
    r = subprocess.run(cmd, cwd=work, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=600,
                       creationflags=NO_WINDOW)
    (work / f'tool-{expect}.log').write_text(r.stdout + r.stderr, encoding='utf-8')
    check(r.returncode == expect, f'tool exit {r.returncode}, wanted {expect}:\n{r.stdout}{r.stderr}')
    return r.stdout + r.stderr


def plain_run(exe, image, input_text, frames, work, name, extra=()):
    """A power-on headless run (no checkpoints, no recording) -> {frame: hash fields}."""
    inp = work / f'{name}.input'
    inp.write_text(input_text)
    hashes = work / f'{name}.hashes'
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', NESRECOMP_REPLAY_RECORD='0',
               NESRECOMP_DIAGNOSTICS_DIR=str(work / 'diag'))
    r = subprocess.run([str(exe), str(image), '--frames', str(frames), '--input', str(inp), '--hash-out', str(hashes),
                        '--no-save', *extra], cwd=work, env=env, capture_output=True, text=True, timeout=600,
                       creationflags=NO_WINDOW)
    check(r.returncode == 0, f'{name}: host exit {r.returncode}\n{r.stdout}{r.stderr}')
    return rd.read_host_hashes(hashes)


def scenario_nes(exe, out):
    rom = out / 'test.nes'
    make_nes_rom(rom)
    w = Window(exe, rom, out, 'nes')
    w.wait_until_frame(24)
    w.hold('a', 9)
    w.hold('right', 6)
    w.hold('b', 7, player=2)
    w.hold('l', 5)
    w.hold('r', 6, player=2)
    w.cmd('write_ram', addr=0x20, val=0x5A)               # a QA poke: recorded as a cheat write
    w.wait_until_frame(100)
    state = w.out / 'mid.cycstate'
    w.cmd('save_state', path=str(state))
    w.hold('start', 5)
    w.hold('up', 11, player=2)
    before_load = w.frame()
    w.cmd('load_state', path=str(state))                  # a rewind: frame numbers go back
    w.hold('left', 8)
    w.cmd('write_ram', addr=0x21, val=0xA5)
    w.wait_until_frame(before_load + 2 * SEGMENT_FRAMES)
    check(w.quit() == 0, 'window exit status')
    segs = segments(w.rec)
    check(len(segs) >= 6, f'expected rollover segments, got {len(segs)}')
    parsed = [rd.Segment(p) for p in segs]
    check(any(v[0] & 1 for s in parsed for v in s.game_inputs.values()), 'seat 1 L recorded')
    check(any(v[1] & 2 for s in parsed for v in s.game_inputs.values()), 'seat 2 R recorded')
    for s in parsed:
        check(s.state_path.is_file(), f'{s.path.name}: checkpoint missing')
        check(s.unfinished is None, f'{s.path.name}: a normal quit must leave every frame finished')
        check(s.completed <= SEGMENT_FRAMES, f'{s.path.name}: {s.completed} frames in a {SEGMENT_FRAMES}-frame segment')
    check(parsed[0].first_frame == 0, 'the first segment starts at power-on')
    whys = [s.header['WHY'] for s in parsed]
    check(whys[0] == 'start' and 'segment length' in whys, f'rollover missing: {whys}')
    check('save state loaded' in whys, f'a mid-session state load must start a segment: {whys}')
    check(not any('skipped' in x or 'replaced' in x for x in whys),
          f'a plain run must not look like a discontinuity (frames skipped/machine replaced): {whys}')
    # the segment after the load starts at the saved frame (a rewind), not at the frame count before it
    load_seg = [s for s in parsed if s.header['WHY'] == 'save state loaded']
    check(len(load_seg) == 1 and load_seg[0].first_frame < before_load, 'the load rewinds the frame counter')
    check(sum(len(s.pokes) for s in parsed) == 2, f'both QA pokes recorded: {[s.pokes for s in parsed]}')
    pads = {tuple(v[:2]) for s in parsed for _, v in s.inputs}
    check(any(b0 for b0, b1 in pads) and any(b1 for b0, b1 in pads), 'both ports saw buttons')
    # the machine latched what was recorded as consumed (no input hook here)
    check(all(v[0] == v[6] and v[1] == v[7] for s in parsed for _, v in s.inputs), 'final pad bytes equal the pads')
    # 1. every segment replays exactly
    text = run_tool([w.rec, '--exe', exe, '--rom', rom, '--work-dir', out / 'nes-play'], out, 0)
    check(text.count('OK ') == len(parsed), f'every segment must report OK:\n{text}')
    # 2. one segment alone, bounded, with screenshots
    text = run_tool([segs[1], '--exe', exe, '--rom', rom, '--until', 5, '--screenshot-every', 2,
                     '--work-dir', out / 'nes-one'], out, 0)
    check('(5 frames)' in text and len(list((out / 'nes-one' / 'shots').glob('*.png'))) >= 2, text)
    run_tool([w.rec, '--check'], out, 0)
    # 3. checkpoints and resumed segments equal one uninterrupted power-on run
    contiguous = [s for s in parsed if s.number < load_seg[0].number]
    check(len(contiguous) >= 3, 'need several contiguous segments before the load')
    last = contiguous[-1].last_frame
    from_all = {}
    for s in contiguous:
        from_all.update(s.hashes)
    held = [0, 0]
    route = []
    for s in contiguous:
        for frame, v in s.inputs:
            for port in range(2):
                if held[port] != v[port]:
                    held[port] = v[port]
                    route.append(f'{frame} {["", "2:"][port]}{rd.buttons_text(v[port])}')
        route += [f'{f} POKE {a:04X} {val:02X}' for f, a, val in s.pokes]
    got = plain_run(exe, rom, '\n'.join(route) + '\n', last + 1, out, 'nes-plain')
    bad = [f for f in sorted(from_all) if not got.get(f) or rd.diff_fields(from_all[f], got[f])]
    check(not bad, f'recorded hashes differ from an uninterrupted run at frames {bad[:5]}')
    # logical-seat lines (L1:/L1:=, what mods see) parse and never reach the machine's pads
    route_logical = route + ['10 L1:A+B', '12 L2:START', '14 L1:=', '14 L2:=']
    got_logical = plain_run(exe, rom, '\n'.join(route_logical) + '\n', last + 1, out, 'nes-plain-logical')
    check(all(got_logical.get(f) == got.get(f) for f in got), 'logical-seat input lines must not change the machine')
    # 4. corruption is detected
    play_rec = out / 'nes-corrupt'
    shutil.copytree(w.rec, play_rec)
    target = [p for p in segments(play_rec) if rd.Segment(p).number == 2][0]
    seg = rd.Segment(target)
    lines = target.read_text().split('\n')
    idx = next(i for i, l in enumerate(lines) if l.startswith('I ') and int(l.split()[1]) == seg.first_frame + 20)
    f = lines[idx].split()
    f[2] = '%02X' % (int(f[2], 16) ^ 0x81)       # flip A and RIGHT on pad 1, as if input were recorded wrong
    lines[idx] = ' '.join(f)
    target.write_text('\n'.join(lines))
    text = run_tool([target, '--exe', exe, '--rom', rom, '--work-dir', out / 'nes-bad-input'], out, 1)
    check('FIRST DIVERGENCE at frame' in text, text)
    frame_bad = int(text.split('FIRST DIVERGENCE at frame ')[1].split()[0])
    check(seg.first_frame + 20 <= frame_bad <= seg.first_frame + 22, f'divergence at {frame_bad}, corrupted {seg.first_frame + 20}')
    check(any((out / 'nes-bad-input').glob('replay-divergence-*.cycstate')), 'a divergence state is saved')
    lines = target.read_text().split('\n')
    idx = next(i for i, l in enumerate(lines) if l.startswith('H ') and int(l.split()[1]) == seg.first_frame + 7)
    f = lines[idx].split()
    f[2] = '%016X' % (int(f[2], 16) ^ 1)
    lines[idx] = ' '.join(f)
    target.write_text('\n'.join(lines))
    text = run_tool([target, '--exe', exe, '--rom', rom, '--work-dir', out / 'nes-bad-hash'], out, 1)
    check(f'FIRST DIVERGENCE at frame {seg.first_frame + 7}' in text and 'mem:' in text, text)
    # a different program refuses the checkpoint
    other = out / 'other.nes'
    make_nes_rom(other, variant=3)
    text = run_tool([segs[0], '--exe', exe, '--rom', other, '--work-dir', out / 'nes-other'], out, 4)
    check('another program' in text, text)
    # a final input with no checkpoint is reported as incomplete, never as success
    trunc = out / 'nes-trunc'
    shutil.copytree(w.rec, trunc)
    target = segments(trunc)[0]
    lines = target.read_text().split('\n')
    while not lines[-1].startswith('I ') and not (lines[-1] == '' and lines[-2].startswith('I ')):
        lines.pop()
    target.write_text('\n'.join(lines) if lines[-1] == '' else '\n'.join(lines) + '\n')
    check(rd.Segment(target).unfinished is not None, 'the truncation leaves an unfinished frame')
    text = run_tool([target, '--exe', exe, '--rom', rom, '--work-dir', out / 'nes-incomplete'], out, 3)
    check('INCOMPLETE' in text, text)
    run_tool([target, '--check'], out, 3)
    print('PASS nes: %d segments, mid-session load, cheat pokes, corruption detected' % len(parsed), flush=True)
    return parsed


def scenario_fds(exe, out):
    fx = out / 'fds-fixtures'
    subprocess.run([sys.executable, str(HERE / 'fds_hle_fixtures.py'), '--out', str(fx)], check=True,
                   stdout=subprocess.DEVNULL, creationflags=NO_WINDOW)
    image, bios = fx / 'disk2_2.fds', fx / 'bios' / 'hle.rom'
    w = Window(exe, image, out, 'fds', args=('--fds-bios', str(bios)))
    w.wait_until_frame(45)
    w.cmd('disk', do='eject')
    w.wait_frames(20)
    w.cmd('disk', do='insert', side=1)
    w.wait_until_frame(w.frame() + 60)
    w.hold('a', 5)
    w.cmd('disk', do='press')
    w.wait_until_frame(w.frame() + 25)
    w.cmd('hle', axis='fast-load', value='off')          # a setting outside the machine: a new segment
    w.wait_until_frame(w.frame() + 20)
    check(w.quit() == 0, 'fds window exit status')
    parsed = [rd.Segment(p) for p in segments(w.rec)]
    drive = [(a, side) for s in parsed for _, a, side in s.drive]
    check(('E', 0) in drive and ('I', 1) in drive, f'drive changes recorded: {drive}')
    check(all(s.header['FDS'] == '1' and 'HLE' in s.header for s in parsed), 'FDS header facts')
    whys = [s.header['WHY'] for s in parsed]
    check('settings changed' in whys, f'an FDS HLE change starts a segment: {whys}')
    text = run_tool([w.rec, '--exe', exe, '--rom', image, '--work-dir', out / 'fds-play', '--host-arg=--fds-bios',
                     '--host-arg', str(bios)], out, 0)
    check(text.count('OK ') == len(parsed), text)
    # the drive lines are load-bearing: without them the replay must diverge
    stripped = out / 'fds-nodrive'
    shutil.copytree(w.rec, stripped)
    victim = next(p for p in segments(stripped) if rd.Segment(p).drive)
    victim.write_text('\n'.join(l for l in victim.read_text().split('\n') if not l.startswith('D ')))
    text = run_tool([victim, '--exe', exe, '--rom', image, '--work-dir', out / 'fds-nodrive-play', '--host-arg=--fds-bios',
                     '--host-arg', str(bios)], out, 1)
    check('FIRST DIVERGENCE' in text, text)
    print('PASS fds: %d segments, drive eject/insert replayed exactly' % len(parsed), flush=True)


def scenario_zapper(exe, out):
    rom = out / 'test.nes'
    w = Window(exe, rom, out, 'zapper', args=('--zapper-port', '2'))
    w.wait_until_frame(20)
    w.cmd('zapper', x=100, y=80, trigger=False)
    w.wait_frames(15)
    w.cmd('zapper', x=100, y=80, trigger=True)
    w.wait_frames(6)
    w.cmd('zapper', x=30, y=200, trigger=False)
    w.wait_until_frame(w.frame() + SEGMENT_FRAMES + 10)
    check(w.quit() == 0, 'window exit')
    parsed = [rd.Segment(p) for p in segments(w.rec)]
    aims = [(x, y, t) for s in parsed for _, x, y, t in s.zapper]
    check((100, 80, 1) in aims and (30, 200, 0) in aims, f'zapper changes recorded: {aims}')
    check(all(s.zapper and s.zapper[0][0] == s.first_frame for s in parsed), 'every segment states the gun at its first frame')
    text = run_tool([w.rec, '--exe', exe, '--rom', rom, '--work-dir', out / 'zapper-play', '--host-arg=--zapper-port',
                     '--host-arg', '2'], out, 0)
    check(text.count('OK ') == len(parsed), text)
    stripped = out / 'zapper-nogun'
    shutil.copytree(w.rec, stripped)
    victim = next(p for p in segments(stripped) if any(f > rd.Segment(p).first_frame for f, *_ in rd.Segment(p).zapper))
    first = rd.Segment(victim).first_frame
    victim.write_text('\n'.join(l for l in victim.read_text().split('\n')
                                if not (l.startswith('Z ') and int(l.split()[1]) != first)))
    text = run_tool([victim, '--exe', exe, '--rom', rom, '--work-dir', out / 'zapper-nogun-play',
                     '--host-arg=--zapper-port', '--host-arg', '2'], out, 1)
    check('FIRST DIVERGENCE' in text, text)
    print('PASS zapper: aim and trigger replayed exactly (%d changes)' % len(aims), flush=True)


def scenario_overflow(exe, out):
    """A disk that stops answering: the bounded queue fills, recording stops loudly, the game never waits."""
    rom = out / 'test.nes'
    w = Window(exe, rom, out, 'overflow', env_extra={'NESRECOMP_REPLAY_TEST_STALL_MS': '120000',
                                                     'NESRECOMP_REPLAY_SEGMENT_FRAMES': '2'})
    started = time.time()
    w.wait_until_frame(400)                    # ~330 KiB of checkpoint every 2 frames overruns a 16 MiB queue by frame ~100
    elapsed = time.time() - started
    check(elapsed < 60, f'the game waited on the writer: 400 frames took {elapsed:.1f}s')
    ring = w.out / 'ring.txt'
    w.cmd('ring_dump', path=str(ring))
    check(' replay FAILED ' in ring.read_text(), 'the ring must say recording failed')
    check(w.quit() == 0, 'window exit status')
    log = (w.out / 'stdout.log').read_text()
    check('[replay] recording stopped' in log and 'queue full' in log, 'a loud log line is required: ' + log[-600:])
    segs = segments(w.rec)
    check(0 < len(segs) < 400 // 2, f'recording stopped part way: {len(segs)} segments')
    print('PASS overflow: stopped loudly after %d segments, 400 frames ran in %.1fs' % (len(segs), elapsed), flush=True)


def scenario_off(exe, out):
    rom = out / 'test.nes'
    w = Window(exe, rom, out, 'off', env_extra={'NESRECOMP_REPLAY_RECORD': '0'})
    w.wait_until_frame(20)
    check(w.quit() == 0, 'window exit')
    check(not segments(w.rec) and not (w.rec.exists() and any(w.rec.iterdir())), 'opt-out must write nothing')
    print('PASS off: NESRECOMP_REPLAY_RECORD=0 records nothing', flush=True)


def scenario_cap(exe, out):
    rom = out / 'test.nes'
    w = Window(exe, rom, out, 'cap', env_extra={'NESRECOMP_REPLAY_HISTORY_MIB': '2'})
    w.wait_until_frame(SEGMENT_FRAMES * 12)
    check(w.quit() == 0, 'window exit')
    segs = [rd.Segment(p) for p in segments(w.rec)]
    check(segs and segs[0].number > 1, 'the oldest segments must have been evicted')
    size = sum(s.state_path.stat().st_size for s in segs)
    check(size <= 2 * 1024 * 1024, f'checkpoint bytes {size} exceed the 2 MiB budget')
    check(all(s.state_path.is_file() for s in segs) and segs[-1].number >= 12, 'newest kept, none orphaned')
    leftovers = [p.name for p in w.rec.iterdir() if p.name.endswith('.state') and not p.with_suffix('').exists()]
    check(not leftovers, f'orphaned checkpoints {leftovers}')
    run_tool([w.rec, '--exe', exe, '--rom', rom, '--work-dir', out / 'cap-play'], out, 0)
    print(f'PASS cap: kept segments {segs[0].number}-{segs[-1].number} in {size} checkpoint bytes', flush=True)


def mods_app(exe_mods, out, name, keep_package=True):
    """A private copy of the mods-built window and its staged catalog (the catalog and the player's
    selection, mods/state.toml, live beside the executable)."""
    app = out / f'app-{name}'
    if app.exists():
        shutil.rmtree(app)
    app.mkdir(parents=True)
    for f in exe_mods.parent.iterdir():
        if f.is_file() and (f == exe_mods or f.suffix.lower() in ('.dll', '.so', '.dylib')):
            shutil.copy2(f, app / f.name)
    shutil.copytree(exe_mods.parent / 'mods', app / 'mods', ignore=shutil.ignore_patterns('state.toml'))
    if not keep_package:
        shutil.rmtree(app / 'mods' / 'packages' / MOD_PACKAGE)
    return app / exe_mods.name


def mod_feature(w, on):
    w.cmd('mod_feature', package=MOD_PACKAGE, feature=MOD_FEATURE, enabled=on)


def snapshot(rec):
    return {p.name: p.stat().st_size for p in sorted(rec.glob('*')) if p.is_file()} if rec.exists() else {}


def settle(rec):
    """The writer flushes every 100 ms: wait until nothing in the folder changes."""
    last = None
    for _ in range(100):
        now = snapshot(rec)
        if now == last:
            return now
        last = now
        time.sleep(0.25)
    check(False, 'the recording folder never settled')


def scenario_mods(exe, exe_mods, out):
    rom = out / 'test.nes'
    app_exe = mods_app(exe_mods, out, 'a')
    manifest = (app_exe.parent / 'mods' / 'packages' / MOD_PACKAGE / '1.0.0' / 'manifest.toml').read_text()
    for needle in ('default_enabled = true', 'group = "Developer"', f'id = "{MOD_FEATURE}"',
                   'id = "nesrecomp.replay-capture"', 'game_id = "cyc-replay-test"'):
        check(needle in manifest, f'the staged package lacks {needle!r}')
    state_file = app_exe.parent / 'mods' / 'state.toml'
    check(not state_file.exists(), 'a fresh catalog has no player selection')

    # 1. default on; a run-time off ends the segment whole; nothing more is written; on resumes
    w = Window(app_exe, rom, out, 'mods-on')
    w.wait_until_frame(SEGMENT_FRAMES + 15)
    time.sleep(0.5)                              # the writer flushes every 100 ms
    before_off = snapshot(w.rec)
    check(any(n.endswith('.cycreplay') for n in before_off), 'Replay capture is on by default: segments are written')
    first = [rd.Segment(p) for p in segments(w.rec)]
    check(all(s.header.get('MODS') == '1' for s in first), 'the header says the run had mods')
    mod_feature(w, False)
    off_frame = w.frame()
    w.wait_frames(2 * SEGMENT_FRAMES + 20)       # would be two more rollovers if it were recording
    closed = settle(w.rec)
    segs = [rd.Segment(p) for p in segments(w.rec)]
    check(all(s.unfinished is None for s in segs), 'turning it off ends the segment on a finished frame')
    check(max(s.last_frame for s in segs) <= off_frame + 2, f'frames after the toggle were recorded (off at {off_frame})')
    n_off = len(segs)
    w.wait_frames(SEGMENT_FRAMES + 20)
    check(settle(w.rec) == closed, 'a disabled recorder writes nothing')
    mod_feature(w, True)
    on_frame = w.frame()
    w.wait_frames(SEGMENT_FRAMES + 20)
    time.sleep(0.5)
    segs = [rd.Segment(p) for p in segments(w.rec)]
    check(len(segs) > n_off, 'enabling it again opens a new segment')
    resumed = segs[n_off]
    check(resumed.header['WHY'] == 'recording resumed' and resumed.first_frame >= on_frame - 3, f'resume segment: {resumed.header}')
    check(resumed.state_path.is_file(), 'the new segment has its checkpoint')
    mod_feature(w, False)                        # leave the player's selection OFF for the next launch
    w.wait_frames(5)
    check(w.quit() == 0, 'window exit status')
    parsed = [rd.Segment(p) for p in segments(w.rec)]
    check(all(s.unfinished is None for s in parsed), 'a normal quit leaves every frame finished')
    text = run_tool([w.rec, '--exe', app_exe, '--rom', rom, '--work-dir', out / 'mods-on-play'], out, 0)
    check(text.count('OK ') == len(parsed), f'every segment (before and after the gap) replays exactly:\n{text}')
    check('replay-capture' in state_file.read_text(), 'the selection persisted in mods/state.toml')

    # 2. the persisted OFF selection: no folder, no file; enabling it in game starts the first segment
    w = Window(app_exe, rom, out, 'mods-persisted-off')
    w.wait_until_frame(2 * SEGMENT_FRAMES + 10)
    check(not w.rec.exists() or not any(w.rec.iterdir()), 'a selection that is off creates no replay folder or file')
    mod_feature(w, True)
    w.wait_frames(SEGMENT_FRAMES + 20)
    time.sleep(0.5)
    segs = [rd.Segment(p) for p in segments(w.rec)]
    check(segs and segs[0].header['WHY'] == 'start' and segs[0].number == 1,
          f'a first segment starts when enabled: {[s.header["WHY"] for s in segs]}')
    check(w.quit() == 0, 'window exit status')
    text = run_tool([w.rec, '--exe', app_exe, '--rom', rom, '--work-dir', out / 'mods-persisted-off-play'], out, 0)
    check(text.count('OK ') == len(segments(w.rec)), text)

    # 3. the environment opt-out wins over an enabled feature (and Mods cannot re-enable it)
    w = Window(app_exe, rom, out, 'mods-env-off', env_extra={'NESRECOMP_REPLAY_RECORD': '0'})
    w.wait_until_frame(30)
    mod_feature(w, False)
    mod_feature(w, True)
    w.wait_frames(SEGMENT_FRAMES + 10)
    check(not w.rec.exists() or not any(w.rec.iterdir()), 'NESRECOMP_REPLAY_RECORD=0 records nothing whatever Mods say')
    check(w.quit() == 0, 'window exit status')

    # 4. a catalog without the package (removed by hand) keeps the build default: it records
    app2 = mods_app(exe_mods, out, 'b', keep_package=False)
    w = Window(app2, rom, out, 'mods-no-package')
    w.wait_until_frame(SEGMENT_FRAMES + 10)
    time.sleep(0.5)
    check(snapshot(w.rec), 'without the package the build default (on) applies')
    check(w.quit() == 0, 'window exit status')
    print('PASS mods: default on, run-time off/on, persisted off, env opt-out, no package -> records', flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--window', type=Path)
    ap.add_argument('--window-mods', type=Path, help='the window built with mods (cyc_window_mods): the mods scenario')
    ap.add_argument('--rom-crc32', action='store_true', help="print the synthetic program's mod target CRC32 and exit")
    ap.add_argument('--out', type=Path)
    ap.add_argument('--only', default='')
    args = ap.parse_args()
    if args.rom_crc32:
        import zlib
        print('%08x' % (zlib.crc32(nes_rom_bytes()[16:]) & 0xFFFFFFFF))
        return
    if not args.window:
        ap.error('--window is required')
    if not args.out:
        ap.error('--out is required')
    exe = args.window.resolve()
    out = args.out.resolve()
    if out.exists():
        shutil.rmtree(out)                       # a clean slate: every work directory below is new
    out.mkdir(parents=True)
    rom = out / 'test.nes'
    make_nes_rom(rom)
    for name, fn in (('nes', scenario_nes), ('fds', scenario_fds), ('zapper', scenario_zapper),
                     ('overflow', scenario_overflow), ('off', scenario_off), ('cap', scenario_cap),
                     ('mods', lambda e, o: scenario_mods(e, args.window_mods.resolve(), o))):
        if args.only and name not in args.only.split(','):
            continue
        if name == 'mods' and not args.window_mods:
            check(not args.only, '--only mods needs --window-mods')
            continue
        fn(exe, out)
    print(f'cyc_replay: {checks} checks passed')


if __name__ == '__main__':
    main()
