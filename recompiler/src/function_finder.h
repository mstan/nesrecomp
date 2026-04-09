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
    uint16_t canonical_addr;/* Canonical function body for secondary entries */
    int      canonical_bank;
    uint16_t covering_addr; /* Stronger body covering this entry, if any */
    int      covering_bank;
    uint8_t  kind;          /* FUNCTION_KIND_* */
    uint8_t  source_flags;  /* FUNCTION_SOURCE_* */
    uint16_t evidence_count;/* Number of independent discoveries / reinforcements */
} FunctionEntry;

typedef struct {
    FunctionEntry entries[MAX_FUNCTIONS];
    int           count;
} FunctionList;

enum {
    FUNCTION_KIND_STANDALONE = 0,
    FUNCTION_KIND_SECONDARY  = 1,
};

enum {
    FUNCTION_SOURCE_CONTROL     = 1 << 0,
    FUNCTION_SOURCE_PTR_SCAN    = 1 << 1,
    FUNCTION_SOURCE_TABLE_RUN   = 1 << 2,
    FUNCTION_SOURCE_SPLIT_TABLE = 1 << 3,
    FUNCTION_SOURCE_KNOWN_TABLE = 1 << 4,
    FUNCTION_SOURCE_XBANK       = 1 << 5,
    FUNCTION_SOURCE_MANUAL      = 1 << 6,
    FUNCTION_SOURCE_BANK_SEED   = 1 << 7,
};

void function_finder_run(const NESRom *rom, FunctionList *out, const GameConfig *cfg);
void function_list_free(FunctionList *list); /* Currently a no-op — entries are static */

/* Returns true if addr is already in the function list */
bool function_list_contains(const FunctionList *list, uint16_t addr, int bank);
void scan_function_boundaries(const NESRom *rom, uint16_t start_addr, int switchable_bank,
                              const GameConfig *cfg, uint16_t *out_boundaries,
                              int *out_count, int max_count);
