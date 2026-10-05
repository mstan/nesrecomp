"""A rejected requested game config must not generate a partial program."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', required=True, type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='nesrecomp-config-') as temp:
    root = Path(temp)
    prg = bytearray([0xea]*16384)
    prg[:3] = bytes([0x4c, 0, 0x80])
    prg[-6:] = bytes([0, 0x80])*3
    rom = root/'synthetic.nes'
    rom.write_bytes(b'NES\x1a'+bytes([1,1])+bytes(10)+prg+bytes(8192))
    for name, text in [
        ('syntax', '[game\noutput_prefix="bad"\n'),
        ('hook', '[game]\noutput_prefix="bad"\ncycle_accurate=true\n'
                 '[[mod_function_hook]]\naddr=0x8000\nlength=0\ncrc32=0\n'),
    ]:
        work = root/name
        work.mkdir()
        config = work/'game.toml'
        config.write_text(text)
        result = subprocess.run([str(args.compiler.resolve()), str(rom), '--game', str(config)],
                                cwd=work, capture_output=True, text=True,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), timeout=30)
        assert result.returncode != 0, (name, result.stdout, result.stderr)
        assert 'no code generated' in result.stderr, (name, result.stderr)
        assert not (work/'generated').exists(), (name, 'partial output created')
print('Malformed TOML and invalid hook length rejected before generation')
