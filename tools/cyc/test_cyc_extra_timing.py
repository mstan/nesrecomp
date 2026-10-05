#!/usr/bin/env python3
"""Synthetic MMC3: CPU headroom, NMI/audio rate, state continuation and rejection."""
import argparse
from pathlib import Path
import re
import struct
import subprocess
import wave


def fixture(mapper, dma=False):
    prg = bytearray([0xEA] * 32768)
    # One pulse, NMI on, rendering on (also exercise odd-frame dot skipping).
    boot = bytes.fromhex('78 D8 A2 FF 9A A9 40 8D 17 40 A9 01 8D 15 40 '
                         'A9 BF 8D 00 40 A9 10 8D 02 40 A9 08 8D 03 40 '
                         'A9 80 8D 00 20 A9 08 8D 01 20')
    if dma:
        # Looping DMC plus repeated OAM transfers cross every blank interval.
        boot += bytes.fromhex('A9 4F 8D 10 40 A9 FF 8D 12 40 A9 01 8D 13 40 '
                              'A9 11 8D 15 40')
    spin = 0xFE00 + len(boot)
    boot += bytes([0xE6, 1])
    if dma: boot += bytes.fromhex('A9 02 8D 14 40')
    boot += bytes([0x4C, spin & 255, spin >> 8])
    prg[0x7E00:0x7E00 + len(boot)] = boot
    prg[0x6000:0x6003] = bytes.fromhex('E6 00 40')
    prg[0x6010] = 0x40
    prg[-6:] = struct.pack('<HHH', 0xE000, 0xFE00, 0xE010)
    return b'NES\x1a' + bytes([2, 0, mapper << 4, 0]) + bytes(8) + prg


def chunks(path):
    data = path.read_bytes()
    at = 32
    result = {}
    while at < len(data):
        tag, size = struct.unpack_from('<4sI', data, at)
        result[tag] = (at + 8, data[at + 8:at + 8 + size])
        at += 8 + size
    return data, result


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--host', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    rom = out / 'budget-mmc3.nes'
    rom.write_bytes(fixture(4))
    si = None
    if hasattr(subprocess, 'STARTUPINFO'):
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0

    def run(name, options, expected=0, image=rom):
        command = [str(args.host.resolve()), str(image), '--frames', '80', '--no-save', *map(str, options)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60,
                                startupinfo=si, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        (out / f'{name}.log').write_text(result.stdout + result.stderr)
        assert result.returncode == expected, result.stdout + result.stderr
        return result

    for alignment in range(4):
        baseline_cycles = None
        baseline_audio = None
        baseline_nmi = None
        for lines in (0, 1, 64, 128, 262):
            name = f'align{alignment}-extra{lines}'
            state, pcm, hashes = (out / f'{name}.{ext}' for ext in ('cycstate', 'wav', 'hash'))
            result = run(name, ['--align', alignment, '--extra-scanlines', lines,
                                '--save-state', f'79:{state}', '--wav-out', pcm, '--hash-out', hashes])
            cycles = int(re.search(r'cycles=(\d+)', result.stdout)[1])
            data, records = chunks(state)
            timing = struct.unpack('<HHB3x', records[b'TIME'][1])
            assert timing == (lines, 0, 0), timing
            nmi = records[b'HW  '][1][23]
            with wave.open(str(pcm)) as audio:
                samples = audio.getnframes()
            if lines == 0:
                baseline_cycles, baseline_audio, baseline_nmi = cycles, samples, nmi
            else:
                assert abs((cycles - baseline_cycles) - 80 * lines * 341 / 3) < 8, (lines, cycles)
                assert abs(samples - baseline_audio) <= 2, (lines, samples, baseline_audio)
                assert nmi == baseline_nmi, (lines, nmi, baseline_nmi)
            if lines == 128:
                middle = out / f'{name}-middle.cycstate'
                run(name + '-save', ['--align', alignment, '--extra-scanlines', lines,
                                     '--save-state', f'39:{middle}', '--hash-out', out / f'{name}-save.hash'])
                resumed = out / f'{name}-resume.hash'
                run(name + '-resume', ['--load-state', middle, '--hash-out', resumed])
                assert resumed.read_text().splitlines() == hashes.read_text().splitlines()[40:]
                damaged = bytearray(data)
                struct.pack_into('<H', damaged, records[b'TIME'][0], 263)
                bad = out / f'{name}-bad.cycstate'
                bad.write_bytes(damaged)
                rejected = run(name + '-reject', ['--load-state', bad], expected=2)
                assert 'invalid CPU budget' in rejected.stderr
                # A legacy v1 state has no TIME section; load it as stock.
                old = bytearray(data)
                struct.pack_into('<I', old, 8, 1)
                off = records[b'TIME'][0] - 8
                del old[off:off + 8 + len(records[b'TIME'][1])]
                legacy = out / f'{name}-v1.cycstate'
                legacy.write_bytes(old)
                restored = out / f'{name}-v1-restored.cycstate'
                run(name + '-v1', ['--load-state', legacy, '--frames', '81', '--save-state', f'80:{restored}'])
                assert struct.unpack('<HHB3x', chunks(restored)[1][b'TIME'][1]) == (0, 0, 0)
        print(f'alignment {alignment}: budget, NMI/audio rate, state continuation and validation passed', flush=True)

    nrom = out / 'unsupported-nrom.nes'
    nrom.write_bytes(fixture(0))
    rejected = run('unsupported', ['--extra-scanlines', 64], expected=2, image=nrom)
    assert 'MMC3 cartridge' in rejected.stderr
    for value in ('-1', '263', 'word', '1x'):
        run('invalid-' + value, ['--extra-scanlines', value], expected=2)

    dma_rom = out / 'budget-dma.nes'
    dma_rom.write_bytes(fixture(4, dma=True))
    for alignment in range(4):
        trace = out / f'dma-align{alignment}.trace'
        run(f'dma-align{alignment}', ['--align', alignment, '--extra-scanlines', 256,
                                    '--frames', 4, '--trace-frame', 2, '--trace-out', trace], image=dma_rom)
        previous = None
        repeat = longest = transfers = 0
        for line in trace.read_text().splitlines():
            fields = line.split()
            kind = fields[1]
            if kind == 'INSN': continue
            if kind in ('r', 'w', 'H'):
                repeat = repeat + 1 if kind == previous else 1
                longest = max(longest, repeat)
                if kind == 'w' and fields[2] == '2004': transfers += 1
            else: repeat = 0
            previous = kind
        assert longest <= 8, (alignment, longest, 'DMA phase froze in added lines')
        assert transfers > 2000, (alignment, transfers)
        print(f'alignment {alignment}: concurrent DMC/OAM DMA stayed bounded during audio pause', flush=True)


if __name__ == '__main__':
    main()
