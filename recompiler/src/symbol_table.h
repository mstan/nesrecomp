/*
 * symbol_table.h — Optional symbol name table for generated code readability
 *
 * Loads a .sym file mapping addresses to human-readable names.
 * The code generator uses these to emit inline comments and #define aliases.
 *
 * .sym file format:
 *   # comment lines and blank lines are ignored
 *   XXXX SymbolName          (hex address, space, name)
 *   XXXX SymbolName func     (optional type: func, ram, label)
 *
 * Examples:
 *   D67A OffscreenBoundsCheck func
 *   071D ScreenRight_X_Pos ram
 *   C998 EraseEnemyObject
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t addr;
    char    *name;   /* heap-allocated, owned by the table */
} SymbolEntry;

typedef struct {
    SymbolEntry *entries;
    int          count;
    int          cap;
    bool         sorted; /* set after sort for binary search */
} SymbolTable;

/* Load .sym file into table. Returns true if any entries were loaded.
 * Returns false (with count=0) if the file does not exist or is empty. */
bool symbol_table_load(SymbolTable *st, const char *path);

/* Free all heap memory in the table. */
void symbol_table_free(SymbolTable *st);

/* Look up a symbol name by address. Returns NULL if not found.
 * Uses binary search after first call triggers sort. */
const char *symbol_lookup(SymbolTable *st, uint16_t addr);
