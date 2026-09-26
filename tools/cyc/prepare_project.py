#!/usr/bin/env python3
"""CMake configure-time bridge; all generated files stay in the build tree."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tomllib


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cmake_quote(value):
    value = str(value)
    equals = '='
    while ']' + equals + ']' in value:
        equals += '='
    return '[' + equals + '[' + value + ']' + equals + ']'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for flag in ('rom', 'recompiler', 'out'):
        ap.add_argument('--' + flag, type=Path, required=True)
    for flag in ('game', 'seeds'):
        ap.add_argument('--' + flag, type=Path)
    args = ap.parse_args()
    rom, compiler, out = (p.resolve() for p in (args.rom, args.recompiler, args.out))
    dependencies = [rom, compiler]
    game = args.game.resolve() if args.game else None
    seeds = args.seeds.resolve() if args.seeds else None
    if game:
        dependencies.append(game)
        config = tomllib.loads(game.read_text(encoding='utf-8'))
        configured_seed = config.get('game', {}).get('cycle_seed_file')
        if seeds is None and configured_seed:
            seeds = (game.parent / configured_seed).resolve()
    if seeds:
        # A typo must not silently turn a profiled build into interpreter-only.
        dependencies.append(seeds)
    identity = {p.as_posix(): digest(p) for p in dependencies}
    identity['bridge'] = digest(Path(__file__))
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()[:20]
    folder = out / key
    folder.mkdir(parents=True, exist_ok=True)
    stamp = folder / 'complete.json'
    sources = sorted((folder / 'generated').glob('game_cyc*.c'))
    if not stamp.exists() or not sources or json.loads(stamp.read_text()) != {p.name: digest(p) for p in sources}:
        # Always pass a configuration to suppress unrelated ./game.toml lookup.
        config_path = game or folder / 'game.toml'
        if not game:
            config_path.write_text('[game]\n', encoding='utf-8', newline='\n')
        command = [str(compiler), str(rom), '--game', str(config_path),
                   '--cycle-accurate', '--output-prefix', 'game']
        if seeds:
            command += ['--cycle-seed-file', str(seeds)]
        run = subprocess.run(command, cwd=folder, capture_output=True, text=True,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        log = folder / 'codegen.log'
        log.write_text(run.stdout + run.stderr, encoding='utf-8', newline='\n')
        if run.returncode:
            raise RuntimeError(f'NESRecomp exited {run.returncode}; see {log}')
        sources = sorted((folder / 'generated').glob('game_cyc*.c'))
        if not sources:
            raise RuntimeError(f'NESRecomp produced no cycle sources; see {log}')
        stamp.write_text(json.dumps({p.name: digest(p) for p in sources}), encoding='utf-8', newline='\n')
    manifest = ''
    for name, values in [('CYC_PROJECT_SOURCES', sources), ('CYC_PROJECT_DEPENDS', dependencies),
                         ('CYC_PROJECT_ROM', [rom]), ('CYC_PROJECT_ROM_SHA256', [digest(rom)])]:
        manifest += 'set(' + name + '\n' + ''.join('  ' + cmake_quote(p.as_posix() if isinstance(p, Path) else p) + '\n' for p in values) + ')\n'
    (out / 'sources.cmake').write_text(manifest, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
