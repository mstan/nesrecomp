/*
 * annotations.c — ROM address annotation table implementation
 */
#include "annotations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *ltrim(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

bool annotations_load(AnnotationTable *t, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        const char *p = ltrim(line);
        if (!*p || *p == '#') continue;

        /* Parse bank */
        char *end;
        int bank = (int)strtol(p, &end, 0);
        if (end == p || *end != ',') continue;
        p = ltrim(end + 1);

        /* Parse address */
        uint16_t addr = (uint16_t)strtoul(p, &end, 0);
        if (end == p || *end != ',') continue;
        p = ltrim(end + 1);

        /* Remainder is the note (may contain commas) */
        if (!*p) continue;

        /* Grow array if needed */
        if (t->count >= t->cap) {
            int newcap = t->cap ? t->cap * 2 : 64;
            Annotation *ne = realloc(t->entries, (size_t)newcap * sizeof(Annotation));
            if (!ne) break;
            t->entries = ne;
            t->cap     = newcap;
        }

        t->entries[t->count].bank = bank;
        t->entries[t->count].addr = addr;
        t->entries[t->count].note = strdup(p);
        if (!t->entries[t->count].note) break;
        t->count++;
    }

    fclose(f);
    return t->count > 0;
}

void annotations_free(AnnotationTable *t) {
    if (!t) return;
    for (int i = 0; i < t->count; i++) free(t->entries[i].note);
    free(t->entries);
    t->entries = NULL;
    t->count   = 0;
    t->cap     = 0;
}

const char *annotation_lookup(const AnnotationTable *t, int bank, uint16_t addr) {
    for (int i = 0; i < t->count; i++)
        if (t->entries[i].bank == bank && t->entries[i].addr == addr)
            return t->entries[i].note;
    return NULL;
}
