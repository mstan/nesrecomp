#!/usr/bin/env python3
"""Import RAM/IO symbol names for Super Mario Bros. from the ca65 disassembly.

Why this exists
---------------
SuperMarioBrosRecomp's ``symbols.sym`` was generated from
``threecreepio/smb-disassembly`` (a ca65 port of doppelganger's disassembly)
via ``ca65 -g`` + ``ld65 --dbgfile``, then deduplicated by address.  That path
resolves *code* labels, and the resulting file carries 1990 ``func`` entries —
but it carries **no RAM symbols at all**.

That gap is a trap for anything that has to touch player state.  Without it,
game-side code ends up writing bare literals like ``g_ram[0x0086]``, which is a
guess dressed up as a fact and is exactly how a wrong address survives review.
``symbol_table.h`` has always documented a ``ram`` type for precisely this
(``071D ScreenRight_X_Pos ram``); the SMB file simply never got that half.

RAM symbols do not need an assembler.  The disassembly declares them as plain
constant equates::

    Player_X_Position = $0086

so they can be lifted directly and merged into the existing table.  Code labels
still come from the ca65 debug-file path, and this script leaves them alone.

Licensing
---------
Output contains **only names and addresses**.  No ROM bytes, no instruction
text, and none of the disassembly's own comments are copied.  The upstream
repository publishes no license, so it is used as a technical reference only —
the same treatment ``snesrecomp/tools/ingest_dkc2_disasm.py`` applies to the
DKC2 disassembly.

Idempotent: the generated block is delimited, so re-running replaces it rather
than appending.  Hand-added entries outside the block are preserved.

Overloaded addresses
--------------------
SMB reuses zero page across object types, so one address often carries several
names: ``$0033`` is ``PlayerFacingDir`` to the player code and
``BulletBill_CannonVar`` to the enemy code.  Both are correct; which applies
depends on the subsystem running.

**Every** name is emitted, because they are all aliases of a single constant
and any of them should resolve.  The first line for an address is the one the
disassembly references most, and single-name consumers (inline comments) use
that one.

Usage
-----
    python tools/ingest_smbdis.py \\
        --disasm ../SuperMarioBrosRecomp/smb-disassembly \\
        --symbols ../SuperMarioBrosRecomp/symbols.sym
    python tools/ingest_smbdis.py ... --dry-run
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# `Name = $XXXX`, at column 0. The disassembly uses this form only for
# constant equates; code labels are `Name:` and are resolved by ca65 instead.
EQUATE_RE = re.compile(
    r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\$([0-9A-Fa-f]{1,4})\s*(?:;.*)?$"
)

BEGIN = "# --- BEGIN auto-ingested RAM symbols (tools/ingest_smbdis.py) ---"
END = "# --- END auto-ingested RAM symbols ---"

# The 6502 address space below $8000 on NROM: zero page, stack, RAM, PPU/APU
# registers and the mirrors. Anything at or above $8000 is PRG ROM and is a
# code label's business, not this script's.
RAM_LIMIT = 0x8000


def parse_equates(asm_path: Path) -> tuple[list[tuple[int, str]], str]:
    out: list[tuple[int, str]] = []
    text = asm_path.read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        m = EQUATE_RE.match(line)
        if not m:
            continue
        name, hexval = m.group(1), m.group(2)
        addr = int(hexval, 16)
        if addr >= RAM_LIMIT:
            continue
        out.append((addr, name))
    return out, text


def count_references(text: str, names: set[str]) -> dict[str, int]:
    """How often each equate name is actually used in the disassembly.

    All names for an overloaded address are emitted, but the FIRST one is
    what single-name consumers (inline comments) show, so its choice matters:
    alphabetical order would label $0033 BulletBill_CannonVar and quietly make
    every player-facing comment wrong. Reference count picks the name the
    disassembly actually leans on.
    """
    counts = {n: 0 for n in names}
    for m in re.finditer(r"(?<![A-Za-z0-9_])([A-Za-z_][A-Za-z0-9_]*)", text):
        n = m.group(1)
        if n in counts:
            counts[n] += 1
    # Discount the defining `Name = $XXXX` line itself.
    for n in counts:
        counts[n] = max(0, counts[n] - 1)
    return counts


def existing_addresses(sym_text: str) -> set[int]:
    """Addresses already claimed outside the generated block."""
    claimed: set[int] = set()
    inside = False
    for line in sym_text.splitlines():
        if line.strip() == BEGIN:
            inside = True
            continue
        if line.strip() == END:
            inside = False
            continue
        if inside:
            continue
        s = line.strip()
        if not s or s.startswith("#") or s.startswith(";"):
            continue
        parts = s.split()
        try:
            claimed.add(int(parts[0], 16))
        except (ValueError, IndexError):
            continue
    return claimed


def strip_block(sym_text: str) -> str:
    lines = sym_text.splitlines()
    out, inside = [], False
    for line in lines:
        if line.strip() == BEGIN:
            inside = True
            continue
        if line.strip() == END:
            inside = False
            continue
        if not inside:
            out.append(line)
    while out and not out[-1].strip():
        out.pop()
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disasm", required=True,
                    help="path to a threecreepio/smb-disassembly checkout")
    ap.add_argument("--symbols", required=True,
                    help="path to the game's symbols.sym")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    disasm = Path(args.disasm)
    asm = disasm / "src" / "smb.asm"
    if not asm.is_file():
        print(f"error: {asm} not found — is --disasm a smb-disassembly "
              f"checkout?", file=sys.stderr)
        return 2

    sym_path = Path(args.symbols)
    if not sym_path.is_file():
        print(f"error: {sym_path} not found", file=sys.stderr)
        return 2

    equates, asm_text = parse_equates(asm)
    if not equates:
        print("error: no equates parsed — the disassembly's format may have "
              "changed", file=sys.stderr)
        return 1

    sym_text = sym_path.read_text(encoding="utf-8")
    claimed = existing_addresses(sym_text)

    # Group names per address, then order each group by how heavily the
    # disassembly uses each name. All of them are emitted; the ordering only
    # decides which one represents the address where just one name fits.
    by_addr: dict[int, list[str]] = {}
    for addr, name in equates:
        by_addr.setdefault(addr, []).append(name)

    refs = count_references(asm_text, {n for _, n in equates})

    rows: list[tuple[int, list[str]]] = []
    aliases = 0
    collisions = 0
    for addr in sorted(by_addr):
        if addr in claimed:
            collisions += 1
            continue
        names = sorted(set(by_addr[addr]))
        names.sort(key=lambda n: (-refs.get(n, 0), n))
        rows.append((addr, names))
        aliases += len(names) - 1

    block = [
        BEGIN,
        "# Names and addresses lifted from threecreepio/smb-disassembly",
        "# (ca65 port of doppelganger's disassembly). Regenerate with:",
        "#   python nesrecomp/tools/ingest_smbdis.py \\",
        "#       --disasm smb-disassembly --symbols symbols.sym",
        "#",
        "# SMB overloads zero page across object types: $0033 is",
        "# PlayerFacingDir to the player code and BulletBill_CannonVar to the",
        "# enemy code. Both names are equally correct; which one applies",
        "# depends on the subsystem running, not on the address.",
        "#",
        "# EVERY name is listed, because they are all aliases of one constant",
        "# and any of them should resolve. The first line for an address is",
        "# the one the disassembly references most, and is what single-name",
        "# consumers (inline comments) use; the rest follow immediately.",
        "# Do not hand-edit inside this block; edits are overwritten.",
        "",
    ]
    for addr, names in rows:
        for name in names:
            block.append(f"{addr:04X} {name} ram")
    block += ["", END, ""]

    new_text = strip_block(sym_text) + "\n" + "\n".join(block)

    print(f"equates parsed        : {len(equates)}")
    print(f"unique RAM addresses  : {len(rows)}")
    print(f"additional alias names: {aliases}")
    print(f"symbol lines written  : {sum(len(n) for _, n in rows)}")
    print(f"skipped (already set) : {collisions}")

    if args.dry_run:
        print("[dry-run] not written")
        return 0

    sym_path.write_text(new_text, encoding="utf-8")
    print(f"wrote {sym_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
