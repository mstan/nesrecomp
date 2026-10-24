#!/usr/bin/env python3
"""Package a content-free Mesen integration recipe; never package imported assets."""
import argparse
from pathlib import Path
import tomllib
import zipfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    text=args.manifest.read_bytes()
    manifest=tomllib.loads(text.decode('utf-8-sig'))
    packs=manifest.get('hd_pack',[])
    if not packs or any(p.get('source')!='mesen' or p.get('directory') or p.get('patch') for p in packs):
        parser.error('Expected content-free Mesen source recipes, without packaged asset/patch paths')
    if args.out.exists():parser.error('Output already exists')
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(args.out,'x',compression=zipfile.ZIP_STORED) as archive:
        archive.writestr('manifest.toml',text)
    print(f'Created {args.out}: integration manifest only; user content excluded')


if __name__=='__main__':main()
