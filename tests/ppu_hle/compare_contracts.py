"""Compare separate LLE/HLE shared-machine hosts on existing ROM-free contracts.

No timing is gathered. Private snapshots, synthetic ROMs and traces stay in
--out. Executables are hidden on Windows and every child has a timeout.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "cyc"))
from mapper_ppu_fixtures import ppu_contract
from mmc3_variant_fixtures import mmc3_irq_program


def run(executable, arguments, log, expected=0):
    result = subprocess.run([str(executable), *map(str, arguments)], capture_output=True,
                            text=True, timeout=60,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    log.write_text(result.stdout + result.stderr)
    if result.returncode != expected:
        raise RuntimeError(f"unexpected exit {result.returncode}: see {log}")
    return result.stdout + result.stderr


def render_register_interrupt():
    """Interrupt a stable rendering span with real CPU register transactions.

    This specifically checks the new event-service cache invalidation and
    alignment-dependent delayed writes, rather than adding game image sweeps.
    """
    name, image, seeds, expected = mmc3_irq_program(4, "render_register_interrupt")
    image = bytearray(image)
    handler = bytearray([0xa9, 0, 0x8d, 0, 0xe0])  # acknowledge IRQ
    def store(addr, value):
        handler.extend([0xa9, value, 0x8d, addr & 255, addr >> 8])
    store(0x2001, 0x18)  # delayed mask write while stable rendering is active
    store(0x2005, 5)
    store(0x2005, 0)
    handler.extend([0x2c, 2, 0x20])  # status read resets the address latch
    store(0x2001, 0)
    handler.extend([0xea] * 8)
    store(0x2001, 0x18)
    store(0x2006, 0x20)
    store(0x2006, 0)
    store(0x2007, 0x5a)  # force the VRAM data-port latch chain
    handler.extend([0xe6, 0, 0x40])
    for bank in range(16):
        at = 16 + bank * 8192 + 0x100
        image[at:at + len(handler)] = handler
    return name, bytes(image), seeds, expected

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lle", required=True, type=Path)
    parser.add_argument("--hle", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    cases = [
        ppu_contract(0, 32, 0, [("write", 0, 0x81), ("read", 0, 0x81),
                               ("write", 0x2000, 0x55), ("read", 0x2400, 0x55)], "_ram"),
        mmc3_irq_program(4, "mmc3_irq"),
        mmc3_irq_program(118, "txsrom_irq"),
        mmc3_irq_program(119, "tqrom_irq", chr_kb=64),
        render_register_interrupt(),
    ]
    checked = []
    for name, image, _, _ in cases:
        rom = args.out / f"{name}.nes"
        rom.write_bytes(image)
        for alignment in range(4):
            traces = []
            for implementation in ("lle", "hle"):
                stem = args.out / f"{name}-{alignment}-{implementation}"
                trace = stem.with_suffix(".hash")
                output = run(getattr(args, implementation), [rom, "--frames", 6,
                             "--align", alignment, "--hash-out", trace,
                             "--state-frame", 5, "--state-out", stem.with_suffix(".state")],
                             stem.with_suffix(".log"))
                if f"implementation={implementation.upper()}" not in output:
                    raise RuntimeError("missing or wrong fixed implementation metadata")
                lines = trace.read_text().splitlines()
                if len(lines) != 6 or "A=42" not in lines[-1]:
                    raise RuntimeError(f"documented fixture assertions failed: {stem}")
                traces.append(trace.read_bytes())
            if traces[0] != traces[1]:
                raise RuntimeError(f"contract trace differs: {name}, alignment {alignment}")
            checked.append({"case": name, "alignment": alignment, "frames": 6})
    (args.out / "summary.json").write_text(json.dumps({"passed": True, "checks": checked}, indent=2))
    print(f"PPU contracts: PASS ({len(checked)} paired hardware routes)")


if __name__ == "__main__":
    main()
