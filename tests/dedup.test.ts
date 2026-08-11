import { readFileSync } from "fs";
import { join } from "path";
import { describe, expect, it } from "vitest";

import { RomBuilder } from "./helpers/rom-builder";
import { recompile } from "./helpers/recompile";

function repeatedRom(secondValue = 0x42) {
  return new RomBuilder({ mapper: 4, prgBanks: 3 })
    .bank(0).org(0x8000).lda(0x42).sta(0x0200).rts()
    .bank(1).org(0x8000).lda(secondValue).sta(0x0200).rts()
    .bank(2).org(0xc000).rts()
    .vectors(0xc000, 0xc000, 0xc000)
    .writeTemp("dedup-banks.nes");
}

const baseConfig = `[game]
output_prefix = "dedup-banks"
deduplicate_functions = true

[functions]
bank0 = [0x8000]
bank1 = [0x8000]
`;

function manifest(result: ReturnType<typeof recompile>) {
  return JSON.parse(readFileSync(
    join(result.generatedDir, "dedup-banks_function_groups.json"), "utf-8"));
}

describe("cross-bank shared function bodies", () => {
  it("emits one exact body and preserves the other public symbol as a wrapper", () => {
    const result = recompile(repeatedRom(), baseConfig);
    const groups = manifest(result).groups;

    expect(groups).toHaveLength(1);
    expect(groups[0].representative).toBe("func_8000_b0");
    expect(groups[0].members.map((m: { symbol: string }) => m.symbol)).toEqual([
      "func_8000_b0", "func_8000_b1",
    ]);
    expect(result.fullC.match(/void func_8000_b0\(void\)/g)).toHaveLength(1);
    expect(result.fullC.match(/void func_8000_b1\(void\)/g)).toHaveLength(1);
    expect(result.fullC).toContain("func_8000_b0();");
    expect(result.fullC).not.toContain("NESRECOMP_DEDUP_BEGIN");
  });

  it("does not merge bodies with different executable source", () => {
    const result = recompile(repeatedRom(0x43), baseConfig);
    expect(manifest(result).groups).toHaveLength(0);
  });

  it("does not treat interpreter-only entries as empty shared bodies", () => {
    const result = recompile(repeatedRom(), `${baseConfig}
[force_interp]
bank0 = [0x8000]
bank1 = [0x8000]
`);
    expect(manifest(result).groups).toHaveLength(0);
    expect(result.fullC.match(/nes_interp_force_generated\(0x8000/g)).toHaveLength(2);
  });

  it("supports an explicit per-member exclusion", () => {
    const result = recompile(repeatedRom(), `${baseConfig}
[[dedup_exclude]]
bank = 1
addr = 0x8000
`);
    expect(manifest(result).groups).toHaveLength(0);
    expect(result.fullC.match(/void func_8000_b[01]\(void\)/g)).toHaveLength(2);
  });

  it("detaches an ordinary replace_func member", () => {
    const result = recompile(repeatedRom(), `${baseConfig}
[[replace_func]]
bank = 1
addr = 0x8000
scope = "member"
`);
    expect(manifest(result).groups).toHaveLength(0);
    expect(result.fullC).toContain("void func_8000_b0(void)");
    expect(result.fullC).not.toContain("void func_8000_b1(void)");
  });

  it("routes a whole group through one external replacement symbol", () => {
    const result = recompile(repeatedRom(), `${baseConfig}
[[replace_func]]
bank = 1
addr = 0x8000
scope = "group"
`);
    const groups = manifest(result).groups;
    expect(groups).toHaveLength(1);
    expect(groups[0].representative).toBe("func_8000_b1");
    expect(groups[0].external_replacement).toBe(true);
    expect(result.fullC).not.toContain("void func_8000_b1(void)");
    expect(result.fullC).toContain("func_8000_b1();");
  });

  it("rejects group replacement when exact equivalence is not proven", () => {
    expect(() => recompile(repeatedRom(0x43), `${baseConfig}
[[replace_func]]
bank = 1
addr = 0x8000
scope = "group"
`)).toThrow();
  });
});
