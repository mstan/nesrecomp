# NESRecomp

**Goal**: Static 6502 recompiler — Faxanadu NES ROM → native PC binary.
**MVP**: Get the game to the title screen. That proves the toolchain works.
This is NOT an NES emulator. We translate 6502 machine code to C, compile it, run it. That's it.

---

## ██████████████████████████████████████████████████
## ██  RULE 0: NO GHIDRA = NO ACTION. FULL STOP.  ██
## ██████████████████████████████████████████████████

At the start of EVERY session, before touching ANY file:

Call `mcp__ghidra__get_program_info`. If it does not respond:

> GHIDRA IS NOT RUNNING.
> I will not read files, write code, or make any suggestions.
> Load the fixed bank (bank15.bin, 16KB, extracted at file offset 0x3C010) into Ghidra
> as Raw Binary, 6502 processor, base address 0xC000. Name it "faxanadu_nesrecomp".
> Start the Ghidra MCP server, reconnect with /mcp, then try again.

Extract bank15.bin:
  python -c "d=open('F:/Projects/nesrecomp/baserom.nes','rb').read(); open('F:/Projects/nesrecomp/bank15.bin','wb').write(d[0x3C010:0x40010])"

This rule has NO exceptions. No guessing 6502 behavior. No action of any kind until Ghidra responds.

Ghidra fixed-bank address = NES address (bank 15 at 0xC000, no offset arithmetic).
For switchable banks: base=0x8000, file_offset = 0x10 + bank*0x4000.

---

## ████████████████████████████████████████████████████████████████
## ██  RULE 1: NEVER TOUCH generated/faxanadu_full.c DIRECTLY   ██
## ████████████████████████████████████████████████████████████████

`generated/faxanadu_full.c` is a BUILD ARTIFACT. Output of the recompiler.

**NEVER read it whole. NEVER modify it. NEVER patch it.**

If generated code is wrong → fix `recompiler/src/code_generator.c` and regenerate.
- To find a function: grep for `func_C123` or `func_8456`
- To read part of it: use Read with offset + limit — never the whole file
- To fix it: **you don't. Fix the recompiler.**

---

## The Loop (this is the entire development methodology)

```
1. BUILD recompiler     →  NESRecomp.exe  (only when recompiler src changes)
2. RUN recompiler       →  generates generated/faxanadu_full.c
3. BUILD runner         →  NESRecompGame.exe  (most common — after runtime/ppu changes)
4. RUN game (timed)     →  start, wait 10s, kill
5. OBSERVE screenshot   →  Read C:/temp/nes_shot_01.png  (saved every 60 NES frames)
6. IDENTIFY bug         →  wrong pixels → ppu_renderer.c;  crash → Ghidra
7. GHIDRA if needed     →  understand what the 6502 code actually does
8. FIX the bug          →  runtime.c / ppu_renderer.c / code_generator.c
9. GOTO 1 (or 3 if only runner changed)
```

## Debugging Hierarchy (DO NOT SKIP STEPS)

**Step 1 — Ghidra any unknown address immediately.**
Call `mcp__ghidra__get_code` before reading source or adding any printf.
Do not guess. Do not read large sections of generated code.

**Step 2 — Check PPU register trace.**
`C:/temp/ppu_trace.csv` — every $2000-$2007 write. Format: `W,$2006,$20,PC=?,F=5`
`C:/temp/mapper_trace.csv` — every bank switch. Format: `BANK_SWITCH,bank=3,PC=?,F=12`

**Step 3 — Add a targeted debug log when the trace files don't tell you enough.**
The standard traces (ppu_trace.csv, mapper_trace.csv) only capture PPU writes and bank switches.
When the execution path is the question (e.g., "is func_FC65 ever reached?"), add a per-frame
log to `C:/temp/debug_trace.txt` by editing `main_runner.c`:

- `debug_log_frame()` is already in main_runner.c. It logs: frame, bank, r13, r14, r20, r1F, S.
- To add more fields: edit the fprintf in `debug_log_frame()`.
- To disable: comment out the `debug_log_frame(s_cb_count)` call in `nes_vblank_callback()`.

**ALWAYS comment out or remove debug log writes after the investigation.**
Do NOT leave per-frame file I/O in the final build — it creates a 60MB/min file.

Use `log_on_change()` in runtime.c for tracking a single RAM value over time without flood output.
If you need more than one new trace per investigation cycle, stop and use Ghidra.

Session resume after context clear: **say "Run the game."** Screenshot + Ghidra = source of truth.

---

## ████████████████████████████████████████████████████████
## ██  RULE 2: CHECK PATTERNS.md BEFORE ANY GHIDRA WORK  ██
## ████████████████████████████████████████████████████████

Before implementing ANY function discovered via Ghidra tracing:

**Read `F:/Projects/nesrecomp/PATTERNS.md` first.**

If a function you are tracing does ANY of these things, stop and check PATTERNS.md:
- PLA/PHA that touches the return address
- RTS used as a computed goto (jump through stack-stored address)
- Inline data bytes immediately following a JSR
- A function whose body reads the 6502 stack to find out who called it

These are **dispatch idioms** that the static recompiler cannot handle as plain function
calls. Each has a required code_generator.c inline expansion. Getting it wrong produces
C code that compiles, runs, and silently executes the wrong game logic.

**Known idiom: JSR $F859** — see PATTERNS.md Pattern 1. Every call to $F859 must be
inlined as a bank-dispatch + call_by_address + bank-restore sequence. The 3 bytes after
the JSR are data (bank, addr_lo, addr_hi), not instructions.

---

## Visual Debugging

Screenshots auto-saved as PNG every 60 NES frames, named by frame number.

| File | Contents |
|------|----------|
| `C:/temp/nes_shot_XXXX.png` | Screenshot at frame XXXX (every 60 frames) |
| `C:/temp/ppu_trace.csv` | PPU register writes: W,ADDR,VALUE,PC,FRAME |
| `C:/temp/mapper_trace.csv` | Mapper bank switches: BANK_SWITCH,bank,PC,FRAME |
| `C:/temp/quicksave.sav` | F6 quick-save slot |
| `src/title_reference.png` | Reference screenshot of Faxanadu title screen |

Screenshots are PNG. **BMP is prohibited** — too large for token limits.
**Clean up old screenshots** with `rm C:/temp/nes_shot_*.png` after examining them.

Faxanadu title: dark blue sky, tree canopy, "FAXANADU" in gold, "PUSH START BUTTON"
- Wrong colors → palette mapping bug in ppu_renderer.c
- Scrambled tiles → CHR bank load order bug in runtime.c
- Black screen → PPU enable not set OR NMI not firing

---

## Input Scripts and Save States

### Running with a script
```batch
# Run synchronously — game exits when script reaches EXIT 0
"F:/Projects/nesrecomp/build/runner/Release/NESRecompGame.exe" baserom.nes \
    --script C:/temp/my_session.txt > C:/temp/stdout.txt 2>&1
ls -t C:/temp/nes_shot_*.png | head -5
```

### Recording a session
```batch
"F:/Projects/nesrecomp/build/runner/Release/NESRecompGame.exe" baserom.nes \
    --record C:/temp/my_session.txt
# Play normally; close window → EXIT 0 written automatically
```

### Save state hotkeys (in-game)
| Key | Action |
|-----|--------|
| F5  | Toggle turbo (fast-forward) |
| F6  | Save state → `C:/temp/quicksave.sav` |
| F7  | Load state ← `C:/temp/quicksave.sav` |

F7 presses are recorded into `--record` files as `LOAD_STATE` commands and replayed
correctly. Frame baseline re-syncs after load so subsequent WAITs are not inflated.

### Loading a save state at startup
```batch
"F:/Projects/nesrecomp/build/runner/Release/NESRecompGame.exe" baserom.nes \
    --loadstate C:/temp/quicksave.sav --script C:/temp/test.txt
```

### Script command reference
| Command | Description |
|---------|-------------|
| `WAIT <n>` | Wait n frames |
| `HOLD <BTN>` | Hold button (A B SELECT START UP DOWN LEFT RIGHT) |
| `RELEASE <BTN>` | Release button |
| `TURBO ON\|OFF` | Toggle fast-forward |
| `SCREENSHOT [file]` | Save PNG to C:/temp/ |
| `LOG <msg>` | Print message to stdout |
| `SAVE_STATE <path>` | Save emulator state to file |
| `LOAD_STATE <path>` | Restore emulator state from file |
| `WAIT_RAM8 <hex_addr> <hex_val>` | Block until g_ram[addr]==val (30s timeout) |
| `ASSERT_RAM8 <hex_addr> <hex_val> [msg]` | Assert RAM value |
| `EXIT [code]` | Exit with code (default 0) |

---

## Build Commands

```batch
# Build recompiler (after code_generator.c changes)
cmake --build F:/Projects/nesrecomp/build/recompiler --config Release
# (do NOT use nesrecomp_build_recompiler.bat — cmake output goes silent in bash)

# Regenerate faxanadu_full.c
F:/Projects/nesrecomp/build/recompiler/Release/NESRecomp.exe F:/Projects/nesrecomp/baserom.nes

# Build runner (most common)
cmake --build F:/Projects/nesrecomp/build/runner --config Release 2>&1 | tail -5

# Run with a script (synchronous — exits at EXIT 0)
powershell -File C:/temp/kill_nes.ps1
"F:/Projects/nesrecomp/build/runner/Release/NESRecompGame.exe" baserom.nes \
    --script C:/temp/my_session.txt > C:/temp/stdout.txt 2>&1
ls -t C:/temp/nes_shot_*.png | head -5
```

---

## Key Files

| File | Purpose | Edit? |
|------|---------|-------|
| `recompiler/src/code_generator.c` | 6502→C emitter — THE PRODUCT | Yes |
| `recompiler/src/function_finder.c` | JSR/RTS boundary detection | Yes if needed |
| `runner/src/runtime.c` | NES memory map, PPU reg stubs | Yes |
| `runner/src/ppu_renderer.c` | Tiles, palettes, BG, sprites | Yes |
| `runner/src/main_runner.c` | SDL2 window, NMI loop, frame timing | Yes |
| `generated/faxanadu_full.c` | **GENERATED. NEVER TOUCH.** | **NEVER** |
| `generated/faxanadu_dispatch.c` | **GENERATED. NEVER TOUCH.** | **NEVER** |
| `baserom.nes` | Faxanadu US ROM — source of truth | Never |
| `faxanadu-disasm/` | chipx86 disassembly — labels are **hypotheses, audit with Ghidra before use** | Never (copy code) |

---

## Log File Rule

Every .c file implementing hardware behavior gets a sibling .log:

`runtime.c` → `runtime.log`
`ppu_renderer.c` → `ppu_renderer.log`

Format:
```
[function_name or NES address]
Ghidra: <what the decompiler/disassembler showed>
Rationale: <why implemented this way>
```

Does NOT go in source as comments. Reference only.

---

## Architecture Notes

**Static recompiler.** 6502 binary → C → native x64. No interpreter loop.
**JSR = direct C function call.** `func_C123()` calls `func_C456()` directly.
**NMI is the frame driver.** Runner calls func_NMI() at 60Hz wall-clock.
**runtime.c starts minimal.** Implement stubs only as the game calls them. Ghidra first.
**6502 stack is real RAM.** Stack at g_ram[0x100 + S]. JSR/RTS manipulate g_cpu.S.

---

## Milestones

| Milestone | Status |
|-----------|--------|
| Recompiler generates faxanadu_full.c | ✅ |
| Runner links and opens window (black screen) | ✅ |
| NMI handler runs without crash | ✅ |
| Any PPU write observed in ppu_trace.csv | ✅ |
| Any pixel visible on screen | ✅ |
| Title screen recognizable | ✅ |
| Title screen correct | ✅ |
| Game starts after button press | ⬜ |

---

## What NOT to Do

- Do not pre-emptively implement PPU features "just in case"
- Do not read faxanadu_full.c whole for "context"
- Do not guess what a function does — Ghidra it
- Do not import or use code from faxanadu-disasm/ — reference labels only
- Do not carry any source from the prior Faxanadu multi-agent port project
- Do not add exhaustive printf traces — use log_on_change() for one targeted value at a time
