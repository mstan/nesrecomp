/*
 * symbol_table.h — Optional symbol name table for generated code readability
 *
 * Loads a .sym file mapping addresses to human-readable names.
 * The code generator uses these to emit inline comments and #define aliases.
 *
 * .sym file format:
 *   # comment lines and blank lines are ignored
 *   XXXX SymbolName          (hex address, space, name)
 *   XXXX SymbolName func     (optional type: func, ram, const, label)
 *
 * Examples:
 *   D67A OffscreenBoundsCheck func
 *   071D ScreenRight_X_Pos ram
 *   C998 EraseEnemyObject
 *
 * `const` is NOT an address. Assembly declares a memory location and a plain
 * constant with identical syntax (`Name = $33`), so a symbol file must say
 * which it meant: SMB's $0033 is PlayerFacingDir in zero page AND enemy id
 * $33 (BulletBill_CannonVar). Those two share a number and nothing else.
 *
 * A number MAY appear more than once. Within one kind that means genuine
 * aliases of the same thing — Player_X_Position is SprObject_X_Position slot
 * 0. All names are kept; the FIRST in file order represents the number
 * wherever only one name fits.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    SYM_KIND_OTHER = 0,  /* no type given, or an unrecognized one */
    SYM_KIND_FUNC,
    SYM_KIND_RAM,        /* RAM / MMIO address, not a code entry */
    SYM_KIND_CONST,      /* a plain VALUE (object/enemy id), not an address */
} SymbolKind;

typedef struct {
    uint16_t   addr;
    char      *name;   /* heap-allocated, owned by the table */
    SymbolKind kind;
    int        ord;    /* original file order; ties are broken on this so an
                        * address with several names keeps the file's own
                        * priority instead of qsort's arbitrary one */
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
 * When an address has several names this returns the first in file order.
 * Uses binary search after first call triggers sort. */
const char *symbol_lookup(SymbolTable *st, uint16_t addr);

/* Kind of the first symbol at an address, or SYM_KIND_OTHER when unknown. */
SymbolKind symbol_kind(SymbolTable *st, uint16_t addr);

/*
 * Collect every name at `addr` whose kind matches, in file order, into `out`.
 * Returns how many exist (which may exceed `max`; only `max` are written).
 * Pass SYM_KIND_OTHER to accept any kind.
 *
 * Filtering by kind matters: a RAM address and an unrelated object id can
 * share a number, and listing them together would assert a relationship that
 * does not exist.
 */
int symbol_names(SymbolTable *st, uint16_t addr, SymbolKind kind,
                 const char **out, int max);
