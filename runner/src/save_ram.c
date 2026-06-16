/*
 * save_ram.c — generic battery-SRAM persistence (see save_ram.h).
 *
 * Mirrors g_sram ($6000-$7FFF, 8 KB) to <exe_dir>/saves/<title>.srm. Loaded on
 * boot, dirty-checked flush on a timer + atexit, import/clear for the launcher.
 */
#include "save_ram.h"
#include "nes_runtime.h"   /* extern uint8_t g_sram[0x2000]; */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#  define save_ram_mkdir(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define save_ram_mkdir(p) mkdir((p), 0755)
#endif

#define SRAM_LEN 0x2000

/* ---- state ---- */
static int      s_active = 0;                /* runtime persistence on (load/flush) */
static int      s_bound = 0;                 /* s_path resolved (runtime OR UI bind) */
static int      s_requested = 0;             /* synthetic-SRAM opt-in via request */
static char     s_basename[256] = "";        /* pinned stem (request or sanitized) */
static char     s_legacy[1024] = "";         /* optional old save path to migrate */
static char     s_path[1024] = "";           /* saves/<title>.srm (absolute) */
static uint8_t  s_snapshot[SRAM_LEN];        /* last-flushed bytes (dirty check) */
static unsigned s_tick = 0;
static int      s_atexit_registered = 0;

/* ---- exe-dir helper ---- */

/* Directory containing the running executable, with trailing separator.
 * Falls back to "./" when the path can't be determined. */
static void exe_dir(char *out, size_t max_len) {
#ifdef _WIN32
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, p, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        char *sep = strrchr(p, '\\');
        if (!sep) sep = strrchr(p, '/');
        if (sep) { *(sep + 1) = '\0'; snprintf(out, max_len, "%s", p); return; }
    }
#endif
    snprintf(out, max_len, "./");
}

/* Lowercase a-z0-9_- filename stem from an arbitrary game name. */
static void sanitize_stem(const char *name, char *out, size_t max_len) {
    size_t j = 0;
    for (size_t i = 0; name && name[i] && j + 1 < max_len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c))            out[j++] = (char)tolower(c);
        else if (c == '-' || c == '_') out[j++] = (char)c;
        else if (c == ' ')         out[j++] = '_';
        /* drop everything else (punctuation, etc.) */
    }
    if (j == 0 && max_len > 1) { /* degenerate name -> a usable default */
        snprintf(out, max_len, "game");
        return;
    }
    out[j] = '\0';
}

/* ---- file I/O ---- */

static int read_file_into(const char *path, uint8_t *dst, size_t len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(dst, 1, len, f);
    fclose(f);
    return n > 0;
}

static int write_bytes(const char *path, const uint8_t *src, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = fwrite(src, 1, len, f);
    fclose(f);
    return n == len;
}

/* Copy file src -> dst (binary). Returns 1 on success. */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return 0;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }
    char buf[4096];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    fclose(in);
    fclose(out);
    return ok;
}

static void backup_path(char *out, size_t max_len) {
    snprintf(out, max_len, "%s.bak", s_path);
}

/* ---- public ---- */

void save_ram_request_enable(const char *basename) {
    s_requested = 1;
    if (basename && *basename)
        snprintf(s_basename, sizeof(s_basename), "%s", basename);
}

void save_ram_set_legacy_path(const char *legacy_abs) {
    if (legacy_abs && *legacy_abs)
        snprintf(s_legacy, sizeof(s_legacy), "%s", legacy_abs);
}

/* Resolve s_path = <exe_dir>/saves/<stem>.srm (creating saves/) and mark bound.
 * stem: explicit basename, else the pinned s_basename, else sanitized default. */
static void resolve_path(const char *basename, const char *default_title) {
    if (basename && *basename)
        snprintf(s_basename, sizeof(s_basename), "%s", basename);
    if (s_basename[0] == '\0')
        sanitize_stem(default_title ? default_title : "game",
                      s_basename, sizeof(s_basename));

    char dir[1024];
    exe_dir(dir, sizeof(dir));
    char saves_dir[1024];
    snprintf(saves_dir, sizeof(saves_dir), "%ssaves", dir);
    save_ram_mkdir(saves_dir);   /* ignore EEXIST */
    snprintf(s_path, sizeof(s_path), "%s/%s.srm", saves_dir, s_basename);
    s_bound = 1;
}

void save_ram_ui_bind(const char *basename) {
    // Sanitize the game name into the stem exactly as save_ram_init() does, so the
    // launcher and the runtime resolve the identical saves/<stem>.srm. (Passed as
    // default_title, not the explicit-basename slot, so it gets sanitized.)
    resolve_path(NULL, basename);       /* UI only: path, no load/flush/atexit */
}

void save_ram_init(const char *default_title, int battery_bit) {
    s_active = 0;
    if (!s_requested && !battery_bit) return;   /* NONE backend */

    resolve_path(NULL, default_title);

    /* One-time migration: if there's no save at the new path yet but a legacy
     * file exists, pull it into place so existing saves carry over. */
    if (s_legacy[0]) {
        FILE *probe = fopen(s_path, "rb");
        if (probe) {
            fclose(probe);
        } else if (copy_file(s_legacy, s_path)) {
            printf("[SRAM] Migrated legacy save %s -> %s\n", s_legacy, s_path);
        }
    }

    /* Load existing save into g_sram, or snapshot the fresh (0xFF) state. */
    if (read_file_into(s_path, g_sram, SRAM_LEN))
        printf("[SRAM] Loaded %s\n", s_path);
    else
        printf("[SRAM] No save yet (%s) — starting fresh\n", s_path);
    memcpy(s_snapshot, g_sram, SRAM_LEN);

    if (!s_atexit_registered) { atexit(save_ram_flush); s_atexit_registered = 1; }
    s_active = 1;
}

void save_ram_flush(void) {
    if (!s_active) return;
    if (memcmp(g_sram, s_snapshot, SRAM_LEN) == 0) return;   /* not dirty */
    if (write_bytes(s_path, g_sram, SRAM_LEN)) {
        memcpy(s_snapshot, g_sram, SRAM_LEN);
        printf("[SRAM] Saved %s\n", s_path);
    } else {
        fprintf(stderr, "[SRAM] Failed to write %s\n", s_path);
    }
}

void save_ram_tick(void) {
    if (!s_active) return;
    if ((++s_tick % 60) != 0) return;   /* ~1 Hz dirty check */
    save_ram_flush();
}

int save_ram_active(void) { return s_active; }

/* path/exists/size/import/clear gate on s_bound so the pre-boot launcher (UI
 * bind, no s_active) can manage the file too. */
const char *save_ram_path(void) { return s_bound ? s_path : ""; }

int save_ram_exists(void) {
    if (!s_bound) return 0;
    FILE *f = fopen(s_path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

long save_ram_size(void) {
    if (!s_bound) return 0;
    FILE *f = fopen(s_path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz < 0 ? 0 : sz;
}

int save_ram_import(const char *src_path) {
    if (!s_bound || !src_path || !*src_path) return 0;
    char bak[1100];
    backup_path(bak, sizeof(bak));
    if (save_ram_exists()) copy_file(s_path, bak);   /* best-effort backup */
    if (!copy_file(src_path, s_path)) {
        fprintf(stderr, "[SRAM] Import failed: %s -> %s\n", src_path, s_path);
        return 0;
    }
    if (read_file_into(s_path, g_sram, SRAM_LEN))
        memcpy(s_snapshot, g_sram, SRAM_LEN);
    printf("[SRAM] Imported %s\n", src_path);
    return 1;
}

int save_ram_clear(void) {
    if (!s_bound) return 0;
    char bak[1100];
    backup_path(bak, sizeof(bak));
    if (save_ram_exists()) { copy_file(s_path, bak); remove(s_path); }
    memset(g_sram, 0xFF, SRAM_LEN);
    memcpy(s_snapshot, g_sram, SRAM_LEN);
    printf("[SRAM] Cleared %s\n", s_path);
    return 1;
}
