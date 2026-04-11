/**
 * Minimal iNES ROM builder for testing NESRecomp.
 *
 * Builds NROM-128 ROMs (mapper 0, 1x16KB PRG mirrored, no CHR).
 * PRG lives at $C000-$FFFF with vectors at $FFFA.
 *
 * Usage:
 *   const rom = new RomBuilder()
 *     .org(0xC000)
 *     .emit([0xA9, 0x01])          // LDA #$01
 *     .jsr(0xC010)                 // JSR $C010
 *     .rts()
 *     .org(0xC010)
 *     .emit([0xA2, 0x00])          // LDX #$00
 *     .rts()
 *     .vectors(0xC000, 0xC000, 0xC000)
 *     .build();
 */
import { writeFileSync, mkdtempSync, mkdirSync } from "fs";
import { join } from "path";
import { tmpdir } from "os";

export class RomBuilder {
  private prg = new Uint8Array(16384); // 16KB
  private cursor = 0; // offset into prg (0 = $C000)

  /** Set the write cursor to an absolute 6502 address ($C000-$FFFF) */
  org(addr: number): this {
    if (addr < 0xc000 || addr > 0xffff)
      throw new Error(`org address $${addr.toString(16)} out of range $C000-$FFFF`);
    this.cursor = addr - 0xc000;
    return this;
  }

  /** Write raw bytes at the cursor */
  emit(bytes: number[]): this {
    for (const b of bytes) {
      this.prg[this.cursor++] = b & 0xff;
    }
    return this;
  }

  /** JSR absolute */
  jsr(addr: number): this {
    return this.emit([0x20, addr & 0xff, (addr >> 8) & 0xff]);
  }

  /** JMP absolute */
  jmp(addr: number): this {
    return this.emit([0x4c, addr & 0xff, (addr >> 8) & 0xff]);
  }

  /** RTS */
  rts(): this {
    return this.emit([0x60]);
  }

  /** RTI */
  rti(): this {
    return this.emit([0x40]);
  }

  /** LDA immediate */
  lda(val: number): this {
    return this.emit([0xa9, val & 0xff]);
  }

  /** LDX immediate */
  ldx(val: number): this {
    return this.emit([0xa2, val & 0xff]);
  }

  /** LDY immediate */
  ldy(val: number): this {
    return this.emit([0xa0, val & 0xff]);
  }

  /** PHA */
  pha(): this {
    return this.emit([0x48]);
  }

  /** STA absolute */
  sta(addr: number): this {
    return this.emit([0x8d, addr & 0xff, (addr >> 8) & 0xff]);
  }

  /** LDA absolute */
  ldaAbs(addr: number): this {
    return this.emit([0xad, addr & 0xff, (addr >> 8) & 0xff]);
  }

  /** BNE relative (target is absolute address, computed relative to cursor+2) */
  bne(target: number): this {
    const next = 0xc000 + this.cursor + 2;
    const offset = target - next;
    if (offset < -128 || offset > 127)
      throw new Error(`BNE target $${target.toString(16)} out of range from $${next.toString(16)}`);
    return this.emit([0xd0, offset & 0xff]);
  }

  /** DEX */
  dex(): this {
    return this.emit([0xca]);
  }

  /** NOP */
  nop(): this {
    return this.emit([0xea]);
  }

  /** Write a 16-bit LE value at cursor (for dispatch tables) */
  word(val: number): this {
    return this.emit([val & 0xff, (val >> 8) & 0xff]);
  }

  /** Set NMI, RESET, IRQ vectors at $FFFA-$FFFF */
  vectors(nmi: number, reset: number, irq: number): this {
    const save = this.cursor;
    this.cursor = 0x3ffa; // $FFFA
    this.word(nmi);
    this.word(reset);
    this.word(irq);
    this.cursor = save;
    return this;
  }

  /** Get current cursor as absolute address */
  get pc(): number {
    return 0xc000 + this.cursor;
  }

  /** Build the iNES ROM as a Buffer */
  toBuffer(): Buffer {
    // iNES header: NROM-128 (1x16KB PRG, 0 CHR, mapper 0, vertical mirroring)
    const header = Buffer.from([
      0x4e, 0x45, 0x53, 0x1a, // "NES\x1a"
      0x01,                     // 1x 16KB PRG
      0x00,                     // 0x 8KB CHR (CHR RAM)
      0x01,                     // flags6: vertical mirroring
      0x00,                     // flags7
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    ]);
    return Buffer.concat([header, Buffer.from(this.prg)]);
  }

  /** Write to a temp file and return the path */
  writeTemp(name = "test.nes"): string {
    const dir = mkdtempSync(join(tmpdir(), "nesrecomp-test-"));
    const path = join(dir, name);
    writeFileSync(path, this.toBuffer());
    return path;
  }
}
