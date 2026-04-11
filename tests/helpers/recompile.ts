/**
 * Run NESRecomp on a ROM and parse the results.
 */
import { execFileSync } from "child_process";
import { existsSync, readFileSync, writeFileSync, mkdtempSync } from "fs";
import { join, dirname, resolve } from "path";
import { tmpdir } from "os";

const NESRECOMP_EXE = resolve(
  import.meta.dirname,
  "../../build/recompiler/Release/NESRecomp.exe"
);

export interface RecompResult {
  functionCount: number;
  dispatchEntries: string[]; // ["C000", "C010", ...]
  output: string;
  generatedDir: string;
  fullC: string;
  dispatchC: string;
}

/**
 * Run NESRecomp on a ROM file with an optional game.toml.
 * Returns parsed results including function count and dispatch entries.
 */
export function recompile(
  romPath: string,
  tomlContent?: string
): RecompResult {
  if (!existsSync(NESRECOMP_EXE)) {
    throw new Error(
      `NESRecomp.exe not found at ${NESRECOMP_EXE} — build the recompiler first`
    );
  }

  const workDir = mkdtempSync(join(tmpdir(), "nesrecomp-test-"));
  const generatedDir = join(workDir, "generated");

  // Write toml if provided
  const args = [romPath];
  if (tomlContent) {
    const tomlPath = join(workDir, "game.toml");
    writeFileSync(tomlPath, tomlContent);
    args.push("--game", tomlPath);
  }

  const output = execFileSync(NESRECOMP_EXE, args, {
    cwd: workDir,
    encoding: "utf-8",
    timeout: 30_000,
    maxBuffer: 50 * 1024 * 1024,
  });

  // Parse function count
  const funcMatch = output.match(/Found (\d+) functions/);
  const functionCount = funcMatch ? parseInt(funcMatch[1], 10) : 0;

  // Find generated files (prefix defaults to ROM name)
  const dispatchFiles = existsSync(generatedDir)
    ? require("fs")
        .readdirSync(generatedDir)
        .filter((f: string) => f.endsWith("_dispatch.c"))
    : [];
  const fullFiles = existsSync(generatedDir)
    ? require("fs")
        .readdirSync(generatedDir)
        .filter((f: string) => f.endsWith("_full.c"))
    : [];

  const dispatchC =
    dispatchFiles.length > 0
      ? readFileSync(join(generatedDir, dispatchFiles[0]), "utf-8")
      : "";
  const fullC =
    fullFiles.length > 0
      ? readFileSync(join(generatedDir, fullFiles[0]), "utf-8")
      : "";

  // Extract dispatch entries
  const dispatchEntries = (dispatchC.match(/case 0x([0-9A-Fa-f]+):/g) || [])
    .map((m) => m.replace("case 0x", "").replace(":", "").toUpperCase());

  return {
    functionCount,
    dispatchEntries,
    output,
    generatedDir,
    fullC,
    dispatchC,
  };
}
