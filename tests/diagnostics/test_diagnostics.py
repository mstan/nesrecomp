"""Exercise real crash handling in a separate process, including dump context."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = args.out.resolve() / f'run-{time.time_ns()}'; root.mkdir(parents=True)
    startup = None
    if os.name == 'nt':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 0
    results = []
    cases = ['off', 'normal', 'commit-fail', 'cycle', 'gui-streams', 'io-failure']
    if os.name == 'nt':
        cases += ['fault', 'abort', 'stack']
    for name in cases:
        folder = root / name
        folder.mkdir(exist_ok=True)
        env = os.environ.copy()
        if name == 'io-failure':
            target = folder / 'existing-file'; target.write_text('must survive')
        else:
            target = folder
        env['NESRECOMP_DIAGNOSTICS_DIR'] = str(target)
        p = subprocess.run([str(args.exe.resolve()), name], cwd=folder, env=env,
                           capture_output=True, startupinfo=startup,
                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), timeout=30)
        logs, dumps = list(folder.glob('*.log')), list(folder.glob('*.dmp'))
        if name in ['fault', 'abort', 'stack']:
            expected = {'fault': 0xC0000005, 'abort': 0xE0000001, 'stack': 0xC00000FD}[name]
            assert p.returncode & 0xffffffff == expected, (name, p.returncode, p.stderr)
            assert len(logs) == len(dumps) == 1, (name, logs, dumps)
            data = dumps[0].read_bytes()
            assert data[:4] == b'MDMP'
            count, table = struct.unpack_from('<II', data, 8)
            exception = next((rva for typ, size, rva in
                (struct.unpack_from('<III', data, table + i * 12) for i in range(count)) if typ == 6), None)
            assert exception is not None, (name, 'missing exception stream')
            assert struct.unpack_from('<I', data, exception + 8)[0] == expected
            context_size, context_rva = struct.unpack_from('<II', data, exception + 160)
            assert context_size > 0 and context_rva + context_size <= len(data)
            text = logs[0].read_text(errors='replace')
            assert 'CRASH exception=' in text and 'minidump=written' in text, (name, text)
        else:
            assert p.returncode == 0, (name, p.stderr)
            assert not dumps
            if name in ['off', 'io-failure']:
                assert not logs, name
                if name == 'io-failure': assert target.read_text() == 'must survive'
            else:
                assert len(logs) == (2 if name == 'cycle' else 1)
                text = ''.join(log.read_text(errors='replace') for log in logs)
                assert 'PLAY: committing' in text and 'known startup error' in text
                assert 'heartbeat frame=300' in text and 'exit code=0' in text
                if name == 'commit-fail': assert 'injected commit failure' in text
                if name == 'gui-streams':
                    assert 'stdout preserved' in text and 'stderr preserved' in text
                else:
                    assert b'stdout preserved' in p.stdout and b'stderr preserved' in p.stderr
        results.append({'case': name, 'returncode': p.returncode, 'logs': len(logs), 'dumps': len(dumps)})
        print(name, 'passed', flush=True)
    (root / 'results.json').write_text(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
