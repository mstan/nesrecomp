/*
 * override_chr.c — CHR override and dump system implementation.
 *
 * Tracks individual CHR RAM transfers (sequences of $2007 writes after
 * $2006 sets an address in $0000-$1FFF).  Each transfer is a discrete
 * game asset (e.g. player sprites, font tiles, background tileset).
 *
 * Dump mode: writes each unique transfer as a .bin + .png to chr_dump/.
 * Override mode: loads manifest.json mapping assets to replacement PNGs.
 */
#include "override_chr.h"
#include "chr_codec.h"  /* chr_write_png, chr_load_cached */
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

static int s_active = 0;
static int s_dump_enabled = 0;
static int s_dump_dir_created = 0;
static const char *s_dump_dir = "chr_dump";
static FILE *s_index_file = NULL;

/* ── Transfer tracking ────────────────────────────────────────────────────
 * A "transfer" is a contiguous run of $2007 writes to CHR RAM that
 * started after $2006 set a CHR-range address.  The game typically does:
 *   STA $2006 (high byte)
 *   STA $2006 (low byte)
 *   loop: LDA (ptr),Y / STA $2007 / INY / ...
 * Each such loop is one transfer = one asset. */

#define XFER_BUF_SIZE 0x2000 /* max CHR RAM size */

static int      s_xfer_active = 0;
static uint16_t s_xfer_start_addr = 0;  /* PPU address where transfer began */
static uint16_t s_xfer_next_addr = 0;   /* expected next $2007 address */
static uint8_t  s_xfer_buf[XFER_BUF_SIZE];
static int      s_xfer_len = 0;
static int      s_xfer_increment = 1;   /* 1 or 32, from PPUCTRL bit 2 */

/* ── Known assets (deduplication) ─────────────────────────────────────────── */

#define MAX_KNOWN_ASSETS 2048

typedef struct {
    uint16_t ppu_addr;     /* destination in CHR RAM */
    int      length;       /* bytes */
    uint32_t crc;          /* content hash */
    int      dump_idx;     /* index in dump output (for filename) */
} KnownAsset;

static KnownAsset s_known[MAX_KNOWN_ASSETS];
static int        s_num_known = 0;
static int        s_total_transfers = 0;

/* ── Override state ───────────────────────────────────────────────────────── */

#define MAX_OVERRIDES 256

typedef struct {
    uint16_t ppu_addr;      /* CHR RAM destination to match */
    int      length;        /* expected transfer length (0 = any) */
    uint32_t match_crc;     /* CRC to match (0 = match by addr only) */
    uint8_t *data;          /* replacement CHR data (owned) */
    int      data_len;      /* replacement data length */
    char     file[256];     /* source file (for display) */
    char     full_path[512];
    time_t   file_mtime;
} ChrOverrideEntry;

static ChrOverrideEntry s_overrides[MAX_OVERRIDES];
static int              s_num_overrides = 0;
static char             s_manifest_dir[512] = "";
static char             s_manifest_path[512] = "";
static time_t           s_manifest_mtime = 0;
static int              s_reload_ticks = 0;
#define RELOAD_INTERVAL 60

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void flush_transfer(void);
static void record_asset(uint16_t ppu_addr, const uint8_t *data, int len);
static void apply_override_for_transfer(uint16_t ppu_addr, const uint8_t *data, int len);
static void chr_rom_callback(uint8_t *chr_data, int size, void *ctx);
static void free_overrides(void);

/* ── JSON parser (same minimal approach) ──────────────────────────────────── */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

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

static const char *parse_int_val(const char *p, int *out) {
    char *end;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        *out = (int)strtol(p, &end, 16);
    else
        *out = (int)strtol(p, &end, 10);
    return end;
}

static int load_override_file(const char *file_path, ChrOverrideEntry *entry) {
    const char *ext = strrchr(file_path, '.');
    if (ext && (strcmp(ext, ".png") == 0 || strcmp(ext, ".PNG") == 0)) {
        uint8_t *data = NULL;
        int size = 0;
        if (chr_load_cached(file_path, &data, &size) != 0)
            return -1;
        entry->data = data;
        entry->data_len = size;
        return 0;
    }

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
    entry->data_len = (int)sz;
    return 0;
}

/*
 * Manifest format:
 * {
 *   "overrides": [
 *     {
 *       "ppu_addr": "0x0400",
 *       "length": 512,
 *       "crc": "0xABCD1234",
 *       "file": "david_sprites.png"
 *     }
 *   ]
 * }
 *
 * ppu_addr = CHR RAM destination address to match (required)
 * length   = transfer length to match (optional, 0 = any)
 * crc      = content CRC to match (optional, 0 = match by addr+length only)
 * file     = replacement PNG or .bin (required)
 */
static int parse_manifest(const char *json_text) {
    free_overrides();

    const char *p = skip_ws(json_text);
    if (*p != '{') return -1;
    p++;

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
            /* Skip value */
            int depth = 0;
            while (*p) {
                if (*p == '{' || *p == '[') depth++;
                else if (*p == '}' || *p == ']') { if (depth == 0) break; depth--; }
                else if (*p == ',' && depth == 0) break;
                else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\') p++; p++; } if (*p == '"') p++; continue; }
                p++;
            }
            continue;
        }

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

                if (strcmp(field, "ppu_addr") == 0) {
                    if (*p == '"') {
                        char tmp[32]; p = parse_string(p, tmp, sizeof(tmp));
                        entry.ppu_addr = (uint16_t)strtol(tmp, NULL, 0);
                    } else {
                        int v; p = parse_int_val(p, &v); entry.ppu_addr = (uint16_t)v;
                    }
                } else if (strcmp(field, "length") == 0) {
                    int v; p = parse_int_val(p, &v); entry.length = v;
                } else if (strcmp(field, "crc") == 0) {
                    if (*p == '"') {
                        char tmp[32]; p = parse_string(p, tmp, sizeof(tmp));
                        entry.match_crc = (uint32_t)strtoul(tmp, NULL, 0);
                    } else {
                        entry.match_crc = (uint32_t)strtoul(p, (char **)&p, 0);
                    }
                } else if (strcmp(field, "file") == 0) {
                    p = parse_string(p, entry.file, sizeof(entry.file));
                    if (!p) break;
                } else {
                    if (*p == '"') { char tmp[256]; p = parse_string(p, tmp, sizeof(tmp)); }
                    else { while (*p && *p != ',' && *p != '}') p++; }
                }
            }

            if (entry.file[0] == '\0') continue;

            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", s_manifest_dir, entry.file);
            for (char *c = full_path; *c; c++) if (*c == '\\') *c = '/';

            if (load_override_file(full_path, &entry) == 0) {
                snprintf(entry.full_path, sizeof(entry.full_path), "%s", full_path);
                StatBuf fst;
                entry.file_mtime = (STAT_CALL(full_path, &fst) == 0) ? fst.st_mtime : 0;

                s_overrides[s_num_overrides++] = entry;
                printf("[ChrOverride] Override: %s -> ppu_addr=$%04X",
                       entry.file, entry.ppu_addr);
                if (entry.length) printf(", len=%d", entry.length);
                if (entry.match_crc) printf(", crc=0x%08X", entry.match_crc);
                printf(" (%d bytes)\n", entry.data_len);
            }
        }
        break;
    }

    return s_num_overrides;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int chr_override_active(void) { return s_active; }

void chr_override_init(void) {
    s_active = 1;
    mapper_set_chr_callback(chr_rom_callback, NULL);
    printf("[ChrOverride] Initialized\n");
}

void chr_override_set_dump(int enable) {
    s_dump_enabled = enable;
    if (enable)
        printf("[ChrOverride] Dump mode enabled — writing to %s/\n", s_dump_dir);
}

int chr_override_load_manifest(const char *dir) {
    snprintf(s_manifest_dir, sizeof(s_manifest_dir), "%s", dir);
    snprintf(s_manifest_path, sizeof(s_manifest_path), "%s/manifest.json", dir);
    for (char *c = s_manifest_dir; *c; c++) if (*c == '\\') *c = '/';
    for (char *c = s_manifest_path; *c; c++) if (*c == '\\') *c = '/';

    StatBuf st;
    if (STAT_CALL(s_manifest_path, &st) != 0) {
        printf("[ChrOverride] No manifest at %s\n", s_manifest_path);
        return 0;
    }
    s_manifest_mtime = st.st_mtime;

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
    printf("[ChrOverride] Loaded %d override(s) from %s\n", count, s_manifest_path);
    return count;
}

void chr_override_reload_if_changed(void) {
    if (s_manifest_path[0] == '\0') return;
    if (++s_reload_ticks < RELOAD_INTERVAL) return;
    s_reload_ticks = 0;

    int changed = 0;
    StatBuf st;
    if (STAT_CALL(s_manifest_path, &st) == 0 && st.st_mtime != s_manifest_mtime)
        changed = 1;

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
    if (unique_snapshots) *unique_snapshots = s_num_known;
    if (total_switches) *total_switches = s_total_transfers;
}

/* ── Transfer tracking (called from runtime.c) ───────────────────────────── */

void chr_override_on_ppuaddr(uint16_t new_addr) {
    /* Flush any in-progress transfer before starting a new one */
    if (s_xfer_active && s_xfer_len > 0)
        flush_transfer();

    if (new_addr < 0x2000) {
        s_xfer_active = 1;
        s_xfer_start_addr = new_addr;
        s_xfer_next_addr = new_addr;
        s_xfer_len = 0;
        s_xfer_increment = (g_ppuctrl & 0x04) ? 32 : 1;
    } else {
        s_xfer_active = 0;
    }
}

void chr_override_on_chr_write(uint16_t addr, uint8_t val) {
    if (!s_xfer_active) {
        /* Unexpected CHR write without a tracked transfer.
         * Start a new one from this address. */
        s_xfer_active = 1;
        s_xfer_start_addr = addr;
        s_xfer_next_addr = addr;
        s_xfer_len = 0;
        s_xfer_increment = (g_ppuctrl & 0x04) ? 32 : 1;
    }

    /* Check for discontinuity — if the address isn't what we expected,
     * flush the current transfer and start a new one. */
    if (addr != s_xfer_next_addr && s_xfer_len > 0) {
        flush_transfer();
        s_xfer_start_addr = addr;
        s_xfer_next_addr = addr;
        s_xfer_len = 0;
        s_xfer_increment = (g_ppuctrl & 0x04) ? 32 : 1;
    }

    if (s_xfer_len < XFER_BUF_SIZE)
        s_xfer_buf[s_xfer_len++] = val;

    s_xfer_next_addr = addr + s_xfer_increment;
}

void chr_override_frame_end(void) {
    if (s_xfer_active && s_xfer_len > 0)
        flush_transfer();
    s_xfer_active = 0;
}

/* ── CHR ROM callback (for mapper bank switch games) ──────────────────────
 * For CHR ROM games the whole 8KB is swapped at once by the mapper.
 * We treat that as a single "transfer" of the entire pattern table. */

static void chr_rom_callback(uint8_t *chr_data, int size, void *ctx) {
    (void)ctx;
    record_asset(0x0000, chr_data, size);
    apply_override_for_transfer(0x0000, chr_data, size);
}

/* ── Transfer flush & asset recording ─────────────────────────────────────── */

static void flush_transfer(void) {
    if (s_xfer_len == 0) return;

    record_asset(s_xfer_start_addr, s_xfer_buf, s_xfer_len);

    /* Apply override: if there's a matching override, patch g_chr_ram */
    apply_override_for_transfer(s_xfer_start_addr, s_xfer_buf, s_xfer_len);

    s_xfer_len = 0;
}

/* ── Dump helpers ─────────────────────────────────────────────────────────── */

static void ensure_dump_dir(void) {
    if (s_dump_dir_created) return;
    MKDIR(s_dump_dir);
    s_dump_dir_created = 1;

    char path[512];
    snprintf(path, sizeof(path), "%s/index.csv", s_dump_dir);
    s_index_file = fopen(path, "w");
    if (s_index_file) {
        fprintf(s_index_file, "idx,ppu_addr,length,crc32,frame\n");
        fflush(s_index_file);
    }
}

static void write_dump_manifest(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/manifest.json", s_dump_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "{\n  \"overrides\": [\n");
    for (int i = 0; i < s_num_known; i++) {
        KnownAsset *a = &s_known[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"ppu_addr\": \"0x%04X\",\n", a->ppu_addr);
        fprintf(f, "      \"length\": %d,\n", a->length);
        fprintf(f, "      \"crc\": \"0x%08X\",\n", a->crc);
        fprintf(f, "      \"file\": \"asset_%04d_addr%04X.png\"\n", a->dump_idx, a->ppu_addr);
        fprintf(f, "    }%s\n", (i < s_num_known - 1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

static void record_asset(uint16_t ppu_addr, const uint8_t *data, int len) {
    s_total_transfers++;

    /* Only track transfers that are tile-aligned (multiple of 16 bytes) */
    if (len < 16) return;

    uint32_t crc = crc32_compute(data, len);

    /* Check if we already know this exact asset */
    for (int i = 0; i < s_num_known; i++) {
        if (s_known[i].ppu_addr == ppu_addr &&
            s_known[i].length == len &&
            s_known[i].crc == crc)
            return; /* already seen */
    }

    if (!s_dump_enabled) return;

    ensure_dump_dir();

    if (s_num_known >= MAX_KNOWN_ASSETS) {
        if (s_num_known == MAX_KNOWN_ASSETS)
            printf("[ChrOverride] Warning: max assets (%d) reached\n", MAX_KNOWN_ASSETS);
        return;
    }

    int idx = s_num_known;
    s_known[idx].ppu_addr = ppu_addr;
    s_known[idx].length = len;
    s_known[idx].crc = crc;
    s_known[idx].dump_idx = idx;
    s_num_known++;

    /* Write .bin */
    char path[512];
    snprintf(path, sizeof(path), "%s/asset_%04d_addr%04X.bin",
             s_dump_dir, idx, ppu_addr);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(data, 1, len, f); fclose(f); }

    /* Write .png (only if tile-aligned) */
    if (len % 16 == 0) {
        snprintf(path, sizeof(path), "%s/asset_%04d_addr%04X.png",
                 s_dump_dir, idx, ppu_addr);
        chr_write_png(path, data, len);
    }

    /* Update index CSV */
    if (s_index_file) {
        fprintf(s_index_file, "%d,0x%04X,%d,0x%08X,%llu\n",
                idx, ppu_addr, len, crc, (unsigned long long)g_frame_count);
        fflush(s_index_file);
    }

    /* Update manifest */
    write_dump_manifest();

    printf("[ChrOverride] Asset #%d: $%04X +%d bytes (CRC=0x%08X, frame=%llu)\n",
           idx, ppu_addr, len, crc, (unsigned long long)g_frame_count);
}

/* ── Override application ─────────────────────────────────────────────────── */

static void apply_override_for_transfer(uint16_t ppu_addr, const uint8_t *data, int len) {
    uint32_t crc = crc32_compute(data, len);

    for (int i = 0; i < s_num_overrides; i++) {
        ChrOverrideEntry *e = &s_overrides[i];
        if (!e->data) continue;

        /* Match by ppu_addr */
        if (e->ppu_addr != ppu_addr) continue;

        /* Match by length if specified */
        if (e->length > 0 && e->length != len) continue;

        /* Match by CRC if specified */
        if (e->match_crc != 0 && e->match_crc != crc) continue;

        /* Apply: overwrite g_chr_ram at the transfer destination */
        int copy_len = e->data_len;
        if (ppu_addr + copy_len > 0x2000)
            copy_len = 0x2000 - ppu_addr;

        memcpy(g_chr_ram + ppu_addr, e->data, copy_len);
        break; /* first match wins */
    }
}

static void free_overrides(void) {
    for (int i = 0; i < s_num_overrides; i++) {
        free(s_overrides[i].data);
        s_overrides[i].data = NULL;
    }
    s_num_overrides = 0;
}
