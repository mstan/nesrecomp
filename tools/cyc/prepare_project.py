#!/usr/bin/env python3
"""CMake configure-time bridge; all generated files stay in the build tree."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tomllib
import zlib

# The one RAM Adapter BIOS with a recorded identity (common/nes_fds.h).
FDS_BIOS = {'size': 8192, 'crc32': 0x5E607DCF}


def is_fds_image(path):
    head = path.read_bytes()[:16]
    return head[:4] == b'FDS\x1a' or head[:15] == b'\x01*NINTENDO-HVC*' or path.suffix.lower() == '.qd'


def check_fds_bios(bios):
    """The BIOS must match the identity its <stem>.toml records (bios/disksys.toml:
    size, crc32, sha1), else the known disksys.rom. Returns the files it read."""
    data = bios.read_bytes()
    identity, used = dict(FDS_BIOS), [bios]
    toml = bios.with_suffix('.toml')
    if toml.exists():
        program = tomllib.loads(toml.read_text(encoding='utf-8')).get('program', {})
        identity = {'size': int(program.get('size', FDS_BIOS['size'])),
                    'crc32': int(str(program.get('crc32', FDS_BIOS['crc32'])), 0)}
        if 'sha1' in program:
            identity['sha1'] = str(program['sha1']).lower()
        used.append(toml)
    got = {'size': len(data), 'crc32': zlib.crc32(data), 'sha1': hashlib.sha1(data).hexdigest()}
    bad = [k for k, v in identity.items() if got[k] != v]
    if bad:
        raise RuntimeError(f'{bios} is not the expected FDS BIOS ({", ".join(bad)} differ: '
                           f'got {got}, expected {identity})')
    return used


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
    for flag in ('game', 'seeds', 'captures', 'fds_bios'):
        ap.add_argument('--' + flag.replace('_', '-'), dest=flag, type=Path)
    ap.add_argument('--fds-bios-only', action='store_true',
                    help='--rom is an FDS BIOS, compiled alone with no disk (runner/cyc/fds-bios)')
    args = ap.parse_args()
    rom, compiler, out = (p.resolve() for p in (args.rom, args.recompiler, args.out))
    dependencies = [rom, compiler]
    game = args.game.resolve() if args.game else None
    seeds = args.seeds.resolve() if args.seeds else None
    captures = args.captures.resolve() if args.captures else None
    if game:
        dependencies.append(game)
        config = tomllib.loads(game.read_text(encoding='utf-8'))
        configured_seed = config.get('game', {}).get('cycle_seed_file')
        if seeds is None and configured_seed:
            seeds = (game.parent / configured_seed).resolve()
        configured_captures = config.get('game', {}).get('cycle_capture_file')
        if captures is None and configured_captures:
            captures = (game.parent / configured_captures).resolve()
    if seeds:
        # A typo must not silently turn a profiled build into interpreter-only.
        dependencies.append(seeds)
    if captures:
        # The RAM capture file (the host's --capture-log) is a build input
        # like the seed file: its content decides which RAM views compile.
        dependencies.append(captures)
    # A Famicom Disk System title compiles the RAM Adapter BIOS; the image is
    # the disk. The BIOS comes from --fds-bios, game.toml [fds] bios, or
    # bios/disksys.rom beside the image, and must match its identity.
    bios = args.fds_bios.resolve() if args.fds_bios else (rom if args.fds_bios_only else None)
    fds = args.fds_bios_only or is_fds_image(rom) or bool(game and config.get('game', {}).get('fds'))
    if fds and bios is None:
        configured = config.get('fds', {}).get('bios') if game else None
        bios = (game.parent / configured).resolve() if configured else rom.parent / 'bios' / 'disksys.rom'
    if fds:
        if not bios.exists():
            raise RuntimeError(f'FDS title without its BIOS: {bios} does not exist')
        dependencies += check_fds_bios(bios)
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
        command = [str(compiler)] + ([] if args.fds_bios_only else [str(rom)])
        command += ['--game', str(config_path), '--cycle-accurate', '--output-prefix', 'game']
        if args.fds_bios_only:
            command += ['--fds-bios-only']
        if seeds:
            command += ['--cycle-seed-file', str(seeds)]
        if captures:
            command += ['--cycle-capture-file', str(captures)]
        if fds:
            command += ['--fds-bios', str(bios)]
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
