#!/usr/bin/env python3
"""
make_cyc_stress_rom.py - build a synthetic NROM test program for NESRecomp's
cycle-accurate mode.

The ROM runs every non-jamming 6502 opcode (documented and undocumented) from
ROM, in every addressing variant the recompiler has a template for: indexed
reads with and without page crossings, zero page index wrap, taken/not-taken
branches with forward and backward page crossings, stack and subroutine
instructions, BRK, and the unstable SHA/SHS/SHX/SHY stores. Registers and
flags change every pass, and a variable delay keeps shifting the loop against
the interrupt and DMA schedule:

  - NMI every frame, with $2002 reads racing it
  - APU frame IRQ, acknowledged in the IRQ handler
  - a looping DMC sample at the fastest rate (DMC DMAs steal cycles)
  - an OAM DMA ($4014 write) every pass

so over a few thousand frames interrupts and DMAs land on every cycle of
every compiled instruction template.

Each pass also exercises the one IRQ line the frame counter and the DMC share
(the 2A03 ORs their flags): after ~1000 cycles with IRQs masked, in which a
frame IRQ may have become pending, the instruction after CLI writes $4015 or
$4010, acknowledging the DMC on the cycle it polls; then the sample is made
one-shot with its IRQ enabled, and the handler leaves that IRQ unacknowledged
for up to three entries (reading $4015 does not clear it, so RTI re-enters)
before restoring the loop. One pass in eight inhibits frame IRQs while the DMC
IRQ is pending. The $4015 writes land on every point of the DMC's schedule
(sample starts, and the explicit and implicit aborts).

Each pass also works the PPU from the same changing state, at a different
point in the frame: $2000 (pattern tables, 8x16 sprites, VRAM increment),
$2001 (every mask combination, so rendering switches on and off mid-frame),
$2005/$2006 scrolling, palette and nametable writes and buffered reads through
$2007, and $2003/$2004. The CHR ROM is a fixed pseudo-random pattern and OAM
comes from the scratch page, so background and sprite pixels, flips, 8x16
rows, priority, sprite 0 hits and overflow all reach the framebuffer.

The program does not check results
itself: build it with NESRecomp --cycle-accurate and compare the per-cycle
trace hashes of a native run against an --interp-only run (see
runner/cyc/README.md).

Writes are confined to scratch RAM; everything the loop depends on (pointers,
counters, JMP vectors, the stack) lives where no tested store can reach.

  python make_cyc_stress_rom.py out.nes
"""
import sys

# ---- addressing modes of all 256 opcodes (NMOS 6502 incl. undocumented) ----

IMP, IMM, ZP, ZPX, ZPY, ABS, ABSX, ABSY, INDX, INDY, IND, REL, HLT = range(13)
SIZE = {IMP: 1, IMM: 2, ZP: 2, ZPX: 2, ZPY: 2, ABS: 3, ABSX: 3, ABSY: 3,
        INDX: 2, INDY: 2, IND: 3, REL: 2, HLT: 1}


def mode_of(op):
    row_odd = (op >> 4) & 1
    col = op & 0x0F
    if col == 0x0:
        if row_odd:
            return REL
        return {0x00: IMP, 0x20: ABS, 0x40: IMP, 0x60: IMP}.get(op, IMM)
    if col in (0x1, 0x3):
        return INDY if row_odd else INDX
    if col == 0x2:
        if op in (0x82, 0xA2, 0xC2, 0xE2):
            return IMM
        return HLT
    if col in (0x4, 0x5, 0x6, 0x7):
        if not row_odd:
            return ZP
        return ZPY if op in (0x96, 0xB6, 0x97, 0xB7) else ZPX
    if col in (0x8, 0xA):
        return IMP
    if col in (0x9, 0xB):
        return ABSY if row_odd else IMM
    if col in (0xC, 0xD, 0xE, 0xF):
        if not row_odd:
            return IND if op == 0x6C else ABS
        return ABSY if op in (0x9E, 0xBE, 0x9F, 0xBF) else ABSX
    raise AssertionError(op)


# ---- RAM layout ----
ZP_SCRATCH = 0xF8      # zp operand; zp,X/zp,Y with X,Y <= $0F wrap into $00-$07
ST_X, ST_Y, ST_A, ST_P = 0x12, 0x13, 0x14, 0x15   # per-pass register/flag values
ST_ACC, ST_SP, ST_NMI = 0x1A, 0x1B, 0x1C
ST_IRQN, ST_ACK = 0x1D, 0x1E   # DMC IRQ handler entries; set when it acknowledges
PTR_INDX = 0x20        # (ind,X) operand; $20-$30 hold $05/$06 so any pointer is safe
PTR_INDY = 0xF0        # ($F0) = $05F8
ABS_BASE = 0x05F8      # abs / abs,X / abs,Y / (zp),Y base: crosses when index >= 8
JMPV = 0x0450          # JMP ($0450)
JMPV_BUG = 0x04FF      # JMP ($04FF): low byte at $04FF, high byte at $0400


class Asm:
    def __init__(self, org):
        self.org = org
        self.mem = bytearray([0xFF] * 0x8000)
        self.pc = org
        self.labels = {}
        self.fixups = []  # (kind, at, label)

    def byte(self, *bs):
        for b in bs:
            self.mem[self.pc - 0x8000] = b & 0xFF
            self.pc += 1

    def word(self, w):
        self.byte(w & 0xFF, w >> 8)

    def label(self, name):
        assert name not in self.labels, name
        self.labels[name] = self.pc

    def abs_ref(self, name):
        self.fixups.append(("abs", self.pc, name))
        self.byte(0, 0)

    def rel_ref(self, name):
        self.fixups.append(("rel", self.pc, name))
        self.byte(0)

    def lo_ref(self, name, minus=0):
        self.fixups.append(("lo", self.pc, (name, minus)))
        self.byte(0)

    def hi_ref(self, name, minus=0):
        self.fixups.append(("hi", self.pc, (name, minus)))
        self.byte(0)

    def jmp(self, name):
        self.byte(0x4C)
        self.abs_ref(name)

    def resolve(self):
        for kind, at, ref in self.fixups:
            i = at - 0x8000
            if kind == "abs":
                v = self.labels[ref]
                self.mem[i], self.mem[i + 1] = v & 0xFF, v >> 8
            elif kind == "rel":
                d = self.labels[ref] - (at + 1)
                assert -128 <= d <= 127, (ref, d)
                self.mem[i] = d & 0xFF
            else:
                name, minus = ref
                v = (self.labels[name] - minus) & 0xFFFF
                self.mem[i] = (v & 0xFF) if kind == "lo" else (v >> 8)


def build():
    a = Asm(0x8000)
    uid = [0]

    def fresh(stem):
        uid[0] += 1
        return f"{stem}_{uid[0]}"

    # -------- reset --------
    a.label("reset")
    a.byte(0x78, 0xD8)                    # SEI CLD
    a.byte(0xA2, 0xFF, 0x9A)              # LDX #$FF / TXS
    a.byte(0xA9, 0x00, 0x8D, 0x00, 0x20)  # LDA #0 / STA $2000
    a.byte(0x8D, 0x01, 0x20)              # STA $2001
    # clear RAM $0000-$07FF except the stack page
    a.byte(0xA2, 0x00)                    # LDX #0
    a.label("clr")
    a.byte(0x95, 0x00)                    # STA $00,X
    a.byte(0x9D, 0x00, 0x02)              # STA $0200,X
    a.byte(0x9D, 0x00, 0x03)              # STA $0300,X
    a.byte(0x9D, 0x00, 0x04)              # STA $0400,X
    a.byte(0x9D, 0x00, 0x05)              # STA $0500,X
    a.byte(0x9D, 0x00, 0x06)              # STA $0600,X
    a.byte(0x9D, 0x00, 0x07)              # STA $0700,X
    a.byte(0xE8, 0xD0); a.rel_ref("clr")  # INX / BNE clr
    # (ind,X) pointer bytes
    for i in range(0x11):
        a.byte(0xA9, 0x05 + (i & 1), 0x85, PTR_INDX + i)
    # ($F0) = ABS_BASE
    a.byte(0xA9, ABS_BASE & 0xFF, 0x85, PTR_INDY, 0xA9, ABS_BASE >> 8, 0x85, PTR_INDY + 1)
    # JMP vectors
    a.byte(0xA9); a.lo_ref("jmpv_target"); a.byte(0x8D); a.word(JMPV)
    a.byte(0xA9); a.hi_ref("jmpv_target"); a.byte(0x8D); a.word(JMPV + 1)
    a.byte(0xA9); a.lo_ref("jmpbug_target"); a.byte(0x8D); a.word(JMPV_BUG)
    a.byte(0xA9); a.hi_ref("jmpbug_target"); a.byte(0x8D); a.word(JMPV_BUG & 0xFF00)
    # APU: looping DMC at the fastest rate from $C000, frame IRQ enabled
    a.byte(0xA9, 0x4F, 0x8D, 0x10, 0x40)  # $4010 = loop | rate F
    a.byte(0xA9, 0x00, 0x8D, 0x12, 0x40)  # $4012 = $C000
    a.byte(0xA9, 0x00, 0x8D, 0x13, 0x40)  # $4013 = 1 byte
    a.byte(0xA9, 0x10, 0x8D, 0x15, 0x40)  # $4015 = DMC on
    a.byte(0xA9, 0x00, 0x8D, 0x17, 0x40)  # $4017 = mode 0, IRQ on
    # PPU: NMI on, rendering on
    a.byte(0xA9, 0x80, 0x8D, 0x00, 0x20)
    a.byte(0xA9, 0x1E, 0x8D, 0x01, 0x20)
    a.byte(0x58)                          # CLI
    a.jmp("main")

    # -------- test units --------
    def preamble(x_or=0, y_or=0, a_or=0):
        """Load A/X/Y/P from this pass's state without disturbing the flags."""
        a.byte(0xA5, ST_P, 0x48)          # LDA st_p / PHA
        a.byte(0xA5, ST_A)                # LDA st_a
        if a_or:
            a.byte(0x09, a_or)            # ORA #a_or
        a.byte(0xA6, ST_X)                # LDX st_x
        a.byte(0xA4, ST_Y)                # LDY st_y
        if x_or:
            a.byte(0x8A, 0x09, x_or, 0xAA)  # TXA / ORA / TAX (A reloaded below)
            a.byte(0xA5, ST_A)
            if a_or:
                a.byte(0x09, a_or)
        if y_or:
            a.byte(0x98, 0x09, y_or, 0xA8)  # TYA / ORA / TAY
            a.byte(0xA5, ST_A)
            if a_or:
                a.byte(0x09, a_or)
        a.byte(0x28)                      # PLP

    def operand(op, m, imm):
        if m == IMM:
            a.byte(op, imm)
        elif m in (ZP, ZPX, ZPY):
            a.byte(op, ZP_SCRATCH)
        elif m in (ABS, ABSX, ABSY):
            a.byte(op); a.word(ABS_BASE)
        elif m == INDX:
            a.byte(op, PTR_INDX)
        elif m == INDY:
            a.byte(op, PTR_INDY)
        elif m == IMP:
            a.byte(op)
        else:
            raise AssertionError(op)

    PREAMBLE_LEN = 10

    def align_page_offset(off):
        """Continue execution at the next address whose low byte is `off`."""
        target = (a.pc & 0xFF00) | off
        if target < a.pc:
            target += 0x100
        if target - a.pc <= 3:
            while a.pc < target:
                a.byte(0xEA)
        else:
            name = fresh("align")
            a.jmp(name)
            a.pc = target
            a.label(name)

    def branch_units(op):
        # in-page, forward over a 2-byte instruction
        preamble()
        skip = fresh("b")
        a.byte(op); a.rel_ref(skip)
        a.byte(0xA9, 0x77)                # LDA #$77 (skipped when taken)
        a.label(skip)

        # forward page crossing: branch at $xxFC, next $xxFE, target $(xx+1)00
        align_page_offset(0xFC - PREAMBLE_LEN)
        preamble()
        assert a.pc & 0xFF == 0xFC, hex(a.pc)
        skip = fresh("bx")
        a.byte(op); a.rel_ref(skip)
        a.byte(0xA9, 0x77)
        a.label(skip)
        assert a.pc & 0xFF == 0x00, hex(a.pc)

        # backward page crossing:
        #   $xxF0 preamble; JMP back
        #   $xxFD pad: JMP after        (taken target)
        #   $(xx+1)00 back: Bxx pad     (next $(xx+1)02)
        #   $(xx+1)02 after:
        pad, back, after = fresh("pad"), fresh("back"), fresh("after")
        align_page_offset(0xFD - PREAMBLE_LEN - 3)
        preamble()
        a.jmp(back)
        assert a.pc & 0xFF == 0xFD, hex(a.pc)
        a.label(pad)
        a.jmp(after)
        a.label(back)
        a.byte(op); a.rel_ref(pad)
        a.label(after)

    a.label("main")
    for op in range(256):
        m = mode_of(op)
        if m == HLT:
            continue
        imm = (op * 37 + 11) & 0xFF
        if m == REL:
            branch_units(op)
        elif op == 0x00:                  # BRK + padding byte, IRQ/BRK handler RTIs past it
            preamble()
            a.byte(0x00, 0xEA)
        elif op == 0x20:                  # JSR within this chunk and to another chunk
            over = fresh("over")
            a.jmp(over)
            a.label("sub_near")
            a.byte(0xC8, 0x60)            # INY / RTS
            a.label(over)
            preamble()
            a.byte(0x20); a.abs_ref("sub_near")
            a.byte(0x20); a.abs_ref("sub_far")
        elif op == 0x40:                  # RTI through a built frame
            ret = fresh("rti")
            preamble()
            a.byte(0xA9); a.hi_ref(ret); a.byte(0x48)
            a.byte(0xA9); a.lo_ref(ret); a.byte(0x48)
            a.byte(0xA5, ST_P, 0x48)
            a.byte(0xA5, ST_A)
            a.byte(0x40)
            a.label(ret)
        elif op == 0x60:                  # RTS through a pushed address
            ret = fresh("rts")
            preamble()
            a.byte(0xA9); a.hi_ref(ret, 1); a.byte(0x48)
            a.byte(0xA9); a.lo_ref(ret, 1); a.byte(0x48)
            a.byte(0xA5, ST_A)
            a.byte(0x60)
            a.label(ret)
        elif op == 0x4C:
            nxt = fresh("jmp")
            preamble()
            a.byte(0x4C); a.abs_ref(nxt)
            a.label(nxt)
        elif op == 0x6C:
            preamble()
            a.byte(0x6C); a.word(JMPV)
            a.label("jmpv_target")
            preamble()
            a.byte(0x6C); a.word(JMPV_BUG)
            a.label("jmpbug_target")
        elif op in (0x08, 0x48):          # PHP / PHA, then the matching pull
            preamble()
            a.byte(op)
            preamble()
            a.byte(op + 0x20)
        elif op in (0x28, 0x68):
            continue                      # emitted with PHP / PHA
        elif op == 0x78:                  # SEI, then CLI
            preamble()
            a.byte(0x78)
            preamble()
            a.byte(0x58)
        elif op == 0x58:
            continue
        elif op == 0x9A:                  # TXS of the real stack pointer
            preamble()
            a.byte(0xBA, 0x9A)
        elif op == 0x9B:                  # SHS: S = A & X. Keep S in $06-$0F
            a.byte(0xBA, 0x86, ST_SP)     # TSX / STX st_sp
            preamble(x_or=0x06, a_or=0x06)
            operand(op, m, imm)
            a.byte(0xA6, ST_SP, 0x9A)     # LDX st_sp / TXS
        elif op in (0x93, 0x9E, 0x9F):    # high byte masked by X: pages 2 or 6
            preamble(x_or=0x02)
            operand(op, m, imm)
        elif op == 0x9C:                  # high byte masked by Y
            preamble(y_or=0x02)
            operand(op, m, imm)
        elif op == 0xB1:
            preamble()
            operand(op, m, imm)
            # (zp),Y with the pointer straddling $FF/$00
            a.byte(0xA9, ABS_BASE & 0xFF, 0x85, 0xFF, 0xA9, ABS_BASE >> 8, 0x85, 0x00)
            preamble()
            a.byte(0xB1, 0xFF)
        else:
            preamble()
            operand(op, m, imm)

    # I/O reads racing the PPU/APU from compiled code
    a.byte(0x2C, 0x02, 0x20)              # BIT $2002 (also resets the $2005/$2006 latch)
    a.byte(0xAD, 0x07, 0x20)              # LDA $2007
    a.byte(0xAD, 0x15, 0x40)              # LDA $4015
    a.byte(0xAD, 0x16, 0x40)              # LDA $4016

    # PPU register workout from this pass's state
    a.byte(0xA5, ST_ACC, 0x29, 0x3F, 0x09, 0x80, 0x8D, 0x00, 0x20)  # $2000 = NMI | acc & $3F
    a.byte(0xA5, ST_A, 0x8D, 0x01, 0x20)                           # $2001 = st_a
    a.byte(0xA5, ST_X, 0x8D, 0x05, 0x20)                           # $2005 = st_x, st_y
    a.byte(0xA5, ST_Y, 0x8D, 0x05, 0x20)
    a.byte(0xA9, 0x3F, 0x8D, 0x06, 0x20)                           # $2006 = $3F, st_x
    a.byte(0xA5, ST_X, 0x8D, 0x06, 0x20)
    a.byte(0xA5, ST_A, 0x8D, 0x07, 0x20)                           # palette write
    a.byte(0xA5, ST_Y, 0x29, 0x03, 0x09, 0x20, 0x8D, 0x06, 0x20)   # $2006 = $20-$23, acc
    a.byte(0xA5, ST_ACC, 0x8D, 0x06, 0x20)
    a.byte(0xA5, ST_A, 0x8D, 0x07, 0x20)                           # nametable write
    a.byte(0xAD, 0x07, 0x20)                                       # buffered read
    a.byte(0xA5, ST_Y, 0x8D, 0x03, 0x20)                           # $2003 = st_y
    a.byte(0xA5, ST_ACC, 0x8D, 0x04, 0x20)                         # $2004 = acc
    a.byte(0xAD, 0x04, 0x20)                                       # LDA $2004

    # APU interrupts. IRQs masked for ~1000 cycles; the write right after CLI
    # polls on its own cycle, with a frame IRQ possibly pending.
    a.byte(0x78)                          # SEI
    a.byte(0xA2, 0xC8)                    # LDX #200
    a.label("masked")
    a.byte(0xCA, 0xD0); a.rel_ref("masked")                   # DEX / BNE masked
    a.byte(0xA5, ST_ACC, 0x4A, 0x90); a.rel_ref("w4010")      # LDA st_acc / LSR A / BCC w4010
    a.byte(0xA5, ST_A, 0x29, 0x10)        # LDA st_a / AND #$10: stop or keep the DMC
    a.byte(0x58, 0x8D, 0x15, 0x40)        # CLI / STA $4015
    a.jmp("w_done")
    a.label("w4010")
    a.byte(0xA9, 0x4F)                    # LDA #$4F (loop, rate F, DMC IRQ off)
    a.byte(0x58, 0x8D, 0x10, 0x40)        # CLI / STA $4010
    a.label("w_done")

    # The DMC IRQ: end the sample with its IRQ enabled, wait for the flag with
    # IRQs masked, then let the handler take it (it acknowledges on every 4th
    # entry and restores the loop).
    a.byte(0x78)                          # SEI
    a.byte(0xA9, 0x8F, 0x8D, 0x10, 0x40)  # $4010 = IRQ | rate F, no loop
    a.byte(0xA9, 0x10, 0x8D, 0x15, 0x40)  # $4015 = DMC on (restarts a stopped sample)
    a.byte(0xA2, 0x00)                    # LDX #0
    a.label("dmc_wait")
    a.byte(0xAD, 0x15, 0x40, 0x30); a.rel_ref("dmc_pending")  # LDA $4015 / BMI dmc_pending
    a.byte(0xCA, 0xD0); a.rel_ref("dmc_wait")                  # DEX / BNE dmc_wait
    a.label("dmc_pending")
    a.byte(0xA5, ST_ACC, 0x29, 0x38, 0xD0); a.rel_ref("no4017")  # LDA st_acc / AND #$38 / BNE
    a.byte(0xA9, 0x40, 0x8D, 0x17, 0x40)  # $4017 = frame IRQ inhibited, DMC IRQ pending
    a.byte(0xA9, 0x00, 0x8D, 0x17, 0x40)  # $4017 = mode 0, IRQ on
    a.label("no4017")
    a.byte(0xA9, 0x00, 0x85, ST_ACK)      # LDA #0 / STA st_ack
    a.byte(0x58)                          # CLI
    a.byte(0xA2, 0x00)                    # LDX #0 (a bound, not a delay)
    a.label("ack_wait")
    a.byte(0xA5, ST_ACK, 0xD0); a.rel_ref("acked")             # LDA st_ack / BNE acked
    a.byte(0xCA, 0xD0); a.rel_ref("ack_wait")                  # DEX / BNE ack_wait
    a.label("acked")

    # -------- per-pass state update and variable delay --------
    a.byte(0xA5, ST_X, 0x18, 0x69, 0x01, 0x29, 0x0F, 0x85, ST_X)
    a.byte(0xA5, ST_Y, 0x18, 0x69, 0x03, 0x29, 0x0F, 0x85, ST_Y)
    a.byte(0xA5, ST_A, 0x18, 0x69, 0x25, 0x85, ST_A)
    a.byte(0xA5, ST_ACC, 0x18, 0x69, 0x07, 0x85, ST_ACC, 0x29, 0xC3, 0x85, ST_P)
    a.byte(0xA6, ST_Y)                    # LDX st_y
    a.label("delay")
    a.byte(0xCA, 0x10); a.rel_ref("delay")  # DEX / BPL delay
    a.byte(0xA9, 0x05, 0x8D, 0x14, 0x40)  # OAM DMA from page 5
    a.byte(0x58)                          # CLI (belt and braces)
    a.jmp("main")

    # -------- handlers and subroutines (another recompiler chunk) --------
    assert a.pc <= 0xF000, hex(a.pc)
    a.pc = 0xF000
    a.label("nmi")
    a.byte(0x48, 0xE6, ST_NMI, 0x68, 0x40)          # PHA / INC / PLA / RTI
    a.label("irq")
    a.byte(0x48, 0xAD, 0x15, 0x40)                  # PHA / LDA $4015 (acknowledges a frame IRQ)
    a.byte(0x10); a.rel_ref("irq_done")             # BPL: no DMC IRQ
    a.byte(0xE6, ST_IRQN, 0xA5, ST_IRQN, 0x29, 0x03)  # INC st_irqn / LDA st_irqn / AND #3
    a.byte(0xD0); a.rel_ref("irq_done")             # BNE: leave it asserted
    a.byte(0xA9, 0x4F, 0x8D, 0x10, 0x40)            # $4010 = loop | rate F (clears the DMC IRQ)
    a.byte(0xA9, 0x10, 0x8D, 0x15, 0x40)            # $4015 = DMC on
    a.byte(0xE6, ST_ACK)                            # INC st_ack
    a.label("irq_done")
    a.byte(0x68, 0x40)                              # PLA / RTI
    a.label("sub_far")
    a.byte(0xE8, 0x60)                              # INX / RTS

    a.pc = 0xFFFA
    a.abs_ref("nmi")
    a.abs_ref("reset")
    a.abs_ref("irq")
    a.resolve()
    end = max(v for v in a.labels.values() if v < 0xF000)
    # Entry points only reachable through RTS/RTI/JMP-indirect.
    dynamic = sorted(v for k, v in a.labels.items()
                     if k.startswith(("rts_", "rti_", "jmpv_target", "jmpbug_target")))
    return bytes(a.mem), end, dynamic


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    out = sys.argv[1]
    prg, end, dynamic = build()
    header = bytes([0x4E, 0x45, 0x53, 0x1A, 2, 1, 0x01, 0x00]) + bytes(8)
    # Fixed pseudo-random pattern tables (a 32-bit LCG's high bytes).
    chr_rom = bytearray(0x2000)
    seed = 0x1234567
    for i in range(len(chr_rom)):
        seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
        chr_rom[i] = seed >> 24
    with open(out, "wb") as f:
        f.write(header + prg + chr_rom)
    seeds = (out[:-4] if out.lower().endswith(".nes") else out) + "_seeds.txt"
    with open(seeds, "w") as f:
        f.write("# dynamic entry points of the stress program (game.toml cycle_seed_file)\n")
        for v in dynamic:
            f.write(f"{v:04X}\n")
    print(f"wrote {out}: main loop $8000-${end:04X}; {len(dynamic)} dynamic entry points -> {seeds}")


if __name__ == "__main__":
    main()
