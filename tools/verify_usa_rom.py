"""Verify the North American cartridge profile before native code generation."""
import argparse
from pathlib import Path
import zlib

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--rom',type=Path,required=True)
ap.add_argument('--crc',required=True)
ap.add_argument('--mapper',type=int,required=True)
ap.add_argument('--title',required=True)
args=ap.parse_args()
raw=args.rom.read_bytes()
if len(raw)<16 or raw[:4]!=b'NES\x1a':
    raise SystemExit('Expected an original iNES cartridge image')
start=16+(512 if raw[6]&4 else 0)
payload=raw[start:]
expected_length=raw[4]*16384+raw[5]*8192
mapper=(raw[6]>>4)|(raw[7]&0xf0)
if raw[7]&0x0c==8:mapper|=(raw[8]&15)<<8
crc=zlib.crc32(payload)
if len(payload)!=expected_length or crc!=int(args.crc,16) or mapper!=args.mapper:
    raise SystemExit(f'{args.title}: this build requires the audited North American/shared NTSC payload '
        f'CRC {args.crc}, mapper {args.mapper}; received CRC {crc:08X}, mapper {mapper}. '
        'Use the USA ROM. Other regional profiles require their own timing/parity qualification.')
print(f'{args.title}: North American/shared NTSC profile verified ({crc:08X})')
