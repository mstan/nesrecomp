"""Build the self-contained Windows NESRecomp CLI release archive."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys


ARCHIVE_NAME = "nesrecomp-cli-windows-x86_64.zip"


def run(command: list[str], cwd: Path) -> None:
    print("+", subprocess.list2cmdline(command))
    subprocess.run(command, cwd=cwd, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", help="CMake executable to use")
    parser.add_argument("--python", default=sys.executable, help="Python with PyInstaller")
    parser.add_argument("--output", help="archive output directory")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    cmake = args.cmake or shutil.which("cmake")
    if not cmake:
        raise SystemExit("CMake was not found.")

    build_root = root / "build" / "cli-release"
    core_build = build_root / "core"
    pyinstaller_work = build_root / "pyinstaller"
    dist = build_root / "dist"
    output = Path(args.output).resolve() if args.output else build_root

    if build_root.exists():
        shutil.rmtree(build_root)
    build_root.mkdir(parents=True)
    output.mkdir(parents=True, exist_ok=True)

    run(
        [cmake, "-S", str(root / "recompiler"), "-B", str(core_build), "-A", "x64"],
        root,
    )
    run([cmake, "--build", str(core_build), "--config", "Release"], root)

    core_candidates = (
        core_build / "Release" / "NESRecomp.exe",
        core_build / "NESRecomp.exe",
    )
    core = next((path for path in core_candidates if path.is_file()), None)
    if core is None:
        raise SystemExit("The native NESRecomp executable was not produced.")

    # Keep the internal native tool distinct from the public nesrecomp.exe.
    # Windows filenames are case-insensitive, so NESRecomp.exe would collide
    # with the PyInstaller launcher during collection.
    packaged_core = build_root / "NESRecomp-core.exe"
    shutil.copy2(core, packaged_core)

    separator = ";" if sys.platform == "win32" else ":"
    run(
        [
            args.python,
            "-m",
            "PyInstaller",
            "--noconfirm",
            "--clean",
            "--onedir",
            "--console",
            "--name",
            "nesrecomp",
            "--distpath",
            str(dist),
            "--workpath",
            str(pyinstaller_work),
            "--specpath",
            str(build_root),
            "--add-binary",
            f"{packaged_core}{separator}.",
            "--add-data",
            f"{root / 'runner' / 'include'}{separator}framework/include",
            str(root / "tools" / "cli.py"),
        ],
        root,
    )

    package = dist / "nesrecomp"
    if not (package / "nesrecomp.exe").is_file():
        raise SystemExit("PyInstaller did not produce nesrecomp.exe.")

    archive_base = output / ARCHIVE_NAME.removesuffix(".zip")
    archive = Path(shutil.make_archive(str(archive_base), "zip", package))
    print(f"Created {archive} ({archive.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
