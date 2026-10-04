"""Exercise cartridge saves through the actual windowed host in a private directory."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    exe = args.out / args.exe.name
    shutil.copy2(args.exe, exe)
    dll = args.exe.parent / 'SDL2.dll'
    if dll.exists():
        shutil.copy2(dll, args.out / dll.name)
    rom = args.out / 'battery.nes'
    prg = bytearray([0xea] * 16384)
    # Increment the saved byte once at reset, then wait. All three vectors are valid.
    prg[:17] = bytes.fromhex('78 d8 a2 ff 9a ad 00 60 18 69 01 8d 00 60 4c 0e 80')
    prg[-6:] = bytes.fromhex('0e 80 00 80 0e 80')
    rom.write_bytes(b'NES\x1a' + bytes([1, 0, 2, 0, 1]) + bytes(7) + prg)
    config = args.out / 'config.ini'
    config.write_text('[Launcher]\nSkipLauncher = 1\n[Audio]\nEnabled = 0\n')
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', NESRECOMP_NO_LAUNCHER='1')
    startup = None
    if os.name == 'nt':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0

    def run(*extra, expected=0):
        p = subprocess.run([str(exe), str(rom), '--hidden', '--exit-after', '2', '--config', str(config), *map(str, extra)],
                           cwd=args.out, env=env, capture_output=True, text=True, timeout=30,
                           startupinfo=startup, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        assert p.returncode == expected, (p.returncode, p.stdout, p.stderr)
        return p

    save = args.out / 'saves/battery.sav'
    if save.exists():
        save.unlink()
    run()
    data = save.read_bytes()
    assert len(data) == 8192 and data[0] == 1, (len(data), data[:8])
    run()
    assert save.read_bytes()[0] == 2, 'battery bytes did not survive restart'
    before = save.read_bytes()
    run('--save-file', save, '--no-save')
    assert save.read_bytes() == before, '--no-save changed the existing save'
    save.write_bytes(b'bad')
    result = run(expected=2)
    assert 'invalid save' in result.stderr and save.read_bytes() == b'bad', result.stderr
    before_rom = hashlib.sha256(rom.read_bytes()).digest()
    result = run('--save-file', rom, expected=2)
    assert 'different from the ROM' in result.stderr
    assert hashlib.sha256(rom.read_bytes()).digest() == before_rom, 'ROM was modified'
    print('PASS: default save, restart, no-save, damaged save retention, ROM write refusal')


if __name__ == '__main__':
    main()
