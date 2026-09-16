#!/usr/bin/env python3
"""Compare a frame NESRecomp drew against the same frame from another emulator.

The picture is compared as the PPU's own output - one 9-bit color index per
pixel (color | emphasis << 6) - so no palette sits between the two machines:

    <game> rom.nes --frames 651 --mem-frame 650 --mem-out mem650.txt
    tools/cyc/mesen_shot/mesen_shot rom.nes shots --input in.txt 650
    python tools/cyc/cmp_picture.py mem650.txt shots/650.idx

A game that needs input to reach the frame worth comparing takes the same
--input file in both (cyc_host.c and mesen_shot both read that format).

Host frame N and Mesen frame N are the same picture unless the program boots
differently in the two; SMB3 reaches every state a frame later in Mesen. Check a
frame from before the first button press against Mesen N and N+1, and if N+K
matches, run mesen_shot with --frame-offset K and compare host frame N with
Mesen frame N+K (see runner/cyc/README.md, "Against Mesen's picture").

Mesen powers CPU RAM up as zeros, so run NESRecomp with --ram-init zeros;
otherwise a program that reads uninitialized RAM legitimately differs.

Exit status is 0 when every pixel matches.
"""
import argparse
import struct
import sys

WIDTH, HEIGHT = 256, 240


def read_mem_dump(path):
    """The `frame <row>: <index> ...` lines cyc_mem_dump writes."""
    rows = {}
    with open(path, "r") as f:
        for line in f:
            if not line.startswith("frame "):
                continue
            head, _, rest = line.partition(":")
            row = int(head.split()[1])
            rows[row] = [int(v, 16) for v in rest.split()]
    if len(rows) != HEIGHT:
        sys.exit(f"{path}: expected {HEIGHT} frame rows, found {len(rows)}")
    out = []
    for y in range(HEIGHT):
        if len(rows[y]) != WIDTH:
            sys.exit(f"{path}: row {y} has {len(rows[y])} pixels")
        out.extend(rows[y])
    return out


def read_idx(path):
    """mesen_shot's raw 16-bit-per-pixel index buffer."""
    with open(path, "rb") as f:
        data = f.read()
    want = WIDTH * HEIGHT * 2
    if len(data) != want:
        sys.exit(f"{path}: expected {want} bytes, found {len(data)}")
    return list(struct.unpack(f"<{WIDTH * HEIGHT}H", data))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mem_dump", help="NESRecomp --mem-out file")
    ap.add_argument("idx", help="mesen_shot <frame>.idx file")
    ap.add_argument("--max-report", type=int, default=20,
                    help="differing pixels to list (default 20)")
    ap.add_argument("--emphasis", action="store_true",
                    help="compare the emphasis bits too (default: color only)")
    args = ap.parse_args()

    ours = read_mem_dump(args.mem_dump)
    theirs = read_idx(args.idx)
    mask = 0x1FF if args.emphasis else 0x3F

    diffs = [i for i in range(WIDTH * HEIGHT) if (ours[i] & mask) != (theirs[i] & mask)]
    if not diffs:
        print(f"identical: {WIDTH * HEIGHT} pixels match"
              f"{'' if args.emphasis else ' (color bits; --emphasis to include emphasis)'}")
        return 0

    print(f"{len(diffs)} of {WIDTH * HEIGHT} pixels differ "
          f"({100.0 * len(diffs) / (WIDTH * HEIGHT):.2f}%)")
    rows = sorted({i // WIDTH for i in diffs})
    cols = sorted({i % WIDTH for i in diffs})
    print(f"  rows {rows[0]}-{rows[-1]} ({len(rows)} of {HEIGHT}), "
          f"columns {cols[0]}-{cols[-1]} ({len(cols)} of {WIDTH})")
    for i in diffs[:args.max_report]:
        print(f"  x={i % WIDTH:3d} y={i // WIDTH:3d}  ours={ours[i] & mask:03X} "
              f"theirs={theirs[i] & mask:03X}")
    if len(diffs) > args.max_report:
        print(f"  ... {len(diffs) - args.max_report} more")
    return 1


if __name__ == "__main__":
    sys.exit(main())
