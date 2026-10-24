#!/usr/bin/env python3
"""Build the standalone general-purpose Mesen HD importer download."""
import argparse
from pathlib import Path
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    files = {'tools/package_hdpack.py': root/'tools/package_hdpack.py',
             'README.txt': root/'tools/HD-IMPORTER-README.txt', 'LICENSE': root/'LICENSE'}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, 'x', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, path in files.items(): archive.write(path, name)
    print(f'Created {args.out}: generic Mesen HD importer, no ROM or pack assets')


if __name__ == '__main__': main()
