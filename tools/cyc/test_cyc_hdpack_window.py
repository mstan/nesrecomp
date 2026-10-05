#!/usr/bin/env python3
"""Check displayed HD pixels after shrinking below picture size and enlarging.

Uses a synthetic ROM/pack and a hidden SDL window. Pass --real-video to also
exercise the local accelerated renderer; default is the SDL dummy driver.
"""
import argparse
import json
import os
import re
from pathlib import Path
import socket
import subprocess
import time

from test_cyc_hdpack import fixture, png_write
from test_cyc_window import free_port, png_pixels


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--real-video', action='store_true')
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    rom = out / 'synthetic.nes'
    rom.write_bytes(fixture(True, False))
    pack = out / 'pack'
    pack.mkdir(exist_ok=True)
    png_write(pack / 'art.png', 32, 32, lambda x, y: (255, 50, 100, 255))
    (pack / 'hires.txt').write_text(
        '<ver>106\n<scale>4\n<img>art.png\n'
        '<tile>0,FFFFFFFFFFFFFFFF0000000000000000,0F010203,0,0,1,N\n')
    config = out / 'config.ini'
    config.write_text('[Display]\nWindowScale=1\nIntegerScale=1\n')
    env = dict(os.environ, SDL_AUDIODRIVER='dummy', NESRECOMP_NO_LAUNCHER='1')
    if args.real_video:
        env.pop('SDL_VIDEODRIVER', None)
    else:
        env['SDL_VIDEODRIVER'] = 'dummy'
    si = None
    if hasattr(subprocess, 'STARTUPINFO'):
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0
    port = free_port()
    with (out / 'window.log').open('w') as log:
        proc = subprocess.Popen([
            str(args.host.resolve()), str(rom), '--hdpack', str(pack),
            '--config', str(config), '--hidden', '--no-save', '--tcp', str(port)],
            cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT,
            startupinfo=si, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        try:
            deadline = time.monotonic() + 30
            while True:
                try:
                    sock = socket.create_connection(('127.0.0.1', port), timeout=15)
                    break
                except OSError:
                    assert proc.poll() is None and time.monotonic() < deadline
                    time.sleep(.05)
            with sock, sock.makefile('rwb', buffering=0) as stream:
                records = []

                def ask(cmd, **fields):
                    stream.write((json.dumps(dict(cmd=cmd, id=len(records)+1, **fields))+'\n').encode())
                    response = json.loads(stream.readline())
                    records.append(response)
                    assert response.get('ok'), response
                    return response

                while ask('state')['frame'] < 24:
                    assert time.monotonic() < deadline
                    time.sleep(.02)
                assert ask('state')['picture'] == [1024, 960]
                for index, (w, h) in enumerate([(512, 480), (256, 240), (1024, 960), (1280, 960), (512, 480)]):
                    ask('window_size', w=w, h=h)
                    # Allow the host to receive the SDL resize event.
                    time.sleep(.1)
                    shot = out / f'window-{index}.png'
                    ask('screenshot', layer='ui', path=str(shot))
                    sw, sh, rows = png_pixels(shot)
                    assert (sw, sh) == (w, h)
                    colored = sum(pixel != 0 for row in rows for pixel in row)
                    assert colored > w*h*.5, (w, h, 'HD picture is black or clipped', colored)
                ask('quit')
                assert proc.wait(timeout=10) == 0
                (out / 'qa.json').write_text(json.dumps(records, indent=2))
            assert re.search(r'IntegerScale\s*=\s*1\b', config.read_text()), 'integer preference was lost'
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=10)
    print('HD window pixels pass below/above picture size; integer preference retained', flush=True)


if __name__ == '__main__':
    main()
