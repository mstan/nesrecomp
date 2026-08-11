#include "function_dedup.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BEGIN_MARKER "/* NESRECOMP_DEDUP_BEGIN "
#define END_MARKER   "/* NESRECOMP_DEDUP_END */"
#define SELF_TOKEN   "__NESRECOMP_DEDUP_SELF__"

typedef struct {
    int path_index;
    long segment_start, body_start, body_end, segment_end;
    char name[64];
    int bank;
    uint16_t addr;
    uint64_t hash;
    size_t normalized_len;
    int group;
} Candidate;

typedef struct {
    int representative;
    int member_count;
    int external_replacement;
    size_t source_bytes;
} Group;

static char *read_file(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long end = ftell(f);
    if (end < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *data = (char *)malloc((size_t)end + 1);
    if (!data) { fclose(f); return NULL; }
    size_t got = fread(data, 1, (size_t)end, f);
    fclose(f);
    if (got != (size_t)end) { free(data); return NULL; }
    data[got] = '\0';
    *size_out = got;
    return data;
}

static uint64_t normalized_hash(const char *body, size_t len, const char *name,
                                size_t *normalized_len) {
    const uint64_t prime = UINT64_C(1099511628211);
    uint64_t h = UINT64_C(1469598103934665603);
    size_t name_len = strlen(name), token_len = strlen(SELF_TOKEN), out_len = 0;
    for (size_t i = 0; i < len;) {
        if (name_len && i + name_len <= len && memcmp(body + i, name, name_len) == 0) {
            for (size_t j = 0; j < token_len; j++) { h ^= (unsigned char)SELF_TOKEN[j]; h *= prime; }
            out_len += token_len;
            i += name_len;
        } else {
            h ^= (unsigned char)body[i++];
            h *= prime;
            out_len++;
        }
    }
    *normalized_len = out_len;
    return h;
}

static char *normalized_copy(const char *path, const Candidate *c) {
    size_t file_size = 0;
    char *file = read_file(path, &file_size);
    if (!file || c->body_start < 0 || c->body_end < c->body_start ||
        (size_t)c->body_end > file_size) {
        free(file);
        return NULL;
    }
    size_t len = (size_t)(c->body_end - c->body_start);
    const char *body = file + c->body_start;
    size_t name_len = strlen(c->name), token_len = strlen(SELF_TOKEN);
    char *out = (char *)malloc(c->normalized_len + 1);
    if (!out) { free(file); return NULL; }
    size_t oi = 0;
    for (size_t i = 0; i < len;) {
        if (name_len && i + name_len <= len && memcmp(body + i, c->name, name_len) == 0) {
            memcpy(out + oi, SELF_TOKEN, token_len);
            oi += token_len;
            i += name_len;
        } else {
            out[oi++] = body[i++];
        }
    }
    out[oi] = '\0';
    free(file);
    return out;
}

static bool candidates_equal(const char *const *paths,
                             const Candidate *a, const Candidate *b) {
    if (a->hash != b->hash || a->normalized_len != b->normalized_len) return false;
    char *na = normalized_copy(paths[a->path_index], a);
    char *nb = normalized_copy(paths[b->path_index], b);
    bool equal = na && nb && memcmp(na, nb, a->normalized_len) == 0;
    free(na);
    free(nb);
    return equal;
}

static int candidate_compare(const void *ap, const void *bp) {
    const Candidate *a = (const Candidate *)ap, *b = (const Candidate *)bp;
    if (a->hash < b->hash) return -1;
    if (a->hash > b->hash) return 1;
    if (a->normalized_len < b->normalized_len) return -1;
    if (a->normalized_len > b->normalized_len) return 1;
    if (a->bank != b->bank) return a->bank - b->bank;
    return (int)a->addr - (int)b->addr;
}

static int offset_ptr_compare(const void *ap, const void *bp) {
    const Candidate *a = *(const Candidate *const *)ap;
    const Candidate *b = *(const Candidate *const *)bp;
    return (a->segment_start > b->segment_start) - (a->segment_start < b->segment_start);
}

static bool append_candidate(Candidate **items, int *count, int *capacity,
                             const Candidate *value) {
    if (*count == *capacity) {
        int next = *capacity ? *capacity * 2 : 256;
        Candidate *grown = (Candidate *)realloc(*items, (size_t)next * sizeof(**items));
        if (!grown) return false;
        *items = grown;
        *capacity = next;
    }
    (*items)[(*count)++] = *value;
    return true;
}

static bool scan_path(const char *path, int path_index,
                      Candidate **items, int *count, int *capacity) {
    size_t size = 0;
    char *data = read_file(path, &size);
    if (!data) return false;
    char *cursor = data;
    while ((cursor = strstr(cursor, BEGIN_MARKER)) != NULL) {
        Candidate c;
        memset(&c, 0, sizeof(c));
        c.group = -1;
        unsigned addr = 0;
        if (sscanf(cursor, "/* NESRECOMP_DEDUP_BEGIN %63s %d %x */",
                   c.name, &c.bank, &addr) != 3) {
            fprintf(stderr, "codegen dedup: malformed marker in %s\n", path);
            free(data);
            return false;
        }
        char *begin_eol = strchr(cursor, '\n');
        char *end = begin_eol ? strstr(begin_eol + 1, END_MARKER) : NULL;
        if (!begin_eol || !end) {
            fprintf(stderr, "codegen dedup: unterminated marker in %s\n", path);
            free(data);
            return false;
        }
        char *end_eol = strchr(end, '\n');
        c.path_index = path_index;
        c.segment_start = (long)(cursor - data);
        c.body_start = (long)(begin_eol + 1 - data);
        c.body_end = (long)(end - data);
        c.segment_end = (long)((end_eol ? end_eol + 1 : data + size) - data);
        c.addr = (uint16_t)addr;
        c.hash = normalized_hash(data + c.body_start,
                                 (size_t)(c.body_end - c.body_start),
                                 c.name, &c.normalized_len);
        if (!append_candidate(items, count, capacity, &c)) {
            free(data);
            return false;
        }
        cursor = data + c.segment_end;
    }
    free(data);
    return true;
}

static int resolved_bank(int configured, int fixed_bank) {
    return configured < 0 ? fixed_bank : configured;
}

static const ReplaceFunc *group_replacement_for(const GameConfig *cfg,
                                                 int fixed_bank,
                                                 const Candidate *c) {
    for (int i = 0; i < cfg->replace_func_count; i++) {
        const ReplaceFunc *r = &cfg->replace_funcs[i];
        if (r->replace_group && resolved_bank(r->bank, fixed_bank) == c->bank &&
            r->addr == c->addr) return r;
    }
    return NULL;
}

static bool write_alias(FILE *out, const Candidate *alias, const Candidate *target) {
    return fprintf(out,
        "/* Shared implementation: %s aliases %s. See function_groups.json. */\n"
        "void %s(void) {\n"
        "#ifdef RECOMP_STACK_TRACKING\n"
        "    recomp_stack_push(\"%s\");\n"
        "#endif\n"
        "    %s();\n"
        "#ifdef RECOMP_STACK_TRACKING\n"
        "    recomp_stack_pop();\n"
        "#endif\n"
        "}\n\n",
        alias->name, target->name, alias->name, alias->name, target->name) > 0;
}

static bool rewrite_path(const char *path, int path_index, Candidate *items,
                         int item_count, const Group *groups) {
    int local_count = 0;
    for (int i = 0; i < item_count; i++) if (items[i].path_index == path_index) local_count++;
    if (!local_count) return true;
    Candidate **local = (Candidate **)malloc((size_t)local_count * sizeof(*local));
    if (!local) return false;
    int li = 0;
    for (int i = 0; i < item_count; i++) if (items[i].path_index == path_index) local[li++] = &items[i];
    qsort(local, (size_t)local_count, sizeof(*local), offset_ptr_compare);

    size_t size = 0;
    char *data = read_file(path, &size);
    if (!data) { free(local); return false; }
    char temp_path[768];
    snprintf(temp_path, sizeof(temp_path), "%s.dedup.tmp", path);
    FILE *out = fopen(temp_path, "wb");
    if (!out) { free(data); free(local); return false; }
    long cursor = 0;
    bool ok = true;
    for (int i = 0; i < local_count && ok; i++) {
        Candidate *c = local[i];
        if (c->segment_start < cursor || (size_t)c->segment_end > size) { ok = false; break; }
        ok = fwrite(data + cursor, 1, (size_t)(c->segment_start - cursor), out) ==
             (size_t)(c->segment_start - cursor);
        if (!ok) break;
        if (c->group < 0) {
            ok = fwrite(data + c->body_start, 1, (size_t)(c->body_end - c->body_start), out) ==
                 (size_t)(c->body_end - c->body_start);
        } else {
            const Group *g = &groups[c->group];
            const Candidate *rep = &items[g->representative];
            if (g->external_replacement && c == rep) {
                ok = fprintf(out, "/* %s is supplied externally for its shared function group. */\n\n",
                             c->name) > 0;
            } else if (c == rep) {
                ok = fwrite(data + c->body_start, 1,
                            (size_t)(c->body_end - c->body_start), out) ==
                     (size_t)(c->body_end - c->body_start);
            } else {
                ok = write_alias(out, c, rep);
            }
        }
        cursor = c->segment_end;
    }
    if (ok) ok = fwrite(data + cursor, 1, size - (size_t)cursor, out) == size - (size_t)cursor;
    if (fclose(out) != 0) ok = false;
    free(data);
    free(local);
    if (!ok) { remove(temp_path); return false; }
    char backup_path[768];
    snprintf(backup_path, sizeof(backup_path), "%s.dedup.bak", path);
    remove(backup_path);
    if (rename(path, backup_path) != 0) {
        fprintf(stderr, "codegen dedup: could not stage existing %s\n", path);
        remove(temp_path);
        return false;
    }
    if (rename(temp_path, path) != 0) {
        fprintf(stderr, "codegen dedup: could not publish rewritten %s\n", path);
        (void)rename(backup_path, path);
        remove(temp_path);
        return false;
    }
    remove(backup_path);
    return true;
}

static bool write_manifest(const char *path, const Candidate *items, int item_count,
                           const Group *groups, int group_count,
                           const GameConfig *cfg, int fixed_bank) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "{\n  \"schema_version\": 1,\n  \"proof\": \"exact-generated-c-self-normalized\",\n  \"groups\": [\n");
    for (int gi = 0; gi < group_count; gi++) {
        const Candidate *rep = &items[groups[gi].representative];
        if (gi) fprintf(f, ",\n");
        fprintf(f, "    {\n      \"representative\": \"%s\",\n", rep->name);
        fprintf(f, "      \"external_replacement\": %s,\n",
                groups[gi].external_replacement ? "true" : "false");
        fprintf(f, "      \"proof_hash\": \"%016llx\",\n",
                (unsigned long long)rep->hash);
        size_t representative_bytes = (size_t)(rep->body_end - rep->body_start);
        size_t saved_bytes = groups[gi].source_bytes > representative_bytes
                           ? groups[gi].source_bytes - representative_bytes : 0;
        fprintf(f, "      \"source_bytes\": %llu,\n",
                (unsigned long long)groups[gi].source_bytes);
        fprintf(f, "      \"estimated_saved_body_bytes\": %llu,\n      \"members\": [",
                (unsigned long long)saved_bytes);
        int emitted = 0;
        for (int i = 0; i < item_count; i++) if (items[i].group == gi) {
            fprintf(f, "%s\n        { \"symbol\": \"%s\", \"bank\": %d, \"addr\": \"0x%04X\" }",
                    emitted++ ? "," : "", items[i].name, items[i].bank, items[i].addr);
        }
        fprintf(f, "\n      ]\n    }");
    }
    fprintf(f, "\n  ],\n  \"exclusions\": [");
    for (int i = 0; i < cfg->dedup_exclude_count; i++) {
        fprintf(f, "%s\n    { \"bank\": %d, \"addr\": \"0x%04X\" }",
                i ? "," : "", resolved_bank(cfg->dedup_excludes[i].bank, fixed_bank),
                cfg->dedup_excludes[i].addr);
    }
    fprintf(f, "\n  ]\n}\n");
    return fclose(f) == 0;
}

bool function_dedup_run(const char *const *paths, int path_count,
                        const GameConfig *cfg, int fixed_bank,
                        const char *manifest_path) {
    Candidate *items = NULL;
    int item_count = 0, item_capacity = 0;
    for (int i = 0; i < path_count; i++) {
        if (!scan_path(paths[i], i, &items, &item_count, &item_capacity)) {
            free(items);
            return false;
        }
    }
    qsort(items, (size_t)item_count, sizeof(*items), candidate_compare);

    Group *groups = (Group *)calloc((size_t)(item_count ? item_count : 1), sizeof(*groups));
    bool *used = (bool *)calloc((size_t)(item_count ? item_count : 1), sizeof(*used));
    if (!groups || !used) { free(groups); free(used); free(items); return false; }
    int group_count = 0;
    for (int start = 0; start < item_count;) {
        int end = start + 1;
        while (end < item_count && items[end].hash == items[start].hash &&
               items[end].normalized_len == items[start].normalized_len) end++;
        for (int i = start; i < end; i++) {
            if (used[i]) continue;
            int members = 1;
            bool cross_bank = false;
            for (int j = i + 1; j < end; j++) {
                if (!used[j] && candidates_equal(paths, &items[i], &items[j])) {
                    members++;
                    if (items[j].bank != items[i].bank) cross_bank = true;
                }
            }
            if (!cross_bank) continue;
            Group *g = &groups[group_count];
            g->representative = i;
            int replacement = -1;
            for (int j = i; j < end; j++) {
                if (j != i && (used[j] || !candidates_equal(paths, &items[i], &items[j]))) continue;
                used[j] = true;
                items[j].group = group_count;
                g->member_count++;
                g->source_bytes += (size_t)(items[j].body_end - items[j].body_start);
                if (group_replacement_for(cfg, fixed_bank, &items[j])) {
                    if (replacement >= 0) {
                        fprintf(stderr, "codegen dedup: multiple scope=group replacements select one group\n");
                        free(groups); free(used); free(items); return false;
                    }
                    replacement = j;
                }
            }
            if (replacement >= 0) {
                g->representative = replacement;
                g->external_replacement = 1;
            }
            group_count++;
            (void)members;
        }
        start = end;
    }

    for (int ri = 0; ri < cfg->replace_func_count; ri++) {
        const ReplaceFunc *r = &cfg->replace_funcs[ri];
        if (!r->replace_group) continue;
        bool found = false;
        int bank = resolved_bank(r->bank, fixed_bank);
        for (int i = 0; i < item_count; i++) {
            if (items[i].bank == bank && items[i].addr == r->addr && items[i].group >= 0) {
                found = true; break;
            }
        }
        if (!found) {
            fprintf(stderr, "codegen dedup: scope=group replacement %d:$%04X has no proven group\n",
                    bank, r->addr);
            free(groups); free(used); free(items); return false;
        }
    }

    bool ok = write_manifest(manifest_path, items, item_count, groups, group_count,
                             cfg, fixed_bank);
    for (int i = 0; ok && i < path_count; i++)
        ok = rewrite_path(paths[i], i, items, item_count, groups);
    if (ok) printf("[NESRecomp] Shared function bodies: %d group(s) across %d candidate(s)\n",
                   group_count, item_count);
    free(groups);
    free(used);
    free(items);
    return ok;
}
