"""Human-friendly NESRecomp release CLI."""

from __future__ import annotations

import argparse
from collections import deque
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


VERSION = "0.3.0"
REQUIRED_HEADERS = (
    "coroutine.h",
    "nes_runtime.h",
    "recomp_stack.h",
)


def _resource_root() -> Path:
    frozen_root = getattr(sys, "_MEIPASS", None)
    if frozen_root:
        return Path(frozen_root)
    return Path(__file__).resolve().parents[1]


def _core_path() -> Path:
    override = os.environ.get("NESRECOMP_CORE")
    if override:
        return Path(override).expanduser().resolve()

    root = _resource_root()
    candidates = (
        root / "NESRecomp-core.exe",
        root / "build" / "cli-core" / "Release" / "NESRecomp.exe",
        root / "build" / "cli-core" / "NESRecomp.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError("The bundled NESRecomp core executable is missing.")


def _framework_include_path() -> Path:
    root = _resource_root()
    bundled = root / "framework" / "include"
    if bundled.is_dir():
        return bundled
    source_tree = root / "runner" / "include"
    if source_tree.is_dir():
        return source_tree
    raise FileNotFoundError("The NESRecomp framework headers are missing.")


def _project_name(rom: Path, requested: str | None) -> str:
    value = requested or rom.stem
    value = re.sub(r"[^A-Za-z0-9_-]+", "-", value).strip("-_")
    if not value:
        value = "nes-game"
    return value[:80]


def _write_project_files(output: Path, name: str, rom: Path, used_config: bool) -> None:
    cmake = f'''cmake_minimum_required(VERSION 3.20)
project({name}_recompiled C)

set(NESRECOMP_GENERATED_SOURCES
    "${{CMAKE_CURRENT_SOURCE_DIR}}/generated/{name}_full.c"
    "${{CMAKE_CURRENT_SOURCE_DIR}}/generated/{name}_dispatch.c"
)
file(GLOB NESRECOMP_BANK_SOURCES CONFIGURE_DEPENDS
    "${{CMAKE_CURRENT_SOURCE_DIR}}/generated/{name}_full_bank*.c"
)

add_library(nesrecomp_game STATIC
    ${{NESRECOMP_GENERATED_SOURCES}}
    ${{NESRECOMP_BANK_SOURCES}}
)
target_include_directories(nesrecomp_game PRIVATE
    "${{CMAKE_CURRENT_SOURCE_DIR}}/framework/include"
    "${{CMAKE_CURRENT_SOURCE_DIR}}/generated"
)
set_target_properties(nesrecomp_game PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
)
'''
    build_ps1 = '''$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
cmake -S $root -B "$root/build"
cmake --build "$root/build" --config Release
'''
    build_sh = '''#!/usr/bin/env sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build"
'''
    readme = f'''# {name} NESRecomp output

This folder contains C source generated from `{rom.name}`.

## Build the generated source

Windows PowerShell:

```powershell
.\\build.ps1
```

macOS or Linux:

```sh
./build.sh
```

The build creates the `nesrecomp_game` static library. This confirms that the
generated source compiles; it is not a complete playable port by itself.

To make a playable port, add game-specific configuration and integrate the
library with the NESRecomp runner. Existing game repositories are useful
starting points: https://github.com/mstan/nesrecomp
'''
    metadata = {
        "format": 1,
        "tool": "nesrecomp",
        "tool_version": VERSION,
        "project_name": name,
        "source_rom_name": rom.name,
        "used_game_config": used_config,
        "generated_at_unix": int(time.time()),
    }

    (output / "CMakeLists.txt").write_text(cmake, encoding="utf-8", newline="\n")
    (output / "build.ps1").write_text(build_ps1, encoding="utf-8", newline="\n")
    (output / "build.sh").write_text(build_sh, encoding="utf-8", newline="\n")
    (output / "README.md").write_text(readme, encoding="utf-8", newline="\n")
    (output / "nesrecomp-project.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8", newline="\n"
    )


def _copy_framework(output: Path) -> None:
    source = _framework_include_path()
    destination = output / "framework" / "include"
    destination.mkdir(parents=True, exist_ok=True)
    for header in REQUIRED_HEADERS:
        source_file = source / header
        if not source_file.is_file():
            raise FileNotFoundError(f"Required framework header is missing: {header}")
        shutil.copy2(source_file, destination / header)


def _run_core(command: list[str], output: Path, verbose: bool) -> int:
    process = subprocess.Popen(
        command,
        cwd=output,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    assert process.stdout is not None
    recent: deque[str] = deque(maxlen=80)
    for raw_line in process.stdout:
        line = raw_line.rstrip()
        recent.append(line)
        useful = (
            line.startswith("[NESRecomp]")
            or line.startswith("[FuncFinder] Total functions")
            or line.startswith("[RegProp] Bank switches")
        )
        if verbose or useful:
            print(line)

    return_code = process.wait()
    if return_code and not verbose:
        print("[nesrecomp] Native recompiler diagnostics:", file=sys.stderr)
        for line in recent:
            print(line, file=sys.stderr)
    return return_code


def _build(args: argparse.Namespace) -> int:
    rom = Path(args.rom).expanduser().resolve()
    output = Path(args.output).expanduser().resolve()
    game = Path(args.game).expanduser().resolve() if args.game else None

    if not rom.is_file():
        raise ValueError(f"ROM file not found: {rom}")
    if rom.suffix.lower() != ".nes":
        raise ValueError("NESRecomp expects an iNES ROM with a .nes extension.")
    if rom.read_bytes()[:4] != b"NES\x1a":
        raise ValueError("The input does not have a valid iNES header.")
    if game and not game.is_file():
        raise ValueError(f"Game config not found: {game}")
    if output.exists() and any(output.iterdir()) and not args.force:
        raise ValueError(
            f"Output folder is not empty: {output}\n"
            "Choose a new folder or add --force to update it."
        )

    name = _project_name(rom, args.name)
    output.mkdir(parents=True, exist_ok=True)
    command = [
        str(_core_path()),
        str(rom),
        "--output-prefix",
        name,
    ]
    if game:
        command.extend(("--game", str(game)))

    print(f"[nesrecomp] Recompiling {rom.name}", flush=True)
    print(f"[nesrecomp] Output: {output}", flush=True)
    return_code = _run_core(command, output, args.verbose)
    if return_code:
        return return_code

    expected = (
        output / "generated" / f"{name}_full.c",
        output / "generated" / f"{name}_dispatch.c",
    )
    missing = [str(path) for path in expected if not path.is_file()]
    if missing:
        raise RuntimeError("Recompiler completed without expected output: " + ", ".join(missing))

    _copy_framework(output)
    _write_project_files(output, name, rom, game is not None)
    print("[nesrecomp] Done. Generated source and build scripts are ready.")
    print(f"[nesrecomp] Next: powershell -File \"{output / 'build.ps1'}\"")
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="nesrecomp",
        description="Recompile an NES ROM into a buildable C source project.",
    )
    parser.add_argument("--version", action="version", version=f"nesrecomp {VERSION}")
    subparsers = parser.add_subparsers(dest="command", required=True)

    build = subparsers.add_parser("build", help="generate C source from an NES ROM")
    build.add_argument("--rom", required=True, help="path to a legally obtained .nes ROM")
    build.add_argument("--output", required=True, help="new folder for generated source")
    build.add_argument("--game", help="optional path to an existing game.toml")
    build.add_argument("--name", help="optional output/project name")
    build.add_argument(
        "--force",
        action="store_true",
        help="allow updating a non-empty output folder",
    )
    build.add_argument(
        "--verbose",
        action="store_true",
        help="show all native recompiler diagnostics",
    )
    build.set_defaults(handler=_build)
    return parser


def main() -> int:
    parser = _parser()
    args = parser.parse_args()
    try:
        return int(args.handler(args))
    except (FileNotFoundError, RuntimeError, ValueError) as error:
        print(f"nesrecomp: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
