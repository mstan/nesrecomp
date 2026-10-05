"""Small compiler regression: default cycle, explicit legacy and file overrides."""
import argparse, os, pathlib, subprocess, tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recompiler', required=True, type=pathlib.Path)
    args = parser.parse_args()
    compiler = args.recompiler.resolve()
    startup = None
    if os.name == 'nt':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    with tempfile.TemporaryDirectory(prefix='nes-backend-default-') as directory:
        root = pathlib.Path(directory)
        prg = bytearray([0xEA] * 32768)
        prg[:3] = bytes([0x4C, 0x00, 0x80])
        prg[-6:] = bytes([0x00, 0x80] * 3)
        rom = root/'test.nes'
        rom.write_bytes(b'NES\x1a'+bytes([2, 1])+bytes(10)+prg+bytes(8192))
        for name, setting, flags, cycle in [
            ('default', None, [], True),
            ('legacy', True, ['--legacy'], False),
            ('file-legacy', False, [], False),
            ('override-cycle', False, ['--cycle-accurate'], True),
        ]:
            folder = root/name
            folder.mkdir()
            command = [str(compiler), str(rom), '--output-prefix', 'test', *flags]
            if setting is not None:
                config = folder/'game.toml'
                config.write_text('[game]\ncycle_accurate = '+str(setting).lower()+'\n')
                command += ['--game', str(config)]
            run = subprocess.run(command, cwd=folder, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, startupinfo=startup, creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0), timeout=20)
            assert run.returncode == 0, run.stdout
            assert (folder/'generated/test_cyc.c').exists() == cycle, (name, run.stdout)
            assert (folder/'generated/test_full.c').exists() != cycle, (name, run.stdout)
            print('PASS', name)

if __name__ == '__main__':
    main()
