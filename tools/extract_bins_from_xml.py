"""
extract_bins_from_xml.py — Extract binary data blobs from a NES ROM into
individual files based on a bins.xml manifest.

This is a common pattern in ca65-based NES disassemblies: the .asm source
files `.INCBIN` data blobs from separate .dat files, and a `bins.xml`
manifest maps each .dat to an (offset, length) within the original ROM.

Format of bins.xml:

    <Binaries>
      <Binary Offset='3677' Length='19' FileName='dat/SongScript0.dat'/>
      ...
    </Binaries>

Offsets are byte positions within the PRG-ROM body (i.e. after the 16-byte
iNES header).

Example usage:

    python extract_bins_from_xml.py \\
        --rom "Original.nes" \\
        --xml src/bins.xml \\
        --out-dir .
"""
from __future__ import annotations
import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def extract(rom_path: Path, xml_path: Path, out_dir: Path,
            ines_header_size: int = 16) -> int:
    rom = rom_path.read_bytes()
    body = rom[ines_header_size:]

    tree = ET.parse(xml_path)
    root = tree.getroot()

    count = 0
    for b in root.findall("Binary"):
        offset = int(b.get("Offset"))
        length = int(b.get("Length"))
        filename = b.get("FileName")
        if not filename:
            continue
        outpath = out_dir / filename
        outpath.parent.mkdir(parents=True, exist_ok=True)
        data = body[offset:offset + length]
        if len(data) != length:
            print(f"warning: short read for {filename} "
                  f"(got {len(data)}, expected {length})",
                  file=sys.stderr)
        outpath.write_bytes(data)
        count += 1
    return count


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description="Extract .dat blobs from a NES ROM per a bins.xml "
                    "manifest (common in ca65 disassemblies).")
    p.add_argument("--rom", type=Path, required=True,
                   help="Path to the source NES ROM")
    p.add_argument("--xml", type=Path, required=True,
                   help="Path to bins.xml manifest")
    p.add_argument("--out-dir", type=Path, default=Path("."),
                   help="Root dir for .dat output (paths in manifest are "
                        "relative to this). Default: current directory.")
    p.add_argument("--header-size", type=int, default=16,
                   help="iNES header size to skip (default 16)")
    args = p.parse_args(argv)

    for pth in (args.rom, args.xml):
        if not pth.exists():
            print(f"error: {pth} not found", file=sys.stderr)
            return 2

    n = extract(args.rom, args.xml, args.out_dir, args.header_size)
    print(f"extracted {n} files", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
