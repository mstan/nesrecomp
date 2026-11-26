#!/usr/bin/env python3
"""Configure, build and find the executables of a CMake project a test
generates (a compiled fixture program), with the toolchain the parent build
used.

The parent build (runner/cyc/CMakeLists.txt) writes <build>/nested_build.json
at configure time: its cmake, generator (and platform, toolset, instance), make
program, C compiler and, for a compiler that needs its environment at build
time (MSVC with a single-configuration generator such as Ninja), the INCLUDE,
LIB and LIBPATH it was configured with and the directories of cl, link, rc
and mt. A test given --toolchain <that file> builds the same way from any
shell: a plain ctest in a fresh configure, with Ninja or Visual Studio,
without a developer prompt. Nothing in it is a temporary path unless the parent
was configured with one.

Tests also accept the individual settings (--cmake, --generator, --platform,
--make-program, --c-compiler, --config), which override the file's. Without
either, cmake's own defaults apply (the first cmake on PATH, its default
generator).

  from nested_build import add_arguments, Toolchain
  add_arguments(ap); tc = Toolchain.from_args(args)
  tc.build(source_dir, build_dir, log_dir); exe = tc.executable(build_dir, 'name')
"""
import json
import os
from pathlib import Path
import subprocess

NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
EXE = '.exe' if os.name == 'nt' else ''


def add_arguments(ap):
    g = ap.add_argument_group('nested CMake builds')
    g.add_argument('--toolchain', type=Path, help="the parent build's nested_build.json")
    g.add_argument('--cmake', help='cmake executable')
    g.add_argument('--generator', help='CMake generator')
    g.add_argument('--platform', help='generator platform (-A)')
    g.add_argument('--toolset', help='generator toolset (-T)')
    g.add_argument('--make-program', help='CMAKE_MAKE_PROGRAM (single-configuration generators)')
    g.add_argument('--c-compiler', help='CMAKE_C_COMPILER (single-configuration generators)')
    g.add_argument('--config', help='configuration (default Release)')


class Toolchain:
    def __init__(self, data=None):
        d = dict(data or {})
        self.cmake = d.get('cmake') or 'cmake'
        self.generator = d.get('generator') or None
        self.platform = d.get('platform') or None
        self.toolset = d.get('toolset') or None
        self.instance = d.get('instance') or None
        self.multi_config = bool(d.get('multi_config'))
        self.make_program = d.get('make_program') or None
        self.c_compiler = d.get('c_compiler') or None
        self.tools = {k: v for k, v in (d.get('tools') or {}).items() if v}
        self.env = {k: v for k, v in (d.get('env') or {}).items() if v}
        self.path = [p for p in (d.get('path') or []) if p]
        self.config = d.get('config') or 'Release'

    @classmethod
    def load(cls, path):
        return cls(json.loads(Path(path).read_text(encoding='utf-8')))

    @classmethod
    def from_args(cls, args):
        tc = cls.load(args.toolchain) if getattr(args, 'toolchain', None) else cls()
        if getattr(args, 'cmake', None): tc.cmake = args.cmake
        if getattr(args, 'generator', None):
            if args.generator != tc.generator:
                # A different generator than the file's: none of its generator settings apply.
                tc.platform = tc.toolset = tc.instance = tc.make_program = None
                tc.multi_config = args.generator.startswith('Visual Studio') or 'Multi-Config' in args.generator \
                    or args.generator == 'Xcode'
            tc.generator = args.generator
        if getattr(args, 'platform', None): tc.platform = args.platform
        if getattr(args, 'toolset', None): tc.toolset = args.toolset
        if getattr(args, 'make_program', None): tc.make_program = args.make_program
        if getattr(args, 'c_compiler', None): tc.c_compiler = args.c_compiler
        if getattr(args, 'config', None): tc.config = args.config
        return tc

    def environment(self):
        """The process environment for cmake and the build."""
        env = dict(os.environ)
        env.update(self.env)
        if self.path:
            env['PATH'] = os.pathsep.join(self.path + [env.get('PATH', '')])
        return env

    def configure_command(self, source, binary, defines=()):
        cmd = [self.cmake, '-S', str(source), '-B', str(binary)]
        if self.generator:
            cmd += ['-G', self.generator]
        if self.platform:
            cmd += ['-A', self.platform]
        if self.toolset:
            cmd += ['-T', self.toolset]
        if self.instance:
            cmd += [f'-DCMAKE_GENERATOR_INSTANCE={self.instance}']
        if not self.multi_config:
            cmd += [f'-DCMAKE_BUILD_TYPE={self.config}']
            if self.make_program:
                cmd += [f'-DCMAKE_MAKE_PROGRAM={self.make_program}']
            if self.c_compiler:
                cmd += [f'-DCMAKE_C_COMPILER={self.c_compiler}']
            for var, tool in (('CMAKE_LINKER', 'linker'), ('CMAKE_RC_COMPILER', 'rc'), ('CMAKE_MT', 'mt'),
                              ('CMAKE_AR', 'ar')):
                if tool in self.tools:
                    cmd += [f'-D{var}={self.tools[tool]}']
        return cmd + list(defines)

    def build_command(self, binary, parallel):
        return [self.cmake, '--build', str(binary), '--config', self.config, '--parallel', str(parallel)]

    def run(self, cmd, cwd, log, timeout=None):
        # Retain the legacy argument for callers; finite workloads and resource
        # guards own completion, rather than elapsed-time process kills.
        p = subprocess.run([str(x) for x in cmd], cwd=cwd, capture_output=True, text=True,
                           env=self.environment(), creationflags=NO_WINDOW)
        Path(log).write_text(' '.join(str(x) for x in cmd) + '\n' + p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(f'exit {p.returncode}: {cmd[0]} {cmd[1]} ...; see {log}')
        return p

    def build(self, source, binary, logs, timeout=None, parallel=8, defines=()):
        """Configure and build; logs go to <logs>/configure.log and build.log."""
        logs = Path(logs)
        self.run(self.configure_command(source, binary, defines), logs, logs / 'configure.log', timeout)
        self.run(self.build_command(binary, parallel), logs, logs / 'build.log', timeout)

    def executable(self, binary, name):
        """The built program: <binary>/<config>/name for multi-configuration
        generators, <binary>/name otherwise."""
        binary = Path(binary)
        for exe in (binary / self.config / (name + EXE), binary / (name + EXE)):
            if exe.exists():
                return exe
        raise FileNotFoundError(f'{name}{EXE} was not built under {binary}')
