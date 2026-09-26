"""One-shot porting aid: TriCNES Emulator.cs (C#) -> C++ translation unit.

TriCNES (https://github.com/100thCoin/TriCNES, MIT, (c) 2025 Chris Siebert)
is the reference emulator for the AccuracyCoin test ROM. The cycle-accurate
runner core (runner/cyc/tric_core.cpp) was produced by running this script
over Emulator.cs and then hand-editing the result (NROM connector wiring,
removal of the NTSC decoder / TAS / debugger paths, host framebuffer).

The script is kept for provenance; it is not part of the build.

Usage: python port_tricnes.py <Emulator.cs> <out.cpp>
"""

from __future__ import annotations

import re
import sys

# Emulator methods carried into the port. Everything else (NTSC signal
# decoding, bordered screen, trace logger, save states, TAS plumbing) is
# dropped.
METHODS = [
    "Reset",
    "_EmulatorCore",
    "EmulateUntilEndOfRead",
    "EmulateNMasterClockCycles",
    "_EmulateAPU",
    "_EmulatePPU",
    "FetchVideoMemory",
    "WriteVideoMemory",
    "_EmulateHalfPPU",
    "PPU_DATA_StateMachine",
    "PPU_DATA_StateMachine2",
    "PPU_DATA_StateMachine_Half",
    "DrawToScreen",
    "CorruptOAM",
    "IncrementOAM2Address",
    "PPU_Render_SpriteEvaluation",
    "PPU_Render_CalculatePixel",
    "CorruptPalettes",
    "PPU_Render_ShiftRegistersAndBitPlanes",
    "PPU_Render_CommitShiftRegistersAndBitPlanes",
    "PPU_Render_ShiftRegistersAndBitPlanes_DummyNT",
    "PPU_CheckPAR",
    "Flip",
    "PPU_UpdateBackgroundShiftRegisters",
    "UpdateSpriteShiftRegisters",
    "PPU_LoadShiftRegisters",
    "PPU_IncrementScrollX",
    "PPU_IncrementScrollY",
    "PPU_ResetXScroll",
    "PPU_ResetYScroll",
    "DecayPPUDataBus",
    "OAMDMA_Get",
    "OAMDMA_Halted",
    "OAMDMA_Put",
    "DMCDMA_Get",
    "DMCDMA_Halted",
    "DMCDMA_Put",
    "PollInterrupts",
    "PollInterrupts_CantDisableIRQ",
    "CompleteOperation",
    "_6502",
    "ResetReadPush",
    "Push",
    "Observe",
    "Fetch",
    "ObservePPU",
    "MapperObserve",
    "MapperFetchPRG",
    "ReadOAM",
    "Store",
    "StorePPURegisters",
    "StartDMCSample",
    "GetImmediate",
    "GetAddressAbsolute",
    "GetAddressZeroPage",
    "GetAddressIndOffX",
    "GetAddressIndOffY",
    "GetAddressZPOffX",
    "GetAddressZPOffY",
    "GetAddressAbsOffX",
    "GetAddressAbsOffY",
    "Op_ORA", "Op_ASL", "Op_ASL_A", "Op_SLO", "Op_AND", "Op_ROL", "Op_ROL_A",
    "Op_RLA", "Op_EOR", "Op_LSR", "Op_LSR_A", "Op_SRE", "Op_ADC", "Op_ROR",
    "Op_ROR_A", "Op_RRA", "Op_CMP", "Op_CPY", "Op_CPX", "Op_SBC", "Op_INC",
    "Op_DEC",
]

SKIP_FIELD_TYPES = (
    "Cartridge", "DirectBitmap", "StringBuilder", "List<", "string",
    "Color", "double[]", "float[]", "Thread", "Random",
)

SCALAR_TYPES = {
    "byte": "byte", "ushort": "ushort", "int": "int", "uint": "uint",
    "bool": "bool", "sbyte": "sbyte", "short": "short", "long": "long",
    "ulong": "ulong", "float": "float", "double": "double",
}


def strip_class_body(lines: list[str]) -> str:
    start = next(i for i, l in enumerate(lines) if re.match(r"\s*public class Emulator\b", l))
    text = "".join(lines[start:])
    open_idx = text.index("{")
    depth = 0
    i = open_idx
    in_str = in_chr = in_line_comment = in_block_comment = False
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
        elif in_block_comment:
            if c == "*" and n == "/":
                in_block_comment = False
                i += 1
        elif in_str:
            if c == "\\":
                i += 1
            elif c == '"':
                in_str = False
        elif in_chr:
            if c == "\\":
                i += 1
            elif c == "'":
                in_chr = False
        else:
            if c == "/" and n == "/":
                in_line_comment = True
                i += 1
            elif c == "/" and n == "*":
                in_block_comment = True
                i += 1
            elif c == '"':
                in_str = True
            elif c == "'":
                in_chr = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return text[open_idx + 1:i]
        i += 1
    raise ValueError("unterminated class body")


def split_members(body: str) -> list[str]:
    """Split a class body into top-level member source chunks."""
    members = []
    depth = 0
    cur = []
    i = 0
    in_str = in_chr = in_line_comment = in_block_comment = False
    while i < len(body):
        c = body[i]
        n = body[i + 1] if i + 1 < len(body) else ""
        cur.append(c)
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
        elif in_block_comment:
            if c == "*" and n == "/":
                in_block_comment = False
                cur.append(n)
                i += 1
        elif in_str:
            if c == "\\":
                cur.append(n)
                i += 1
            elif c == '"':
                in_str = False
        elif in_chr:
            if c == "\\":
                cur.append(n)
                i += 1
            elif c == "'":
                in_chr = False
        else:
            if c == "/" and n == "/":
                in_line_comment = True
            elif c == "/" and n == "*":
                in_block_comment = True
                cur.append(n)
                i += 1
            elif c == '"':
                in_str = True
            elif c == "'":
                in_chr = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    # end of a method / initializer block, unless followed by ';'
                    j = i + 1
                    while j < len(body) and body[j] in " \t":
                        j += 1
                    if j < len(body) and body[j] == ";":
                        pass
                    else:
                        members.append("".join(cur))
                        cur = []
            elif c == ";" and depth == 0:
                members.append("".join(cur))
                cur = []
        i += 1
    if "".join(cur).strip():
        members.append("".join(cur))
    return members


def code_only(chunk: str) -> str:
    """Remove comments to classify a member chunk."""
    chunk = re.sub(r"/\*.*?\*/", "", chunk, flags=re.S)
    chunk = re.sub(r"//[^\n]*", "", chunk)
    chunk = re.sub(r"^\s*#\w+[^\n]*", "", chunk, flags=re.M)
    return chunk.strip()


MODIFIERS = r"(?:(?:public|private|protected|internal|static|readonly|virtual|override|const)\s+)*"


def binlit(m: re.Match) -> str:
    return hex(int(m.group(1).replace("_", ""), 2)).upper().replace("X", "x")


def translate_body(src: str) -> str:
    src = re.sub(r"\b0b([01_]+)\b", binlit, src)
    src = src.replace("Cart.Emu.", "")
    src = re.sub(r"\bunchecked\s*\(", "(", src)
    src = re.sub(r"\b([A-Za-z_]\w*)\.Length\b", r"LEN(\1)", src)
    return src


def translate_field(chunk: str) -> str | None:
    code = code_only(chunk)
    if not code or code.startswith("#"):
        return None
    if "enum " in code:
        return None
    decl = re.sub(r"^" + MODIFIERS, "", code)
    type_token = re.match(r"[\w<>\[\]]+", decl)
    if type_token and any(type_token.group(0).startswith(t) for t in SKIP_FIELD_TYPES):
        return None
    is_const = bool(re.search(r"\bconst\b|\breadonly\b", code))
    # Arrays: type[] Name = new type[N];  or  type[] Name = { ... };
    m = re.match(r"(\w+)\[\]\s+(\w+)\s*=\s*new\s+\w+\[(.+?)\]\s*;$", decl, re.S)
    if m:
        t, name, size = m.groups()
        return f"{t} {name}[{translate_body(size)}];"
    m = re.match(r"(\w+)\[\]\s+(\w+)\s*=\s*(\{.*\})\s*;$", decl, re.S)
    if m:
        t, name, init = m.groups()
        q = "static const " if is_const or "static" in code else ""
        return f"{q}{t} {name}[] = {translate_body(init)};"
    m = re.match(r"(\w+)\[\]\s+(\w+)\s*;$", decl)
    if m:
        return f"/* unsized array dropped: {decl} */"
    m = re.match(r"(\w+)\s+(\w+)\s*(=\s*(.+))?;$", decl, re.S)
    if m:
        t, name, _, init = m.groups()
        if t not in SCALAR_TYPES:
            return f"/* dropped field: {decl} */"
        q = "const " if "const" in code else ""
        if init is not None:
            return f"{q}{t} {name} = {translate_body(init)};"
        return f"{t} {name};"
    m = re.match(r"(\w+)\s+(\w+(?:\s*,\s*\w+)+)\s*;$", decl)
    if m:
        t, names = m.groups()
        return f"{t} {names};"
    return f"/* unparsed field: {decl} */"


def main() -> int:
    src_path, out_path = sys.argv[1], sys.argv[2]
    with open(src_path, encoding="utf-8-sig") as f:
        lines = f.readlines()
    body = strip_class_body(lines)
    members = split_members(body)

    fields: list[str] = []
    methods: dict[str, str] = {}
    for chunk in members:
        code = code_only(chunk)
        if not code:
            continue
        mm = re.match(MODIFIERS + r"([\w<>\[\]]+)\s+(\w+)\s*\(([^)]*)\)\s*\{", code, re.S)
        ctor = re.match(r"public Emulator\s*\(\s*\)\s*\{", code)
        if ctor:
            at = chunk.index("Emulator(")
            body_src = chunk[chunk.index("{", at):]
            methods["Emulator_Init"] = "void Emulator_Init()\n" + translate_body(body_src)
            continue
        if mm:
            ret, name, params = mm.groups()
            if name in METHODS:
                at = re.search(r"\b" + re.escape(name) + r"\s*\(", chunk).start()
                body_src = chunk[chunk.index("{", at):]
                params = translate_body(params)
                methods[name] = f"{ret} {name}({params})\n" + translate_body(body_src)
            continue
        if "(" in code and "{" in code:
            continue
        t = translate_field(chunk)
        if t:
            fields.append(t)

    missing = [m for m in METHODS if m not in methods]
    with open(out_path, "w", encoding="utf-8", newline="\n") as out:
        out.write("// Machine translation of TriCNES Emulator.cs. See tools/cyc/port_tricnes.py.\n")
        out.write("// Missing methods: " + ", ".join(missing) + "\n\n")
        out.write("// ---- fields ----\n")
        for f in fields:
            out.write(f + "\n")
        out.write("\n// ---- prototypes ----\n")
        for name, text in methods.items():
            sig = text.split("\n", 1)[0]
            out.write(sig + ";\n")
        out.write("\n// ---- methods ----\n")
        for name, text in methods.items():
            out.write(text + "\n\n")
    print(f"fields={len(fields)} methods={len(methods)} missing={missing}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
