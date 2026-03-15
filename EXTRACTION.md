# Bank Extraction Guide

How to extract PRG banks from NES ROM files for Ghidra analysis.
Getting this wrong produces a Ghidra project that appears to work but returns zeros or wrong
addresses — causing silent misanalysis. Always verify vectors after extraction.

---

## iNES Header Primer

Every `.nes` file starts with a 16-byte iNES header:

```
Bytes 0–3:   "NES\x1A"  (magic)
Byte  4:     Number of 16KB PRG-ROM banks
Byte  5:     Number of 8KB CHR-ROM banks (0 = CHR RAM)
Byte  6:     Flags (mapper low nibble, mirroring, battery, trainer)
Byte  7:     Flags (mapper high nibble, NES 2.0 indicator)
Bytes 8–15:  Padding / extended header
```

PRG-ROM starts at file offset **`0x10`** (16 bytes in), unless a 512-byte trainer is present
(bit 2 of byte 6), in which case PRG starts at `0x210`.

Each PRG bank is **16KB = 0x4000 bytes**.

---

## Faxanadu — Mapper 1 (MMC1), 16 banks × 16KB

**ROM:** `baserom.nes`
**PRG banks:** 16 (numbered 0–15)
**CHR:** RAM (no CHR ROM banks)

### Memory map
| Bank | NES address | Notes |
|------|-------------|-------|
| 0–14 | `$8000–$BFFF` | Switchable via MMC1 |
| 15 | `$C000–$FFFF` | **Fixed** — always mapped |

### Extraction formula
```
file_offset = 0x10 + (bank_number × 0x4000)
size        = 0x4000  (16KB each)
```

### Extract all banks (run once to populate `banks/`)
```python
d = open('F:/Projects/nesrecomp/baserom.nes', 'rb').read()
import os; os.makedirs('banks', exist_ok=True)
for i in range(16):
    open(f'banks/bank{i:02d}.bin', 'wb').write(d[0x10 + i*0x4000 : 0x10 + (i+1)*0x4000])
```

### Ghidra load settings — per bank
| Setting | Fixed bank (15) | Switchable banks (0–14) |
|---------|-----------------|-------------------------|
| Format | Raw Binary | Raw Binary |
| Processor | 6502 / little / 16 / default | 6502 / little / 16 / default |
| Base address | **`0xC000`** | **`0x8000`** |
| Name | `bank15.bin` | `bankNN.bin` |

### Verification
After loading bank15.bin at `0xC000`, the vectors at `$FFFA–$FFFF` should read:
```
$FFFA–$FFFB: NMI  vector → $C999
$FFFC–$FFFD: RESET vector → $C913
$FFFE–$FFFF: IRQ  vector → $C9D5
```

---

## Super Mario Bros. — Mapper 0 (NROM-256), 2 banks × 16KB

**ROM:** `Super Mario Bros. (World).nes`
**PRG banks:** 2 (32KB total, **no bank switching**)
**CHR:** 1 × 8KB ROM bank

### Memory map
Because NROM-256 has no switching, **both banks are always mapped simultaneously**:

| Region | NES address | File offset |
|--------|-------------|-------------|
| Bank 0 | `$8000–$BFFF` | `0x10 – 0x400F` |
| Bank 1 | `$C000–$FFFF` | `0x4010 – 0x800F` |

The two banks are one contiguous 32KB block. **Load them as a single file.**

### Extraction
```python
d = open('F:/Projects/nesrecomp/Super Mario Bros. (World).nes', 'rb').read()
open('F:/Projects/nesrecomp/smb_prg.bin', 'wb').write(d[0x10:0x8010])
# Produces 32768 bytes covering NES $8000–$FFFF
```

### Ghidra load settings
| Setting | Value |
|---------|-------|
| Format | Raw Binary |
| Processor | 6502 / little / 16 / default |
| Base address | **`0x8000`** |
| Name | `smb_bank0.bin` |

Load the **single 32KB file** — do **not** split into two 16KB files. NROM has no mapper,
so splitting creates an artificial bank boundary that doesn't exist at runtime.

### Verification
After loading `smb_prg.bin` at `0x8000`, the vectors at `$FFFA–$FFFF` should read:
```
$FFFA–$FFFB: NMI  vector → $8082
$FFFC–$FFFD: RESET vector → $8000
$FFFE–$FFFF: IRQ  vector → $FFF0
```

---

## Adding a new game — checklist

1. Read the iNES header: `hexdump -C game.nes | head -2`
2. Check byte 4 (PRG bank count) and byte 6/7 (mapper number)
3. Determine if the mapper has bank switching:
   - **No switching (NROM):** extract full PRG as one file, base `0x8000`
   - **Fixed+switchable (MMC1, MMC3, etc.):** extract each bank separately; fixed bank
     goes at its NES address (`$C000` for 16KB fixed), switchable banks at `$8000`
4. Verify the NMI/RESET/IRQ vectors match the game's known entry points
5. Name each file `bankNN.bin` (or `game_prg.bin`) so the MCP server can find it
6. Run Ghidra Auto Analysis before any manual work

---

## Common mistakes

| Mistake | Symptom | Fix |
|---------|---------|-----|
| Wrong base address (e.g. `0x0000` instead of `0x8000`) | `get_hexdump` returns zeros; addresses are off | Re-import with correct base |
| Split NROM into two 16KB banks | Bank 1 code unreachable from Bank 0 analysis; cross-bank refs broken | Merge into single 32KB file |
| Missing `--game` flag when running recompiler | `inline_dispatch` tables not expanded; no default-case miss logging | `NESRecomp.exe rom.nes --game games/<name>/game.cfg` |
| Loaded bank at wrong NES address | Vector addresses look right but function addresses are all shifted | Check `Min Address` in `get_program_info` — must match bank's NES base |
