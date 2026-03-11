# NESRecomp Session Memory

Key findings that must survive context resets. Update this file whenever a root cause
is confirmed or a bug is fixed.

---

## Current Status (2026-03-11)

**Milestone reached**: Game runs 600+ frames with correct bank switching.
NMI fires every frame. Banks 0, 2, 5 switch correctly.

**Blocker**: Screen is black. $0013 (render-enable flag) is never set non-zero.
Root cause confirmed: JSR $F859 trampoline is not handled by code generator.

---

## Confirmed Root Cause: func_F859 never calls bank-12 title screen

### The problem

The main game loop at $FC65 uses `JSR $F859` + 3 inline bytes as a cross-bank
dispatch mechanism. Example at $FC68:

```
FC68: JSR $F859
FC6B: 0C          ; bank = 12
FC6C: 20          ; lo of (target_addr - 1)
FC6D: 9E          ; hi of (target_addr - 1)  → target = $9E21 in bank 12
FC6E: [real next instruction]
```

On hardware, $F859 manipulates the 6502 stack to effectively call $9E21 in bank 12
and return to $FC6E when done.

In our C recompiler, `func_F859()` is called as a normal C function. No return
address is on the 6502 stack. The PLA×2 inside func_F859 reads garbage. The inline
bytes are decoded as instructions (illegal $0C + JSR $209E = no-op). **The entire
title screen manager in bank 12 is never executed.**

### The fix (TODO: implement in code_generator.c + function_finder.c)

See `PATTERNS.md` Pattern 1 for the full inline expansion.

Short version: when the code generator sees `JSR $F859`, it reads the next 3 bytes
from ROM as [bank, lo, hi], and emits:
1. Save A/X/Y
2. Push $0100 (current bank) to 6502 stack
3. Call func_CC1A() with X=bank (switch bank)
4. Restore A/X/Y
5. call_by_address(lo | hi<<8 | 1)  — the target function
6. PLA → TAX → func_CC1A()  — $F8C6 return continuation, restores bank

PC advances 6 bytes total (3 for JSR + 3 for inline data).

function_finder.c must also skip 6 bytes and add the target function to its queue.

### Calls from main loop

| Call site | Bank | Target   | Purpose                    |
|-----------|------|----------|----------------------------|
| $FC68     | 12   | $9E21    | Title screen manager (main)|
| $FC74     | 12   | $9F44    | Title screen (loop)        |
| $FC89     | 12   | TBD      | (after start button press) |

---

## MMC1 Bank Switching — Two Entry Points

| Function | When used              | Behaviour                        |
|----------|------------------------|----------------------------------|
| $CC85    | NMI, normal bank saves | Checks $0012 guard; safe for NMI |
| $CC1A    | Init, F859 dispatch    | Direct 5-write sequence          |

Both correctly update g_current_bank. $CC1A also sets/clears $0012 flag.

---

## $0013 — Render-Enable Flag

- Cleared to 0 by: $CA90 (in func_CA78, init) and $CB01 (in func_CAF7, frame sync)
- Set to non-zero by: switchable bank code only (banks 0, 2, OR bank 12 title screen)
- NMI at $C999 checks $0013: 0 → renders-off path, non-zero → renders-on path
- **Must be set by bank-12 title screen logic once tiles/palettes are loaded**

---

## Execution Path from RESET

```
$C913 RESET → JSR $CBBF → JMP $CC1A (bank 14)
            → JSR $CA78 → CC1A(bank 5), JSR $8006/$8000, CC1A(restore), JMP $CB2F
            → JMP $DA6A
$DA6A       → call_by_address($B7AE), JSR $DA7D, JMP $FC65
$DA7D       → JSR $CAF7 (frame sync, waits 2 NMI frames)
              → JSR $E0AA, call_by_address($BA55), JSR $CE80 (CHR load bank 8)
$FC65       → LDX #$FF/TXS, JSR $F859[bank=12, $9E21], JSR $CA25, JSR $CB4F
              → loop: JSR $F859[bank=12, $9F44], check $0019 bit 4
```

---

## Ghidra Setup

- All banks in ONE Ghidra project (project locking issue known).
- Stand up one MCP server at a time. Always pass `program_name` param.
- Banks 5, 12, 14, 15 have been used; ports 9005, 9012, 9014, 9015.
- bank05 Ghidra import has base 0x0000 instead of 0x8000 — use decode_bank5.py instead.
- bank15 Ghidra: addresses match NES addresses directly (base 0xC000).

---

## Files

- `PATTERNS.md` — dispatch idioms requiring code_generator.c special handling
- `decode_fixed.py` — Python 6502 disassembler for bank15 (pass hex addr as arg)
- `decode_bank5.py` — Python 6502 disassembler for bank05
- `check_bank.py` — verifies bank extraction from ROM
