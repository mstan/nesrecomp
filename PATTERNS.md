# Faxanadu 6502 Dispatch Patterns

**CONSULT THIS FILE** before implementing any fixed-bank function that involves:
- PLA/PHA manipulation of the return address
- RTS used as a computed goto
- Inline data bytes immediately following a JSR

Failure to handle these correctly produces silently-broken C that compiles and runs but
executes the wrong game logic.

---

## Pattern 1: JSR $F859 — Inline-Parameter Bank Dispatch

### What it looks like in ROM

```
JSR $F859       ; 3 bytes: 20 59 F8
<bank>          ; 1 byte:  e.g. $0C  (target PRG bank)
<addr_lo>       ; 1 byte:  e.g. $20  (low byte of target address - 1)
<addr_hi>       ; 1 byte:  e.g. $9E  (high byte of target address - 1)
; real next instruction starts here (+6 from JSR)
```

### What it does on hardware

$F859 is a trampoline. It:
1. Pops the return address (call-site + 2) off the 6502 stack
2. Reads the 3 inline bytes using that address as a pointer
3. Switches PRG bank to `<bank>` via $CC1A
4. Uses RTS to jump to `<addr_hi>:<addr_lo> + 1` (the target function)
5. When target returns → goes to $F8C6 (PLA/TAX/JMP $CC1A → restore bank)
6. After bank restore → returns to call-site + 6 (skipping the 3 data bytes)

### Target address formula

`target_addr = (<addr_lo> | (<addr_hi> << 8)) + 1`

Example: lo=$20, hi=$9E → target = $9E20 + 1 = **$9E21**

### $F8C6 — the return continuation

```
F8C6: PLA          ; pop saved bank (pushed at $F875)
F8C7: TAX
F8C8: JMP $CC1A    ; restore saved bank via CC1A
```

### Code generator fix (emit this instead of func_F859())

```c
/* F859 dispatch: bank=BANK addr=$ADDR */
{ nes_write(0xDE,g_cpu.A); nes_write(0xDF,g_cpu.X); nes_write(0xE0,g_cpu.Y);
  g_ram[0x100+g_cpu.S] = g_ram[0x100]; g_cpu.S--;   /* push current bank */
  g_cpu.X = BANK; func_CC1A();                        /* switch to target bank */
  g_cpu.A=nes_read(0xDE); g_cpu.X=nes_read(0xDF); g_cpu.Y=nes_read(0xE0); /* restore regs */
  call_by_address(ADDR);                              /* call target */
  g_cpu.S++; g_cpu.A=g_ram[0x100+g_cpu.S]; FLAG_NZ(g_cpu.A); /* F8C6: PLA */
  g_cpu.X=g_cpu.A; FLAG_NZ(g_cpu.X); func_CC1A(); }  /* F8C6: TAX + JMP CC1A */
```

PC must advance by **6** (3 for JSR + 3 inline bytes).

### function_finder fix

When JSR $F859 is encountered:
- Read bytes at PC+3 (bank), PC+4 (lo), PC+5 (hi)
- Compute target_addr = (lo | hi<<8) + 1
- add_function(list, target_addr, disp_bank)
- Also add_function(list, 0xF859, fixed_bank) as before
- Advance PC by **6**, not 3

### Known call sites (fixed bank)

| Call site | Bank | Target addr |
|-----------|------|-------------|
| $FC68     | 12   | $9E21       |
| $FC74     | 12   | $9F44       |
| $FC89     | 12   | $?          |
| $FC98     | 12   | $?          |
| $898F (b?) | ?   | $?          |
| $E024     | ?    | $?          |
| $C4D4     | ?    | $?          |
| $C812–$C87C | ?  | $?          |
| $D990     | ?    | $?          |

### Why static recompiler breaks without fix

`func_FC65()` calls `func_F859()` as a normal C function. No return address is pushed
to the 6502 stack. The PLA×2 inside func_F859 reads garbage RAM. The 3 inline bytes
are then decoded as real instructions (usually `??? $0C` + `JSR $209E` which is a no-op).
**The bank-12 title screen functions are never called. g_current_bank never becomes 12.
$0013 (render-enable) is never set. Screen stays black forever.**

---

## Pattern 2: $CC1A vs $CC85 — Two PRG Bank Switch Entry Points

Both switch the PRG bank using X. They differ in shift-register ceremony:

| Function | Behaviour |
|----------|-----------|
| $CC85    | Checks $0012 guard; if non-zero, resets MMC1 shift register first (interrupt-safe) |
| $CC1A    | Always does a clean 5-write sequence; also used as tail-call target from $F8C6 |

Both end with the same net result: PRG bank = X, g_current_bank updated.
The code generator correctly uses both; do not conflate them.

---

## Pattern 3: $CB2F — NMI Enable

```
CB2F: LDA $000A
CB31: ORA #$80        ; always set — unconditional NMI enable
CB33: BNE $CB39       ; always branches
CB39: STA $000A
CB3B: STA $2000       ; writes $000A|$80 to PPUCTRL → enables NMI
CB3E: RTS
```

This is called at the end of every major initialization function. Seeing it in a trace
means the game just finished a reset/init phase and is about to enter the main loop.

---

## Patterns to watch for (not yet encountered)

- `JMP ($XXXX)` where vector is in ROM → static target, can be resolved
- PLA/PHA around a function that modifies the stack depth (probe with Ghidra xrefs)
- Self-modifying code writing to $8000–$BFFF (would break static recompilation entirely;
  Faxanadu is not known to do this)
