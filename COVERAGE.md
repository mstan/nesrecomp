# NESRecomp Recompiler Coverage Audit

Date: 2026-05-03

Scope: recompiler behavior only: `cpu6502_decoder`, `code_generator`, `function_finder`,
and generated dispatch behavior. Runner code is mentioned only where generated C depends
on runtime helpers.

Reference baseline:

- MOS/MCS6500 programming manual material, via the searchable Synertek/MOS mirror:
  https://syncopate.us/books/Synertek6502ProgrammingManual.html
- NESdev 6502 instruction reference:
  https://www.nesdev.org/wiki/Instruction_reference
- NESdev 2A03 notes:
  https://www.nesdev.org/wiki/2A03
- NESdev decimal-mode notes:
  https://www.nesdev.org/wiki/Visual6502wiki/6502DecimalMode
- NESdev CPU errata:
  https://www.nesdev.org/wiki/Errata
- NESdev unofficial opcode reference:
  https://www.nesdev.org/wiki/CPU_unofficial_opcodes

## Summary

The decoder has full byte coverage: all 256 opcode bytes are present in
`recompiler/src/cpu6502_decoder.c`. The current table classifies 151 official
6502 opcodes, 7 undocumented `LAX` opcodes with real semantics, and 98 other
undocumented/unofficial opcodes as `MN_ILLEGAL`.

The code generator has switch cases for every official mnemonic in
`cpu6502_decoder.h`; I did not find an official mnemonic that falls through to
`/* unhandled mnemonic */`. The biggest gaps are not missing official switch
cases. They are semantic and control-flow coverage gaps:

- 98 undocumented opcode bytes are emitted as sized NOPs.
- `BRK` is treated as a comment/stop, not as the official software interrupt.
- `JMP (addr)` appears to miss the NMOS/NES page-wrap behavior for vectors at
  `$xxFF`.
- Base cycle counts are emitted, but taken-branch and page-crossing cycle
  penalties are mostly not modeled.
- Function discovery intentionally rejects opcode streams containing
  `MN_ILLEGAL`, which can miss real functions in ROMs that use unofficial
  opcodes.
- Dynamic dispatch misses and inline dispatch misses become generated early
  returns, so unresolved control flow can silently terminate a path after logging.

## Opcode Coverage

Current decoder counts from `g_opcode_table`:

| Category | Count | Status |
| --- | ---: | --- |
| Official 6502 opcodes | 151 | Decoded and emitted |
| Undocumented `LAX` opcodes | 7 | Decoded and emitted |
| Other undocumented/unofficial opcodes | 98 | Decoded for size/cycles, emitted as skip |

Official coverage is good at the mnemonic/addressing-mode level. The enum lists
all official mnemonics, and the emitter handles them in `emit_instruction`.
Important references:

- `recompiler/src/cpu6502_decoder.h:8` lists official mnemonics.
- `recompiler/src/code_generator.c:658` starts the mnemonic switch.
- `recompiler/src/code_generator.c:1446` is the fallback unhandled-mnemonic
  comment; no official mnemonic appears to depend on it.

Main opcode gap: `MN_ILLEGAL` is handled before the switch:

- `recompiler/src/code_generator.c:653` emits `/* ILLEGAL ... skip */`.
- `recompiler/src/cpu6502_decoder.c:5` documents that only `LAX` is emitted with
  behavior and other illegals are treated as sized NOPs.
- `recompiler/src/function_finder.c:208` and `:215` reject `MN_ILLEGAL` during
  deep target validation.

This means the instruction stream stays aligned, but behavior is wrong if a game
intentionally executes an unofficial opcode. The missing groups include stable and
commonly documented NMOS/NES opcodes such as `SAX`, `DCP`, `ISC`, `SLO`, `RLA`,
`SRE`, `RRA`, `ANC/AAC`, `ALR`, `ARR`, `AXS`, the `$EB` immediate `SBC` alias,
multi-byte NOPs, and halt/unstable opcodes such as `KIL`, `XAA`, `SHA`, `TAS`,
`SHX`, `SHY`, and `LAS`.

Priority:

1. Implement stable, useful opcodes first: `$EB` as `SBC #imm`, multi-byte NOPs
   with their memory-read/timing behavior, `SAX`, `DCP`, `ISC`, `SLO`, `RLA`,
   `SRE`, `RRA`, `ANC`, `ALR`, `ARR`, and `AXS`.
2. Decide explicit behavior for `KIL/STP`: halt/trap/log, not fall-through skip.
3. Treat unstable opcodes as a separate compatibility tier, because NESdev notes
   that some are not predictable across variants.

## Semantic Gaps

### `BRK`

Reference behavior: `BRK` pushes the interrupt return address and status flags,
sets interrupt disable, sets the break bit in the pushed flags byte, and jumps to
the IRQ vector. NESdev also notes that the pushed return address skips the byte
after the opcode.

Current behavior:

- `recompiler/src/code_generator.c:1443` emits only `/* BRK - software interrupt, skip */`.
- `recompiler/src/code_generator.c:1976` treats `BRK` as a generated-body stop
  condition.
- `recompiler/src/function_finder.c:1148` treats `BRK` as a walk stop.
- `validate_code_target` rejects `BRK` at function entry via opcode `$00`.

Impact: games that use `BRK` as a software interrupt, syscall, debug trap, or
patch mechanism will not work correctly. This is probably low frequency for
licensed NES games, but it is an official-opcode coverage gap.

### `JMP (addr)` page-wrap erratum

NESdev CPU errata documents the inherited NMOS 6502 behavior: for `JMP ($xxFF)`,
the high byte is fetched from `$xx00`, not from the next page.

Current behavior:

- `runner/src/runtime.c:403` implements `nes_read16(addr)` as `addr` and
  `addr + 1`.
- `recompiler/src/code_generator.c:1242` uses `nes_read16(abs16)` for non-zero
  page indirect jumps.
- `recompiler/src/function_finder.c:1127` statically resolves ROM indirect
  vectors with `vec_addr + 1`.

Impact: indirect jump tables placed at the end of a page will dispatch to the
wrong target compared with real NES hardware.

### Decimal mode

For a generic NMOS 6502, `SED` affects `ADC`/`SBC` decimal adjust. For the NES
RP2A03/RP2A07, decimal adjust is disconnected while the D flag can still be set
and cleared.

Current behavior:

- `recompiler/src/code_generator.c:709` and `:720` always emit binary `ADC` and
  `SBC`.
- `recompiler/src/code_generator.c:1436` and `:1437` preserve `CLD`/`SED` flag
  behavior.

Impact: this is correct for NES 2A03 behavior and should not be changed for the
NES target. It would be a gap only if this recompiler were generalized to a true
NMOS 6502 target with decimal arithmetic.

### Cycle and bus behavior

The decoder table stores base cycles, and generated code calls
`maybe_trigger_vblank(e->cycles)` at each instruction boundary
(`recompiler/src/code_generator.c:651`). The decoder comments explicitly say
these are base cycles and exclude page-cross penalties.

Missing or approximate timing:

- Loads/logical ops/compare/`ADC`/`SBC` do not add the documented extra cycle
  for absolute indexed or `(indirect),Y` page crossing.
- Branches do not model the normal taken-branch `+1` and page-cross `+2` cycle
  rules. The generated code adds an extra `maybe_trigger_vblank(2)` only on
  taken backward branches.
- Unofficial NOPs with operand reads are skipped without performing the read.
- Memory read-modify-write instructions emit one read and one final write; real
  6502 bus behavior includes extra/dummy writes or reads that can matter for
  memory-mapped I/O or mapper registers.

Impact: this is mostly a timing and side-effect fidelity issue, not a basic
register-result issue for normal RAM. It can affect games that rely on exact
NMI timing, status-register polling cadence, mapper side effects, or illegal NOP
reads against I/O.

## Control-Flow Discovery Gaps

The function finder has several deliberate conservative stops. They are good for
avoiding data-as-code false positives, but they are also coverage limits.

Important examples:

- `validate_code_target` rejects any target stream containing `MN_ILLEGAL`
  (`recompiler/src/function_finder.c:208` and `:215`). This misses valid code
  that uses unofficial opcodes.
- `BRK` is treated as suspicious at entry and as a stop in walks.
- Function walks and body emission cap at `MAX_INSNS_PER_FUNC` 2048
  (`recompiler/src/function_finder.h:11`).
- Pending forward branch queues are fixed at 256 entries
  (`recompiler/src/function_finder.c:231` and `:572`).
- `nop_jsr` intentionally skips configured JSRs
  (`recompiler/src/function_finder.c:984`, `recompiler/src/code_generator.c:978`).
- `inline_dispatch`, `inline_pointer`, split tables, trampoline handling, and
  SRAM-mapped code are pattern/config driven. Unrecognized variants can become
  missing functions.

Generated early-return risks:

- Cross-function branch targets that are not local labels become
  `call_by_address(target); return;` (`recompiler/src/code_generator.c:797`).
- Inline dispatch miss defaults log and return
  (`recompiler/src/code_generator.c:977`).
- Generated dispatch misses log and return failure in the default case
  (`recompiler/src/code_generator.c:2126` and `:2137`).

Impact: when discovery misses a dynamic target, generated C can compile and run
but terminate the path after a miss log instead of preserving 6502 control flow.
That is the main "early return" class worth tracking.

Doc drift found during audit:

- `README.md:155` says `emit_dispatch` skips bank variants where more than 50%
  of the first 8 instructions are illegal opcodes. I did not find that guard in
  the current `code_generator.c`. Either the README is stale or the guard was
  removed.

## Test Coverage Gaps

The existing TypeScript tests mostly exercise discovery and generated-text shape:
RESET/JSR/JMP discovery, RTI hijack, MMC3 bank resolution, branch labels,
dispatch table scanning, data-region exclusion, and "illegal opcodes do not
crash". They do not execute generated C against a CPU-state oracle for each
opcode.

Highest-value test additions:

1. A generated-C opcode harness that runs each official opcode/addressing mode
   against a small CPU/memory fixture and compares A/X/Y/S/P/memory changes
   with an oracle.
2. Specific tests for `BRK`, `RTI`, stack flags, `PHP`/`PLP`, `JSR`/`RTS`
   return-address behavior, and `JMP ($xxFF)`.
3. Timing-focused tests for branch taken/not-taken/page-cross and indexed load
   page crossing, if NMI timing fidelity is a goal.
4. Unofficial opcode tests in tiers: `$EB`/NOP variants, stable combined
   operations, then unstable/halt behavior.
5. Discovery tests where table targets contain stable unofficial opcodes, to
   verify that target validation no longer rejects real code once those opcodes
   are implemented.

## Suggested Improvement Plan

Recommended order:

1. Add an explicit unsupported-opcode policy before changing behavior: decide
   whether `KIL` and unstable opcodes should halt, log/fail, or remain
   compatibility skips.
2. Implement low-risk unofficial opcode semantics: `$EB` as `SBC #imm`, DOP/TOP
   NOPs with operand reads/cycles, and `SAX`.
3. Implement stable combined unofficial operations using existing helpers:
   `DCP`, `ISC`, `SLO`, `RLA`, `SRE`, `RRA`, `ANC`, `ALR`, `ARR`, and `AXS`.
4. Add a `nes_read16_bug()` or equivalent for `JMP (abs)` and use it in both
   codegen and static ROM-vector resolution.
5. Implement `BRK` or intentionally emit a hard diagnostic/trap so unsupported
   execution cannot silently continue.
6. Add optional cycle-accuracy helpers for page-cross and branch-taken timing,
   then gate broader bus-accurate dummy read/write behavior behind tests or a
   clear compatibility need.
7. Make discovery misses more measurable: count rejected unofficial opcodes,
   rejected BRK entries, unresolved indirect jumps, inline-dispatch misses, and
   generated dispatch misses in recompiler output.

## Current Risk Ranking

| Risk | Severity | Why |
| --- | --- | --- |
| Unofficial opcodes emitted as skips | High | Known NES software can rely on them; side effects are lost. |
| Discovery rejects unofficial opcode streams | High | Can hide real functions before codegen has a chance to emit them. |
| Dynamic dispatch miss returns | High | Produces a running build with lost control flow. |
| `BRK` skipped | Medium | Official behavior missing, but likely rare in production NES games. |
| `JMP ($xxFF)` page-wrap behavior | Medium | Rare but concrete CPU compatibility bug. |
| Page-cross/taken-branch timing | Medium | Matters for NMI/polling-sensitive code. |
| RMW/dummy bus behavior | Low to Medium | Mostly visible through memory-mapped I/O or mapper side effects. |
| Decimal mode | Low for NES | Correct to ignore decimal adjust on RP2A03/RP2A07. |

