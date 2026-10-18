#!/usr/bin/env python3
"""The cycle host's window, driven over its TCP debug server (CTest cyc_window_run).

Nothing reaches the desktop: the window is hidden on SDL's dummy video
driver with the dummy audio driver, and every input goes in through the TCP
server (runner/cyc/cyc_tcp.h): `key` holds an SDL key the way a keyboard
does (through the bindings in config.ini), `pad` holds buttons of an SDL
virtual game controller. The program and disks are the synthetic ones of
tools/cyc/fds_hle_fixtures.py (scenario 2 waits for the player to turn the
disk to side B, then loads it).

  production   the bound Disk key: a first press shows the toast (drive state,
               the binding's name), a press while it is up ejects side A,
               holds the drive empty 30 frames (ring: fds.side events) and
               inserts side B, which the program then loads; after the toast
               times out a press only shows it again; the controller's bound
               button swaps once more (back to side A); the picture never
               holds the toast, the window's frame does; the HLE axis set as
               the menu sets it lands in config.ini; F1-F7 do nothing
  rebinding    a config.ini that binds Disk to K: K works, D does nothing
  menu         (a build with recomp-ui) the Menu key opens recomp-ui's runtime
               menu over the game, which pauses; closing it resumes
  dev          the dev build's keys: F1 ejects and inserts, F2 switches to the
               interpreter, F4 hides the drive bar

python tools/cyc/test_cyc_window.py --window build/cyc/cyc_window.exe --dev-window build/cyc/cyc_window_dev.exe
    [--recomp-ui] --out build/cyc-window
"""
import argparse
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time
import zlib

HERE = Path(__file__).resolve().parent
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
HOLD = 30
checks = 0


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        raise AssertionError(what)


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    return port


def png_pixels(path):
    """(width, height, rows of 32-bit ints) of an 8-bit RGB/RGBA PNG."""
    data = Path(path).read_bytes()
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, color = struct.unpack('>IIBB', body[:10])
            bpp = {2: 3, 6: 4}[color]
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([int.from_bytes(line[x * bpp:x * bpp + 3], 'big') for x in range(w)])
        prev = line
    return w, h, rows


OPEN = []


class Window:
    def __init__(self, exe, image, bios, out, tag, config_text=None):
        self.out = out / tag
        self.out.mkdir(parents=True, exist_ok=True)
        self.config = self.out / 'config.ini'
        if config_text is not None:
            self.config.write_text(config_text, newline='\n')
        elif self.config.exists():
            self.config.unlink()
        self.port = free_port()
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', NESRECOMP_NO_LAUNCHER='1')
        env.pop('NESRECOMP_FDS_HLE', None)
        self.log = open(self.out / 'stdout.log', 'w')
        self.proc = subprocess.Popen([str(exe), str(image), '--fds-bios', str(bios), '--config', str(self.config),
                                      '--tcp', str(self.port), '--hidden', '--no-save'],
                                     cwd=self.out, env=env, stdout=self.log, stderr=subprocess.STDOUT,
                                     creationflags=NO_WINDOW)
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
        return r

    def ok(self, _cmd, **fields):
        r = self.cmd(_cmd, **fields)
        check(r.get('ok'), f'{_cmd} {fields}: {r}')
        return r

    def state(self):
        return self.ok('state')

    def until(self, pred, frames, what):
        start = self.state()['frame']
        while True:
            s = self.state()
            if pred(s):
                return s
            check(s['frame'] - start <= frames, f'{what}: not within {frames} frames: {s}')
            time.sleep(0.02)

    def wait_frames(self, n):
        start = self.state()['frame']
        return self.until(lambda s: s['frame'] >= start + n, n + 600, f'{n} frames')

    def ram(self, addr):
        return int(self.ok('read_ram', addr=addr, len=1)['hex'], 16)

    def ring_sides(self):
        path = self.out / f'ring_{self.next_id}.txt'
        self.ok('ring_dump', path=str(path))
        sides = []
        for line in open(path):
            parts = line.split(None, 4)
            if len(parts) >= 5 and parts[3] == 'fds.side':
                sides.append((int(parts[1]), parts[4].strip()))
        return sides

    def quit(self):
        self.ok('quit')
        self.sock.close()
        rc = self.proc.wait(60)
        self.log.close()
        return rc


def production(exe, image, bios, out, recomp_ui):
    w = Window(exe, image, bios, out, 'production')
    s = w.until(lambda s: s['frame'] >= 60, 900, 'boot')
    check(not s['dev_ui'] and s['fds'] and s['side'] == 0 and s['sides'] == 2, f'start state {s}')
    check(s['disk_binding'] == 'D / LB', f'disk binding {s["disk_binding"]}')
    check(s['recomp_ui'] == recomp_ui, f'recomp_ui {s["recomp_ui"]}')
    # dev keys do nothing in production
    for k in ('F1', 'F3', 'F4', 'F6', 'F7'):
        w.ok('key', name=k, frames=3)
    s = w.wait_frames(10)
    check(s['side'] == 0 and not s['toast']['visible'], f'a dev key acted in production: {s}')
    w.ok('key', name='F2', frames=3)
    check(w.wait_frames(5)['native'], 'F2 switched to the interpreter in production')
    clean_ui = w.out / 'before_toast_ui.png'
    w.ok('screenshot', layer='ui', path=str(clean_ui))
    clean_width, clean_height, clean_rows = png_pixels(clean_ui)
    # peek: the toast, nothing else
    w.ok('key', name='D', frames=3)
    s = w.until(lambda s: s['toast']['visible'], 30, 'toast after the first press')
    check(s['side'] == 0 and s['toast']['title'] == 'DISK 1 SIDE A', f'peek {s}')
    check('D / LB AGAIN: DISK 1 SIDE B' in s['toast']['body'], f'toast body {s["toast"]["body"]}')
    pic, ui = w.out / 'peek_picture.png', w.out / 'peek_ui.png'
    w.ok('screenshot', layer='picture', path=str(pic))
    w.ok('screenshot', layer='ui', path=str(ui))
    pw, ph, prows = png_pixels(pic)
    check((pw, ph) == (256, 240), f'picture size {pw}x{ph}')
    colors = {c for row in prows for c in row}
    check(len(colors) == 1, f'the picture holds more than the program\'s backdrop: {len(colors)} colors')
    uw, uh, urows = png_pixels(ui)
    check((uw, uh) == (clean_width, clean_height), 'toast changed window dimensions')
    check(urows[:uh // 4] != clean_rows[:uh // 4], 'the toast did not change the window frame')
    # swap: eject now, empty for the hold, side B in
    before = len(w.ring_sides())
    w.ok('key', name='D', frames=3)
    s = w.until(lambda s: s['side'] == -1, 30, 'eject on the second press')
    check(s['toast']['title'] == 'SWAPPING TO DISK 1 SIDE B', f'swapping toast {s["toast"]}')
    w.until(lambda s: s['side'] == 1, 90, 'side B in')
    sides = w.ring_sides()[before:]
    check(len(sides) == 2 and sides[0][1] == 'ejected' and sides[1][1] == 'side 1 inserted', f'ring side events {sides}')
    check(sides[1][0] - sides[0][0] == HOLD, f'drive empty {sides[1][0] - sides[0][0]} frames, want {HOLD}: {sides}')
    w.until(lambda s: w.ram(0x420) == 0xC3, 600, 'the program loading side B')
    check(w.ram(0x412) == 0x11, f'loaded token {w.ram(0x412):02X}, want 11 (side B)')
    # after the toast times out a press only shows it
    w.until(lambda s: not s['toast']['visible'], 400, 'toast timeout')
    n = len(w.ring_sides())
    w.ok('key', name='D', frames=3)
    s = w.until(lambda s: s['toast']['visible'], 30, 'toast after the timeout')
    s = w.wait_frames(40)
    check(s['side'] == 1 and len(w.ring_sides()) == n, f'a press after the timeout changed the drive: {s}')
    # the controller's bound button: a press while the toast is up swaps (to side A: all sides cycle)
    w.ok('pad', buttons='leftshoulder', frames=4)
    w.until(lambda s: s['side'] == -1, 30, 'eject from the controller')
    w.until(lambda s: s['side'] == 0, 90, 'side A back in from the controller')
    # an HLE axis as the menu row sets it: persisted as the player's setting
    r = w.ok('hle', axis='fast-load', value='on')
    check('FAST' in r['hle'], f'hle reply {r}')
    if recomp_ui:
        w.ok('key', name='Escape', frames=3)
        s = w.until(lambda s: s['menu_open'], 30, 'the menu opening')
        f1 = s['frame']
        time.sleep(0.5)
        check(w.state()['frame'] == f1, 'the game ran with the menu open')
        mui = w.out / 'menu_ui.png'
        w.ok('screenshot', layer='ui', path=str(mui))
        mw, mh, mrows = png_pixels(mui)
        back = prows[120][128]              # the program's backdrop, as the picture shows it
        covered = sum(c != back for row in mrows for c in row) / (mw * mh)
        check(covered > 0.5, f'no menu over the game in the window\'s frame ({covered:.0%} covered)')
        for nav in ('down', 'down', 'down', 'down', 'accept'):
            w.ok('menu', nav=nav)
        w.ok('screenshot', layer='ui', path=str(w.out / 'menu_disk_drive.png'))
        w.ok('key', name='Escape', frames=3)
        w.until(lambda s: not s['menu_open'], 30, 'the menu closing')
        w.wait_frames(10)
    rc = w.quit()
    check(rc == 0, f'window exit {rc}')
    cfg = w.config.read_text()
    check('FastLoad = on' in cfg and '[Keyboard.Shortcuts]\ndisk = D' in cfg, 'config.ini after the run')
    return cfg


def rebinding(exe, image, bios, out, cfg):
    text = cfg.replace('[Keyboard.Shortcuts]\ndisk = D', '[Keyboard.Shortcuts]\ndisk = K')
    w = Window(exe, image, bios, out, 'rebinding', text)
    s = w.until(lambda s: s['frame'] >= 60, 900, 'boot')
    check(s['disk_binding'] == 'K / LB', f'disk binding {s["disk_binding"]}')
    check('HLE FAST' in s['hle'], f'the saved HLE setting was not applied: {s["hle"]}')
    w.ok('key', name='D', frames=3)
    check(not w.wait_frames(10)['toast']['visible'], 'the old key still shows the toast')
    w.ok('key', name='K', frames=3)
    w.until(lambda s: s['toast']['visible'], 30, 'toast from the new key')
    check(w.quit() == 0, 'window exit')


def dev(exe, image, bios, out):
    w = Window(exe, image, bios, out, 'dev')
    s = w.until(lambda s: s['frame'] >= 60, 900, 'boot')
    check(s['dev_ui'], 'dev build reports no dev UI')
    w.ok('key', name='F1', frames=3)
    w.until(lambda s: s['side'] == -1, 30, 'F1 eject')
    w.ok('key', name='F1', frames=3)
    w.until(lambda s: s['side'] == 0, 30, 'F1 insert')
    w.ok('key', name='F2', frames=3)
    w.until(lambda s: not s['native'], 30, 'F2 interpreter')
    with_bar = w.out / 'bar.png'
    w.ok('screenshot', layer='ui', path=str(with_bar))
    w.ok('key', name='F4', frames=3)
    w.wait_frames(5)
    no_bar = w.out / 'no_bar.png'
    w.ok('screenshot', layer='ui', path=str(no_bar))
    check(png_pixels(with_bar)[2] != png_pixels(no_bar)[2], 'F4 did not change the window')
    check(w.quit() == 0, 'window exit')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--window', type=Path, required=True)
    ap.add_argument('--dev-window', type=Path, required=True)
    ap.add_argument('--recomp-ui', action='store_true', help='the production window has the runtime menu')
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    fx = out / 'fx'
    subprocess.run([sys.executable, str(HERE / 'fds_hle_fixtures.py'), '--out', str(fx)], check=True,
                   capture_output=True, creationflags=NO_WINDOW)
    image, bios = fx / 'disk2_2.fds', fx / 'bios' / 'hle.rom'
    try:
        cfg = production(args.window.resolve(), image, bios, out, args.recomp_ui)
        rebinding(args.window.resolve(), image, bios, out, cfg)
        dev(args.dev_window.resolve(), image, bios, out)
    finally:
        for p in OPEN:
            if p.poll() is None:
                p.kill()
    print(f'window: {checks} checks passed')


if __name__ == '__main__':
    main()
