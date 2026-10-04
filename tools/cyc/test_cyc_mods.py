#!/usr/bin/env python3
"""The mod surface on the cycle backend (CTest cyc_mods_test; no owner data).

tools/cyc/fds_mod_fixtures.py writes a synthetic FDS program (overlays A and B
loaded at the same address in turn) and a game.toml declaring two hook sites;
runner/cyc/tests/mods/mod_test_extras.c is a test game's plugin. Checks:

  codegen   a RAM site without a content key, and one no disk file holds, are
            refused; sites compile to hook exits only where the image holds the key
  inert     the plugin build with every hook disabled runs exactly as the same
            program without the plugin (--hash-out, native and --interp-only)
  hooks     the overlay site fires only while overlay A is resident (20 of 30
            frames; the interpreter counts the other 10 as content mismatches,
            compiled overlay B has no hook exit there at all) and identically
            on recompiled code and the interpreter; the handled site returns as
            its RTS would (its own store never happens, the plugin's does) and
            sees the caller's registers; the ring has per-frame mod.hook events
  isolation isolated calls every frame (RAM pokes, a memory routine, a device
            store) change nothing: --hash-out identical to the inert run, the
            routine's result was right inside, RAM back afterwards, and the ring
            has mod.call summaries
  commit    a committed call that stores to a device register is refused; a
            memory-only one lands its result in the running machine
  states    a save state taken mid-run (hooks on) and loaded in a new process
            continues the --hash-out lines exactly (trace, memory, hardware),
            and the plugin's own counters come back from its mod record
  binding   a plugin registered for a site the program does not declare stops
            the program at start

python tools/cyc/test_cyc_mods.py --recompiler build/cyc/recompiler/NESRecomp.exe --out build/cyc/mods_test \\
    [--toolchain build/cyc/nested_build.json] [--source RUNNER_CYC_DIR]
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nested_build                        # noqa: E402

NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
FRAMES = 40


def run(cmd, cwd, log, timeout=900, ok=(0,)):
    p = subprocess.run([str(x) for x in cmd], cwd=cwd, capture_output=True, text=True, timeout=timeout,
                       creationflags=NO_WINDOW)
    Path(log).write_text(' '.join(str(x) for x in cmd) + '\n' + p.stdout + p.stderr)
    if ok is not None and p.returncode not in ok:
        raise RuntimeError(f'exit {p.returncode}: {cmd}; see {log}')
    return p


def ram_from_mem(path):
    ram = bytearray(0x800)
    for line in Path(path).read_text().splitlines():
        if line.startswith('ram '):
            addr, rest = line[4:].split(':')
            base = int(addr, 16)
            for i, b in enumerate(rest.split()):
                ram[base + i] = int(b, 16)
    return ram


def build(args, out, name, extras, defines=()):
    folder = out / name
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    shutil.copy(out / 'game.toml', folder / 'game.toml')
    text = (folder / 'game.toml').read_text().replace('"disk.fds"', '"../disk.fds"').replace(
        '"bios/disksys.rom"', '"../bios/disksys.rom"')
    (folder / 'game.toml').write_text(text, newline='\n')
    run([args.recompiler.resolve(), '--game', 'game.toml'], folder, folder / 'codegen.log')
    lines = ['cmake_minimum_required(VERSION 3.20)', 'project(cyc_mods C)', 'set(CMAKE_C_STANDARD 11)',
             f'include("{args.source_.as_posix()}/cyc.cmake")',
             'file(GLOB GEN CONFIGURE_DEPENDS "generated/*_cyc*.c")',
             'add_executable(mods ${NESRECOMP_CYC_SOURCES} ${GEN}' +
             (f' "{(args.source_ / "tests" / "mods" / "mod_test_extras.c").as_posix()}"' if extras else '') + ')',
             'target_include_directories(mods PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})',
             'target_link_libraries(mods PRIVATE ${NESRECOMP_CYC_LIBRARIES})',
             'target_compile_definitions(mods PRIVATE _CRT_SECURE_NO_WARNINGS ' + ' '.join(defines) + ')']
    (folder / 'CMakeLists.txt').write_text('\n'.join(lines) + '\n')
    args.toolchain_.build(folder, folder / 'build', folder)
    return args.toolchain_.executable(folder / 'build', 'mods'), folder


def play(exe, folder, out, tag, extra, frames=FRAMES, ok=(0,)):
    trace = folder / f'{tag}.hash'
    mem = folder / f'{tag}.mem'
    p = run([exe, out / 'disk.fds', '--fds-bios', out / 'bios' / 'disksys.rom', '--frames', frames,
             '--hash-out', trace, '--mem-frame', frames - 1, '--mem-out', mem] + extra, folder,
            folder / f'{tag}.log', ok=ok)
    return p, (trace.read_text().splitlines() if trace.exists() else []), (ram_from_mem(mem) if mem.exists() else None)


def counters(stdout):
    m = re.search(r'mod-test: (.*)', stdout)
    if not m:
        raise AssertionError('no mod-test report')
    return {k: int(v) for k, v in (kv.split('=') for kv in m[1].split())}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--recompiler', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    nested_build.add_arguments(ap)
    ap.add_argument('--source', type=Path, help='runner/cyc sources to build (default: this checkout)')
    args = ap.parse_args()
    args.toolchain_ = nested_build.Toolchain.from_args(args)
    here = Path(__file__).resolve().parent
    args.source_ = (args.source or here.parents[1] / 'runner' / 'cyc').resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    run([sys.executable, here / 'fds_mod_fixtures.py', '--out', out], out, out / 'fixtures.log')
    layout = {n: int(a, 16) for n, a in (l.split() for l in (out / 'layout.txt').read_text().splitlines())}

    # ---- codegen: keys ----
    game = (out / 'game.toml').read_text()
    for tag, text, why in (('nokey', re.sub(r'length = 6\ncrc32 = 0x[0-9A-F]+\n', '', game), 'needs bytes'),
                           ('nofile', re.sub(r'crc32 = 0x[0-9A-F]+', 'crc32 = 0x12345678', game), 'no disk file holds')):
        folder = out / f'codegen_{tag}'
        folder.mkdir(exist_ok=True)
        (folder / 'game.toml').write_text(text.replace('"disk.fds"', '"../disk.fds"').replace(
            '"bios/disksys.rom"', '"../bios/disksys.rom"'), newline='\n')
        p = run([args.recompiler.resolve(), '--game', 'game.toml'], folder, folder / 'codegen.log', ok=None)
        if p.returncode == 0 or why not in p.stdout + p.stderr:
            raise AssertionError(f'codegen {tag}: expected a refusal ("{why}")')
    print('codegen: a RAM site without a key and one no file holds are refused')

    plain, fplain = build(args, out, 'plain', False)
    exe, fmods = build(args, out, 'mods', True)
    gen = ''.join(p.read_text() for p in (fmods / 'generated').glob('mods_cyc_r*.c'))
    exits = re.findall(r'cyc_hook_stop\(0x([0-9A-F]{4})\)', gen)
    if exits.count('7000') != 1 or f'{layout["tail"]:04X}' not in exits:
        raise AssertionError(f'codegen: hook exits {exits}: expected one at $7000 (overlay A only) and one at tail')
    print(f'codegen: hook exits at {sorted(set(exits))} (overlay A\'s view only at $7000)')

    # ---- inert and isolation: the machine is untouched ----
    _, base, base_ram = play(plain, fplain, out, 'plain', [])
    for mode in ('none', 'iso'):
        for flavour, extra in (('native', []), ('interp', ['--interp-only'])):
            p, lines, _ = play(exe, fmods, out, f'{mode}_{flavour}', ['--test-mode', mode] + extra)
            if lines != base:
                i = next((i for i, (x, y) in enumerate(zip(base, lines)) if x != y), min(len(base), len(lines)))
                raise AssertionError(f'{mode} {flavour}: --hash-out differs from the plain program at frame {i}')
            if mode == 'iso':
                c = counters(p.stdout)
                if c['iso_ok'] < 30 or c['iso_bad']:
                    raise AssertionError(f'iso {flavour}: {c}')
    print(f'inert + isolation: {FRAMES} frames identical to the plain program, native and interpreted; '
          f'{c["iso_ok"]} isolated scopes restored the machine')
    # Explicit isolated hooks and return observers share the same scheduler on
    # generated code and the interpreter, including a handled routine's RTS.
    for flavour, extra in (('native', []), ('interp', ['--interp-only'])):
        p, lines, _ = play(exe, fmods, out, f'observe_{flavour}', ['--test-mode', 'observe'] + extra)
        m = re.search(r'return-test: ok=(\d+) bad=(\d+)', p.stdout)
        if lines != base or not m or int(m[1]) < 30 or int(m[2]):
            raise AssertionError(f'observers {flavour}: {m and m.groups()}')
    print('return observers: actual and handled RTS, explicit isolated hook permission, full machine parity')
    ring = fmods / 'iso_ring.txt'
    run([exe, out / 'disk.fds', '--fds-bios', out / 'bios' / 'disksys.rom', '--frames', FRAMES, '--test-mode', 'iso',
         '--ring-out', ring], fmods, fmods / 'iso_ring.log')
    calls = [l for l in ring.read_text().splitlines() if ' mod.call ' in l]
    if len(calls) != 2 * c['iso_ok'] or not all('calls=1' in l for l in calls):
        raise AssertionError(f'iso: {len(calls)} mod.call ring events, expected {2 * c["iso_ok"]}')

    # ---- hooks ----
    results = {}
    for flavour, extra in (('native', []), ('interp', ['--interp-only'])):
        p, lines, ram = play(exe, fmods, out, f'hooks_{flavour}', ['--test-mode', 'hooks'] + extra)
        c = counters(p.stdout)
        nmis = ram[0x0460]          # the program's frames (one per NMI)
        if nmis < 31 or c['ova_fires'] != 20 or c['tail_handled'] != nmis or c['regs_bad']:
            raise AssertionError(f'hooks {flavour}: {c}, {nmis} NMIs')
        if ram[0x0450] != 0x77 or ram[0x0451] != 0 or base_ram[0x0451] != 0x88:
            raise AssertionError(f'hooks {flavour}: $0450={ram[0x450]:02X} $0451={ram[0x451]:02X}')
        m = re.search(r'mod hooks: 2 sites, (\d+) callbacks run, (\d+) handled, (\d+) content mismatches', p.stdout)
        # Content mismatches are seen where the scheduler looks: every
        # interpreted instruction; compiled overlay B has no exit at $7000.
        if not m or (int(m[1]), int(m[2]), int(m[3])) != (20 + nmis, nmis, 10 if extra else 0):
            raise AssertionError(f'hooks {flavour}: summary {m and m.groups()}')
        results[flavour] = lines
    if results['native'] != results['interp']:
        raise AssertionError('hooks: native and --interp-only runs differ')
    ring = fmods / 'hooks_ring.txt'
    run([exe, out / 'disk.fds', '--fds-bios', out / 'bios' / 'disksys.rom', '--frames', FRAMES, '--test-mode',
         'hooks', '--ring-out', ring], fmods, fmods / 'hooks_ring.log')
    hook_events = [l for l in ring.read_text().splitlines() if ' mod.hook ' in l]
    if len(hook_events) != 20 + nmis:
        raise AssertionError(f'hooks: {len(hook_events)} mod.hook ring events, expected {20 + nmis}')
    print(f'hooks: overlay site 20/30 (10 content mismatches), handled site {nmis}/{nmis}, native = interpreter, '
          'ring summaries')

    # ---- commit ----
    p, _, ram = play(exe, fmods, out, 'commit', ['--test-mode', 'commit'])
    c = counters(p.stdout)
    if c['commit_refused'] != 1 or c['commit_ok'] != 1 or ram[0x0440] != 16:
        raise AssertionError(f'commit: {c} $0440={ram[0x440]}')
    print('commit: a device store refused, a memory routine committed')

    # ---- save states ----
    state = fmods / 'mid.state'
    p1, full, _ = play(exe, fmods, out, 'state_full', ['--test-mode', 'hooks', '--save-state', f'19:{state}'])
    p2, loaded, _ = play(exe, fmods, out, 'state_loaded', ['--test-mode', 'hooks', '--load-state', state])
    if loaded != full[20:]:
        i = next((i for i, (x, y) in enumerate(zip(full[20:], loaded)) if x != y), 0)
        raise AssertionError(f'states: the loaded run differs at frame {20 + i}')
    c1, c2 = counters(p1.stdout), counters(p2.stdout)
    if c1 != c2:
        raise AssertionError(f'states: plugin counters {c1} vs {c2}')
    print(f'states: frames 20-{FRAMES - 1} identical after a load in a new process; mod record restored')

    # ---- binding ----
    bad, fbad = build(args, out, 'badsite', True, ['MOD_TEST_BAD_SITE'])
    p = run([bad, out / 'disk.fds', '--fds-bios', out / 'bios' / 'disksys.rom', '--frames', 5], fbad,
            fbad / 'run.log', ok=None)
    if p.returncode == 0 or 'declares no such' not in p.stderr:
        raise AssertionError('binding: a plugin at an undeclared site did not stop the program')
    print('binding: a plugin at an undeclared site stops the program')
    return 0


if __name__ == '__main__':
    sys.exit(main())
