/*
 * code_generator.h — Emit C for each 6502 instruction
 */
#pragma once
#include <stdbool.h>
#include "rom_parser.h"
#include "function_finder.h"
#include "annotations.h"
#include "game_config.h"

bool codegen_emit(const NESRom *rom, const FunctionList *funcs,
                  const char *out_full_path,
                  const char *out_dispatch_path,
                  const AnnotationTable *at,
                  const GameConfig *cfg);
