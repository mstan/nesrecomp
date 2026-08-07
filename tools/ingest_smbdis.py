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

Addresses versus constants
--------------------------
``Name = $XX`` is the same syntax whether $XX is a memory location or a plain
constant, and this file has both.  ``PlayerFacingDir = $33`` is zero-page RAM;
``BulletBill_CannonVar = $33`` is the *enemy id* $33, declared beside
``Bowser = $2d``.  They share a number and nothing else, so they are emitted
as different types (``ram`` and ``const``) and are not aliases.
See :func:`classify` for how they are told apart.

Genuine aliases
---------------
Within one kind, several names on one address really are the same thing:
``Player_X_Position`` is ``SprObject_X_Position`` slot 0.  Every name is
emitted, because they are aliases of one constant and any of them should
resolve.  The first for an address is the one the disassembly references most,
which is what single-name consumers (inline comments) show.

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


def parse_equates(asm_path: Path):
    """Returns (entries, lines, decl_line) for equates below RAM_LIMIT."""
    out: list[tuple[int, str]] = []
    decl_line: dict[str, int] = {}
    text = asm_path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    for i, line in enumerate(lines, 1):
        m = EQUATE_RE.match(line)
        if not m:
            continue
        name, hexval = m.group(1), m.group(2)
        addr = int(hexval, 16)
        if addr >= RAM_LIMIT:
            continue
        out.append((addr, name))
        decl_line.setdefault(name, i)
    return out, lines, decl_line


DATA_DIRECTIVE_RE = re.compile(r"^\s*\.(byte|word|dbyt|db|dw)\b", re.IGNORECASE)


def classify(lines: list[str], decl_line: dict[str, int],
             names: set[str]) -> tuple[dict[str, str], dict[str, int]]:
    """Split equates into ADDRESS (memory) and VALUE (plain constant).

    ``Name = $33`` is syntactically identical whether $33 is a zero-page
    address or an object-type id, and SMB's file has both: PlayerFacingDir is
    RAM at $0033, while BulletBill_CannonVar is the *enemy id* $33, declared
    beside Bowser = $2d and VineObject = $2f. Emitting the second as a RAM
    address is worse than omitting it — it is a confident mislabel.

    Usage settles it:
      * a bare name in an instruction operand (``sta Foo``, ``lda Foo,x``)
        can only be an address;
      * ``#Foo`` is an immediate, so a value;
      * a bare name inside ``.byte``/``.word`` is a table entry, so a value.

    An address may also appear in a pointer table, so a single bare operand
    use outranks any number of data-table appearances. Names never referenced
    at all inherit from their neighbours, because the file groups equates into
    a RAM block and a constants block.
    """
    imm: dict[str, int] = {n: 0 for n in names}
    addr: dict[str, int] = {n: 0 for n in names}
    data: dict[str, int] = {n: 0 for n in names}
    refs: dict[str, int] = {n: 0 for n in names}

    for i, raw in enumerate(lines, 1):
        line = raw.split(";", 1)[0]
        is_data = bool(DATA_DIRECTIVE_RE.match(line))
        for m in re.finditer(r"(#?)(?<![A-Za-z0-9_.])([A-Za-z_][A-Za-z0-9_]*)",
                             line):
            n = m.group(2)
            if n not in names:
                continue
            if i == decl_line.get(n):
                continue  # the defining line itself
            refs[n] += 1
            if m.group(1) == "#":
                imm[n] += 1
            elif is_data:
                data[n] += 1
            else:
                addr[n] += 1

    kind: dict[str, str] = {}
    for n in names:
        if addr[n]:
            kind[n] = "ADDRESS"
        elif imm[n] or data[n]:
            kind[n] = "VALUE"
        else:
            kind[n] = "UNKNOWN"

    # Unreferenced names inherit the classification of the nearest classified
    # equate by declaration line — the file keeps RAM and constants in
    # separate blocks, so a neighbour is a reliable witness.
    ordered = sorted(names, key=lambda n: decl_line[n])
    for idx, n in enumerate(ordered):
        if kind[n] != "UNKNOWN":
            continue
        for step in range(1, len(ordered)):
            for j in (idx - step, idx + step):
                if 0 <= j < len(ordered) and kind[ordered[j]] != "UNKNOWN":
                    kind[n] = kind[ordered[j]]
                    break
            if kind[n] != "UNKNOWN":
                break
        if kind[n] == "UNKNOWN":
            kind[n] = "ADDRESS"
    return kind, refs


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

    equates, asm_lines, decl_line = parse_equates(asm)
    if not equates:
        print("error: no equates parsed — the disassembly's format may have "
              "changed", file=sys.stderr)
        return 1

    sym_text = sym_path.read_text(encoding="utf-8")
    claimed = existing_addresses(sym_text)

    all_names = {n for _, n in equates}
    kind, refs = classify(asm_lines, decl_line, all_names)

    # ADDRESS equates name a memory location; VALUE equates are plain
    # constants (object/enemy ids) that merely happen to be small numbers.
    # They go in separate blocks with separate types, because calling an
    # enemy id a RAM address is a confident mislabel, not a harmless one.
    addr_by: dict[int, list[str]] = {}
    const_by: dict[int, list[str]] = {}
    for value, name in equates:
        target = addr_by if kind[name] == "ADDRESS" else const_by
        target.setdefault(value, []).append(name)

    def order(names: list[str]) -> list[str]:
        uniq = sorted(set(names))
        uniq.sort(key=lambda n: (-refs.get(n, 0), n))
        return uniq

    rows: list[tuple[int, list[str]]] = []
    aliases = 0
    collisions = 0
    for value in sorted(addr_by):
        if value in claimed:
            collisions += 1
            continue
        names = order(addr_by[value])
        rows.append((value, names))
        aliases += len(names) - 1

    const_rows = [(v, order(const_by[v])) for v in sorted(const_by)]

    block = [
        BEGIN,
        "# Names and addresses lifted from threecreepio/smb-disassembly",
        "# (ca65 port of doppelganger's disassembly). Regenerate with:",
        "#   python nesrecomp/tools/ingest_smbdis.py \\",
        "#       --disasm smb-disassembly --symbols symbols.sym",
        "#",
        "# `ram` entries name a memory location. `const` entries are plain",
        "# constants -- object and enemy ids -- that merely happen to be small",
        "# numbers. The disassembly declares both as `Name = $XX`, so they are",
        "# told apart by use: a bare instruction operand is an address, while",
        "# `#Name` or a .byte table entry is a value. $0033 is BOTH:",
        "# PlayerFacingDir is zero-page RAM, BulletBill_CannonVar is enemy id",
        "# $33. They are unrelated and are NOT aliases of each other.",
        "#",
        "# Within one kind, several names on one address ARE aliases of the",
        "# same byte -- Player_X_Position is SprObject_X_Position slot 0. All",
        "# are listed; the first is the one the disassembly references most,",
        "# and is what single-name consumers (inline comments) use.",
        "# Do not hand-edit inside this block; edits are overwritten.",
        "",
    ]
    for value, names in rows:
        for name in names:
            block.append(f"{value:04X} {name} ram")
    if const_rows:
        block += ["", "# Object / enemy type ids -- VALUES, not addresses.", ""]
        for value, names in const_rows:
            for name in names:
                block.append(f"{value:04X} {name} const")
    block += ["", END, ""]

    new_text = strip_block(sym_text) + "\n" + "\n".join(block)

    print(f"equates parsed        : {len(equates)}")
    print(f"RAM addresses         : {len(rows)}  "
          f"({sum(len(n) for _, n in rows)} names, {aliases} aliases)")
    print(f"value constants       : {len(const_rows)}  "
          f"({sum(len(n) for _, n in const_rows)} names)")
    print(f"skipped (already set) : {collisions}")

    if args.dry_run:
        print("[dry-run] not written")
        return 0

    sym_path.write_text(new_text, encoding="utf-8")
    print(f"wrote {sym_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
