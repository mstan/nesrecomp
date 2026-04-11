/*
 * override_chr.c — CHR override and dump system implementation.
 *
 * Two modes:
 *   1. Dump: snapshot g_chr_ram on each bank switch, deduplicate by CRC32.
 *   2. Override: load manifest.json, JIT convert PNGs -> CHR with disk cache,
 *      apply overrides after each bank switch / frame.
 */
#include "override_chr.h"
#include "chr_codec.h"
#include "mapper.h"
#include "nes_runtime.h"
#include "crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <direct.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  define MKDIR(p) _mkdir(p)
#  define STAT_CALL _stat
#  define StatBuf   struct _stat
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define MKDIR(p) mkdir(p, 0755)
#  define STAT_CALL stat
#  define StatBuf   struct stat
#endif

/* ── Dump state ───────────────────────────────────────────────────────────── */

static int s_dump_enabled = 0;

#define MAX_UNIQUE_SNAPSHOTS 1024
static uint32_t s_seen_crcs[MAX_UNIQUE_SNAPSHOTS];
static int      s_num_unique = 0;
static int      s_total_switches = 0;

static FILE *s_index_file = NULL;
static int   s_dump_dir_created = 0;
static const char *s_dump_dir = "chr_dump";

/* ── Override state ───────────────────────────────────────────────────────── */

#define MAX_OVERRIDES 64

typedef struct {
    int      offset;       /* byte offset into g_chr_ram (0x0000-0x1FFF) */
    int      length;       /* bytes of CHR data to patch */
    uint8_t *data;         /* decoded CHR data (owned, freed on reload) */
    char     file[256];    /* source file path (relative to manifest dir) */
    char     full_path[512]; /* resolved absolute path for mtime checks */
    time_t   file_mtime;  /* last known mtime of source file */
} ChrOverrideEntry;

static ChrOverrideEntry s_overrides[MAX_OVERRIDES];
static int              s_num_overrides = 0;
static char             s_manifest_dir[512] = "";
static char             s_manifest_path[512] = "";
static time_t           s_manifest_mtime = 0;
static int              s_reload_ticks = 0;
#define RELOAD_INTERVAL 60 /* check every ~1 second at 60fps */

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void chr_callback(uint8_t *chr_data, int size, void *ctx);
static void dump_snapshot(const uint8_t *data, int size);
static void apply_overrides(uint8_t *chr_data, int size);
static void free_overrides(void);

/* ── Minimal JSON parser (manifest.json) ──────────────────────────────────
 *
 * Manifest format:
 * {
 *   "overrides": [
 *     { "offset": 512, "file": "player_tiles.png" },
 *     { "offset": 0, "length": 8192, "file": "full_chr.png" }
 *   ]
 * }
 *
 * "offset" = byte offset into CHR RAM (0-8191).
 * "length" = optional, auto-detected from file size if omitted.
 * "file"   = PNG or .bin file relative to manifest directory.
 *            PNGs are JIT-converted with .chr.bin disk cache.
 * ──────────────────────────────────────────────────────────────────────── */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Parse a JSON string literal, write into buf[max]. Returns pointer past closing quote. */
static const char *parse_string(const char *p, char *buf, int max) {
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (*p == 'n') { if (i < max-1) buf[i++] = '\n'; }
            else if (*p == '"') { if (i < max-1) buf[i++] = '"'; }
            else if (*p == '\\') { if (i < max-1) buf[i++] = '\\'; }
            else if (*p == '/') { if (i < max-1) buf[i++] = '/'; }
            else { if (i < max-1) buf[i++] = *p; }
        } else {
            if (i < max-1) buf[i++] = *p;
        }
        p++;
    }
    buf[i] = '\0';
    if (*p == '"') p++;
    return p;
}

/* Parse an integer (decimal or hex with 0x prefix). Returns pointer past number. */
static const char *parse_int(const char *p, int *out) {
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        *out = (int)strtol(p, (char **)&p, 16);
    } else {
        *out = (int)strtol(p, (char **)&p, 10);
    }
    return p;
}

static int load_override_file(const char *file_path, ChrOverrideEntry *entry) {
    /* Check extension: .png -> JIT decode with cache; .bin -> load raw. */
    const char *ext = strrchr(file_path, '.');
    if (ext && (strcmp(ext, ".png") == 0 || strcmp(ext, ".PNG") == 0)) {
        uint8_t *data = NULL;
        int size = 0;
        if (chr_load_cached(file_path, &data, &size) != 0)
            return -1;
        entry->data = data;
        if (entry->length == 0)
            entry->length = size;
        else if (entry->length > size)
            entry->length = size; /* clamp */
        return 0;
    }

    /* Raw .bin file */
    FILE *f = fopen(file_path, "rb");
    if (!f) {
        fprintf(stderr, "[ChrOverride] Cannot open: %s\n", file_path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) { fclose(f); return -1; }

    uint8_t *data = (uint8_t *)malloc(sz);
    if (!data) { fclose(f); return -1; }
    fread(data, 1, sz, f);
    fclose(f);

    entry->data = data;
    if (entry->length == 0)
        entry->length = (int)sz;
    else if (entry->length > (int)sz)
        entry->length = (int)sz;
    return 0;
}

static int parse_manifest(const char *json_text) {
    free_overrides();

    const char *p = skip_ws(json_text);
    if (*p != '{') {
        fprintf(stderr, "[ChrOverride] Manifest: expected '{'\n");
        return -1;
    }
    p++;

    /* Find "overrides" key */
    while (*p) {
        p = skip_ws(p);
        if (*p == '}') break;
        if (*p == ',') { p++; continue; }

        char key[64] = "";
        p = parse_string(p, key, sizeof(key));
        if (!p) break;
        p = skip_ws(p);
        if (*p == ':') p++;
        p = skip_ws(p);

        if (strcmp(key, "overrides") != 0) {
            /* Skip value — simple: skip until next key or end */
            int depth = 0;
            while (*p) {
                if (*p == '{' || *p == '[') depth++;
                else if (*p == '}' || *p == ']') {
                    if (depth == 0) break;
                    depth--;
                }
                else if (*p == ',' && depth == 0) break;
                else if (*p == '"') {
                    p++;
                    while (*p && *p != '"') { if (*p == '\\') p++; p++; }
                    if (*p == '"') p++;
                    continue;
                }
                p++;
            }
            continue;
        }

        /* Parse overrides array */
        if (*p != '[') break;
        p++;

        while (*p && s_num_overrides < MAX_OVERRIDES) {
            p = skip_ws(p);
            if (*p == ']') { p++; break; }
            if (*p == ',') { p++; continue; }
            if (*p != '{') break;
            p++;

            ChrOverrideEntry entry;
            memset(&entry, 0, sizeof(entry));

            /* Parse object fields */
            while (*p) {
                p = skip_ws(p);
                if (*p == '}') { p++; break; }
                if (*p == ',') { p++; continue; }

                char field[32] = "";
                p = parse_string(p, field, sizeof(field));
                if (!p) break;
                p = skip_ws(p);
                if (*p == ':') p++;
                p = skip_ws(p);

                if (strcmp(field, "offset") == 0) {
                    p = parse_int(p, &entry.offset);
                } else if (strcmp(field, "length") == 0) {
                    p = parse_int(p, &entry.length);
                } else if (strcmp(field, "file") == 0) {
                    p = parse_string(p, entry.file, sizeof(entry.file));
                    if (!p) break;
                } else {
                    /* Skip unknown value */
                    if (*p == '"') {
                        char tmp[256];
                        p = parse_string(p, tmp, sizeof(tmp));
                    } else {
                        while (*p && *p != ',' && *p != '}') p++;
                    }
                }
            }

            if (entry.file[0] == '\0') continue;

            /* Resolve file path relative to manifest dir */
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s",
                     s_manifest_dir, entry.file);

            /* Normalize path separators */
            for (char *c = full_path; *c; c++)
                if (*c == '\\') *c = '/';

            if (load_override_file(full_path, &entry) == 0) {
                /* Validate offset + length fits in CHR RAM */
                if (entry.offset < 0 || entry.offset >= 0x2000) {
                    fprintf(stderr, "[ChrOverride] Invalid offset 0x%X for %s\n",
                            entry.offset, entry.file);
                    free(entry.data);
                    continue;
                }
                if (entry.offset + entry.length > 0x2000) {
                    entry.length = 0x2000 - entry.offset;
                }

                /* Store resolved path and mtime for hot reload */
                snprintf(entry.full_path, sizeof(entry.full_path), "%s", full_path);
                StatBuf fst;
                entry.file_mtime = (STAT_CALL(full_path, &fst) == 0) ? fst.st_mtime : 0;

                s_overrides[s_num_overrides++] = entry;
                printf("[ChrOverride] Loaded: %s -> offset=0x%04X, %d bytes\n",
                       entry.file, entry.offset, entry.length);
            }
        }
        break; /* done with "overrides" */
    }

    return s_num_overrides;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void chr_override_init(void) {
    mapper_set_chr_callback(chr_callback, NULL);
    printf("[ChrOverride] Initialized — mapper callback registered\n");
}

void chr_override_set_dump(int enable) {
    s_dump_enabled = enable;
    if (enable)
        printf("[ChrOverride] Dump mode enabled — writing to %s/\n", s_dump_dir);
}

int chr_override_load_manifest(const char *dir) {
    snprintf(s_manifest_dir, sizeof(s_manifest_dir), "%s", dir);
    snprintf(s_manifest_path, sizeof(s_manifest_path), "%s/manifest.json", dir);

    /* Normalize separators */
    for (char *c = s_manifest_dir; *c; c++) if (*c == '\\') *c = '/';
    for (char *c = s_manifest_path; *c; c++) if (*c == '\\') *c = '/';

    StatBuf st;
    if (STAT_CALL(s_manifest_path, &st) != 0) {
        printf("[ChrOverride] No manifest found at %s (not an error)\n",
               s_manifest_path);
        return 0;
    }
    s_manifest_mtime = st.st_mtime;

    /* Read manifest */
    FILE *f = fopen(s_manifest_path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    int count = parse_manifest(buf);
    free(buf);

    printf("[ChrOverride] Loaded %d override(s) from %s\n",
           count, s_manifest_path);
    return count;
}

void chr_override_reload_if_changed(void) {
    if (s_manifest_path[0] == '\0') return;

    if (++s_reload_ticks < RELOAD_INTERVAL) return;
    s_reload_ticks = 0;

    /* Check manifest mtime */
    int changed = 0;
    StatBuf st;
    if (STAT_CALL(s_manifest_path, &st) == 0 && st.st_mtime != s_manifest_mtime) {
        changed = 1;
    }

    /* Check referenced file mtimes (detect PNG edits without manifest change) */
    if (!changed) {
        for (int i = 0; i < s_num_overrides; i++) {
            StatBuf fst;
            if (s_overrides[i].full_path[0] &&
                STAT_CALL(s_overrides[i].full_path, &fst) == 0 &&
                fst.st_mtime != s_overrides[i].file_mtime) {
                changed = 1;
                break;
            }
        }
    }

    if (!changed) return;

    printf("[ChrOverride] Change detected, reloading...\n");

    /* Update manifest mtime so we don't re-trigger next poll */
    if (STAT_CALL(s_manifest_path, &st) == 0)
        s_manifest_mtime = st.st_mtime;

    FILE *f = fopen(s_manifest_path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);

    int count = parse_manifest(buf);
    free(buf);

    printf("[ChrOverride] Reloaded %d override(s)\n", count);
}

void chr_override_get_dump_stats(int *unique_snapshots, int *total_switches) {
    if (unique_snapshots) *unique_snapshots = s_num_unique;
    if (total_switches) *total_switches = s_total_switches;
}

/* ── Per-frame snapshot (for CHR RAM games) ──────────────────────────────── */

void chr_override_frame_snapshot(void) {
    if (s_dump_enabled)
        dump_snapshot(g_chr_ram, sizeof(g_chr_ram));

    apply_overrides(g_chr_ram, sizeof(g_chr_ram));
}

/* ── Mapper callback (for CHR ROM games) ─────────────────────────────────── */

static void chr_callback(uint8_t *chr_data, int size, void *ctx) {
    (void)ctx;

    if (s_dump_enabled)
        dump_snapshot(chr_data, size);

    apply_overrides(chr_data, size);
}

/* ── Override application ─────────────────────────────────────────────────── */

static void apply_overrides(uint8_t *chr_data, int size) {
    for (int i = 0; i < s_num_overrides; i++) {
        ChrOverrideEntry *e = &s_overrides[i];
        if (!e->data) continue;
        if (e->offset >= size) continue;

        int len = e->length;
        if (e->offset + len > size)
            len = size - e->offset;

        memcpy(chr_data + e->offset, e->data, len);
    }
}

static void free_overrides(void) {
    for (int i = 0; i < s_num_overrides; i++) {
        free(s_overrides[i].data);
        s_overrides[i].data = NULL;
    }
    s_num_overrides = 0;
}

/* ── Dump implementation ──────────────────────────────────────────────────── */

static void ensure_dump_dir(void) {
    if (s_dump_dir_created) return;
    MKDIR(s_dump_dir);
    s_dump_dir_created = 1;

    char path[512];
    snprintf(path, sizeof(path), "%s/index.csv", s_dump_dir);
    s_index_file = fopen(path, "w");
    if (s_index_file) {
        fprintf(s_index_file, "snapshot,crc32,frame,size\n");
        fflush(s_index_file);
    }
}

static int crc_already_seen(uint32_t crc) {
    for (int i = 0; i < s_num_unique; i++) {
        if (s_seen_crcs[i] == crc) return 1;
    }
    return 0;
}

static void dump_snapshot(const uint8_t *data, int size) {
    s_total_switches++;

    uint32_t crc = crc32_compute(data, size);
    if (crc_already_seen(crc)) return;

    ensure_dump_dir();

    if (s_num_unique >= MAX_UNIQUE_SNAPSHOTS) {
        if (s_num_unique == MAX_UNIQUE_SNAPSHOTS)
            printf("[ChrOverride] Warning: max unique snapshots (%d) reached\n",
                   MAX_UNIQUE_SNAPSHOTS);
        return;
    }

    s_seen_crcs[s_num_unique] = crc;
    int idx = s_num_unique++;

    char path[512];
    snprintf(path, sizeof(path), "%s/snapshot_%04d.bin", s_dump_dir, idx);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, size, f);
        fclose(f);
    }

    if (s_index_file) {
        fprintf(s_index_file, "%d,0x%08X,%llu,%d\n",
                idx, crc, (unsigned long long)g_frame_count, size);
        fflush(s_index_file);
    }

    printf("[ChrOverride] Dump: snapshot_%04d.bin (CRC=0x%08X, frame=%llu)\n",
           idx, crc, (unsigned long long)g_frame_count);
}
