#!/usr/bin/env python3
"""Wrap a local Mesen HD pack in a portable, default-off .nesmod archive.

No ROM is included. A pack's optional <patch> IPS is verified against --rom;
the shared Mods runtime applies it only to an in-memory cartridge copy.
Use --author and --license to preserve the pack creator's attribution/terms.
"""
import argparse
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile
import tomllib
import zipfile
import zlib

MAX_FILES = 4096
MAX_BYTES = 256 * 1024 * 1024
MAX_DEPTH = 8
MAX_ARCHIVE_DEPTH = 3


class InputBudget:
    def __init__(self):
        self.files = self.bytes = 0

    def add(self, files, size):
        self.files += files
        self.bytes += size
        if self.files > MAX_FILES or self.bytes > MAX_BYTES:
            raise ValueError('Pack input exceeds size/file limits (256 MiB, 4096 entries)')


def unpack(archive, destination, budget):
    entries = archive.infolist()
    budget.add(len(entries), sum(p.file_size for p in entries))
    seen = set()
    # Validate the entire archive before creating any files.
    for entry in entries:
        path = relative(entry.filename)
        key = path.as_posix().casefold()
        if (not path.parts or key in seen or stat.S_ISLNK(entry.external_attr >> 16)
                or any(part.endswith(('.', ' ')) for part in path.parts)):
            raise ValueError(f'Unsafe or duplicate pack path: {entry.filename}')
        seen.add(key)
    for entry in entries:
        target = destination / relative(entry.filename)
        if entry.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as source, target.open('xb') as output:
                shutil.copyfileobj(source, output)


def pack_files(folder):
    """Bound discovery in the selected input folder; never follow directory links."""
    files = []
    count = size = 0
    for current, directories, names in os.walk(folder, followlinks=False):
        if len(Path(current).relative_to(folder).parts) > MAX_DEPTH:
            raise ValueError('Pack input exceeds directory depth limit')
        for name in [*directories, *names]:
            path = Path(current) / name
            info = path.lstat()
            if (stat.S_ISLNK(info.st_mode) or
                    getattr(info, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0)):
                raise ValueError('Pack assets must not be links or junctions')
            count += 1
            if path.is_file():
                files.append(path)
                size += path.stat().st_size
            elif not path.is_dir():
                raise ValueError('Pack assets must be ordinary files and directories')
            if count > MAX_FILES or size > MAX_BYTES:
                raise ValueError('Pack input exceeds size/file limits (256 MiB, 4096 entries)')
    return files


def is_notice(path):
    return (path.name.lower() != 'hires.txt' and
            (path.suffix.lower() in {'.txt', '.md', '.license'} or
             path.name.lower() in {'license', 'copying', 'notice'}))


def discover(folder, scratch, budget, depth=0):
    files = pack_files(folder)
    if (folder / 'hires.txt').is_file():
        return [(folder, [])]
    manifests = [p for p in files if p.name == 'hires.txt']
    if manifests:
        return [(p.parent, [n for n in files if is_notice(n) and not n.is_relative_to(p.parent)])
                for p in manifests]
    candidates = []
    notices = [p for p in files if is_notice(p)]
    for path in files:
        if path.suffix.lower() != '.zip':
            continue
        if depth >= MAX_ARCHIVE_DEPTH:
            raise ValueError('Pack input exceeds nested ZIP depth limit')
        target = Path(tempfile.mkdtemp(prefix='archive-', dir=scratch))
        with zipfile.ZipFile(path) as archive:
            unpack(archive, target, budget)
        candidates.extend((pack, [*notices, *extra])
                          for pack, extra in discover(target, scratch, budget, depth + 1))
    return candidates


@contextmanager
def pack_folder(path):
    """Find one Mesen pack in a directory, ZIP, or wrapper containing nested ZIPs."""
    path = path.resolve(strict=True)
    if path.parent == path:
        raise ValueError('Select one pack folder, not a filesystem root')
    budget = InputBudget()
    with tempfile.TemporaryDirectory(prefix='nes-hd-import-') as temporary:
        scratch = Path(temporary)
        if path.is_dir():
            files = pack_files(path)
            budget.add(len(files), sum(p.stat().st_size for p in files))
            candidates = discover(path, scratch, budget)
        else:
            target = scratch / 'input'
            target.mkdir()
            with zipfile.ZipFile(path) as archive:
                unpack(archive, target, budget)
            candidates = discover(target, scratch, budget, depth=1)
        if not candidates:
            raise ValueError('No Mesen HD pack found: select a folder or ZIP containing hires.txt')
        if len(candidates) != 1:
            raise ValueError('Multiple HD packs found; select the intended inner ZIP or hires.txt folder')
        yield candidates[0]

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
    devices = {'con', 'prn', 'aux', 'nul',
               *(f'com{i}' for i in range(1, 10)), *(f'lpt{i}' for i in range(1, 10))}
    if (not name or path.is_absolute() or '..' in path.parts
            or any(any(c in '<>:"|?*' or ord(c) < 32 for c in part)
                   or part.split('.')[0].casefold() in devices for part in path.parts)):
        raise ValueError(f'Unsafe pack path: {name}')
    return Path(*path.parts)

def quoted(value: str) -> str:
    # JSON strings are compatible with these TOML basic-string fields.
    import json
    return json.dumps(value, ensure_ascii=False)

def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack', required=True, type=Path,
                        help='Mesen HD pack folder, ZIP, or wrapper ZIP containing one pack')
    parser.add_argument('--rom', required=True, type=Path)
    parser.add_argument('--game-id', required=True)
    parser.add_argument('--id', required=True, help='Stable package id, e.g. creator.hd-pack')
    parser.add_argument('--version', default='1.0.0')
    parser.add_argument('--name', required=True)
    parser.add_argument('--author', required=True)
    parser.add_argument('--license', required=True)
    parser.add_argument('--notice', action='append', type=Path, default=[],
                        help='Additional original license/credits file to preserve in the archive')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    try:
        with pack_folder(args.pack) as (folder, notices):
            package(args, folder, notices)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.error(str(error))


def package(args, folder, source_notices):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', args.id):
        raise ValueError('Invalid package id')
    image = args.rom.read_bytes()
    if len(image) < 16 or image[:4] != b'NES\x1a':
        raise ValueError('Expected an iNES ROM')
    text = (folder / 'hires.txt').read_text(encoding='utf-8-sig')
    patches = re.findall(r'^\s*<patch>([^\r\n]+)', text, re.MULTILINE)
    if len(patches) > 1:
        raise ValueError('Only one IPS patch per feature is supported')
    patch_line = ''
    if patches:
        fields = [v.strip() for v in patches[0].split(',')]
        if len(fields) != 2 or fields[1].lower() != hashlib.sha1(image).hexdigest():
            raise ValueError('The pack patch requires a different stock ROM SHA-1')
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
    files = sorted(pack_files(folder))
    if any(p.suffix.lower() in {'.nes', '.fds'} for p in files):
        raise ValueError('ROM images cannot be included in an HD package')
    if args.out.resolve().is_relative_to(folder):
        raise ValueError('Write the archive outside the input pack directory')
    notices = {p.name: p for p in args.notice}
    if len(notices) != len(args.notice) or any(not p.is_file() for p in notices.values()):
        raise ValueError('Notice files must exist and have distinct names')
    notices.update({f'source/{index}-{p.name}': p for index, p in enumerate(source_notices)})
    if any(p.suffix.lower() in {'.nes', '.fds'} for p in notices.values()):
        raise ValueError('ROM images cannot be included as notices')
    if len(files) + len(notices) + 1 > 4096 or sum(p.stat().st_size for p in [*files, *notices.values()]) > 256 * 1024 * 1024:
        raise ValueError('Pack exceeds .nesmod size/file limits')
    if args.out.exists():
        raise ValueError('Output already exists; choose a new archive path')
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
