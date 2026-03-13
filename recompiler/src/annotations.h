/*
 * annotations.h — ROM address annotation table
 *
 * Generic mechanism: loads a CSV file of (bank, address, note) tuples and
 * makes them available to the code generator for inline comment emission.
 *
 * The CSV format is game-specific data; this module is game-agnostic.
 * The code generator calls annotation_lookup(bank, addr) at each emit point
 * and injects the result as a C block comment if non-NULL.
 *
 * CSV format:
 *   # comment lines (and blank lines) are ignored
 *   bank, address, note text — note may contain commas
 *   bank:    decimal integer (e.g. 5, 14, 15)
 *   address: hex with 0x prefix (e.g. 0xC49C) or decimal
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int      bank;
    uint16_t addr;
    char    *note;  /* heap-allocated, owned by the table */
} Annotation;

typedef struct {
    Annotation *entries;
    int         count;
    int         cap;
} AnnotationTable;

/* Load CSV into t.  Returns true if any entries were loaded.
 * Silently succeeds with count=0 if path does not exist. */
bool        annotations_load(AnnotationTable *t, const char *csv_path);

void        annotations_free(AnnotationTable *t);

/* Returns the note string for (bank, addr), or NULL if no entry exists. */
const char *annotation_lookup(const AnnotationTable *t, int bank, uint16_t addr);
