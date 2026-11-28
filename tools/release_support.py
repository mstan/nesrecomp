"""Common package checks and notices for NES game release recipes."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


def run(argv, **kwargs):
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    return subprocess.run([str(x) for x in argv], check=True, creationflags=flags, **kwargs)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def cache_values(build):
    return dict(re.findall(r'^([^:#=\r\n]+):[^=\r\n]+=([^\r\n]*)$',
                           (Path(build) / 'CMakeCache.txt').read_text(), re.MULTILINE))


def check_files(names):
    for name in names:
        p = Path(name)
        if p.suffix.lower() in {'.nes', '.fds', '.sav', '.srm', '.cycstate', '.state', '.log', '.dmp'}:
            raise RuntimeError(f'Player or original-image data in package: {name}')
        if p.name.lower() in {'rom.cfg', 'config.ini', 'state.toml', 'keybinds.ini', 'debug.ini'}:
            raise RuntimeError(f'Player settings in package: {name}')
        if 'cd-speed' in str(name).lower() or 'fast-loading' in str(name).lower():
            raise RuntimeError(f'Forbidden release package: {name}')


def collect_licenses(stage, cache, project):
    project, stage = Path(project), Path(stage)
    engine = Path(cache['NESRECOMP_ROOT'])
    ui = Path(cache.get('NESRECOMP_RECOMP_UI') or cache.get('RECOMP_UI_ROOT') or project / 'recomp-ui')
    notices = {'NESRecomp-LICENSE.txt': engine / 'LICENSE',
               'SDL2-COPYING.txt': engine / 'runner/external/SDL2/COPYING.txt',
               'recomp-ui-LICENSE.txt': ui / 'LICENSE',
               'ImGui-LICENSE.txt': ui / 'src/third_party/imgui/LICENSE.txt'}
    if (project / 'LICENSE').is_file():
        notices['Game-LICENSE.txt'] = project / 'LICENSE'
    if cache.get('SDL2_DIR'):
        notice = Path(cache['SDL2_DIR']).parents[2] / 'share/licenses/SDL2/LICENSE.txt'
        if notice.is_file():
            notices['SDL2-COPYING.txt'] = notice
    for name in ('LatoLatin-OFL.txt', 'NotoSansSymbols2-OFL.txt', 'OpenMoji-LICENSE.txt'):
        notices[name] = engine / 'tools/release-licenses' / name
    for key, fallback, name in [('recomp_net_SOURCE_DIR', engine / 'lib/recomp-net', 'recomp-net'),
                                ('retcomm_rbengine_SOURCE_DIR', engine / 'lib/retcomm-rbengine', 'retcomm-rbengine')]:
        notices[name + '-LICENSE.txt'] = Path(cache.get(key, str(fallback))) / 'LICENSE'
    for provider in ('mod', 'persistence', 'input', 'diagnostics'):
        directory = Path(cache.get('NESRECOMP_SHARED_CORE_ROOT', str(engine / 'external/shared-cores')))
        notice = directory / ('recomp-' + provider + '-core') / 'LICENSE'
        if notice.is_file():
            notices['recomp-' + provider + '-core-LICENSE.txt'] = notice
    dest = stage / 'licenses'
    dest.mkdir(exist_ok=True)
    for name, path in notices.items():
        shutil.copy2(path, dest / name)
    return notices


def verify_zip(path, expected):
    """Verify safe unique paths, CRCs and exact staged bytes."""
    with zipfile.ZipFile(path) as archive:
        seen = set()
        for item in archive.infolist():
            name = item.filename
            if item.is_dir():
                continue
            if '\\' in name or name.startswith('/') or '..' in Path(name).parts or name in seen:
                raise RuntimeError(f'Unsafe or duplicate archive path: {name}')
            seen.add(name)
            if name not in expected or archive.read(item) != expected[name]:
                raise RuntimeError(f'Archive content differs from staging: {name}')
        if seen != set(expected):
            raise RuntimeError('Archive missing staged payload')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', required=True, type=Path)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--project', required=True, type=Path)
    parser.add_argument('--licenses', action='store_true')
    args = parser.parse_args()
    if args.licenses:
        collect_licenses(args.stage, cache_values(args.build), args.project)
    check_files(p.relative_to(args.stage) for p in args.stage.rglob('*') if p.is_file())


if __name__ == '__main__':
    main()
