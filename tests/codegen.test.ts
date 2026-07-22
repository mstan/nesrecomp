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

  it("Tier-1 unofficial: \\$EB SBC #imm alias generates SBC code", () => {
    // Function uses \$EB, which the audit identified as the SBC #imm
    // alias. Codegen should emit the same SBC scaffold as \$E9.
    const rom = new RomBuilder()
      .org(0xc000)
      .lda(0x10)
      .sbcAltImm(0x05) // \$EB \$05
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("tier1_sbc_alt.nes");

    const result = recompile(rom);
    expect(result.dispatchEntries).toContain("C000");
    // Body must contain SBC subtraction (FLAG_NZC_SUB) — the same
    // generator helper used for \$E9. Treating \$EB as ILLEGAL (skip)
    // would not produce this text.
    expect(result.fullC).toMatch(/m=0x05.*FLAG_NZC_SUB/);
  });

  it("Tier-1 unofficial: \\$1A 1-byte NOP doesn't break discovery and emits NOP", () => {
    // Function with \$1A (unofficial NOP) followed by real code. Pre-Phase-3
    // the function finder rejected any function whose first 7 instructions
    // contained \$1A (validate_code_target's MN_ILLEGAL check); now it's
    // MN_NOP and the function is accepted.
    const rom = new RomBuilder()
      .org(0xc000)
      .emit([0x1a]) // \$1A — 1-byte unofficial NOP
      .lda(0x42)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("tier1_nop1A.nes");

    const result = recompile(rom);
    expect(result.dispatchEntries).toContain("C000");
    expect(result.fullC).toContain("/* NOP */");
    expect(result.fullC).toContain("0x42");
  });

  it("Tier-1 unofficial: \\$0C TOP abs emits a discarded read for bus accuracy", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .topAbs(0x2002) // TOP \$2002 — must perform the read so the
                     //               PPUSTATUS latch reset semantic is preserved
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("tier1_top_abs.nes");

    const result = recompile(rom);
    // The dummy read is emitted as (void)nes_read(0x2002) — without it,
    // bus side effects on memory-mapped I/O would be lost.
    expect(result.fullC).toMatch(/\(void\)nes_read\(0x2002\)/);
  });

  it("Tier-1 unofficial: SAX writes A & X to memory", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .lda(0xF0)       // A = \$F0
      .ldx(0x0F)       // X = \$0F
      .saxAbs(0x0200)  // [\$0200] = A & X = \$00
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("tier1_sax_abs.nes");

    const result = recompile(rom);
    expect(result.dispatchEntries).toContain("C000");
    // SAX must compute A & X and store the result without touching flags.
    expect(result.fullC).toMatch(/nes_write\(0x0200,\s*g_cpu\.A\s*&\s*g_cpu\.X\)/);
  });

  it("JMP (\\$xxFF) page-wrap: static vector resolution uses same-page hi byte", () => {
    // NMOS 6502 erratum: JMP ($DEFF) reads lo from $DEFF, hi from $DE00,
    // NOT from $DF00. The function finder must apply the same wrap as the
    // runtime helper (nes_read16_jmpbug), or the statically-resolved target
    // will be different from the runtime target — code is generated for the
    // wrong address and the dispatch table never sees the real target.
    //
    // Layout:
    //   $C000  JMP ($DEFF)            — exercises the erratum
    //   $DEFF  $50                    — lo byte of vector
    //   $DE00  $E1                    — hi byte the wrap-correct fetch reads
    //   $DF00  $00                    — hi byte the BUGGY (addr+1) fetch would read
    //   $E150  RTS                    — the wrap-correct target body
    //
    // With the fix:  target = $E150, $E150 appears in dispatchEntries.
    // Without the fix: target = $0050, < $8000, finder skips it, $E150 absent.
    const rom = new RomBuilder()
      .org(0xc000)
      .jmpInd(0xdeff)
      .poke(0xdeff, 0x50)
      .poke(0xde00, 0xe1)
      .poke(0xdf00, 0x00)
      .org(0xe150)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("jmp_indirect_xxff_pagewrap.nes");

    const result = recompile(rom);
    // Static vector resolution must have used wrap-correct hi byte.
    expect(result.dispatchEntries).toContain("E150");
    // Codegen must use the bug-modeling helper, not plain nes_read16,
    // so JMP ($DEFF) at runtime also reads the correct page.
    expect(result.fullC).toContain("nes_read16_jmpbug(0xDEFF)");
    expect(result.fullC).not.toContain("nes_read16(0xDEFF)");
  });
});

describe("code generation", () => {
  it("lowers a fixed-bank scheduler handoff to coroutine_yield", () => {
    // A task saves its 6502 stack pointer, then jumps to a fixed-bank
    // scheduler loop. The jump must preserve the task's C continuation so
    // the scheduler RESUME path can return to the instruction after its JSR.
    const rom = new RomBuilder()
      .org(0xc000)
      .emit([0xba, 0x96, 0x82, 0x4c, 0x10, 0xc0]) // TSX; STX $82,Y; JMP $C010
      .org(0xc010)
      .emit([0xa2, 0xff, 0x9a]) // LDX #$FF; TXS scheduler loop entry
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("fixed_bank_scheduler_handoff.nes");

    const result = recompile(rom);
    expect(result.fullC).toContain("coroutine_yield(); return;");
    expect(result.fullC).not.toContain("call_by_address_tail(0xC010, -1)");
  });

  it("assigns merge-range aliases only to their canonical body", () => {
    // C000 and C010 both branch into the C020-C022 range, so permissive
    // boundary scans see C022 inside all three bodies. Only the canonical
    // range body may publish the C022 wrapper.
    const rom = new RomBuilder()
      .org(0xc000)
      .jsr(0xc010)
      .jsr(0xc020)
      .bne(0xc020)
      .rts()
      .org(0xc010)
      .jsr(0xc022)
      .bne(0xc020)
      .rts()
      .org(0xc020)
      .nop()
      .nop()
      .lda(0x42)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("merge_range_alias_owner.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "merge-range-owner"\n\n[[merge_range]]\nbank = 0\naddr_lo = 0xC020\naddr_hi = 0xC022\n`
    );

    expect(result.fullC.match(/void func_C022\(void\)/g)).toHaveLength(1);
    expect(result.fullC).toMatch(
      /void func_C022\(void\) \{[\s\S]*?func_C020_body\(2\)/
    );
  });

  it("publishes explicit labels inside private merge ranges", () => {
    const rom = new RomBuilder({ mapper: 4, prgBanks: 4 })
      .bank(3)
      .org(0xc000)
      .jsr(0x8000)
      .jsr(0x8002)
      .rts()
      .bank(0)
      .org(0x8000)
      .nop()
      .nop()
      .lda(0x42)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("private_merge_range_explicit_label.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "private-range-label"\n\n[[merge_range]]\nbank = 0\naddr_lo = 0x8000\naddr_hi = 0x8002\npublic = false\n\n[[extra_label]]\nbank = 0\naddr = 0x8002\n`
    );

    expect(result.dispatchEntries).toContain("8002");
    expect(result.fullC.match(/void func_8002_b0\(void\)/g)).toHaveLength(1);
    expect(result.fullC).toMatch(
      /void func_8002_b0\(void\) \{[\s\S]*?func_8000_b0_body\([0-9]+\)/
    );
  });

  it("publishes private merge-range labels after a terminating jump", () => {
    const rom = new RomBuilder({ mapper: 4, prgBanks: 4 })
      .bank(3)
      .org(0xc000)
      .jsr(0x8000)
      .jsr(0x8003)
      .rts()
      .bank(0)
      .org(0x8000)
      .jmp(0x8100)
      .org(0x8003)
      .lda(0x42)
      .rts()
      .org(0x8100)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("private_merge_range_label_after_jmp.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "private-range-label-jmp"\n\n[[merge_range]]\nbank = 0\naddr_lo = 0x8000\naddr_hi = 0x8003\npublic = false\n\n[[extra_label]]\nbank = 0\naddr = 0x8003\n`
    );

    expect(result.dispatchEntries).toContain("8003");
    expect(result.fullC).toMatch(
      /void func_8003_b0\(void\) \{[\s\S]*?func_8000_b0_body\([0-9]+\)/
    );
  });

  it("prioritizes manual contracts before auto secondaries", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .jsr(0xc020)
      .rts()
      .org(0xc020)
      .nop()
      .nop()
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("manual_contract_before_auto_secondaries.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "manual-priority"\n\n[functions]\nfixed = [0xC022]\n`
    );

    const wrapper = result.fullC.match(
      /void func_C022\(void\) \{[\s\S]*?^\}/m
    );
    expect(wrapper).not.toBeNull();
    expect(wrapper![0]).toContain("func_C020_body(1)");
    expect(wrapper![0]).not.toContain("nes_interp_dispatch_bank");
  });

  it("does not assign skipped post-JMP bytes to an enclosing body", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .bne(0xc014)
      .jmp(0xc020)
      .emit(new Array(0x0f).fill(0xea))
      .rts()
      .org(0xc020)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("manual_entry_after_jmp.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "manual-after-jmp"\n\n[functions]\nfixed = [0xC008]\n`
    );

    const directBody = result.fullC.match(
      /void func_C008\(void\) \{[\s\S]*?^\}/m
    );
    expect(directBody).not.toBeNull();
    expect(directBody![0]).toContain("label_C008");
    expect(directBody![0]).not.toContain("nes_interp_dispatch_bank");
    expect(directBody![0]).not.toMatch(/func_[0-9A-F]+_body\(/);
  });

  it("charges only the taken and page-cross branch cycle deltas", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .jsr(0xc020)
      .jsr(0xc0f0)
      .rts()
      .org(0xc020)
      .bne(0xc028)
      .emit(new Array(6).fill(0xea))
      .rts()
      .org(0xc0f0)
      .emit(new Array(13).fill(0xea))
      .bne(0xc105)
      .emit(new Array(6).fill(0xea))
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("branch_cycle_deltas.nes");

    const result = recompile(rom);

    expect(result.fullC).toMatch(
      /\$C020: D0 \*\/ maybe_trigger_vblank\(2\); if \(!g_cpu\.Z\) \{ maybe_trigger_vblank\(1\); goto label_C028; \}/
    );
    expect(result.fullC).toMatch(
      /\$C0FD: D0 \*\/ maybe_trigger_vblank\(2\); if \(!g_cpu\.Z\) \{ maybe_trigger_vblank\(2\); goto label_C105; \}/
    );
  });

  it("dispatches backward branches whose local label was never emitted", () => {
    // The branch into the middle of the JSR leaves a stale pre-scan target.
    // That keeps C008-C020 in valid_starts, while the manual C020 secondary
    // makes the emitter jump directly from C005 to C020. C020's backward
    // branch must dispatch to C008 instead of targeting a return-only label.
    const rom = new RomBuilder()
      .org(0xc000)
      .bne(0xc003)
      .jsr(0xc100)
      .jmp(0xc100)
      .org(0xc008)
      .emit(new Array(0x18).fill(0xea))
      .org(0xc020)
      .bne(0xc008)
      .rts()
      .org(0xc100)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("unemitted_backward_branch_label.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "unemitted-backward"\n\n[[extra_label]]\nbank = 0\naddr = 0xC020\n`
    );

    expect(result.fullC).toMatch(
      /\$C020: D0 \*\/ maybe_trigger_vblank\(2\); if \(!g_cpu\.Z\) \{ maybe_trigger_vblank\(1\); call_by_address\(0xC008\); return; \}/
    );
  });

  it("publishes nested manual entries from the ultimate emitted body", () => {
    // C008 is a manual entry inside the C000 body. C010 is also manual and
    // lies inside both C000 and C008, so nearest-owner selection initially
    // produces C010 -> C008 -> C000. Both public contracts must be emitted
    // by the ultimate standalone body at C000.
    const rom = new RomBuilder()
      .org(0xc000)
      .emit(new Array(0x20).fill(0xea))
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("nested_manual_entries.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "nested-manual"\n\n[functions]\nfixed = [0xC008, 0xC010]\n`
    );

    expect(result.dispatchEntries).toContain("C008");
    expect(result.dispatchEntries).toContain("C010");
    expect(result.fullC).toContain("void func_C008(void)");
    expect(result.fullC).toContain("void func_C010(void)");
  });

  it("publishes automatic secondaries owned by a wrapper-only manual entry", () => {
    // C00F is a real RTS branch target. The stronger manual C010 entry owns it
    // structurally, while C010 is itself emitted as a wrapper into the broad
    // pointer-discovered C000 body. C000 must publish both public entries.
    const rom = new RomBuilder()
      .org(0xc000)
      .bne(0xc020)
      .emit(new Array(13).fill(0xea))
      .rts()
      .org(0xc010)
      .lda(0x00)
      .emit([0xf0, 0xfb])
      .emit(new Array(12).fill(0xea))
      .org(0xc020)
      .rts()
      .org(0xe000)
      .jsr(0xc00f)
      .rts()
      .poke(0xe100, 0x00)
      .poke(0xe101, 0xc0)
      .vectors(0xe000, 0xe000, 0xe000)
      .writeTemp("nested_auto_secondary_owner.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "nested-auto-owner"\n\n[functions]\nfixed = [0xC010]\n`
    );

    expect(result.dispatchEntries).toContain("C00F");
    expect(result.fullC).toContain("void func_C00F(void)");
    expect(result.fullC).not.toContain(
      "nes_interp_dispatch_bank(0xC00F, 0xC00F"
    );
  });

  it("dispatches an adjacent two-PHA indirect-call continuation across function bodies", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .emit([0xa9, 0xc0, 0x48, 0xa9, 0x10, 0x48, 0x6c, 0x00, 0x00])
      .org(0xc011)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("two_pha_indirect_continuation.nes");

    const result = recompile(rom);

    expect(result.fullC).toContain("call_by_address_tail(0xC011, 0)");
    expect(result.fullC).not.toContain("goto label_C011");
  });

  it("publishes a branch target that overlaps the fallthrough instruction", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .emit([0x90, 0x02, 0xad, 0x00, 0x60, 0x60])
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("overlapping_branch_target.nes");

    const result = recompile(rom);

    expect(result.dispatchEntries).toContain("C004");
    expect(result.fullC).toContain("void func_C004(void)");
  });

  it("keeps nested manual entries standalone when the final owner cannot publish them", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .emit(new Array(0x800).fill(0xea))
      .org(0xc800)
      .bne(0xc803)
      .rts()
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("nested_manual_beyond_owner_cap.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "nested-manual-cap"\n\n[functions]\nfixed = [0xC7FF, 0xC803]\n`
    );

    expect(result.dispatchEntries).toContain("C7FF");
    expect(result.dispatchEntries).toContain("C803");
    expect(result.fullC).toContain("void func_C803(void) {");
  });

  it("keeps a manual bare RTS independent from an enclosing PHA dispatch", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .emit([0x48, 0x48])
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("manual_bare_rts.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "manual-bare-rts"\n\n[functions]\nfixed = [0xC002]\n`
    );

    const parentBody = result.fullC.match(/void func_C000_body\(int _entry\) \{[\s\S]*?\n\}/);
    const directBody = result.fullC.match(/void func_C002\(void\) \{[\s\S]*?\n\}/);
    expect(parentBody).not.toBeNull();
    expect(parentBody![0]).toContain("call_by_address_tail");
    expect(directBody).not.toBeNull();
    expect(directBody![0]).not.toContain("call_by_address_tail");
  });

  it("does not inherit unmatched PHA state through a manual entry", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .emit([0x48, 0x48, 0xd0, 0x06, 0xea, 0xea, 0xea, 0xea])
      .emit([0xa4, 0x00, 0x60])
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("manual_entry_pha_state.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "manual-entry-pha"\n\n[functions]\nfixed = [0xC004, 0xC008]\n`
    );

    const outerBody = result.fullC.match(/void func_C000(?:_body\(int _entry\)|\(void\)) \{[\s\S]*?\n\}/);
    const directBody = result.fullC.match(/void func_C008(?:_body\(int _entry\)|\(void\)) \{[\s\S]*?\n\}/);
    expect(outerBody).not.toBeNull();
    expect(outerBody![0]).toContain("call_by_address_tail");
    expect(directBody).not.toBeNull();
    expect(directBody![0]).not.toContain("call_by_address_tail");
  });

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

  it("captures an RTI-hijacked NMI target for post-NMI dispatch", () => {
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
    // RTI captures the target from the emulated stack. The NMI wrapper then
    // dispatches that dynamic target after the handler returns.
    const nmiMatch = result.fullC.match(/func_C080_body\([^)]*\)\s*\{[\s\S]*?\n\}/);
    expect(nmiMatch, "func_C080_body definition should be present").not.toBeNull();
    const nmiBody = nmiMatch![0];
    expect(nmiBody).toContain("g_rti_target =");
    expect(result.fullC).toContain("call_by_address(g_rti_target)");
    expect(result.fullC).toContain(
      "g_cpu.I = 1; /* NMI entry sets I after pushing the old P */"
    );
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
    // The NMI wrapper dispatches the stack-derived target dynamically.
    expect(result.fullC).toContain("call_by_address(g_rti_target)");
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

  it("Mapper 40 normalizes fixed and switchable 8KB CPU windows", () => {
    const builder = new RomBuilder({ mapper: 40, prgBanks: 4 })
      .bank8(7)
      .org(0xa100)
      .jsr(0x6000)
      .jsr(0xc200)
      .rts()
      .bank8(6)
      .org(0x8000)
      .rts();

    for (let bank8 = 0; bank8 < 8; bank8++) {
      builder
        .bank8(bank8)
        .org((bank8 & 1) ? 0xa200 : 0x8200)
        .rts();
    }

    const rom = builder
      .vectors(0xe100, 0xe100, 0xe100)
      .writeTemp("mapper40_windows.nes");
    const result = recompile(
      rom,
      `[game]\noutput_prefix = "mapper40-windows"\npush_all_jsr = true\n`
    );

    expect(result.fullC).toContain("void func_A100_b3(void)");
    expect(result.fullC).toContain("void func_8000_b3(void)");
    expect(result.fullC).toContain("func_A100_b3()");
    expect(result.fullC).toContain("nes_dispatch_call(0x6000");
    expect(result.fullC).toContain("g_code_window_base | 0x0102");
    expect(result.dispatchEntries).toContain("8200");
    expect(result.dispatchEntries).toContain("A200");
    expect(result.dispatchC).toContain("g_mapper40_bank_c000_8k");
    expect(result.dispatchC).toContain("if (addr < 0x6000)");
  });

  it("Mapper 40 discovers per-bank indirect vector targets", () => {
    const rom = new RomBuilder({ mapper: 40, prgBanks: 4 })
      .bank8(0)
      .org(0x9ffa)
      .word(0x60a0)
      .word(0x60a0)
      .word(0x60a0)
      .bank8(6)
      .org(0x80a0)
      .rti()
      .bank8(7)
      .org(0xa100)
      .rti()
      .vectors(0xe100, 0xe100, 0xe100)
      .writeTemp("mapper40_bank_vectors.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "mapper40-bank-vectors"\npush_all_jsr = true\n`
    );

    expect(result.fullC).toContain("void func_80A0_b3(void)");
    expect(result.dispatchC).toMatch(
      /case 0x80A0:\r?\n\s*func_80A0_b3\(\); break;/
    );
  });

  it("Mapper 40 interprets instructions that straddle an 8KB PRG boundary", () => {
    const rom = new RomBuilder({ mapper: 40, prgBanks: 4 })
      .bank8(7)
      .org(0xa100)
      .jmp(0xbffd)
      .bank8(5)
      .org(0xbffd)
      .lda(0x01)
      .emit([0x8d])
      .bank8(0)
      .org(0x8000)
      .emit([0x70, 0x07])
      .rts()
      .bank8(7)
      .vectors(0xe100, 0xe100, 0xe100)
      .writeTemp("mapper40_boundary_fetch.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "mapper40-boundary"\npush_all_jsr = true\n`
    );

    expect(result.fullC).toContain("label_BFFF:");
    expect(result.fullC).toContain(
      "nes_interp_step_tail((uint16_t)(g_code_window_base | 0x1FFF), 2)"
    );
    expect(result.fullC).not.toContain("nes_write(0x5FAD");
    expect(result.dispatchC).toMatch(/case 0x8002:[\s\S]*func_8002_b0/);
  });

  it("Mapper 40 keeps generated 8KB boundary entries out of cross-window bodies", () => {
    // $9FFF is the end of the fixed $8000 window and $A000 begins the
    // separate fixed $A000 window. The fallthrough discovers $A000, but it
    // cannot be a secondary entry of the $9FFF body: codegen tails through
    // the live window instead of emitting label_A000 in that body.
    const rom = new RomBuilder({ mapper: 40, prgBanks: 4 })
      .bank8(7)
      .org(0xa100)
      .jmp(0x9fff)
      .bank8(4)
      .org(0x9fff)
      .nop()
      .bank8(5)
      .org(0xa000)
      .rts()
      .bank8(7)
      .vectors(0xe100, 0xe100, 0xe100)
      .writeTemp("mapper40_boundary_entry_ownership.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "mapper40-boundary-owner"\npush_all_jsr = true\n`
    );

    expect(result.fullC).toContain("void func_9FFF_b2(void)");
    expect(result.fullC).toContain("void func_A000_b2(void)");
    expect(result.fullC).not.toMatch(
      /void func_A000_b2\(void\) \{[\s\S]*?func_9FFF_b2_body\(1\)/
    );
    expect(result.fullC).not.toContain("case 1: goto label_A000;");
  });

  it("Mapper 40 maps relative branches across fixed 8KB CPU windows", () => {
    const rom = new RomBuilder({ mapper: 40, prgBanks: 4 })
      .bank8(7)
      .org(0xa100)
      .jmp(0x7ff9)
      .bank8(6)
      .org(0x9ff9)
      .emit([0xf0, 0x11, 0x60]) // CPU $7FF9: BEQ $800C; RTS
      .bank8(4)
      .org(0x800c)
      .rts()
      .bank8(7)
      .vectors(0xe100, 0xe100, 0xe100)
      .writeTemp("mapper40_relative_boundary.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "mapper40-relative-boundary"\npush_all_jsr = true\n`
    );

    expect(result.fullC).toContain("void func_9FF9_b3(void)");
    expect(result.fullC).toContain("void func_800C_b2(void)");
    expect(result.fullC).toContain("g_code_window_base + 0x200C");
    expect(result.fullC).not.toContain("g_code_window_base | 0x000C");
    expect(result.dispatchC).toMatch(/case 0x800C:[\s\S]*func_800C_b2/);
  });

  it("Mapper 40 normalizes inline-dispatch table targets", () => {
    const rom = new RomBuilder({ mapper: 40, prgBanks: 4 })
      .bank8(7)
      .org(0xa100)
      .lda(0x00)
      .jsr(0x6c7d)
      .word(0xbfb0)
      .word(0x0000)
      .rts()
      .bank8(6)
      .org(0x8c7d)
      .emit([
        0x0a, 0xa8, 0x68, 0x85, 0x04, 0x68, 0x85, 0x05,
        0xc8, 0xb1, 0x04, 0x85, 0x06, 0xc8, 0xb1, 0x04,
        0x85, 0x07, 0x6c, 0x06, 0x00
      ])
      .bank8(5)
      .org(0xbfb0)
      .rts()
      .bank8(7)
      .vectors(0xe100, 0xe100, 0xe100)
      .writeTemp("mapper40_inline_dispatch.nes");

    const result = recompile(
      rom,
      `[game]\noutput_prefix = "mapper40-inline"\npush_all_jsr = true\n\n[[inline_dispatch]]\naddr = 0x6C7D\n`
    );

    expect(result.fullC).toContain("void func_BFB0_b2(void)");
    expect(result.fullC).toContain("inline_dispatch $6C7D: 1 entries");
    expect(result.fullC).toContain(
      "uint16_t _inline_ret = (uint16_t)((g_code_window_base | 0x0102) + 2)"
    );
    expect(result.fullC).toContain("nes_write(0x0004, (uint8_t)_inline_ret)");
    expect(result.fullC).toContain(
      "nes_write(0x0005, (uint8_t)(_inline_ret >> 8))"
    );
    expect(result.fullC).toContain("call_by_address_tail(0xBFB0");
    expect(result.fullC).not.toContain("void func_8C7D_b3(void)");
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
    // CPU $C100 in an even R6 bank is generated at that bank's natural $8100
    // identity, and must be publicly dispatchable there.
    expect(result.fullC).toContain("func_8100_b2");
    expect(result.dispatchC).toContain("case 0x8100:");
    // At least one bank switch should have been detected during the walk.
    expect(result.output).toMatch(/Bank switches detected: [1-9]/);
  });

  it("MMC3: unknown mode-1 window fans out generated R6 identities", () => {
    const rom = new RomBuilder({ mapper: 4, prgBanks: 4 })
      .bank(3)
      .org(0xc000)
      .jsr(0x8000)
      .rts()
      .bank(0)
      .org(0x8000)
      .jsr(0xc100)
      .rts()
      .org(0x8100)
      .lda(0x55)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("mmc3_unknown_mode1_fanout.nes");

    const result = recompile(rom);
    expect(result.fullC).toContain("func_8100_b0");
    expect(result.dispatchC).toContain("case 0x8100:");
  });

  it("dispatches switchable MMC3 absolute JMPs through the live window", () => {
    const rom = new RomBuilder({ mapper: 4, prgBanks: 4 })
      .bank(0)
      .org(0x8ce8)
      .emit(new Array(0x18).fill(0xea))
      .jmp(0x8ce8)
      .bank(3)
      .org(0xc000)
      .jsr(0x8ce8)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("mmc3_relocated_absolute_jmp.nes");

    const result = recompile(rom);
    expect(result.fullC).toMatch(
      /\$8D00: 4C \*\/[^\n]*call_by_address_tail\(0x8CE8, 0\); return;/
    );
  });

  it("scopes mapper-4 vector handlers to their execution windows", () => {
    const rom = new RomBuilder({ mapper: 4, prgBanks: 4 })
      .bank(3)
      .org(0xc000)
      .rts()
      .org(0xe080)
      .jsr(0xe0a0)
      .rti()
      .org(0xe0a0)
      .rts()
      .org(0xe100)
      .rti()
      .vectors(0xe080, 0xc000, 0xe100)
      .writeTemp("mmc3_vector_windows.nes");

    const result = recompile(rom);
    expect(result.fullC).toContain(
      "void func_RESET(void) { uint16_t _saved_wb = g_code_window_base; " +
      "g_code_window_base = 0xC000; func_C000(); g_code_window_base = _saved_wb; }"
    );
    expect(result.fullC).toMatch(
      /g_code_window_base = 0xE000;\r?\n\s*func_E080\(\);/
    );
    expect(result.fullC).toContain(
      "void func_IRQ(void)   { uint16_t _saved_wb = g_code_window_base; " +
      "g_code_window_base = 0xE000; func_E100(); g_code_window_base = _saved_wb; }"
    );

    const restore = result.fullC.indexOf("g_code_window_base = _nmi_saved_wb;");
    const postNmi = result.fullC.indexOf("runtime_begin_post_nmi();");
    expect(result.fullC).toContain("runtime_note_interrupt_entry();");
    expect(restore).toBeGreaterThan(-1);
    expect(postNmi).toBeGreaterThan(restore);
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
