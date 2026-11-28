"""Build the port without opening native tool windows or launching the game."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import json
import re
import sys


def windows_toolchain():
    finder = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if not finder.is_file():
        return None, None
    result = subprocess.run([str(finder), '-latest', '-products', '*', '-requires',
                             'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-format', 'json'],
                            capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW, check=True)
    installations = json.loads(result.stdout)
    if not installations:
        return None, None
    info = installations[0]
    major = int(info['installationVersion'].split('.')[0])
    generator = {16: 'Visual Studio 16 2019', 17: 'Visual Studio 17 2022'}.get(major)
    cmake = Path(info['installationPath']) / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    return generator, cmake if cmake.is_file() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-root', required=True, type=Path)
    parser.add_argument('--project-version', default='development')
    parser.add_argument('--engine-root', type=Path)
    parser.add_argument('--recomp-ui', type=Path)
    parser.add_argument('--cmake-arg', action='append', default=[])
    parser.add_argument('--rom', required=True, type=Path)
    parser.add_argument('--build-dir', type=Path, default=Path('build'))
    parser.add_argument('--headless', action='store_true')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--cmake', type=Path)
    parser.add_argument('--generator')
    parser.add_argument('--version', help='Release version; defaults to the project version')
    parser.add_argument('--revision', help='Source commit recorded in the executable')
    args = parser.parse_args()
    if not args.rom.is_file():
        parser.error('ROM file does not exist')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    root = args.project_root.resolve()
    build = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    generator, bundled_cmake = windows_toolchain() if os.name == 'nt' else (None, None)
    cmake = args.cmake or bundled_cmake or shutil.which('cmake')
    if not cmake or not Path(cmake).is_file():
        parser.error('CMake is required; supply --cmake when it is not on PATH')
    version = args.version or args.project_version
    if version != 'development' and not re.fullmatch(r'\d+\.\d+\.\d+(?:[-+][A-Za-z0-9.-]+)?', version):
        parser.error('Version must be a semantic version, such as 0.1.0')
    configure = [str(cmake), '-S', str(root), '-B', str(build),
                 f'-DNESRECOMP_ROM={args.rom.resolve()}',
                 f'-DNESRECOMP_GAME_VERSION:STRING={version}',
                 f'-DNESRECOMP_RECOMP_UI:STRING={args.recomp_ui or root / "recomp-ui"}',
                 f'-DPython3_EXECUTABLE:FILEPATH={sys.executable}',
                 '-DNESRECOMP_BACKEND:STRING=cycle',
                 '-DNESRECOMP_PPU_IMPL:STRING=HLE',
                 f'-DNESRECOMP_ROOT:PATH={args.engine_root or root / "nesrecomp"}',
                 '-DNESRECOMP_ENABLE_TRACE:BOOL=OFF']
    if os.name == 'nt':
        cached_generator = None
        cache = build / 'CMakeCache.txt'
        if cache.is_file():
            match = re.search(r'^CMAKE_GENERATOR:INTERNAL=(.*)$', cache.read_text(), re.MULTILINE)
            if match:
                cached_generator = match[1].strip()
        selected_generator = args.generator or cached_generator or generator or 'Visual Studio 17 2022'
        configure += ['-G', selected_generator]
        if selected_generator.startswith('Visual Studio'):
            configure += ['-A', 'x64']
        else:
            configure += ['-DCMAKE_BUILD_TYPE=Release']
    else:
        configure += ['-DCMAKE_BUILD_TYPE=Release']
        if shutil.which('pkg-config'):
            if subprocess.run(['pkg-config', '--atleast-version=2.24.0', 'sdl2']).returncode:
                parser.error('SDL2 2.24 or newer is required; select its installation with PKG_CONFIG_PATH')
            sdl = subprocess.run(['pkg-config', '--variable=libdir', 'sdl2'],
                                 capture_output=True, text=True, check=True)
            directory = Path(sdl.stdout.strip()) / 'cmake/SDL2'
            if directory.is_dir():
                configure += [f'-DSDL2_DIR:PATH={directory}']
        if args.generator:
            configure += ['-G', args.generator]
    configure += [f'-DNESRECOMP_HEADLESS:BOOL={"ON" if args.headless else "OFF"}']
    if args.revision:
        configure += [f'-DNESRECOMP_BUILD_REVISION:STRING={args.revision}']
    configure += args.cmake_arg
    flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    for command in (configure, [str(cmake), '--build', str(build), '--config',
                               'Release', '--parallel', str(args.jobs)]):
        with subprocess.Popen(command, cwd=root, creationflags=flags,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, errors='replace') as process:
            for line in process.stdout:
                print(line, end='', flush=True)
            if process.wait():
                raise subprocess.CalledProcessError(process.returncode, command)


if __name__ == '__main__':
    main()
