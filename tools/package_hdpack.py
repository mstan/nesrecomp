#!/usr/bin/env python3
"""Wrap a local Mesen HD pack in a portable, default-off .nesmod archive.

No ROM is included. A pack's optional <patch> IPS is verified against --rom;
the shared Mods runtime applies it only to an in-memory cartridge copy.
Use --author and --license to preserve the pack creator's attribution/terms.
"""
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import re
import tomllib
import zipfile
import zlib

def apply_ips(image: bytes, patch: bytes) -> bytes:
    if not patch.startswith(b'PATCH'):
        raise ValueError('IPS is missing PATCH magic')
    result = bytearray(image)
    start = 16 + (512 if image[6] & 4 else 0)
    p = 5
    while p + 3 <= len(patch):
        if patch[p:p+3] == b'EOF':
            tail = patch[p+3:]
            if tail and (len(tail) != 3 or int.from_bytes(tail, 'big') != len(image)):
                raise ValueError('IPS cannot resize the cartridge')
            return bytes(result)
        offset = int.from_bytes(patch[p:p+3], 'big'); p += 3
        if len(patch) - p < 2:
            break
        count = int.from_bytes(patch[p:p+2], 'big'); p += 2
        if count:
            data = patch[p:p+count]; p += count
        else:
            if len(patch) - p < 3:
                break
            count = int.from_bytes(patch[p:p+2], 'big'); p += 2
            data = patch[p:p+1] * count; p += 1
        if not count or len(data) != count or offset < start or offset + count > len(image):
            raise ValueError('IPS record is truncated or changes cartridge geometry')
        result[offset:offset+count] = data
    raise ValueError('IPS is truncated or missing EOF')

def relative(name: str) -> Path:
    path = PurePosixPath(name.replace('\\', '/'))
    if not name or path.is_absolute() or ':' in name or '..' in path.parts:
        raise ValueError(f'Unsafe pack path: {name}')
    return Path(*path.parts)

def quoted(value: str) -> str:
    # JSON strings are compatible with these TOML basic-string fields.
    import json
    return json.dumps(value, ensure_ascii=False)

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack', required=True, type=Path)
    parser.add_argument('--rom', required=True, type=Path)
    parser.add_argument('--game-id', required=True)
    parser.add_argument('--id', required=True, help='Stable package id, e.g. legend-of-zelda.remastered')
    parser.add_argument('--version', default='1.0.0')
    parser.add_argument('--name', required=True)
    parser.add_argument('--author', required=True)
    parser.add_argument('--license', required=True)
    parser.add_argument('--notice', action='append', type=Path, default=[],
                        help='Additional original license/credits file to preserve in the archive')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', args.id):
        parser.error('Invalid package id')
    image = args.rom.read_bytes()
    if len(image) < 16 or image[:4] != b'NES\x1a':
        parser.error('Expected an iNES ROM')
    folder = args.pack.resolve(strict=True)
    text = (folder / 'hires.txt').read_text(encoding='utf-8-sig')
    patches = re.findall(r'^\s*<patch>([^\r\n]+)', text, re.MULTILINE)
    if len(patches) > 1:
        parser.error('Only one IPS patch per feature is supported')
    patch_line = ''
    if patches:
        fields = [v.strip() for v in patches[0].split(',')]
        if len(fields) != 2 or fields[1].lower() != hashlib.sha1(image).hexdigest():
            parser.error('The pack patch requires a different stock ROM SHA-1')
        patch_path = relative(fields[0])
        patched = apply_ips(image, (folder / patch_path).read_bytes())
        patch_line = f'patch = {quoted("pack/" + patch_path.as_posix())}\npatched_rom_crc32 = "{zlib.crc32(patched[16:]):08x}"\n'
    manifest = f'''format_version = 1
id = {quoted(args.id)}
version = {quoted(args.version)}
name = {quoted(args.name)}
author = {quoted(args.author)}
license = {quoted(args.license)}
resolver = "declarative"
[[target]]
game_id = {quoted(args.game_id)}
rom_crc32 = "{zlib.crc32(image[16:]):08x}"
[[feature]]
id = "hd-pack"
name = {quoted(args.name)}
description = "HD textures and backgrounds. Select in the launcher before starting the game."
group = "Display"
default_enabled = false
exclusive_group = "display-mode"
[[hd_pack]]
feature = "hd-pack"
directory = "pack"
{patch_line}'''
    tomllib.loads(manifest)
    files = sorted(folder.rglob('*'))
    if any(p.is_symlink() or (not p.is_file() and not p.is_dir()) for p in files):
        parser.error('Pack assets must be ordinary files and directories')
    files = [p for p in files if p.is_file()]
    if any(p.suffix.lower() in {'.nes', '.fds'} for p in files):
        parser.error('ROM images cannot be included in an HD package')
    if args.out.resolve().is_relative_to(folder):
        parser.error('Write the archive outside the input pack directory')
    notices = {p.name: p for p in args.notice}
    if len(notices) != len(args.notice) or any(not p.is_file() for p in notices.values()):
        parser.error('Notice files must exist and have distinct names')
    if len(files) + len(notices) + 1 > 4096 or sum(p.stat().st_size for p in [*files, *notices.values()]) > 256 * 1024 * 1024:
        parser.error('Pack exceeds .nesmod size/file limits')
    if args.out.exists():
        parser.error('Output already exists; choose a new archive path')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    # ZIP_STORED matches the runtime installer without external zip libraries.
    with zipfile.ZipFile(args.out, 'x', compression=zipfile.ZIP_STORED) as archive:
        archive.writestr('manifest.toml', manifest)
        for path in files:
            archive.write(path, 'pack/' + path.relative_to(folder).as_posix())
        for name, path in notices.items():
            archive.write(path, 'notices/' + name)
    print(f'Created {args.out}: {len(files)} assets, optional IPS={bool(patches)}, stock ROM excluded')

if __name__ == '__main__':
    main()
