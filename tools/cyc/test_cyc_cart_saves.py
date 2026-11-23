"""Exercise cartridge saves through the actual windowed host in a private directory."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--appimage', action='store_true',
                        help='Keep native saves beside the outer AppImage, not the embedded executable')
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
    # Isolate ordinary executable behavior from an inherited AppImage session.
    env.pop('APPIMAGE', None)
    save_root = args.out
    if args.appimage:
        assert os.name == 'posix', 'AppImage is a POSIX host contract'
        save_root = args.out / 'AppImages with spaces'
        save_root.mkdir(exist_ok=True)
        image = save_root / 'battery.AppImage'
        image.touch()
        env['APPIMAGE'] = str(image.resolve())
    startup = None
    if os.name == 'nt':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0

    def run(*extra, expected=0):
        p = subprocess.run([str(exe), str(rom), '--hidden', '--exit-after', '2', '--config', str(config), *map(str, extra)],
                           cwd=args.out, env=env, capture_output=True, text=True,
                           startupinfo=startup, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        assert p.returncode == expected, (p.returncode, p.stdout, p.stderr)
        return p

    save = save_root / 'saves/battery.sav'
    if save.exists():
        save.unlink()
    run()
    assert save.is_file(), f'native save is missing from the selected host location: {save}'
    if args.appimage:
        assert not (args.out / 'saves/battery.sav').exists(), 'save escaped into the embedded executable directory'
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
    failed_write_checked = False
    if os.name == 'posix' and os.geteuid() != 0:
        # The game can read its progress, but the atomic writer cannot create
        # its temporary file. A failed write must retain the previous bytes.
        # Use the host's temporary filesystem: an NTFS-mounted output folder
        # may intentionally ignore POSIX chmod permissions.
        with tempfile.TemporaryDirectory(prefix='nesrecomp-cart-save-') as scratch:
            parent = Path(scratch)
            protected = parent / 'native.sav'
            protected.write_bytes(before)
            mode = stat.S_IMODE(parent.stat().st_mode)
            parent.chmod(0o555)
            try:
                assert not os.access(parent, os.W_OK), 'read-only fixture filesystem ignores chmod'
                result = run('--save-file', protected, expected=2)
                assert 'cannot write save' in result.stderr, result.stderr
                assert protected.read_bytes() == before, 'failed write damaged previous native progress'
                failed_write_checked = True
            finally:
                parent.chmod(mode)
    print('PASS: host save location, restart, no-save, damaged save retention and ROM write refusal')
    print('PASS: failed-write retention' if failed_write_checked else
          'NOT CHECKED: POSIX failed-write retention requires a non-root POSIX user')


if __name__ == '__main__':
    main()
