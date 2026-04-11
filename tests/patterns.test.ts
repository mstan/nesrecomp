/**
 * Advanced pattern tests — trampolines, PHA dispatch, split tables,
 * pointer scan, extra function seeds, and data region exclusion.
 */
import { describe, it, expect } from "vitest";
import { RomBuilder } from "./helpers/rom-builder.js";
import { recompile } from "./helpers/recompile.js";

describe("extra function seeds", () => {
  it("discovers functions listed in game.toml [functions]", () => {
    // $C100 is not reachable from RESET — only discoverable via extra seed
    const rom = new RomBuilder()
      .org(0xc000)
      .rts()
      .org(0xc100)
      .lda(0x42)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("extra_seed.nes");

    // Without seed: $C100 should not be found
    const without = recompile(rom, `
[game]
output_prefix = "test"
`);
    expect(without.dispatchEntries).not.toContain("C100");

    // With seed: $C100 should be found
    const withSeed = recompile(rom, `
[game]
output_prefix = "test"

[functions]
fixed = [0xC100]
`);
    expect(withSeed.dispatchEntries).toContain("C100");
  });

  it("walks JSR targets inside seeded functions", () => {
    // $C100 calls $C120 — both should be discovered when $C100 is seeded
    const rom = new RomBuilder()
      .org(0xc000)
      .rts()
      .org(0xc100)
      .jsr(0xc120)
      .rts()
      .org(0xc120)
      .lda(0xff)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("seed_walk.nes");

    const result = recompile(rom, `
[game]
output_prefix = "test"

[functions]
fixed = [0xC100]
`);
    expect(result.dispatchEntries).toContain("C100");
    expect(result.dispatchEntries).toContain("C120");
  });
});

describe("split dispatch tables", () => {
  it("discovers targets from split lo/hi byte tables", () => {
    // Split table: lo bytes at $C080, hi bytes at $C083, 3 entries
    // Targets (addr-1): $C0FF/$C10F/$C11F → actual: $C100, $C110, $C120
    const rom = new RomBuilder()
      .org(0xc000)
      .rts()
      // lo bytes at $C080
      .org(0xc080)
      .emit([0xff, 0x0f, 0x1f]) // lo bytes of (target-1)
      // hi bytes at $C083
      .emit([0xc0, 0xc1, 0xc1]) // hi bytes of (target-1)
      // targets
      .org(0xc100)
      .lda(0x01)
      .rts()
      .org(0xc110)
      .lda(0x02)
      .rts()
      .org(0xc120)
      .lda(0x03)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("split_table.nes");

    const result = recompile(rom, `
[game]
output_prefix = "test"

[[split_table]]
bank = 0
lo_addr = 0xC080
hi_addr = 0xC083
count = 3
stride = 1
`);
    expect(result.dispatchEntries).toContain("C100");
    expect(result.dispatchEntries).toContain("C110");
    expect(result.dispatchEntries).toContain("C120");
  });
});

describe("pointer scan", () => {
  it("discovers functions from embedded pointer values in fixed bank", () => {
    // Embed a 16-bit LE pointer to $C100 in a non-zero area.
    // Surround with non-zero bytes so the zero-fill detector doesn't
    // exclude the region.
    const rom = new RomBuilder()
      .org(0xc000)
      .rts()
      // Fill $C002-$C05F with non-zero data so it's not zero-fill excluded
      .org(0xc002)
      .emit(Array(90).fill(0xea)) // NOPs
      // Embedded pointer to $C100 at $C05C (inside the NOP run)
      .org(0xc05c)
      .word(0xc100)
      // More non-zero fill after
      .org(0xc05e)
      .emit(Array(20).fill(0xea))
      // The target function
      .org(0xc100)
      .lda(0x42)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("ptr_scan.nes");

    const result = recompile(rom, `
[game]
output_prefix = "test"
`);
    // Pointer scan should discover $C100 (or $C101 via +1 heuristic)
    expect(result.dispatchEntries).toContain("C100");
  });

  it("respects data_region exclusions", () => {
    // Same pointer but inside a declared data region — should be ignored
    const rom = new RomBuilder()
      .org(0xc000)
      .rts()
      .org(0xc050)
      .word(0xc200) // pointer to $C200
      .org(0xc200)
      .lda(0x42)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("data_region_exclude.nes");

    // With data_region covering $C050-$C060, the pointer should not be scanned
    const result = recompile(rom, `
[game]
output_prefix = "test"

[[data_region]]
bank = 0
start = 0xC050
end = 0xC060
`);
    // $C200 should not be found (only reachable via the excluded pointer)
    expect(result.dispatchEntries).not.toContain("C200");
  });
});

describe("illegal opcodes", () => {
  it("does not crash on ROMs containing illegal opcodes", () => {
    // Fill an area with illegal opcodes, but put valid code at entry points
    const rom = new RomBuilder()
      .org(0xc000)
      .jsr(0xc010)
      .rts()
      .org(0xc008) // illegal opcode area
      .emit([0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72])
      .org(0xc010)
      .lda(0x01)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("illegal_opcodes.nes");

    const result = recompile(rom);
    expect(result.functionCount).toBeGreaterThan(0);
    expect(result.dispatchEntries).toContain("C000");
    expect(result.dispatchEntries).toContain("C010");
  });
});

describe("zero-fill regions", () => {
  it("auto-excludes large zero-fill runs from function discovery", () => {
    // $C080-$C0FF is all zeros (128 bytes) — should be auto-excluded
    // A pointer to the middle of the zero region should not create a function
    const rom = new RomBuilder()
      .org(0xc000)
      .rts()
      // Leave $C080-$C0FF as zeros (default)
      // Put a pointer to $C090 (inside zero region) at $C050
      .org(0xc050)
      .word(0xc090)
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("zero_fill.nes");

    const result = recompile(rom);
    // $C090 is inside a zero-fill region, should not be a function
    expect(result.dispatchEntries).not.toContain("C090");
  });
});

describe("multiple JSR to same target", () => {
  it("deduplicates function entries from multiple call sites", () => {
    const rom = new RomBuilder()
      .org(0xc000)
      .jsr(0xc020) // call 1
      .jsr(0xc020) // call 2
      .jsr(0xc020) // call 3
      .rts()
      .org(0xc020)
      .lda(0x01)
      .rts()
      .vectors(0xc000, 0xc000, 0xc000)
      .writeTemp("dedup_jsr.nes");

    const result = recompile(rom);
    // $C020 should appear exactly once in dispatch
    const count = result.dispatchEntries.filter((e) => e === "C020").length;
    expect(count).toBe(1);
  });
});
