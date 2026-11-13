#!/usr/bin/env python3
"""Authored sprite coverage, OAM priority and mirrored addition layers.

The expected colors are independent of the renderer's recorded metadata.
All cartridges, textures and persistent state are disposable synthetic data.
"""
import argparse
from pathlib import Path
import subprocess
from test_cyc_hdpack import PATTERNS, fixture, png_read, png_write


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    red, green, blue = bytes((220, 30, 40)), bytes((20, 210, 70)), bytes((31, 63, 95))
    scenarios = ('transparent', 'overlap', 'behind', 'mirrored-addition', 'background-addition')
    for name, chr_ram in ((n, r) for r in (False, True) for n in scenarios):
        folder = out / (('ram-' if chr_ram else 'rom-') + name)
        folder.mkdir(exist_ok=True)
        image = bytearray(fixture(chr_ram, False))
        # The first two OAM entries share a position, with distinct palettes.
        # All other entries are offscreen. The original test uploads 20 bytes.
        objects = [31, 1, 0, 16] + [255] * 16
        if name in ('overlap', 'behind'):
            objects[4:8] = [31, 1, 1, 16]
        if name == 'behind':
            objects[2] = 0x20
        if name == 'mirrored-addition':
            objects[2] = 0xc0
        image[16 + 0x240:16 + 0x254] = bytes(objects)
        image[16 + 0x234:16 + 0x238] = bytes((0x0f, 0x21, 0x22, 0x23))
        rom = folder / 'fixture.nes'
        rom.write_bytes(image)
        pack = folder / 'pack'
        pack.mkdir(exist_ok=True)
        png_write(pack / 'under.png', 512, 480, lambda x, y: (*blue, 255))
        # Three authored cells: red (or half red), green, transparent.
        half = name in ('overlap', 'behind')
        png_write(pack / 'art.png', 48, 16,
                  lambda x, y: (*red, 0 if half and x % 2 else 255) if x < 16
                  else (*green, 255) if x < 32 else (0, 0, 0, 0))
        manifest = ('<ver>108\n<scale>2\n<img>art.png\n'
                    '<background>under.png,1,0,0,0\n')
        for bank in ((0,) if chr_ram else (0, 512)):
            bg_key = PATTERNS[0].hex() if chr_ram else f'{bank:X}'
            sp_key = PATTERNS[1].hex() if chr_ram else f'{bank+1:X}'
            manifest += f'<tile>0,{bg_key},0F010203,32,0,1,N\n'
            manifest += f'<tile>0,{sp_key},FF111213,0,0,1,N\n'
            manifest += f'<tile>0,{sp_key},FF212223,16,0,1,N\n'
            manifest += f'<fallback>{bank+1:X},1\n' if bank else ''
            manifest += f'<fallback>{bank:X},0\n' if bank else ''
        bg_key = PATTERNS[0].hex() if chr_ram else '0'
        sp_key = PATTERNS[1].hex() if chr_ram else '1'
        extra_key = '0'*32 if chr_ram else '2'
        manifest += f'<tile>0,{extra_key},0F010203,16,0,1,N\n'
        if name == 'mirrored-addition':
            # Ignore source palette, preserve the extra BG palette, mirror both
            # displacement axes, and ignore the addition tag's prefix.
            manifest += f'[undefined-prefix]<addition>{sp_key},FFFFFFFF,8,8,{extra_key},0F010203,Y\n'
        if name == 'background-addition':
            manifest += f'<addition>{bg_key},0F010203,8,0,{extra_key},0F010203\n'
        (pack / 'hires.txt').write_text(manifest)
        state = folder / 'frame.cycstate'

        def run(tag, extra=()):
            result = subprocess.run(
                [str(args.host.resolve()), str(rom), '--frames', '24', '--no-save',
                 '--hdpack', str(pack), '--present-out', str(folder / (tag+'.png')),
                 '--hash-out', str(folder / (tag+'.hash')), *map(str, extra)],
                capture_output=True, text=True, timeout=90,
                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            (folder / (tag+'.log')).write_text(result.stdout+result.stderr)
            if result.returncode:
                raise AssertionError(result.stdout+result.stderr)
            return png_read(folder / (tag+'.png'))[2]

        rows = run('full', ['--save-state', f'18:{state}'])
        resumed = run('resumed', ['--load-state', state])
        assert rows == resumed, (name, 'current-build HD continuation')
        assert (folder/'full.hash').read_text().splitlines()[19:] == (folder/'resumed.hash').read_text().splitlines()

        def pixel(x, y, dx=0, dy=0):
            return rows[y*2+dy][(x*2+dx)*3:(x*2+dx+1)*3]

        if name == 'transparent':
            # Native color is zero at ox4,row2. Authored art still paints it.
            assert pixel(20, 34) == red and pixel(20, 34, 1) == red, name
        elif name == 'overlap':
            assert pixel(20, 34) == red and pixel(20, 34, 1) == green, name
        elif name == 'behind':
            assert pixel(16, 34) == red and pixel(16, 34, 1) == blue, name
            assert pixel(20, 34) == green and pixel(20, 34, 1) == green, name
        elif name == 'mirrored-addition':
            assert pixel(8, 24) == green and pixel(15, 31, 1, 1) == green, name
        else:
            assert pixel(100, 100) == green and pixel(101, 101, 1, 1) == green, name
        print(folder.name, 'PASS', flush=True)


if __name__ == '__main__':
    main()
