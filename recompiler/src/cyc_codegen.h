/*
 * cyc_codegen.h - cycle-accurate code generation (NESRecomp --cycle-accurate)
 *
 * Emits generated/<prefix>_cyc.c for runner/cyc: every reachable ROM
 * instruction becomes C that performs the instruction's individual CPU cycles
 * (bus reads and writes, interrupt polls, the instruction's last cycle)
 * through runner/cyc/cpu6502.h, with PC, opcode and operands folded in and
 * static control flow compiled to gotos. The same instruction templates also
 * produce runner/cyc/cpu6502_interp.c. See runner/cyc/README.md.
 */
#pragma once
#include <stdbool.h>

#include "game_config.h"
#include "rom_parser.h"

bool cyc_codegen_emit(const NESRom *rom, const GameConfig *cfg, const char *output_prefix);
/* Write the cycle-accurate interpreter (runner/cyc/cpu6502_interp.c). */
bool cyc_codegen_emit_interpreter(const char *path);
