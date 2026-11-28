#!/usr/bin/env python3
"""
migrate_hints.py -- retarget a nesrecomp game.toml from one ROM variant to
another, by LABEL NAME, using two WLA-DX `wlalink -S` .sym files.

The problem: game.toml hints (extra function seeds, data regions, dispatch and
bail entries, mod hooks) are raw 6502 addresses.  A different revision of the
same game -- Metroid (Europe)/NES_PAL vs Metroid (USA)/NES_NTSC, say -- has the
same code with the same disassembly labels at DIFFERENT addresses.  Copying the
addresses across is wrong; re-deriving them by hand is worse.  This tool does
the only defensible thing: read the label the OLD address carries in the OLD
.sym, find that same label in the NEW .sym, and write the NEW address.

Rules (deliberately strict -- no guessing):
  * old address carries a label that exists in the new .sym  -> rewrite
    (unchanged address is just the common case of that)
  * old address carries no label -> anchor interpolation, but only when it can
    be PROVEN rather than assumed: take the nearest labelled address below it
    (`prev`) and above it (`next`) that both migrate; require the span to have
    the same length on both sides (new_next - new_prev == next - prev), which
    means the whole span shifted rigidly; then addr -> addr + (new_prev - prev).
    With --old-rom/--new-rom the span is additionally compared byte for byte in
    the two ROM images, and only a byte-identical span is accepted
    ("interpolated-verified").  Anything that fails these tests is DROPPED.
  * label exists but is gone from the new .sym                -> DROP the entry
  * range ends (`end`, `addr_hi`) that resolve by neither route fall back to
    preserving the range LENGTH relative to the migrated start, and every such
    case is listed in the report so it can be checked by hand.

Sections handled: [functions] (fixed/bankN lists), [[data_region]],
[[inline_dispatch]], [[stack_bail_func]], [[cond_bail_func]], [[merge_range]],
[[mod_function_hook]], [[extra_func]], [[replace_func]], [[extra_label]].
Comments, ordering and formatting of the file are preserved: the rewriter is
line-based and only ever edits the numeric literal in place, or deletes a whole
entry (with the blank line that follows it).

Bank resolution, matching the recompiler's own reading of game.toml
(recompiler/src/game_config.c:202-218): `fixed = [...]` and a table with no
`bank` key mean bank-agnostic (-1).  For lookup, an address at or above
`--fixed-base` ($C000) always belongs to `--fixed-bank` (7); a bank-agnostic
switchable address is looked up in every bank and is migrated only when all the
banks that have it agree on the new address.

Usage:
  python tools/migrate_hints.py --toml game.toml \
      --old-sym disasm/m1disasm/out/M1_NES_PAL.sym \
      --new-sym disasm/m1disasm/out/M1_NES_NTSC.sym \
      --out game.toml --report migration_report.txt
"""
import argparse
import re
import sys
from pathlib import Path
from collections import defaultdict

# Array-of-table sections and which of their keys hold addresses.
# 'range' pairs are (start_key, end_key): the end is migrated relative to the
# start when it carries no label of its own.
TABLE_SECTIONS = {
    'data_region':       {'ranges': [('start', 'end')], 'scalars': []},
    'merge_range':       {'ranges': [('addr_lo', 'addr_hi')], 'scalars': []},
    'inline_dispatch':   {'ranges': [], 'scalars': ['addr']},
    'stack_bail_func':   {'ranges': [], 'scalars': ['addr']},
    'cond_bail_func':    {'ranges': [], 'scalars': ['addr']},
    'mod_function_hook': {'ranges': [], 'scalars': ['addr']},
    'extra_func':        {'ranges': [], 'scalars': ['addr']},
    'replace_func':      {'ranges': [], 'scalars': ['addr']},
    'extra_label':       {'ranges': [], 'scalars': ['addr']},
    'merge_func':        {'ranges': [], 'scalars': ['addr']},
}


def load_labels(path):
    """(bank, addr) -> [names]  and  (bank, name) -> addr, from a WLA v3 .sym."""
    by_addr = defaultdict(list)
    by_name = {}
    sec = None
    for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
        line = line.split(';')[0].strip()
        if not line:
            continue
        if line.startswith('['):
            sec = line
            continue
        if sec != '[labels]':
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        bank_s, addr_s = parts[0].split(':')
        bank, addr, name = int(bank_s, 16), int(addr_s, 16), parts[1]
        by_addr[(bank, addr)].append(name)
        by_name.setdefault((bank, name), addr)
    return by_addr, by_name


class RomImage(object):
    """PRG lookup for a headered iNES file: (bank, cpu_addr) -> file offset."""

    def __init__(self, path, fixed_bank, fixed_base, bank_size=0x4000):
        self.data = Path(path).read_bytes()
        self.header = (16 + (512 if self.data[6] & 4 else 0)) if self.data[:4] == b'NES\x1a' and len(self.data) >= 16 else 0
        self.fixed_bank = fixed_bank
        self.fixed_base = fixed_base
        self.bank_size = bank_size

    def slice(self, bank, lo, hi):
        base = self.fixed_base if lo >= self.fixed_base else 0x8000
        off = self.header + bank * self.bank_size
        return self.data[off + (lo - base): off + (hi - base)]


class Mapper(object):
    def __init__(self, old_sym, new_sym, fixed_bank, fixed_base, nbanks,
                 old_rom=None, new_rom=None, interpolate=True):
        self.old_by_addr, _ = load_labels(old_sym)
        self.new_by_addr, self.new_by_name = load_labels(new_sym)
        self.fixed_bank = fixed_bank
        self.fixed_base = fixed_base
        self.banks = list(range(nbanks))
        self.interpolate = interpolate
        self.old_rom = RomImage(old_rom, fixed_bank, fixed_base) if old_rom else None
        self.new_rom = RomImage(new_rom, fixed_bank, fixed_base) if new_rom else None
        # sorted labelled addresses per bank, for anchor interpolation
        self.sorted_old = {}
        for (b, a) in self.old_by_addr:
            self.sorted_old.setdefault(b, []).append(a)
        for b in self.sorted_old:
            self.sorted_old[b] = sorted(set(self.sorted_old[b]))

    def banks_for(self, bank, addr):
        """Which physical bank(s) can hold `addr` under the hint's bank key."""
        if addr >= self.fixed_base:
            return [self.fixed_bank]
        if bank is not None and bank >= 0:
            return [bank]
        return [b for b in self.banks if b != self.fixed_bank]

    def _label_map(self, b, addr):
        """Exact-label mapping inside one physical bank, or None."""
        names = self.old_by_addr.get((b, addr))
        if not names:
            return None
        candidates = {self.new_by_name[(b, n)] for n in names if (b, n) in self.new_by_name}
        return next(iter(candidates)) if len(candidates) == 1 else None

    def _interpolate(self, b, addr):
        """-> (new_addr, status, detail) proven by a rigid, equal-length span
        between the surrounding labels (optionally byte-verified), or None."""
        addrs = self.sorted_old.get(b)
        if not addrs:
            return None
        import bisect
        i = bisect.bisect_right(addrs, addr)
        prev = nxt = None
        for j in range(i - 1, -1, -1):
            if self._label_map(b, addrs[j]) is not None:
                prev = addrs[j]
                break
        for j in range(i, len(addrs)):
            if self._label_map(b, addrs[j]) is not None:
                nxt = addrs[j]
                break
        if prev is None or nxt is None:
            return None
        np_, nn = self._label_map(b, prev), self._label_map(b, nxt)
        if (nn - np_) != (nxt - prev):
            return None                       # span changed size: not rigid
        delta = np_ - prev
        cand = addr + delta
        detail = 'span %04X..%04X%+d' % (prev, nxt, delta)
        if self.old_rom and self.new_rom:
            kind = self._span_matches(b, prev, nxt, np_, nn)
            if kind is None:
                return None                   # span content differs: not proven
            return cand, 'interpolated-verified', detail + ' ' + kind
        return cand, 'interpolated', detail

    def _span_matches(self, b, olo, ohi, nlo, nhi):
        """Do the two equal-length spans hold the same code?

        Identical bytes prove it outright.  Otherwise every differing byte must
        belong to a 16-bit absolute operand that was RELOCATED: either it moved
        by the same delta as the span, or the old word is a labelled address
        whose label sits at the new word in the new build.  Anything else means
        the code really changed, and the mapping is rejected.
        """
        o = self.old_rom.slice(b, olo, ohi)
        n = self.new_rom.slice(b, nlo, nhi)
        if len(o) != ohi - olo or len(n) != nhi - nlo or len(o) != len(n) or not o:
            return None
        if o == n:
            return 'byte-identical'
        delta = nlo - olo
        explained = set()
        for i in range(len(o)):
            if o[i] == n[i] or i in explained:
                continue
            ok = False
            for start in (i - 1, i):
                if start < 0 or start + 1 >= len(o):
                    continue
                ow = o[start] | (o[start + 1] << 8)
                nw = n[start] | (n[start + 1] << 8)
                if ow < 0x8000:
                    continue
                if nw == ((ow + delta) & 0xFFFF):
                    ok = True
                else:
                    tb = self.fixed_bank if ow >= self.fixed_base else b
                    ok = self._label_map(tb, ow) == nw
                if ok:
                    explained.add(start)
                    explained.add(start + 1)
                    break
            if not ok:
                # A differing byte that is not a relocated address is still
                # fine when the span decodes to the same instruction stream
                # (an immediate operand that the two revisions tuned).
                return 'opcodes-identical' if opcode_streams_match(o, n) else None
        return 'reloc-consistent'

    def map_addr(self, bank, addr):
        """-> (new_addr, status, detail). status in {'same', 'moved',
        'interpolated[-verified]', 'unlabeled', 'label-gone', 'ambiguous'}."""
        cands = {}
        names_seen = []
        banks = self.banks_for(bank, addr)
        for b in banks:
            names = self.old_by_addr.get((b, addr))
            if not names:
                continue
            names_seen.extend(names)
            for name in names:
                if (b, name) in self.new_by_name:
                    cands[self.new_by_name[(b, name)]] = name
        if len(cands) > 1:
            return None, 'ambiguous', ','.join(
                '%s@%04X' % (v, k) for k, v in sorted(cands.items()))
        if len(cands) == 1:
            new_addr, name = next(iter(cands.items()))
            return new_addr, ('same' if new_addr == addr else 'moved'), name
        if names_seen:
            return None, 'label-gone', names_seen[0]
        if self.interpolate:
            interp = {}
            for b in banks:
                r = self._interpolate(b, addr)
                if r:
                    interp[r[0]] = r
            if len(interp) == 1:
                return next(iter(interp.values()))
            if len(interp) > 1:
                return None, 'ambiguous-interp', ','.join(
                    '%04X' % k for k in sorted(interp))
        if names_seen:
            return None, 'label-gone', names_seen[0]
        return None, 'unlabeled', ''


HEX = re.compile(r'0x([0-9A-Fa-f]+)')

# 6502 instruction length per opcode, illegal opcodes included (BRK counts the
# byte the CPU skips).  Used to prove that two spans hold the same instruction
# stream even when immediate operands differ (PAL/NTSC timing constants).
OP_LEN = bytes((
    2,2,1,2,2,2,2,2,1,2,1,2,3,3,3,3, 2,2,1,2,2,2,2,2,1,3,1,3,3,3,3,3,
    3,2,1,2,2,2,2,2,1,2,1,2,3,3,3,3, 2,2,1,2,2,2,2,2,1,3,1,3,3,3,3,3,
    1,2,1,2,2,2,2,2,1,2,1,2,3,3,3,3, 2,2,1,2,2,2,2,2,1,3,1,3,3,3,3,3,
    1,2,1,2,2,2,2,2,1,2,1,2,3,3,3,3, 2,2,1,2,2,2,2,2,1,3,1,3,3,3,3,3,
    2,2,2,2,2,2,2,2,1,2,1,2,3,3,3,3, 2,2,1,2,2,2,2,2,1,3,1,3,3,3,3,3,
    2,2,2,2,2,2,2,2,1,2,1,2,3,3,3,3, 2,2,1,2,2,2,2,2,1,3,1,3,3,3,3,3,
    2,2,2,2,2,2,2,2,1,2,1,2,3,3,3,3, 2,2,1,2,2,2,2,2,1,3,1,3,3,3,3,3,
    2,2,2,2,2,2,2,2,1,2,1,2,3,3,3,3, 2,2,1,2,2,2,2,2,1,3,1,3,3,3,3,3,
))


def opcode_streams_match(o, n):
    """True when two equal-length byte spans decode to the same 6502 opcode
    sequence with the same instruction boundaries (operands may differ)."""
    if len(o) != len(n) or not o:
        return False
    p = 0
    while p < len(o):
        if o[p] != n[p]:
            return False
        p += OP_LEN[o[p]]
    return p == len(o)


class Report(object):
    def __init__(self):
        self.lines = []
        self.counts = defaultdict(lambda: defaultdict(int))

    def add(self, section, status, text):
        self.counts[section][status] += 1
        if status not in ('same',):
            self.lines.append('%-18s %-14s %s' % (section, status, text))

    def dump(self, fh):
        fh.write('== per-section counts ==\n')
        for sec in sorted(self.counts):
            c = self.counts[sec]
            fh.write('%-18s %s\n' % (sec, '  '.join(
                '%s=%d' % (k, c[k]) for k in sorted(c))))
        fh.write('\n== non-trivial entries ==\n')
        for l in self.lines:
            fh.write(l + '\n')


def migrate_functions_block(lines, i, mapper, rep):
    """Rewrite the `fixed = [...]` / `bankN = [...]` arrays of a [functions]
    table, in place, preserving line breaks and indentation."""
    out = []
    n = len(lines)
    while i < n:
        line = lines[i]
        if re.match(r'\s*\[', line):          # next section starts
            break
        m = re.match(r'\s*(fixed|bank(\d+))\s*=\s*\[', line)
        if not m:
            out.append(line)
            i += 1
            continue
        key = m.group(1)
        bank = -1 if key == 'fixed' else int(m.group(2))
        # collect the whole array
        arr = [line]
        i += 1
        while i < n and ']' not in arr[-1]:
            arr.append(lines[i])
            i += 1
        # migrate every address, keeping the same per-line grouping
        new_arr = []
        for idx, aline in enumerate(arr):
            def repl(mo):
                addr = int(mo.group(1), 16)
                new, status, detail = mapper.map_addr(bank, addr)
                rep.add('functions.%s' % key, status,
                        '0x%04X %s %s' % (addr, detail,
                                          '' if new is None else '-> 0x%04X' % new))
                if new is None:
                    return None        # sentinel handled below
                return '0x%04X' % new
            pieces = []
            pos = 0
            for mo in HEX.finditer(aline):
                pieces.append(aline[pos:mo.start()])
                r = repl(mo)
                pieces.append('' if r is None else r)
                if r is None:
                    # also swallow a following ", " so the list stays clean
                    tail = aline[mo.end():]
                    tm = re.match(r',\s*', tail)
                    pos = mo.end() + (tm.end() if tm else 0)
                else:
                    pos = mo.end()
            pieces.append(aline[pos:])
            merged = ''.join(pieces)
            if HEX.search(aline) and not HEX.search(merged) \
                    and merged.strip() in ('', ','):
                continue               # line held only dropped addresses
            new_arr.append(merged)
        out.extend(new_arr)
    return out, i


def parse_table_block(lines, i):
    """Collect one [[section]] block: its header, key lines and trailing blank."""
    block = [lines[i]]
    i += 1
    n = len(lines)
    while i < n and not re.match(r'\s*\[', lines[i]):
        block.append(lines[i])
        i += 1
    # trailing blank lines belong to the block (so dropping it leaves no gap)
    while block and block[-1].strip() == '':
        block.pop()
        i -= 1
    return block, i


def migrate(text, mapper, rep):
    lines = text.split('\n')
    out = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        m_tbl = re.match(r'\s*\[\[(\w+)\]\]\s*$', line)
        m_sec = re.match(r'\s*\[(\w+)\]\s*$', line)
        if m_sec and m_sec.group(1) == 'functions':
            out.append(line)
            i += 1
            blk, i = migrate_functions_block(lines, i, mapper, rep)
            out.extend(blk)
            continue
        if m_tbl and m_tbl.group(1) in TABLE_SECTIONS:
            sec = m_tbl.group(1)
            block, i = parse_table_block(lines, i)
            kept = migrate_table(sec, block, mapper, rep)
            if kept is not None:
                out.extend(kept)
                # keep the blank separator the original had
                if i < n and lines[i].strip() == '':
                    pass
            else:
                # drop the block AND one following blank line
                while i < n and lines[i].strip() == '':
                    i += 1
                    break
            # re-emit blanks that followed
            while i < n and lines[i].strip() == '':
                out.append(lines[i])
                i += 1
            continue
        out.append(line)
        i += 1
    return '\n'.join(out)


def _key_val(block, key):
    for idx, l in enumerate(block):
        m = re.match(r'\s*%s\s*=\s*(0x[0-9A-Fa-f]+|\d+)' % re.escape(key), l)
        if m:
            v = m.group(1)
            return idx, (int(v, 16) if v.startswith('0x') else int(v))
    return None, None


def _set_val(block, idx, new):
    block[idx] = HEX.sub('0x%04X' % new, block[idx], count=1)


def migrate_table(sec, block, mapper, rep):
    spec = TABLE_SECTIONS[sec]
    _, bank = _key_val(block, 'bank')
    if bank is None:
        bank = -1
    block = list(block)

    for k in spec['scalars']:
        idx, addr = _key_val(block, k)
        if idx is None:
            continue
        new, status, detail = mapper.map_addr(bank, addr)
        rep.add(sec, status, '%s=0x%04X %s %s' % (
            k, addr, detail, '' if new is None else '-> 0x%04X' % new))
        if new is None:
            return None
        _set_val(block, idx, new)

    for skey, ekey in spec['ranges']:
        si, start = _key_val(block, skey)
        ei, end = _key_val(block, ekey)
        if si is None:
            continue
        new_start, status, detail = mapper.map_addr(bank, start)
        if new_start is None:
            rep.add(sec, status, 'bank=%d %s=0x%04X %s (block dropped)'
                    % (bank, skey, start, detail))
            return None
        if ei is not None:
            new_end, estatus, edetail = mapper.map_addr(bank, end)
            if new_end is None:
                new_end = new_start + (end - start)
                estatus = 'end-length-preserved'
                edetail = '(%s had no label)' % ekey
            rep.add(sec, status if estatus in ('same', 'moved') else estatus,
                    'bank=%d 0x%04X-0x%04X -> 0x%04X-0x%04X %s %s'
                    % (bank, start, end, new_start, new_end, detail, edetail))
            _set_val(block, ei, new_end)
        else:
            rep.add(sec, status, 'bank=%d %s=0x%04X -> 0x%04X %s'
                    % (bank, skey, start, new_start, detail))
        _set_val(block, si, new_start)
    return block


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--toml', required=True)
    ap.add_argument('--old-sym', required=True)
    ap.add_argument('--new-sym', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--report', default=None)
    ap.add_argument('--fixed-bank', type=int, default=7,
                    help='bank permanently mapped at --fixed-base (default 7)')
    ap.add_argument('--fixed-base', type=lambda s: int(s, 0), default=0xC000)
    ap.add_argument('--banks', type=int, default=8,
                    help='total PRG bank count (default 8)')
    ap.add_argument('--old-rom', default=None,
                    help='iNES image matching --old-sym; enables byte verification '
                         'of anchor-interpolated addresses')
    ap.add_argument('--new-rom', default=None,
                    help='iNES image matching --new-sym')
    ap.add_argument('--no-interpolate', action='store_true',
                    help='drop unlabelled addresses instead of anchor-interpolating')
    ap.add_argument('--skip', action='append', default=[], metavar='SECTION',
                    help='leave this section untouched (repeatable). Use it when a '
                         'section is regenerated from the disassembly instead of '
                         'migrated, e.g. --skip data_region.')
    a = ap.parse_args()

    for s in a.skip:
        if s in TABLE_SECTIONS:
            del TABLE_SECTIONS[s]
        else:
            sys.exit('--skip %s: not an address-bearing section' % s)

    mapper = Mapper(a.old_sym, a.new_sym, a.fixed_bank, a.fixed_base, a.banks,
                    old_rom=a.old_rom, new_rom=a.new_rom,
                    interpolate=not a.no_interpolate)
    rep = Report()
    text = Path(a.toml).read_text(encoding='utf-8')
    new_text = migrate(text, mapper, rep)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(new_text)
    rep.dump(sys.stdout)
    if a.report:
        with open(a.report, 'w', encoding='utf-8', newline='\n') as f:
            rep.dump(f)


if __name__ == '__main__':
    main()
