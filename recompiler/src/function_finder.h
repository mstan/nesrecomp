/*
 * function_finder.h — JSR/RTS graph walk to detect function boundaries
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "rom_parser.h"
#include "game_config.h"

#define MAX_FUNCTIONS 32768
#define MAX_INSNS_PER_FUNC 2048

typedef struct {
    uint16_t addr;          /* NES address of function entry */
    int      bank;          /* Switchable bank (-1 for fixed bank) */
    int      size;          /* Number of instructions */
} FunctionEntry;

typedef struct {
    FunctionEntry entries[MAX_FUNCTIONS];
    int           count;
} FunctionList;

void function_finder_run(const NESRom *rom, FunctionList *out, const GameConfig *cfg);
void function_list_free(FunctionList *list); /* Currently a no-op — entries are static */

/* Returns true if addr is already in the function list */
bool function_list_contains(const FunctionList *list, uint16_t addr, int bank);
