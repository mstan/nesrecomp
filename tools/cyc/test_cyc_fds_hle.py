#!/usr/bin/env python3
"""The FDS HLE tier on the whole machine (CTest cyc_fds_hle_test; no owner data).

tools/cyc/fds_hle_fixtures.py writes a synthetic BIOS-and-game program whose
disk-ID check is declared as its anchor in bios/hle.toml, and 2- and 4-sided
disks, one per scenario. The program is compiled from a disk (NESRecomp
--game, BIOS identity from the synthetic .toml), once plainly and once with
game.toml [fds] hle = "auto-swap", and run native, --interp-only and on the
standalone cyc_interp:

  scenarios    every scenario's results in CPU RAM and its ring events (disk-ID
               requests with the sides they match, HLE decisions, fds.side
               changes) with auto swap on and off, at all four alignments
  no request   a side different from the one ejected is only ever inserted
               after a request naming it (fds.hle swap) or a wait round; a
               program that never asks (none, poller, ambiguous, nomatch, keep)
               ends on the side it booted from
  parity       native = --interp-only = cyc_interp --hash-out with auto swap on
  identity     fast load alone hashes exactly as HLE off, and auto swap + fast
               load exactly as auto swap: fast load never changes the machine
  config       game.toml [fds] hle is the default; NESRECOMP_FDS_HLE overrides
               it and --fds-hle overrides both; a bad word is refused (exit 2);
               the two compiles differ only in the umbrella's hle line
  refusals     no anchor in the BIOS identity, or a one-sided disk: auto swap
               is refused with the reason and the run is the HLE-off run; fast
               load is still granted (the axes are independent)
  host         a host eject/insert before the tier acts takes the drive back:
               no HLE disk change follows
  fast load    load spans in the ring (fds.span, marked fast when fast load ran
               them), and the host's count of load frames it runs unpaced: all
               of them with fast load, none without; the machine is the same.
               Deterministic: no wall-clock time is measured here (the timing
               check is tools/cyc/perf_fds_fast_load.py, a local perf script)

python tools/cyc/test_cyc_fds_hle.py --recompiler build/compiler/NESRecomp.exe \\
    --interp build/cyc/cyc_interp.exe --out build/cyc-fds-hle [--toolchain build/cyc/nested_build.json]
    [--no-native]   (cyc_interp only: the quick form the mutation checks use)
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fds_hle_fixtures as fx      # noqa: E402
import nested_build                # noqa: E402

NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
FRAMES = 900
PREDELAY_MIN = fx.PREDELAY + 10     # a load: the ID check's wait, spin-up, lead-in and blocks
checks = 0

# CPU RAM results (tools/cyc/fds_hle_fixtures.py): field -> offset from $0400
FIELDS = {'t': 0x12, 'fail': 0x13, 'ej': 0x16, 'empty': 0x17, 'rej': 0x18, 'done': 0x20}
# HLE decisions each scenario must produce with auto swap on (after the boot request's keep), in order
DECISIONS = {
    1: ['swap 1', 'eject 0', 'insert 1'],
    2: ['wait 0', 'eject 0', 'insert 0', 'swap 1', 'eject 0', 'insert 1'],
    3: [],
    4: ['wait 0', 'eject 0', 'insert 0'],
    5: ['ambiguous', 'ambiguous', 'ambiguous'],
    6: ['nomatch', 'nomatch', 'nomatch'],
    7: ['swap 3', 'eject 0', 'insert 3'],
    8: ['wait 0', 'eject 0', 'insert 0', 'wait 1', 'eject 0', 'insert 1', 'keep 1'],
    9: ['keep 0'],
    10: [],
    11: ['wait 0', 'eject 0', 'insert 0', 'swap 1', 'eject 0', 'insert 1',
         'wait 1', 'eject 1', 'insert 1', 'swap 0', 'eject 1', 'insert 0'],
}
# expected results with auto swap on (ON) and with the tier off (OFF)
DISKS = {1: 'disk2_1', 2: 'disk2_2', 3: 'disk2_3', 4: 'disk2_4', 5: 'disk4_5', 6: 'disk2_6', 7: 'disk4_7',
         8: 'disk2_8', 9: 'disk2_9', 10: 'disk2_10', 11: 'disk2_11'}
ON = {1: dict(t=0x11, fail=0, ej=0, done=0xC3), 2: dict(t=0x11, fail=0, ej=1, empty=10, done=0xC3),
      3: dict(t=0x10, fail=0, ej=0, done=0xC3), 4: dict(t=0x10, empty=10, done=0xC3),
      5: dict(t=0x10, fail=3, done=0xC3), 6: dict(t=0x10, fail=3, done=0xC3), 7: dict(t=0x13, fail=0, done=0xC3),
      8: dict(t=0x11, fail=0, ej=2, rej=1, done=0xC3), 9: dict(t=0x10, fail=0, done=0xC3),
      10: dict(t=0x10, fail=0, done=0xC3), 11: dict(t=0x10, fail=0, ej=2, empty=20, done=0xC3)}
OFF = {1: dict(t=0x10, done=0), 2: dict(t=0x10, ej=0, done=0), 3: dict(t=0x10, fail=0, ej=0, done=0xC3),
       4: dict(t=0x10, empty=0, done=0xC3), 5: dict(t=0x10, fail=3, done=0xC3), 6: dict(t=0x10, fail=3, done=0xC3),
       7: dict(t=0x10, done=0), 8: dict(t=0x10, ej=0, done=0), 9: dict(t=0x10, fail=0, done=0xC3),
       10: dict(t=0x10, fail=0, done=0xC3), 11: dict(t=0x10, ej=0, done=0)}


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        raise AssertionError(what)


def run(cmd, cwd, log, env=None, ok=(0,), timeout=900):
    e = {k: v for k, v in os.environ.items() if k != 'NESRECOMP_FDS_HLE'}
    if env:
        e.update(env)
    p = subprocess.run([str(x) for x in cmd], cwd=cwd, capture_output=True, text=True, timeout=timeout, env=e,
                       creationflags=NO_WINDOW)
    Path(log).write_text(' '.join(str(x) for x in cmd) + '\n' + p.stdout + p.stderr)
    if p.returncode not in ok:
        raise RuntimeError(f'exit {p.returncode}: {cmd}; see {log}')
    return p


def build(args, out, name, hle):
    folder = out / name
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    extra = f'hle = "{hle}"\n' if hle else ''
    (folder / 'game.toml').write_text(f'[game]\noutput_prefix = "hle"\ncycle_accurate = true\nfds = true\n\n'
                                      f'[fds]\nimage = "../fx/disk2_2.fds"\nbios = "../fx/bios/hle.rom"\n{extra}',
                                      newline='\n')
    run([args.recompiler.resolve(), '--game', 'game.toml'], folder, folder / 'codegen.log')
    source = (HERE.parent.parent / 'runner' / 'cyc').resolve()
    (folder / 'CMakeLists.txt').write_text('\n'.join([
        'cmake_minimum_required(VERSION 3.20)', f'project(cyc_fds_hle_{name} C)', 'set(CMAKE_C_STANDARD 11)',
        f'include("{source.as_posix()}/cyc.cmake")',
        'file(GLOB GEN CONFIGURE_DEPENDS "generated/*_cyc*.c")',
        f'add_executable({name} ${{NESRECOMP_CYC_SOURCES}} ${{GEN}})',
        f'target_include_directories({name} PRIVATE ${{NESRECOMP_CYC_INCLUDE_DIRS}})',
        f'target_link_libraries({name} PRIVATE ${{NESRECOMP_CYC_LIBRARIES}})',
        f'target_compile_definitions({name} PRIVATE _CRT_SECURE_NO_WARNINGS)']) + '\n')
    args.toolchain_.build(folder, folder / 'build', folder)
    return args.toolchain_.executable(folder / 'build', name), folder


class Run:
    """One run: CPU RAM at the last frame, ring events, hash trace, stdout."""

    def __init__(self, exe, image, bios, tag, out, extra=(), env=None, frames=FRAMES, ok=(0,)):
        self.tag = tag
        mem, ring, hashes = out / f'{tag}.mem', out / f'{tag}.ring', out / f'{tag}.hash'
        cmd = [exe, image, '--fds-bios', bios, '--frames', frames, '--mem-frame', frames - 1, '--mem-out', mem,
               '--ring-out', ring, '--hash-out', hashes] + list(extra)
        p = run(cmd, out, out / f'{tag}.log', env=env, ok=ok)
        self.rc, self.stdout = p.returncode, p.stdout
        if p.returncode:
            return
        ram = []
        for line in open(mem):
            if line.startswith('ram '):
                ram += [int(x, 16) for x in line.split()[2:]]
        self.ram = ram
        self.hash = hashes.read_text()
        self.events = []
        for line in open(ring):
            if line.startswith('#'):
                continue
            parts = line.split(None, 4)
            if len(parts) >= 5 and parts[3] in ('fds.hle', 'fds.side', 'fds.idreq', 'fds.idbytes', 'fds.span'):
                self.events.append((int(parts[1]), parts[3], parts[4].strip()))

    def value(self, field):
        return self.ram[0x400 + FIELDS[field]]

    def decisions(self):
        """fds.hle decisions after the boot request, as 'swap 1', 'eject 0', ..."""
        out, boot = [], True
        for _, kind, text in self.events:
            if kind != 'fds.hle' or text.startswith('config'):
                continue
            word = text.split()[0]
            if boot and word == 'keep':
                boot = False
                continue
            side = re.search(r'(?:side|put_back)=(\d+)', text)
            out.append(f'{word} {side.group(1)}' if side and word != 'ambiguous' and word != 'nomatch' else word)
        return out

    def sides(self):
        return [(f, text) for f, kind, text in self.events if kind == 'fds.side']


def check_ram(r, want, what):
    for field, value in want.items():
        check(r.value(field) == value, f'{what}: ${0x400 + FIELDS[field]:04X} = {r.value(field):02X}, want {value:02X}')


def check_holds(r, scenario):
    """The drive stays empty SWAP_HOLD frames in a requested swap, WAIT_HOLD after a wait."""
    hold, out = None, None
    for f, kind, text in r.events:
        if kind == 'fds.hle' and (text.startswith('swap') or text.startswith('wait')):
            hold = 3 if text.startswith('swap') else 10
        elif kind == 'fds.side' and '(hle)' in text:
            if text.startswith('ejected'):
                out = f
            else:
                check(out is not None and f - out == hold,
                      f'scenario {scenario} {r.tag}: drive empty {None if out is None else f - out} frames, want {hold}')
                out = None


def check_no_unrequested_swap(r, scenario):
    """A different side goes in only after a swap decision or a wait round naming it."""
    ejected, allowed = None, set()
    for _, kind, text in r.events:
        if kind == 'fds.hle':
            m = re.match(r'(swap|wait) (?:side|put_back)=(\d+)', text)
            if m:
                allowed.add(int(m.group(2)))
        elif kind == 'fds.side' and '(hle)' in text:
            if text.startswith('ejected'):
                ejected = True
            else:
                side = int(re.search(r'side (\d+)', text).group(1))
                check(side in allowed, f'scenario {scenario} {r.tag}: side {side} inserted with no request for it')
                allowed.discard(side)
    if scenario in (3, 4, 5, 6, 9, 10):
        last = [t for _, t in r.sides()][-1]
        check(last.startswith('side 0 inserted'), f'scenario {scenario} {r.tag}: ends with "{last}", not side 0')
        check(not any(k == 'fds.hle' and t.startswith('swap') for _, k, t in r.events),
              f'scenario {scenario} {r.tag}: a swap decision with no request for another side')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--recompiler', type=Path, required=True)
    ap.add_argument('--interp', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    nested_build.add_arguments(ap)
    ap.add_argument('--no-native', action='store_true', help='cyc_interp only (the quick form for mutation checks)')
    ap.add_argument('--align', type=int, nargs='*', default=[0, 1, 2, 3])
    args = ap.parse_args()
    args.toolchain_ = nested_build.Toolchain.from_args(args)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    fxdir = out / 'fx'
    run([sys.executable, HERE / 'fds_hle_fixtures.py', '--out', fxdir], out, out / 'fixtures.log')
    bios = fxdir / 'bios' / 'hle.rom'
    interp = args.interp.resolve()
    runs = out / 'runs'
    runs.mkdir(exist_ok=True)

    if args.no_native:
        native = plain_dir = config = config_dir = None
    else:
        native, plain_dir = build(args, out, 'plain', None)
        config, config_dir = build(args, out, 'config', 'auto-swap')
        # the compiles differ only in the umbrella's hle line
        gp, gc = plain_dir / 'generated', config_dir / 'generated'
        names = sorted(p.name for p in gp.glob('*_cyc*.c'))
        check(names == sorted(p.name for p in gc.glob('*_cyc*.c')) and len(names) >= 2, 'generated file sets')
        for n in names:
            a, b = (gp / n).read_text().splitlines(), (gc / n).read_text().splitlines()
            if n == 'hle_cyc.c':
                diff = [(x, y) for x, y in zip(a, b) if x != y]
                check(len(a) == len(b) and len(diff) == 1 and 'cyc_native_fds_hle = NULL' in diff[0][0] and
                      'cyc_native_fds_hle = "auto-swap"' in diff[0][1], f'umbrella differs beyond the hle line: {diff}')
            else:
                check(a == b, f'{n} differs between the plain and the hle compiles')
    main_exe = native or interp

    # ---- scenarios, both modes, four alignments
    for scenario, disk in DISKS.items():
        image = fxdir / f'{disk}.fds'
        for align in args.align:
            on = Run(main_exe, image, bios, f's{scenario}_a{align}_on', runs, ['--align', align, '--fds-hle', 'auto-swap'])
            check_ram(on, ON[scenario], f'scenario {scenario} align {align} auto swap')
            check(on.decisions() == DECISIONS[scenario],
                  f'scenario {scenario} align {align}: decisions {on.decisions()}, want {DECISIONS[scenario]}')
            check_no_unrequested_swap(on, scenario)
            check_holds(on, scenario)
            check(on.value('t') != 0 and on.ram[0x410] == 0x10 and on.ram[0x411] == scenario, 'boot')
            off = Run(main_exe, image, bios, f's{scenario}_a{align}_off', runs, ['--align', align])
            check_ram(off, OFF[scenario], f'scenario {scenario} align {align} HLE off')
            check(not any(k == 'fds.hle' for _, k, _ in off.events), f'scenario {scenario}: HLE events with HLE off')
            if scenario == 3:
                # nothing swapped: the same machine, but with auto swap on its state is hashed with the hardware's
                strip = lambda h: [l.split(' hw=')[0] for l in h.splitlines()]
                check(strip(on.hash) == strip(off.hash), 'scenario 3: auto swap changed a machine it left alone')
                check(on.hash != off.hash, 'scenario 3: the auto-swap state is not in the hardware hash')
            check(all('(hle)' not in t for _, t in off.sides()), f'scenario {scenario}: HLE side change with HLE off')
            # the observation is always on: every request is recorded
            req_on = [t for _, k, t in on.events if k == 'fds.idreq']
            req_off = [t for _, k, t in off.events if k == 'fds.idreq']
            check(len(req_on) >= 1 and len(req_off) >= 1, f'scenario {scenario}: no disk-ID requests recorded')
            check(req_off[0] == req_on[0] and 'matches=1 drive=0' in req_on[0], f'scenario {scenario}: boot request')
            boot_frame = next(f for f, k, _ in off.events if k == 'fds.idreq')
            check(boot_frame >= fx.BOOT_POLLS, f'scenario {scenario}: a request before the boot load ({boot_frame})')
            # the boot load is one span from its request, every frame of it loading
            # (a program that keeps retrying keeps its span open to the end of the run)
            span = next((t for _, k, t in off.events if k == 'fds.span'), None)
            check(span is not None or scenario in (1, 7), f'scenario {scenario}: the boot load made no span')
            m = re.match(r'first=(\d+) frames=(\d+) load_frames=(\d+)', span or 'first=0 frames=0 load_frames=0')
            check(span is None or int(m.group(1)) == boot_frame,
                  f'scenario {scenario}: the boot span starts at {m.group(1)}, not at the request ({boot_frame})')
            if scenario in (3, 4, 10):
                check(m.group(2) == m.group(3) and int(m.group(2)) >= PREDELAY_MIN,
                      f'scenario {scenario}: boot span {span}: a frame of the load not counted')
            # fast load never changes the machine
            if scenario in (1, 2, 7, 8, 11) or align == 0:
                fast = Run(main_exe, image, bios, f's{scenario}_a{align}_fast', runs,
                           ['--align', align, '--fds-hle', 'fast-load'])
                check(fast.hash == off.hash, f'scenario {scenario} align {align}: fast load changed the machine')
                both = Run(main_exe, image, bios, f's{scenario}_a{align}_both', runs,
                           ['--align', align, '--fds-hle', 'auto-swap,fast-load'])
                check(both.hash == on.hash, f'scenario {scenario} align {align}: fast load changed the auto-swap run')
                spans = [t for _, k, t in both.events if k == 'fds.span']
                check(spans and all(t.endswith(' fast') for t in spans), f'scenario {scenario}: fast spans')
                check(all(not t.endswith(' fast') for _, k, t in off.events if k == 'fds.span'), 'spans off')
                check(any(int(re.search(r'load_frames=(\d+)', t).group(1)) >= 20 for t in spans), 'span length')
            # parity: native = --interp-only = cyc_interp
            if native and (align == 0 or scenario in (2, 7, 8, 11)):
                io = Run(native, image, bios, f's{scenario}_a{align}_on_interp', runs,
                         ['--align', align, '--fds-hle', 'auto-swap', '--interp-only'])
                ci = Run(interp, image, bios, f's{scenario}_a{align}_on_ci', runs,
                         ['--align', align, '--fds-hle', 'auto-swap'])
                check(io.hash == on.hash, f'scenario {scenario} align {align}: --interp-only differs from native')
                check(ci.hash == on.hash, f'scenario {scenario} align {align}: cyc_interp differs from native')
        print(f'scenario {scenario} ({fx.SCENARIOS[scenario]}): ok, decisions {DECISIONS[scenario]}', flush=True)

    # ---- config precedence (the program compiled with [fds] hle = "auto-swap")
    image = fxdir / 'disk2_1.fds'
    if config:
        for tag, extra, env, want_on in [('cfg_default', [], None, True),
                                         ('cfg_cli_off', ['--fds-hle', 'off'], None, False),
                                         ('cfg_env_off', [], {'NESRECOMP_FDS_HLE': 'off'}, False),
                                         ('cfg_env_off_cli_on', ['--fds-hle', 'auto-swap'], {'NESRECOMP_FDS_HLE': 'off'}, True),
                                         ('cfg_env_fast', [], {'NESRECOMP_FDS_HLE': 'no-auto-swap,fast-load'}, False)]:
            r = Run(config, image, bios, tag, runs, extra, env)
            check_ram(r, ON[1] if want_on else OFF[1], tag)
            source = 'game.toml' if tag == 'cfg_default' else 'cli' if extra else 'env'
            check(f'auto-swap {"on" if want_on else "off"} ({source})' in r.stdout, f'{tag}: banner {r.stdout[:300]}')
        r = Run(native, image, bios, 'plain_env_on', runs, [], {'NESRECOMP_FDS_HLE': 'auto-swap'})
        check_ram(r, ON[1], 'plain program, NESRECOMP_FDS_HLE=auto-swap')
    for bad_env, bad_cli in [(None, 'auto-swap,turbo'), ('fastload', None)]:
        r = Run(main_exe, image, bios, 'bad', runs, ['--fds-hle', bad_cli] if bad_cli else [],
                {'NESRECOMP_FDS_HLE': bad_env} if bad_env else None, ok=(2,))
        check(r.rc == 2, 'a bad HLE word is refused')

    # ---- refusals: no anchor; one side
    noanchor = out / 'noanchor'
    noanchor.mkdir(exist_ok=True)
    shutil.copy(bios, noanchor / 'hle.rom')
    (noanchor / 'hle.toml').write_text(''.join(l for l in (fxdir / 'bios' / 'hle.toml').read_text().splitlines(True)
                                               if not l.startswith('hle_')), newline='\n')
    off = Run(main_exe, image, bios, 'ref_off', runs)
    r = Run(main_exe, image, noanchor / 'hle.rom', 'ref_noanchor', runs, ['--fds-hle', 'auto-swap,fast-load'])
    check('auto-swap REFUSED' in r.stdout and 'anchor' in r.stdout and 'fast-load on' in r.stdout, r.stdout[:400])
    check_ram(r, OFF[1], 'no anchor: behaves as HLE off')
    check(not any(k == 'fds.idreq' for _, k, _ in r.events), 'no anchor: no request observation')
    check([l.split(' hw=')[0] for l in r.hash.splitlines()] == [l.split(' hw=')[0] for l in off.hash.splitlines()],
          'no anchor: the machine is the HLE-off machine')
    one = fxdir / 'disk1_1.fds'
    one.write_bytes(fx.fwnes([fx.side(1, 0, 0)]))
    r = Run(main_exe, one, bios, 'ref_oneside', runs, ['--fds-hle', 'all'])
    check('auto-swap REFUSED' in r.stdout and 'one side' in r.stdout and 'fast-load on' in r.stdout, r.stdout[:400])
    check_ram(r, OFF[1], 'one side: behaves as HLE off')
    check(any(k == 'fds.idreq' for _, k, _ in r.events), 'one side: requests still observed')

    # ---- a host disk change takes the drive back (scenario 2: eject before the tier acts, insert side B)
    r = Run(main_exe, fxdir / 'disk2_2.fds', bios, 'host', runs,
            ['--fds-hle', 'auto-swap', '--fds-event', '70:eject', '--fds-event', '76:insert=1'])
    check_ram(r, dict(t=0x11, fail=0, ej=1, done=0xC3), 'host swap')
    check(all('(hle)' not in t for _, t in r.sides()), f'host swap: the tier changed the disk: {r.sides()}')
    check(r.decisions() == ['keep 1'], f'host swap: decisions {r.decisions()}')

    # a host bump before the tier acts: it leaves the drive to the host (no wait, no bump after it)
    r = Run(main_exe, fxdir / 'disk2_4.fds', bios, 'host_poller', runs,
            ['--fds-hle', 'auto-swap', '--fds-event', '70:eject', '--fds-event', '75:insert=0'])
    check_ram(r, dict(t=0x10, empty=5, done=0xC3), 'host bump of the poller')
    check(r.decisions() == [], f'host bump of the poller: decisions {r.decisions()}')

    # an identity file naming a byte that is not a JSR: no anchor
    wrong = out / 'wronganchor'
    wrong.mkdir(exist_ok=True)
    shutil.copy(bios, wrong / 'hle.rom')
    text = (fxdir / 'bios' / 'hle.toml').read_text()
    anchor = int(re.search(r'hle_id_check = "0x([0-9A-F]+)"', text).group(1), 16)
    (wrong / 'hle.toml').write_text(text.replace(f'0x{anchor:04X}', f'0x{anchor + 3:04X}'), newline='\n')
    r = Run(main_exe, image, wrong / 'hle.rom', 'ref_wronganchor', runs, ['--fds-hle', 'auto-swap'])
    check('auto-swap REFUSED' in r.stdout and 'anchor' in r.stdout, f'wrong anchor: {r.stdout[:300]}')
    check_ram(r, OFF[1], 'wrong anchor: behaves as HLE off')

    # ---- fast load: which frames the host runs unpaced (scenario 7: most frames
    # are loads). Deterministic counts only; wall-clock timing is measured by
    # tools/cyc/perf_fds_fast_load.py outside CTest.
    counts = {}
    for tag, hle in [('paced', 'auto-swap'), ('fast', 'auto-swap,fast-load')]:
        r = Run(main_exe, fxdir / 'disk4_7.fds', bios, f'rt_{tag}', runs, ['--fds-hle', hle], frames=240)
        counts[tag + '_hash'] = r.hash
        m = re.search(r'(\d+) load frames \((\d+) unpaced\)', r.stdout)
        check(m is not None, f'{tag}: no load summary')
        counts[tag] = (int(m.group(1)), int(m.group(2)))
    check(counts['paced_hash'] == counts['fast_hash'], 'fast load changed the machine')
    check(counts['paced'][0] == counts['fast'][0] and counts['paced'][0] >= 60, f'load frames {counts}')
    check(counts['paced'][1] == 0, f'frames unpaced without fast load: {counts}')
    check(counts['fast'][1] == counts['fast'][0], f'fast load left load frames paced: {counts}')
    print(f'fast load: {counts["fast"][0]} load frames, all run unpaced with fast load, none without')
    print(f'test_cyc_fds_hle: {checks} checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
