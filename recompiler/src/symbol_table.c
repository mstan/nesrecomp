/*
 * symbol_table.c — Symbol name table implementation
 */
#include "symbol_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* MSVC lacks strndup */
static char *sym_strndup(const char *s, size_t n) {
    size_t len = strlen(s);
    if (n < len) len = n;
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

bool symbol_table_load(SymbolTable *st, const char *path) {
    memset(st, 0, sizeof(*st));

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        line[strcspn(line, "\r\n")] = '\0';

        /* Skip leading whitespace */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        /* Skip blank lines and comments */
        if (!*p || *p == '#' || *p == ';') continue;

        /* Parse hex address */
        char *end;
        unsigned long addr = strtoul(p, &end, 16);
        if (end == p || addr > 0xFFFF) continue;

        /* Skip whitespace between address and name */
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) continue;

        /* Extract name (up to next whitespace or end of line) */
        const char *name_start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t name_len = (size_t)(p - name_start);
        if (name_len == 0) continue;

        /* Validate name is a valid C identifier */
        if (!isalpha((unsigned char)name_start[0]) && name_start[0] != '_') continue;

        /* Grow array if needed */
        if (st->count >= st->cap) {
            int newcap = st->cap ? st->cap * 2 : 128;
            SymbolEntry *ne = realloc(st->entries, (size_t)newcap * sizeof(SymbolEntry));
            if (!ne) break;
            st->entries = ne;
            st->cap = newcap;
        }

        st->entries[st->count].addr = (uint16_t)addr;
        st->entries[st->count].name = sym_strndup(name_start, name_len);
        if (!st->entries[st->count].name) break;
        st->count++;
    }

    fclose(f);
    st->sorted = false;
    return st->count > 0;
}

void symbol_table_free(SymbolTable *st) {
    if (!st) return;
    for (int i = 0; i < st->count; i++)
        free(st->entries[i].name);
    free(st->entries);
    memset(st, 0, sizeof(*st));
}

static int sym_cmp(const void *a, const void *b) {
    uint16_t aa = ((const SymbolEntry *)a)->addr;
    uint16_t bb = ((const SymbolEntry *)b)->addr;
    return (aa > bb) - (aa < bb);
}

const char *symbol_lookup(SymbolTable *st, uint16_t addr) {
    if (!st || st->count == 0) return NULL;

    /* Sort on first lookup */
    if (!st->sorted) {
        qsort(st->entries, (size_t)st->count, sizeof(SymbolEntry), sym_cmp);
        st->sorted = true;
    }

    /* Binary search */
    int lo = 0, hi = st->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t ma = st->entries[mid].addr;
        if (ma == addr) return st->entries[mid].name;
        if (ma < addr) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}
