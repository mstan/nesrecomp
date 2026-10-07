"""Attribute Windows RIP samples to sorted GNU nm text symbols, correcting ASLR.

No target is launched. Supply nm output from the exact sampled executable and
its preferred PE image base (GNU x64 default: 0x140000000). Nearest preceding
symbol attribution has no inline stacks and cannot rank callers inclusively.
"""
import argparse
import bisect
import collections
import json
import re
from pathlib import Path


def attribute(sample_text, symbol_text, preferred_base):
    identity = re.search(r"module_base=([0-9a-fA-F]+) module_size=(\d+)", sample_text)
    if identity is None:
        raise ValueError("samples lack executable module identity; ASLR cannot be corrected")
    base, size = int(identity[1], 16), int(identity[2])
    outcome = re.search(r"sample_errors=(\d+) child_exit=(\d+)", sample_text)
    if outcome is None:
        raise ValueError("samples lack capture outcome")
    errors, child_exit = int(outcome[1]), int(outcome[2])
    if errors or child_exit:
        raise ValueError(f"failed capture: sample_errors={errors}, child_exit={child_exit}")
    if not size:
        raise ValueError("empty executable module")
    symbols = []
    for line in symbol_text.splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]+) [Tt] ([^.].*)", line.strip())
        if match:
            address = int(match[1], 16)
            if preferred_base <= address < preferred_base + size:
                symbols.append((address, match[2]))
    symbols.sort()
    if not symbols:
        raise ValueError("no GNU nm text symbols inside the preferred executable image")
    addresses = [item[0] for item in symbols]
    counts = collections.Counter()
    for line in sample_text.splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]+),(\d+)", line.strip())
        if not match:
            continue
        rip, count = int(match[1], 16), int(match[2])
        name = "outside-executable"
        if base <= rip < base + size:
            address = rip - base + preferred_base
            index = bisect.bisect_right(addresses, address) - 1
            name = symbols[index][1] if index >= 0 else "unresolved-executable"
        counts[name] += count
    total = sum(counts.values())
    if not total:
        raise ValueError("capture has zero samples")
    return {"samples": total, "module_base": hex(base), "module_size": size,
            "sample_errors": errors, "child_exit": child_exit,
            "functions": [{"function": name, "samples": count,
                           "percent": round(100 * count / total, 3) if total else 0}
                          for name, count in counts.most_common()]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples", type=Path)
    parser.add_argument("symbols", type=Path)
    parser.add_argument("--preferred-base", required=True, type=lambda text: int(text, 0))
    args = parser.parse_args()
    print(json.dumps(attribute(args.samples.read_text(), args.symbols.read_text(),
                               args.preferred_base), indent=2))
