#!/usr/bin/env python3
"""Write repository-owned project metadata for https://recomp.fyi/spec.

Adapted from psxrecomp's accepted new-project metadata writer. Only absent
fields are filled; project maintainers own the values and lifecycle status.
This describes a project, not how to install or launch it.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

SYSTEM = "NES"
TOOLCHAIN = "nesrecomp"
SCHEMA = "https://recomp.fyi/schema/v1.json"


def merge_missing(existing: dict, starter: dict) -> dict:
    out = dict(existing)
    for key, value in starter.items():
        if key not in out:
            out[key] = value
        elif key == "original" and isinstance(out[key], dict):
            out[key] = {**value, **out[key]}
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", help="the game repository's .recomp.json path")
    parser.add_argument("--game", required=True, help="game title as it shipped")
    parser.add_argument("--project", default="", help="the project's own name")
    parser.add_argument("--region", default="", help="known original release region")
    parser.add_argument("--serial", default="", help="known original product code")
    identity = parser.add_mutually_exclusive_group()
    identity.add_argument("--rom", type=Path, help="hash this owned input; never copy its data or path")
    identity.add_argument("--sha1", default="", help="SHA-1 already obtained by the engine's ROM probe")
    args = parser.parse_args()
    if not args.game.strip():
        parser.error("--game must not be empty")
    if args.sha1 and not re.fullmatch(r"[0-9a-fA-F]{40}", args.sha1):
        parser.error("--sha1 must contain exactly 40 hexadecimal digits")

    try:
        original = {key: value.strip() for key, value in
                    (("region", args.region), ("serial", args.serial)) if value.strip()}
        if args.rom:
            digest = hashlib.sha1()
            with args.rom.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            original["sha1"] = digest.hexdigest()
        elif args.sha1:
            original["sha1"] = args.sha1.lower()
        starter = {"$schema": SCHEMA, "game": args.game.strip(), "system": SYSTEM,
                   "type": "recomp", "toolchain": TOOLCHAIN, "status": "exploring"}
        if args.project.strip() and args.project.strip() != args.game.strip():
            starter["project"] = args.project.strip()
        if original:
            starter["original"] = original
        out = Path(args.out)
        if out.is_symlink():
            raise ValueError("metadata path is a symlink; leaving it alone")
        existing = None
        if out.exists():
            existing = json.loads(out.read_text(encoding="utf-8-sig"))
            if not isinstance(existing, dict):
                raise ValueError("existing metadata is not a JSON object; leaving it alone")
        doc = merge_missing(existing, starter) if existing is not None else starter
        if doc != existing:
            out.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"Project metadata: {out}")
        return 0
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
