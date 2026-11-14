#!/usr/bin/env python3
"""The console output stage end to end (CTest cyc_console_audio_pcm; no owner data).

cyc_console_audio_test checks the stage itself (defaults, frequency and step
response). This checks what a host renders, on synthetic programs:

  defaults  cyc_interp picks nes for an NROM program and famicom for a VRC6
            (mapper 24) program and an FDS program; the default WAV is
            byte-identical to the explicit --console one.
  pinned    the nes WAVs are byte-identical to the build before the console
            axis existed (1a555ef: SHA-256 below) and the famicom WAVs to the
            ones this axis was validated with, so any change to either output
            stage is a visible decision.
  spectra   the same program rendered under both models: at every frequency
            the ratio of the two outputs' spectra is the ratio of the models'
            responses (tools/cyc/console_output.py), within 0.25 dB, over the
            tone program's noise, pulse, triangle and DMC content (20 Hz-16 kHz).
  config    game.toml [game] console, compiled in: a famicom NROM program and
            a nes VRC6 program render their configured model by default and
            --console still overrides it; an unknown value fails to compile.

python tools/cyc/test_cyc_console_audio.py --recompiler build/cyc/NESRecomp.exe \\
    --interp build/cyc/cyc_interp.exe --out build/cyc-console-audio [--toolchain build/cyc/nested_build.json]
"""
import argparse
import cmath
import hashlib
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import wave

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import console_output                      # noqa: E402
import make_apu_tone_rom                   # noqa: E402
import nested_build                        # noqa: E402
from expansion_fixtures import expansion_fixtures  # noqa: E402

NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
RATE = 48000

# name: (frames, SHA-256 of the nes WAV, SHA-256 of the famicom WAV)
PINNED = {
    'tones': (720, '9fbbd58aee1d2d2b52dce60bdea4463b5c1a1bd1c2a2bf58c3dcf319f8b6e783',
              '7cfb163aa436c14e16af6993a44dc795b30c341adf9cb44e2c917516a8d7f119'),
    'vrc6': (90, '72d8b448c0ee4b32616199e3823569d2598577daaf07acf8f4e51af6d73dc80a',
             '7ac27490de175b52b5a48668d70bd81870ece1649551b5816619012fc3bc3623'),
    'fds': (1100, '8072e5cf10efa34af52568c096da832729cbb90e245a268742dd552f350002d3',
            'f6eac2a979d09a4309d805391dc79e40e3714046650ee0975e5bc1de9b05bea5'),
}
DEFAULT = {'tones': 'nes', 'vrc6': 'famicom', 'fds': 'famicom'}


def run(cmd, cwd, log, ok=(0,)):
    p = subprocess.run([str(x) for x in cmd], cwd=cwd, capture_output=True, text=True, timeout=900,
                       creationflags=NO_WINDOW)
    Path(log).write_text(' '.join(str(x) for x in cmd) + '\n' + p.stdout + p.stderr)
    if p.returncode not in ok:
        raise RuntimeError(f'exit {p.returncode}: {cmd}; see {log}')
    return p


def load(path):
    with wave.open(str(path)) as f:
        assert f.getnchannels() == 1 and f.getsampwidth() == 2 and f.getframerate() == RATE
        data = f.readframes(f.getnframes())
    return struct.unpack('<' + 'h' * (len(data) // 2), data)


def render(exe, args, wav, folder, console=None):
    extra = ['--console', console] if console else []
    p = run([exe] + args + ['--wav-out', wav] + extra, folder, wav.with_suffix('.log'))
    want = console or 'default'
    line = next((l for l in p.stdout.splitlines() if l.startswith('audio:')), '')
    return wav.read_bytes(), line, want


def dtft(x, f):
    """sum x[n] e^{-j w n} by recurrence (a Goertzel-style rotation)."""
    w = cmath.exp(-2j * math.pi * f / RATE)
    acc, z = 0j, 1 + 0j
    for i, v in enumerate(x):
        if v:
            acc += v * z
        z *= w
        if i & 4095 == 4095:
            z /= abs(z)
    return acc


def check_spectra(nes, fam):
    """Both renders of one program: |Y_nes / Y_fam| against |H_nes / H_fam|."""
    rows, worst = [], 0.0
    for f in (20, 30, 50, 80, 120, 200, 330, 500, 800, 1300, 2000, 3300, 5000, 8000, 11000, 16000):
            yn, yf = dtft(nes, f), dtft(fam, f)
            got = 20 * math.log10(abs(yn) / abs(yf))
            want = 20 * math.log10(console_output.response('nes', RATE, f) / console_output.response('famicom', RATE, f))
            worst = max(worst, abs(got - want))
            rows.append((f, got, want, 20 * math.log10(abs(yf) / len(fam) + 1e-30)))
    for f, got, want, level in rows:
        print(f'  {f:6d} Hz  nes/famicom {got:+7.2f} dB (models {want:+7.2f} dB)  famicom level {level:6.1f} dB')
    if worst > 0.25:
        raise AssertionError(f'spectra: nes/famicom ratio off the models by {worst:.2f} dB')
    return worst


def build_programs(args, out, tc, cases):
    """cases: (name, image, toml text). One CMake project, one executable each."""
    source = HERE.parents[1] / 'runner' / 'cyc'
    cmake = ['cmake_minimum_required(VERSION 3.20)', 'project(cyc_console_audio C)', 'set(CMAKE_C_STANDARD 11)',
             f'include("{source.as_posix()}/cyc.cmake")',
             'add_library(console_runtime OBJECT ${NESRECOMP_CYC_SOURCES})',
             'target_include_directories(console_runtime PRIVATE ${NESRECOMP_CYC_INCLUDE_DIRS})',
             'target_compile_definitions(console_runtime PRIVATE _CRT_SECURE_NO_WARNINGS CYC_HOST_EXTRAS_STRONG=1)']
    for name, image, toml in cases:
        case = out / name
        case.mkdir(parents=True, exist_ok=True)
        (case / f'{name}.nes').write_bytes(image)
        (case / 'game.toml').write_text(toml, newline='\n')
        shutil.rmtree(case / 'generated', ignore_errors=True)
        run([args.recompiler.resolve(), f'{name}.nes', '--game', 'game.toml'], case, case / 'codegen.log')
        cmake += [f'file(GLOB {name}_GEN CONFIGURE_DEPENDS "{name}/generated/*_cyc*.c")',
                  f'add_executable({name} $<TARGET_OBJECTS:console_runtime> ${{{name}_GEN}})',
                  f'target_include_directories({name} PRIVATE ${{NESRECOMP_CYC_INCLUDE_DIRS}})',
                  f'target_link_libraries({name} PRIVATE ${{NESRECOMP_CYC_LIBRARIES}})',
                  f'target_compile_definitions({name} PRIVATE _CRT_SECURE_NO_WARNINGS)']
    (out / 'CMakeLists.txt').write_text('\n'.join(cmake) + '\n', newline='\n')
    tc.build(out, out / 'build', out, timeout=1200, parallel=8)
    return {name: tc.executable(out / 'build', name) for name, _, _ in cases}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--recompiler', required=True, type=Path)
    ap.add_argument('--interp', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    nested_build.add_arguments(ap)
    args = ap.parse_args()
    tc = nested_build.Toolchain.from_args(args)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    interp = args.interp.resolve()

    tones = make_apu_tone_rom.build()
    vrc6 = next(c for c in expansion_fixtures() if c[0] == 'exp_tone_24_pulse')[1]
    (out / 'tones.nes').write_bytes(tones)
    (out / 'vrc6.nes').write_bytes(vrc6)
    run([sys.executable, HERE / 'fds_audio_fixtures.py', '--out', out / 'fds'], out, out / 'fds_fixtures.log')
    programs = {'tones': [out / 'tones.nes'], 'vrc6': [out / 'vrc6.nes'],
                'fds': [out / 'fds' / 'pcm.fds', '--fds-bios', out / 'fds' / 'bios' / 'pcm.rom']}

    rendered = {}
    for name, base in programs.items():
        frames, nes_sha, fam_sha = PINNED[name]
        cmd = base + ['--frames', frames]
        wavs = {}
        for console in (None, 'nes', 'famicom'):
            data, line, _ = render(interp, cmd, out / f'{name}_{console or "default"}.wav', out, console)
            wavs[console or 'default'] = data
            model = console or DEFAULT[name]
            if f', {model} output stage' not in line:
                raise AssertionError(f'{name} --console {console}: host reported "{line}", expected {model}')
        if wavs['default'] != wavs[DEFAULT[name]]:
            raise AssertionError(f'{name}: the default render is not the {DEFAULT[name]} one')
        if wavs['nes'] == wavs['famicom']:
            raise AssertionError(f'{name}: nes and famicom renders are identical')
        for model, sha in (('nes', nes_sha), ('famicom', fam_sha)):
            got = hashlib.sha256(wavs[model]).hexdigest()
            if got != sha:
                raise AssertionError(f'{name} {model}: WAV SHA-256 {got}, pinned {sha}')
        rendered[name] = wavs
        print(f'{name}: default {DEFAULT[name]}; nes and famicom WAVs match their pinned SHA-256')

    print('spectra (tone program, nes vs famicom):')
    nes = load(out / 'tones_nes.wav')
    fam = load(out / 'tones_famicom.wav')
    worst = check_spectra(nes, fam)
    print(f'spectra: within {worst:.3f} dB of the models')

    # game.toml [game] console, compiled in.
    cases = [('tones_famicom', tones, '[game]\noutput_prefix="tones_famicom"\ncycle_accurate=true\nconsole="famicom"\n'),
             ('vrc6_nes', vrc6, '[game]\noutput_prefix="vrc6_nes"\ncycle_accurate=true\nconsole="nes"\n')]
    exes = build_programs(args, out / 'compiled', tc, cases)
    checks = [('tones_famicom', 'tones', None, 'famicom'), ('tones_famicom', 'tones', 'nes', 'nes'),
              ('vrc6_nes', 'vrc6', None, 'nes'), ('vrc6_nes', 'vrc6', 'famicom', 'famicom')]
    for exe_name, prog, console, model in checks:
        folder = out / 'compiled' / exe_name
        frames = PINNED[prog][0]
        data, line, _ = render(exes[exe_name], [folder / f'{exe_name}.nes', '--frames', frames],
                               folder / f'{console or "default"}.wav', folder, console)
        if data != rendered[prog][model]:
            raise AssertionError(f'{exe_name} --console {console}: not the {model} render')
        if f', {model} output stage' not in line:
            raise AssertionError(f'{exe_name}: host reported "{line}"')
    print('config: [game] console compiled in as the default; --console overrides it')
    bad = out / 'compiled' / 'bad'
    bad.mkdir(parents=True, exist_ok=True)
    (bad / 'bad.nes').write_bytes(tones)
    (bad / 'game.toml').write_text('[game]\noutput_prefix="bad"\ncycle_accurate=true\nconsole="pal"\n', newline='\n')
    p = run([args.recompiler.resolve(), 'bad.nes', '--game', 'game.toml'], bad, bad / 'codegen.log', ok=(1,))
    if 'console' not in p.stderr:
        raise AssertionError('an unknown [game] console compiled, or failed without saying why')
    print('config: an unknown console value fails to compile')
    print('PASS')


if __name__ == '__main__':
    main()
