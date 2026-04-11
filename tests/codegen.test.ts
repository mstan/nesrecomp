/**
 * NESRecomp codegen tests using synthetic ROMs.
 *
 * Each test constructs a minimal iNES ROM exercising a specific 6502 pattern,
 * runs NESRecomp, and verifies the function finder and code generator produce
 * correct output.
 */
import { describe, it, expect } from "vitest";
import { RomBuilder } from "./helpers/rom-builder.js";
import { recompile } from "./helpers/recompile.js";

describe("function discovery", () => {
  it("finds RESET vector entry point", () => {
    // Simplest possible ROM: RESET → RTS
    const rom = new RomBuilder()
      .org(0xc000)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("reset_only.nes");

    const result = recompile(rom);
    expect(result.functionCount).toBeGreaterThan(0);
    expect(result.dispatchEntries).toContain("C000");
  });

  it("discovers JSR targets", () => {
    // RESET calls two subroutines via JSR
    const rom = new RomBuilder()
      .org(0xc000) // RESET
      .jsr(0xc010)
      .jsr(0xc020)
      .rts()
      .org(0xc010)
      .lda(0x42)
      .rts()
      .org(0xc020)
      .ldx(0x00)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("jsr_targets.nes");

    const result = recompile(rom);
    expect(result.dispatchEntries).toContain("C000");
    expect(result.dispatchEntries).toContain("C010");
    expect(result.dispatchEntries).toContain("C020");
  });

  it("discovers separate NMI and RESET entry points", () => {
    const rom = new RomBuilder()
      .org(0xc000) // RESET
      .nop()
      .rts()
      .org(0xc080) // NMI
      .lda(0x01)
      .rti()
      .vectors(0xc080, 0xc000, 0xc000)
      .writeTemp("nmi_reset.nes");

    const result = recompile(rom);
    expect(result.dispatchEntries).toContain("C000");
    expect(result.dispatchEntries).toContain("C080");
  });

  it("follows JSR chains (A → B → C)", () => {
    const rom = new RomBuilder()
      .org(0xc000) // A
      .jsr(0xc010)
      .rts()
      .org(0xc010) // B
      .jsr(0xc020)
      .rts()
      .org(0xc020) // C
      .lda(0xff)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("jsr_chain.nes");

    const result = recompile(rom);
    expect(result.dispatchEntries).toContain("C000");
    expect(result.dispatchEntries).toContain("C010");
    expect(result.dispatchEntries).toContain("C020");
  });

  it("discovers JMP tail-call targets", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .lda(0x01)
      .jmp(0xc010) // tail call
      .org(0xc010)
      .ldx(0x02)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("jmp_tailcall.nes");

    const result = recompile(rom);
    expect(result.dispatchEntries).toContain("C000");
    expect(result.dispatchEntries).toContain("C010");
  });
});

describe("code generation", () => {
  it("generates compilable C for simple functions", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .jsr(0xc010)
      .rts()
      .org(0xc010)
      .lda(0x42)
      .sta(0x0200)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("simple_codegen.nes");

    const result = recompile(rom);

    // Generated code should contain function definitions
    expect(result.fullC).toContain("func_C000");
    expect(result.fullC).toContain("func_C010");
    // Should contain the store to $0200
    expect(result.fullC).toContain("0x0200");
  });

  it("generates dispatch table entries for all discovered functions", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .jsr(0xc010)
      .jsr(0xc020)
      .jsr(0xc030)
      .rts()
      .org(0xc010)
      .rts()
      .org(0xc020)
      .rts()
      .org(0xc030)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("dispatch_entries.nes");

    const result = recompile(rom);

    // Every discovered function should have a dispatch entry
    expect(result.dispatchEntries).toContain("C000");
    expect(result.dispatchEntries).toContain("C010");
    expect(result.dispatchEntries).toContain("C020");
    expect(result.dispatchEntries).toContain("C030");

    // Dispatch table should have case statements
    expect(result.dispatchC).toContain("case 0xC000:");
    expect(result.dispatchC).toContain("case 0xC010:");
  });
});

describe("known dispatch tables", () => {
  it("discovers RTS-as-JMP dispatch table targets", () => {
    // Build a dispatch table: 3 entries, each is (target-1) in LE16
    // Targets: $C040, $C050, $C060 → stored as $C03F, $C04F, $C05F
    const rom = new RomBuilder()
      .org(0xc000) // RESET: do nothing
      .rts()
      .org(0xc010) // dispatch table (6 bytes, 3 entries)
      .word(0xc03f) // target $C040 - 1
      .word(0xc04f) // target $C050 - 1
      .word(0xc05f) // target $C060 - 1
      .org(0xc040)
      .lda(0x01)
      .rts()
      .org(0xc050)
      .lda(0x02)
      .rts()
      .org(0xc060)
      .lda(0x03)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("dispatch_table.nes");

    const toml = `
[game]
output_prefix = "test"

[[known_table]]
bank = 0
start = 0xC010
end = 0xC016
`;

    const result = recompile(rom, toml);
    expect(result.dispatchEntries).toContain("C040");
    expect(result.dispatchEntries).toContain("C050");
    expect(result.dispatchEntries).toContain("C060");
  });
});

describe("branch handling", () => {
  it("discovers code past forward branches", () => {
    // Function with a forward BNE that skips over a JSR
    // The JSR target should still be discovered via the fall-through path
    const rom = new RomBuilder()
      .org(0xc000)
      .ldx(0x05)
      .org(0xc002); // cursor now at $C002

    // BNE to $C008 (skip the JSR)
    const builder = rom
      .bne(0xc008)
      .jsr(0xc020) // only reached on fall-through
      .org(0xc008) // branch lands here
      .rts()
      .org(0xc020)
      .lda(0xff)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("forward_branch.nes");

    const result = recompile(builder);
    expect(result.dispatchEntries).toContain("C000");
    expect(result.dispatchEntries).toContain("C020");
  });

  it("generates loop code for backward branches", () => {
    // Simple loop: LDX #5 / DEX / BNE back
    const rom = new RomBuilder()
      .org(0xc000)
      .ldx(0x05); // $C000: LDX #$05

    const loopTop = rom.pc; // $C002
    const builder = rom
      .dex() // $C002: DEX
      .bne(loopTop) // $C003: BNE $C002
      .rts() // $C005: RTS
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("backward_branch.nes");

    const result = recompile(builder);
    expect(result.dispatchEntries).toContain("C000");
    // The generated code should contain a goto or loop construct
    expect(result.fullC).toContain("label_C002");
  });
});
