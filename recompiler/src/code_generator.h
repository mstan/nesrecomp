/*
 * code_generator.h — Emit C for each 6502 instruction
 */
#pragma once
#include <stdbool.h>
#include "rom_parser.h"
#include "function_finder.h"
#include "annotations.h"
#include "symbol_table.h"
#include "game_config.h"

/* Global toggle: when true, emit RDB_STORE8(pc_hint, addr, val) in place
 * of nes_write(addr, val) for 6502 store instructions, and emit
 * `g_rdb_current_func = 0xXXXX;` at every function prologue. Default
 * false. Driven by main_nes --reverse-debug flag. See REVERSE_DEBUGGER.md. */
extern bool g_codegen_reverse_debug;

bool codegen_emit(const NESRom *rom, const FunctionList *funcs,
                  const char *out_full_path,
                  const char *out_dispatch_path,
                  const AnnotationTable *at,
                  const GameConfig *cfg,
                  SymbolTable *st);
