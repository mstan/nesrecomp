#!/usr/bin/env python3
"""The FDS boot skip and auto insert on the whole machine (CTest cyc_fds_boot_test;
no owner data).

tools/cyc/fds_boot_fixtures.py writes a synthetic BIOS that boots the way
disksys.rom does (its identity file declares the boot model and the proof
records) and disks for it. The program is compiled from main.fds (NESRecomp
--game, BIOS identity from the synthetic .toml), once plainly and once with
game.toml [fds] hle = "boot-skip", and run native, --interp-only and on the
standalone cyc_interp:

  equivalence  the machine at the game's first instruction (--boot-state-out),
               skipped against the BIOS's own boot, at all four alignments:
               every memory byte but the stack page, the CPU registers, every
               hashed hardware field but the documented phase and rendering
               latch classes (runner/cyc/README.md "Boot skip"); the game then
               runs the same (its marker)
  decisions    the ring's fds.boot file events: every LoadFiles rule
  parity       native = --interp-only = cyc_interp --hash-out with the skip on
  refusals     a boot file into registers, one reaching $0000-$01FF through
               the mirrors, an empty one, a disk with no proof record, side B in
               the drive, no disk: refused with the reason, the run is the
               HLE-off run
  auto insert  no disk at power-on: side A goes in at the end of the first
               frame the boot polls $4032 with the drive empty, and the run is
               the run with a host insert at that frame (a player's action);
               off, the drive stays empty; a host insert first stands it down
  config       game.toml [fds] hle = "boot-skip" is the default,
               NESRECOMP_FDS_HLE overrides it, --fds-hle both; a bad word exits 2

python tools/cyc/test_cyc_fds_boot.py --recompiler build/compiler/NESRecomp.exe \\
    --interp build/cyc/cyc_interp.exe --out build/cyc-fds-boot [--toolchain build/cyc/nested_build.json]
    [--no-native]
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
import fds_boot_fixtures as fx      # noqa: E402
import nested_build                 # noqa: E402

NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
checks = 0
# Fields that count time or hold the PPU's rendering pipeline (README "Boot skip").
PHASE = re.compile(r'(boot\..*|cycles|hw\.cycles|apu\.(counter|dmc_bits|dmc_timer|put)|fds\.mod|ppu\.io_decay.*|'
                   r'ppu\.(dot|odd_frame|bg_hi|bg_lo|attr_hi|attr_lo|attr_latch|attribute|fetch_data|hi_plane|lo_plane|'
                   r'par_chr|spr_hi|spr_lo|spr_attr|spr_x|oam2|oam_buffer|oam_latch|oam_buffer_in|sprite_row))$')
FILES = ['loaded'] * 8 + ['skipped-address', 'skipped-range', 'not-boot', 'loaded']


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
    (folder / 'game.toml').write_text(f'[game]\noutput_prefix = "boot"\ncycle_accurate = true\nfds = true\n\n'
                                      f'[fds]\nimage = "../fx/main.fds"\nbios = "../fx/bios/boot.rom"\n{extra}',
                                      newline='\n')
    run([args.recompiler.resolve(), '--game', 'game.toml'], folder, folder / 'codegen.log')
    source = (HERE.parent.parent / 'runner' / 'cyc').resolve()
    (folder / 'CMakeLists.txt').write_text('\n'.join([
        'cmake_minimum_required(VERSION 3.20)', f'project(cyc_fds_boot_{name} C)', 'set(CMAKE_C_STANDARD 11)',
        f'include("{source.as_posix()}/cyc.cmake")',
        'file(GLOB GEN CONFIGURE_DEPENDS "generated/*_cyc*.c")',
        f'add_executable({name} ${{NESRECOMP_CYC_SOURCES}} ${{GEN}})',
        f'target_include_directories({name} PRIVATE ${{NESRECOMP_CYC_INCLUDE_DIRS}})',
        f'target_link_libraries({name} PRIVATE ${{NESRECOMP_CYC_LIBRARIES}})',
        f'target_compile_definitions({name} PRIVATE _CRT_SECURE_NO_WARNINGS)']) + '\n')
    args.toolchain_.build(folder, folder / 'build', folder)
    return args.toolchain_.executable(folder / 'build', name)


def state(path):
    """--boot-state-out: scalars by key, memories by name -> bytes."""
    sc, mem = {}, {}
    for line in open(path):
        line = line.rstrip('\n')
        if not line or line.startswith('frame '):
            continue
        m = re.match(r'(ram|ciram|oam|palette|chr_ram|wram|exram) ([0-9A-F]+): (.*)', line)
        if m:
            buf = mem.setdefault(m.group(1), {})
            for i, b in enumerate(m.group(3).split()):
                buf[int(m.group(2), 16) + i] = int(b, 16)
        else:
            k, _, v = line.partition(' ')
            sc[k] = v
    return sc, mem


class Run:
    def __init__(self, exe, image, bios, tag, out, extra=(), env=None, frames=700, ok=(0,)):
        self.tag = tag
        self.boot, ring, hashes = out / f'{tag}.state', out / f'{tag}.ring', out / f'{tag}.hash'
        for f in (self.boot, ring, hashes):
            f.unlink(missing_ok=True)
        cmd = [exe, image, '--fds-bios', bios, '--frames', frames, '--boot-state-out', self.boot, '--ring-out', ring,
               '--hash-out', hashes, '--mem-frame', frames - 1, '--mem-out', out / f'{tag}.mem'] + list(extra)
        p = run(cmd, out, out / f'{tag}.log', env=env, ok=ok)
        self.rc, self.stdout = p.returncode, p.stdout
        if p.returncode:
            return
        self.hash = hashes.read_text()
        self.events = [l.split(None, 4) for l in ring.read_text().splitlines() if not l.startswith('#')]
        self.boots = [e[4] for e in self.events if len(e) == 5 and e[3] == 'fds.boot']
        self.sides = [e[4] for e in self.events if len(e) == 5 and e[3] == 'fds.side']
        self.entered = self.boot.exists()
        self.ram = []
        for line in open(out / f'{tag}.mem'):
            if line.startswith('ram '):
                self.ram += [int(x, 16) for x in line.split()[2:]]

    def no_hw(self):
        return [l.split(' hw=')[0] for l in self.hash.splitlines()]


def equivalent(lle, skip, what):
    """Everything but the stack page and the documented time classes."""
    a, b = state(lle.boot), state(skip.boot)
    for k in sorted(set(a[0]) | set(b[0])):
        if PHASE.fullmatch(k):
            continue
        check(a[0].get(k) == b[0].get(k), f'{what}: {k} {a[0].get(k)} (BIOS) != {b[0].get(k)} (skip)')
    for name in sorted(set(a[1]) | set(b[1])):
        x, y = a[1].get(name, {}), b[1].get(name, {})
        diff = [i for i in set(x) | set(y) if x.get(i) != y.get(i) and not (name == 'ram' and 0x100 <= i < 0x200)]
        check(not diff, f'{what}: {name} differs at {[hex(i) for i in sorted(diff)[:12]]} ({len(diff)} bytes)')
    check(a[0]['boot.skipped'] == '0' and b[0]['boot.skipped'] == '1', f'{what}: skipped flags')
    check(int(b[0]['boot.frame']) < int(a[0]['boot.frame']), f'{what}: the skip is not earlier')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--recompiler', type=Path, required=True)
    ap.add_argument('--interp', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    nested_build.add_arguments(ap)
    ap.add_argument('--no-native', action='store_true', help='cyc_interp only')
    ap.add_argument('--align', type=int, nargs='*', default=[0, 1, 2, 3])
    args = ap.parse_args()
    args.toolchain_ = nested_build.Toolchain.from_args(args)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    fxdir = out / 'fx'
    run([sys.executable, HERE / 'fds_boot_fixtures.py', '--out', fxdir], out, out / 'fixtures.log')
    bios = fxdir / 'bios' / 'boot.rom'
    main_img = fxdir / 'main.fds'
    interp = args.interp.resolve()
    runs = out / 'runs'
    runs.mkdir(exist_ok=True)
    native = config = None
    if not args.no_native:
        native = build(args, out, 'plain', None)
        config = build(args, out, 'config', 'boot-skip')

    # ---- equivalence at every alignment, and the decisions
    for align in args.align:
        lle = Run(interp, main_img, bios, f'lle_a{align}', runs, ['--align', align])
        skip = Run(interp, main_img, bios, f'skip_a{align}', runs, ['--align', align, '--fds-hle', 'boot-skip'])
        check(lle.entered and skip.entered, f'align {align}: a boot did not reach the game')
        check('boot-skip on (cli)' in skip.stdout, f'align {align}: banner {skip.stdout[:300]}')
        equivalent(lle, skip, f'align {align}')
        check(lle.ram[fx.MARK] == 0xC3 and skip.ram[fx.MARK] == 0xC3, f'align {align}: the game did not run')
        files = [re.search(r'(loaded|not-boot|skipped-address|skipped-range)', b).group(1)
                 for b in skip.boots if b.startswith('file ')]
        check(files == FILES, f'align {align}: file decisions {files}')
        check(any(b.startswith('entry') and 'skipped' in b for b in skip.boots), 'skip entry event')
        check(any(b.startswith('entry') and 'skipped' not in b for b in lle.boots), 'BIOS entry event')
        check(any(b.startswith('check oam-corrupt-row=') for b in skip.boots), 'the OAM corruption check event')
        print(f'align {align}: skip = BIOS boot at the game entry (skip frame {state(skip.boot)[0]["boot.frame"]}, '
              f'BIOS frame {state(lle.boot)[0]["boot.frame"]})', flush=True)

    # ---- parity with the skip on
    if native:
        n = Run(native, main_img, bios, 'par_native', runs, ['--fds-hle', 'boot-skip'], frames=120)
        i = Run(native, main_img, bios, 'par_interp', runs, ['--fds-hle', 'boot-skip', '--interp-only'], frames=120)
        c = Run(interp, main_img, bios, 'par_ci', runs, ['--fds-hle', 'boot-skip'], frames=120)
        check(n.hash == i.hash == c.hash, 'native, --interp-only and cyc_interp differ with the skip on')
        check(re.search(r'native_cycles=\d+ \(100\.0%\)', n.stdout) is not None, f'not all native: {n.stdout[:400]}')

    # ---- refusals: the reason, and the HLE-off run
    off = Run(interp, main_img, bios, 'off', runs, frames=120)
    cases = [('io', fxdir / 'io.fds', [], '', 'registers'), ('mirror', fxdir / 'mirror.fds', [], '', '$0000-$01FF'),
             ('empty', fxdir / 'empty.fds', [], '', 'empty'),
             ('unproven', fxdir / 'unproven.fds', [], '', 'no equivalence proof'),
             ('sideb', main_img, ['--fds-boot-disk', '1'], '', 'side A'),
             ('nodisk', main_img, ['--fds-boot-disk', 'none'], ',no-auto-insert', 'side A')]
    for tag, image, extra, words, why in cases:
        r = Run(interp, image, bios, f'ref_{tag}', runs, extra + ['--fds-hle', 'boot-skip' + words], frames=120)
        check('boot-skip REFUSED' in r.stdout and why in r.stdout, f'refusal {tag}: {r.stdout[:400]}')
        base = Run(interp, image, bios, f'ref_{tag}_off', runs, extra + (['--fds-hle', words[1:]] if words else []),
                   frames=120)
        check(r.no_hw() == base.no_hw(), f'refusal {tag}: the run is not the HLE-off run')
        check(not any(s.endswith('skipped') for s in r.boots), f'refusal {tag}: skipped anyway')
    check(off.hash == Run(interp, main_img, bios, 'off2', runs, ['--fds-hle', 'no-boot-skip'], frames=120).hash,
          'no-boot-skip is not the default')

    # ---- auto insert
    ai = Run(interp, main_img, bios, 'ai', runs, ['--fds-boot-disk', 'none'])
    m = re.search(r'side A auto-inserted at the end of frame (\d+)', ai.stdout)
    check(m is not None and ai.entered, f'auto insert: {ai.stdout[:400]}')
    frame = int(m.group(1))
    check(any(s.startswith('side 0 inserted (auto-insert)') for s in ai.sides), f'auto insert side event {ai.sides}')
    check(any(b.startswith('wait') for b in ai.boots) and any(b.startswith('auto-insert side=0') for b in ai.boots),
          f'auto insert ring {ai.boots}')
    host = Run(interp, main_img, bios, 'ai_host', runs,
               ['--fds-boot-disk', 'none', '--fds-hle', 'no-auto-insert', '--fds-event', f'{frame + 1}:insert=0'])
    check(ai.no_hw() == host.no_hw(), 'auto insert is not a host insert at the same frame')
    none = Run(interp, main_img, bios, 'ai_off', runs, ['--fds-boot-disk', 'none', '--fds-hle', 'no-auto-insert'],
               frames=300)
    check(not none.entered and all('inserted' not in s for s in none.sides[1:]), 'no-auto-insert inserted')
    early = Run(interp, main_img, bios, 'ai_early', runs, ['--fds-boot-disk', 'none', '--fds-event', '1:insert=0'])
    check(early.entered and any('host disk change' in b for b in early.boots), f'host first: {early.boots}')
    check(not any('(auto-insert)' in s for s in early.sides), 'auto insert after a host insert')
    disk_in = Run(interp, main_img, bios, 'ai_diskin', runs, frames=120)
    check(disk_in.hash == off.hash, 'auto insert changed a boot with a disk in')

    # ---- config precedence (compiled with [fds] hle = "boot-skip")
    if config:
        for tag, extra, env, on, src in [('cfg_default', [], None, True, 'game.toml'),
                                         ('cfg_env_off', [], {'NESRECOMP_FDS_HLE': 'no-boot-skip'}, False, 'env'),
                                         ('cfg_env_off_cli_on', ['--fds-hle', 'boot-skip'],
                                          {'NESRECOMP_FDS_HLE': 'off'}, True, 'cli'),
                                         ('cfg_cli_off', ['--fds-hle', 'off'], None, False, 'cli')]:
            r = Run(config, main_img, bios, tag, runs, extra, env, frames=120)
            check(f'boot-skip {"on" if on else "off"} ({src})' in r.stdout, f'{tag}: {r.stdout[:300]}')
            check(any(b.startswith('entry') and 'skipped' in b for b in r.boots) == on, f'{tag}: skipped = {not on}')
    for bad_env, bad_cli in [(None, 'boot-skip,turbo'), ('bootskip', None)]:
        r = Run(interp, main_img, bios, 'bad', runs, ['--fds-hle', bad_cli] if bad_cli else [],
                {'NESRECOMP_FDS_HLE': bad_env} if bad_env else None, ok=(2,), frames=10)
        check(r.rc == 2, 'a bad word is refused')
    print(f'test_cyc_fds_boot: {checks} checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
