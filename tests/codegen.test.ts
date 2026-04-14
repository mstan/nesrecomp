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

  it("detects RTI-hijack in NMI handler and emits call to hijacked target", () => {
    // Minimal NMI handler that overwrites its hardware-pushed return PC
    // with a trampoline target, then RTIs.  Mirrors the shape of MM3's
    // $C000 PostNMI trampoline idiom.
    //
    // At NMI entry: stack top = [P, PCL, PCH] (hardware push).
    // TSX; STA $0102,X overwrites PCH slot, STA $0101,X overwrites PCL.
    // RTI then pops modified PC → control resumes at $C150.
    const rom = new RomBuilder()
      .org(0xc000) // RESET: do nothing
      .rts()
      .org(0xc080) // NMI handler
      .emit([0xba])              // TSX
      .lda(0xc1)                 // LDA #$C1 (PCH)
      .emit([0x9d, 0x02, 0x01])  // STA $0102,X
      .lda(0x50)                 // LDA #$50 (PCL)
      .emit([0x9d, 0x01, 0x01])  // STA $0101,X
      .rti()
      .org(0xc150) // the hijacked trampoline target
      .lda(0x99)
      .rts()
      .vectors(0xc080, 0xc000, 0xc000)
      .writeTemp("rti_hijack.nes");

    const result = recompile(rom);
    // The NMI handler's emitted body should contain a call to the hijack target.
    // The actual instructions live in func_C080_body (the outer func_C080 is a
    // dispatcher shim that just calls _body).
    const nmiMatch = result.fullC.match(/func_C080_body\([^)]*\)\s*\{[\s\S]*?\n\}/);
    expect(nmiMatch, "func_C080_body definition should be present").not.toBeNull();
    const nmiBody = nmiMatch![0];
    expect(nmiBody).toContain("RTI-hijack");
    expect(nmiBody).toContain("call_by_address(0xC150)");
  });

  it("function finder seeds RTI-hijack target when otherwise unreachable", () => {
    // Same shape as the RTI-hijack test, but the trampoline target at $C150
    // is NOT reachable via JSR/JMP/branch from anywhere else in the ROM.
    // The function finder must discover it via RTI-hijack pattern matching
    // during BFS, or it will never make it into the dispatch table.
    const rom = new RomBuilder()
      .org(0xc000) // RESET: does nothing (no JSR/JMP to $C150)
      .rts()
      .org(0xc080) // NMI handler with hijack
      .emit([0xba])              // TSX
      .lda(0xc1)
      .emit([0x9d, 0x02, 0x01])  // STA $0102,X (PCH)
      .lda(0x50)
      .emit([0x9d, 0x01, 0x01])  // STA $0101,X (PCL)
      .rti()
      .org(0xc150) // the hijacked target — NOT reached via JSR/JMP
      .lda(0x99)
      .rts()
      .vectors(0xc080, 0xc000, 0xc000)
      .writeTemp("rti_hijack_unreachable.nes");

    const result = recompile(rom);
    // $C150 must be discovered and dispatched, even though nothing else
    // references it in the ROM.
    expect(result.dispatchEntries).toContain("C150");
    // And the codegen emission for the NMI handler should still reference it.
    expect(result.fullC).toContain("call_by_address(0xC150)");
  });

  it("MMC3: cross-bank JSR to $8000 region resolved via R6 switch", () => {
    // 4-bank MMC3 ROM (banks 0-3; bank 3 is fixed).
    // RESET in fixed bank selects R6=4 (= 16KB bank 2), mode 0, then JSRs
    // into $8000 — target should be discovered at bank 2.
    const rom = new RomBuilder({ mapper: 4, prgBanks: 4 })
      .bank(3) // fixed bank
      .org(0xc000) // RESET
      // Select R6 (bank_select byte 0 → reg 6, mode 0)
      .lda(0x06).sta(0x8000)
      // Write R6 = 4 (8KB bank 4 = 16KB bank 2)
      .lda(0x04).sta(0x8001)
      // Select R7 (bank_select byte 7 → reg 7)
      .lda(0x07).sta(0x8000)
      // Write R7 = 6 (8KB bank 6 = 16KB bank 3 — but clamp doesn't matter here)
      .lda(0x06).sta(0x8001)
      // Now JSR into $8100 — should resolve to bank 2 (R6)
      .jsr(0x8100)
      .rts()
      .bank(2) // target bank
      .org(0x8100)
      .lda(0x99)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("mmc3_jsr_8000.nes");

    const result = recompile(rom);
    // func_8100_b2 should be emitted (bank 2 is the walk-detected R6).
    expect(result.fullC).toContain("func_8100_b2");
  });

  it("MMC3: cross-bank JSR to $C000 region in mode 1 resolved via R6", () => {
    // 4-bank MMC3 ROM. In mode 1 the $C000-$DFFF region is R6 (switchable),
    // which is the bug class that caused MM3's $DBE1 miss. Test that when a
    // function in the fixed bank switches to mode 1 with R6=4 (bank 2) and
    // then JSRs into $C100, the target is discovered at bank 2.
    const rom = new RomBuilder({ mapper: 4, prgBanks: 4 })
      .bank(3) // fixed bank
      .org(0xc000) // RESET
      // Select R6 with mode=1 (bit 6 set): $46 = reg 6 + mode 1
      .lda(0x46).sta(0x8000)
      .lda(0x04).sta(0x8001) // R6 = 4 (16KB bank 2)
      // JSR into $C100 — in mode 1 that's R6 = bank 2
      .jsr(0xc100)
      .rts()
      .bank(2) // target bank — body at $C100 because $C000-$DFFF is R6 in mode 1
      .org(0x8100) // NB: test-ROM source offset is still relative to the bank's
                   // base. Active bank 2's "natural" address space is $8000-$BFFF;
                   // we place the body at offset 0x100 so it lines up with $C100
                   // when R6 maps this bank into the $C000 slot at runtime.
      .lda(0x77)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("mmc3_jsr_c000_mode1.nes");

    const result = recompile(rom);
    // Target address is $C100. In mode 1 with R6=2 the resolver adds $C100
    // at bank 2. The finder log proves discovery even if the codegen later
    // filters/remaps the entry (codegen handling of mode-1 $C000+ addresses
    // from switchable banks is orthogonal to this spike).
    //
    // Without the cross-bank fix, the finder would not mention $C100 at
    // bank 2 at all.
    expect(result.output).toMatch(/\$C100 bank=2/);
    // At least one bank switch should have been detected during the walk.
    expect(result.output).toMatch(/Bank switches detected: [1-9]/);
  });

  it("emits bare RTI for non-NMI functions (no over-firing)", () => {
    // A non-NMI function that happens to do STA $01NN,X writes should NOT
    // be treated as RTI-hijack — guard is that func_base == rom->nmi_vector.
    // Here NMI = RESET = $C000 (doesn't contain STA,X writes), and the
    // IRQ handler at $C080 contains the writes + RTI but is not NMI.
    const rom = new RomBuilder()
      .org(0xc000) // RESET/NMI: plain NMI with RTI, no hijack
      .rti()
      .org(0xc080) // IRQ handler: has the shape of a hijack but is not NMI
      .emit([0xba])              // TSX
      .lda(0xc1)
      .emit([0x9d, 0x02, 0x01])  // STA $0102,X
      .lda(0x50)
      .emit([0x9d, 0x01, 0x01])  // STA $0101,X
      .rti()
      .vectors(0xc000, 0xc000, 0xc080) // NMI=$C000, IRQ=$C080
      .writeTemp("rti_no_hijack.nes");

    const result = recompile(rom);
    // No function anywhere in the generated output should have the hijack
    // marker or a call to the would-be trampoline target — the detector
    // only fires for functions whose entry address equals the NMI vector.
    expect(result.fullC).not.toContain("RTI-hijack");
    expect(result.fullC).not.toContain("call_by_address(0xC150)");
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
