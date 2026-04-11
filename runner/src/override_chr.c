/*
 * override_chr.c — CHR override and dump system implementation.
 *
 * Phase 1: dump mode — snapshots of g_chr_ram written to chr_dump/ on
 * each bank switch, deduplicated by CRC32.
 */
#include "override_chr.h"
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

/* Track unique CHR snapshots by CRC32 */
#define MAX_UNIQUE_SNAPSHOTS 1024
static uint32_t s_seen_crcs[MAX_UNIQUE_SNAPSHOTS];
static int      s_num_unique = 0;
static int      s_total_switches = 0;

static FILE *s_index_file = NULL;
static int   s_dump_dir_created = 0;

static const char *s_dump_dir = "chr_dump";

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void chr_callback(uint8_t *chr_data, int size, void *ctx);
static void dump_snapshot(const uint8_t *data, int size);

/* ── Public API ───────────────────────────────────────────────────────────── */

void chr_override_init(void) {
    mapper_set_chr_callback(chr_callback, NULL);
    printf("[ChrOverride] Initialized — mapper callback registered\n");
}

void chr_override_set_dump(int enable) {
    s_dump_enabled = enable;
    if (enable) {
        printf("[ChrOverride] Dump mode enabled — writing to %s/\n", s_dump_dir);
    }
}

int chr_override_load_manifest(const char *dir) {
    /* Phase 3 — stub for now */
    (void)dir;
    return 0;
}

void chr_override_reload_if_changed(void) {
    /* Phase 4 — stub for now */
}

void chr_override_get_dump_stats(int *unique_snapshots, int *total_switches) {
    if (unique_snapshots) *unique_snapshots = s_num_unique;
    if (total_switches) *total_switches = s_total_switches;
}

/* ── Mapper callback ──────────────────────────────────────────────────────── */

static void chr_callback(uint8_t *chr_data, int size, void *ctx) {
    (void)ctx;

    if (s_dump_enabled)
        dump_snapshot(chr_data, size);

    /* Phase 3: apply overrides here (after dump, before render) */
}

/* ── Dump implementation ──────────────────────────────────────────────────── */

static void ensure_dump_dir(void) {
    if (s_dump_dir_created) return;
    MKDIR(s_dump_dir);
    s_dump_dir_created = 1;

    /* Open index CSV */
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

    /* New unique snapshot */
    ensure_dump_dir();

    if (s_num_unique >= MAX_UNIQUE_SNAPSHOTS) {
        if (s_num_unique == MAX_UNIQUE_SNAPSHOTS)
            printf("[ChrOverride] Warning: max unique snapshots (%d) reached\n",
                   MAX_UNIQUE_SNAPSHOTS);
        return;
    }

    s_seen_crcs[s_num_unique] = crc;
    int idx = s_num_unique++;

    /* Write binary dump */
    char path[512];
    snprintf(path, sizeof(path), "%s/snapshot_%04d.bin", s_dump_dir, idx);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, size, f);
        fclose(f);
    }

    /* Update index */
    if (s_index_file) {
        fprintf(s_index_file, "%d,0x%08X,%llu,%d\n",
                idx, crc, (unsigned long long)g_frame_count, size);
        fflush(s_index_file);
    }

    printf("[ChrOverride] Dump: snapshot_%04d.bin (CRC=0x%08X, frame=%llu)\n",
           idx, crc, (unsigned long long)g_frame_count);
}
