/**
 * Minimal iNES ROM builder for testing NESRecomp.
 *
 * Default: NROM-128 (mapper 0, 1x16KB PRG mirrored, no CHR). PRG lives
 * at $C000-$FFFF with vectors at $FFFA.
 *
 * For MMC3 (mapper 4) tests, pass `{ mapper: 4, prgBanks: N }` to the
 * constructor. PRG is stored as an array of N 16KB banks; the last bank
 * is the "fixed" bank exposed at $E000 (and $C000 in mode 0). `bank(n)`
 * sets the active bank for subsequent writes; `org()` within a bank
 * treats addresses $8000-$BFFF for switchable banks and $C000-$FFFF for
 * the fixed bank.
 */
import { writeFileSync, mkdtempSync, mkdirSync } from "fs";
import { join } from "path";
import { tmpdir } from "os";

interface RomBuilderOpts {
  mapper?: number;   // 0 or 4 (MMC3). Default 0.
  prgBanks?: number; // 16KB banks. Default 1 (mapper 0) or 2 (mapper 4).
}

export class RomBuilder {
  private prgBanks: Uint8Array[]; // array of 16KB banks
  private mapper: number;
  private activeBank: number = 0;
  private cursor = 0; // offset into the active bank (0 = $8000 for switchable, 0 = $C000 for fixed)

  constructor(opts: RomBuilderOpts = {}) {
    this.mapper = opts.mapper ?? 0;
    const defaultBanks = this.mapper === 4 ? 2 : 1;
    const n = opts.prgBanks ?? defaultBanks;
    this.prgBanks = Array.from({ length: n }, () => new Uint8Array(16384));
    // Default active bank: last (fixed) bank for mapper 4, bank 0 for mapper 0
    this.activeBank = this.mapper === 4 ? n - 1 : 0;
  }

  /** Set which 16KB PRG bank subsequent writes target. */
  bank(n: number): this {
    if (n < 0 || n >= this.prgBanks.length)
      throw new Error(`bank ${n} out of range 0-${this.prgBanks.length - 1}`);
    this.activeBank = n;
    this.cursor = 0;
    return this;
  }

  /** Set the write cursor to an absolute 6502 address.
   *  For the fixed/last bank: valid range $C000-$FFFF.
   *  For any other bank: valid range $8000-$BFFF. */
  org(addr: number): this {
    const isFixed = this.activeBank === this.prgBanks.length - 1;
    if (isFixed && this.mapper === 0) {
      // NROM-128 legacy: $C000-$FFFF
      if (addr < 0xc000 || addr > 0xffff)
        throw new Error(`org address $${addr.toString(16)} out of range $C000-$FFFF`);
      this.cursor = addr - 0xc000;
    } else if (isFixed) {
      // Fixed bank in multi-bank ROM: $C000-$FFFF
      if (addr < 0xc000 || addr > 0xffff)
        throw new Error(`org address $${addr.toString(16)} out of range $C000-$FFFF for fixed bank`);
      this.cursor = addr - 0xc000;
    } else {
      // Switchable bank: $8000-$BFFF
      if (addr < 0x8000 || addr > 0xbfff)
        throw new Error(`org address $${addr.toString(16)} out of range $8000-$BFFF for switchable bank`);
      this.cursor = addr - 0x8000;
    }
    return this;
  }

  private get prg(): Uint8Array {
    return this.prgBanks[this.activeBank];
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

  /** Set NMI, RESET, IRQ vectors at $FFFA-$FFFF (always in the fixed/last bank). */
  vectors(nmi: number, reset: number, irq: number): this {
    const saveBank = this.activeBank;
    const saveCursor = this.cursor;
    this.activeBank = this.prgBanks.length - 1;
    this.cursor = 0x3ffa; // $FFFA offset within 16KB bank
    this.word(nmi);
    this.word(reset);
    this.word(irq);
    this.activeBank = saveBank;
    this.cursor = saveCursor;
    return this;
  }

  /** Get current cursor as absolute address */
  get pc(): number {
    const isFixed = this.activeBank === this.prgBanks.length - 1;
    return isFixed ? 0xc000 + this.cursor : 0x8000 + this.cursor;
  }

  /** Build the iNES ROM as a Buffer */
  toBuffer(): Buffer {
    const prgBanksByte = this.prgBanks.length;
    const mapperLo = (this.mapper & 0x0f) << 4;
    const header = Buffer.from([
      0x4e, 0x45, 0x53, 0x1a, // "NES\x1a"
      prgBanksByte,             // N x 16KB PRG
      0x00,                     // 0x 8KB CHR (CHR RAM)
      0x01 | mapperLo,          // flags6: vertical mirroring + mapper low nibble
      0x00,                     // flags7
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    ]);
    return Buffer.concat([header, ...this.prgBanks.map((b) => Buffer.from(b))]);
  }

  /** Write to a temp file and return the path */
  writeTemp(name = "test.nes"): string {
    const dir = mkdtempSync(join(tmpdir(), "nesrecomp-test-"));
    const path = join(dir, name);
    writeFileSync(path, this.toBuffer());
    return path;
  }
}
