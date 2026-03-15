/*
 * launcher.c — ROM discovery, CRC32 verification, and main() entry point.
 *
 * Responsibilities:
 *   1. Accept ROM path from argv[1] (backwards-compatible).
 *   2. If no argv[1]: check rom.cfg next to the exe for the last-used path.
 *   3. If still no path: open a Windows file-picker dialog.
 *   4. CRC32-verify the file against game_get_expected_crc32() (skip if 0).
 *   5. On success: persist path to rom.cfg, then call nesrecomp_runner_run().
 *
 * rom.cfg is stored in the same directory as the exe (GetModuleFileNameA).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <commdlg.h>
#  pragma comment(lib, "comdlg32.lib")
#endif

#include "game_extras.h"
#include "crc32.h"

/* Declared in main_runner.c */
void nesrecomp_runner_run(int argc, char **argv);

/* ---- rom.cfg helpers ---- */

/* Build path: <exe_dir>/rom.cfg */
static void get_rom_cfg_path(char *out, int max_len) {
#ifdef _WIN32
    char exe_path[MAX_PATH];
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    /* strip filename, keep trailing backslash */
    char *last_sep = strrchr(exe_path, '\\');
    if (last_sep) *(last_sep + 1) = '\0';
    snprintf(out, max_len, "%srom.cfg", exe_path);
#else
    /* Fallback: current directory */
    snprintf(out, max_len, "rom.cfg");
#endif
}

static void rom_cfg_read(char *path_out, int max_len) {
    char cfg_path[512];
    get_rom_cfg_path(cfg_path, sizeof(cfg_path));
    FILE *f = fopen(cfg_path, "r");
    if (!f) { path_out[0] = '\0'; return; }
    if (!fgets(path_out, max_len, f)) path_out[0] = '\0';
    fclose(f);
    /* strip trailing newline */
    int len = (int)strlen(path_out);
    while (len > 0 && (path_out[len-1] == '\n' || path_out[len-1] == '\r'))
        path_out[--len] = '\0';
}

static void rom_cfg_write(const char *rom_path) {
    char cfg_path[512];
    get_rom_cfg_path(cfg_path, sizeof(cfg_path));
    FILE *f = fopen(cfg_path, "w");
    if (!f) return;
    fprintf(f, "%s\n", rom_path);
    fclose(f);
}

/* ---- File picker ---- */

/* Returns 1 on success (path written to out), 0 on cancel. */
static int pick_rom_file(char *out, int max_len) {
#ifdef _WIN32
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    out[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = NULL;
    ofn.lpstrFilter = "NES ROMs (*.nes)\0*.nes\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = out;
    ofn.nMaxFile    = (DWORD)max_len;
    ofn.lpstrTitle  = "Select NES ROM";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
#else
    fprintf(stderr, "[Launcher] No ROM specified and no file picker available on this platform.\n");
    fprintf(stderr, "Usage: %s <rom.nes>\n", "NESRecompGame");
    return 0;
#endif
}

/* ---- CRC32 verification ---- */

static int verify_rom(const char *path, uint32_t expected_crc) {
    if (expected_crc == 0) return 1; /* skip */

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[Launcher] Cannot open '%s'\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(f); return 0; }
    fread(data, 1, (size_t)sz, f);
    fclose(f);

    uint32_t actual = crc32_compute(data, (size_t)sz);
    free(data);

    if (actual != expected_crc) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "ROM CRC32 mismatch!\n\nExpected: %08X\nGot:      %08X\n\n"
            "Please select the correct ROM file.",
            expected_crc, actual);
        fprintf(stderr, "[Launcher] %s\n", msg);
#ifdef _WIN32
        MessageBoxA(NULL, msg, "Wrong ROM", MB_ICONWARNING | MB_OK);
#endif
        return 0;
    }
    return 1;
}

/* ---- main ---- */

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    static char rom_path[512];
    uint32_t expected_crc = game_get_expected_crc32();

    if (argc >= 2) {
        /* Backwards-compatible: ROM path given on command line */
        strncpy(rom_path, argv[1], sizeof(rom_path) - 1);
        /* Still verify CRC, but don't re-prompt on mismatch — just warn */
        if (expected_crc != 0 && !verify_rom(rom_path, expected_crc)) {
            fprintf(stderr, "[Launcher] Warning: CRC mismatch for '%s' — continuing anyway\n",
                    rom_path);
        }
    } else {
        /* Try rom.cfg first */
        rom_cfg_read(rom_path, sizeof(rom_path));

        int valid = 0;
        while (!valid) {
            if (rom_path[0] == '\0') {
                /* No saved path — open picker */
                if (!pick_rom_file(rom_path, sizeof(rom_path))) {
                    fprintf(stderr, "[Launcher] No ROM selected — exiting.\n");
                    return 1;
                }
            }

            /* Verify the ROM */
            if (verify_rom(rom_path, expected_crc)) {
                valid = 1;
            } else {
                /* Wrong file — clear path and pick again */
                rom_path[0] = '\0';
            }
        }

        rom_cfg_write(rom_path);
        printf("[Launcher] ROM: %s\n", rom_path);
    }

    /* Re-build argv so that argv[1] == rom_path for the runner */
    char *new_argv[64];
    int new_argc = 0;
    new_argv[new_argc++] = argv[0];
    new_argv[new_argc++] = rom_path;
    for (int i = 2; i < argc && new_argc < 63; i++)
        new_argv[new_argc++] = argv[i];
    new_argv[new_argc] = NULL;

    nesrecomp_runner_run(new_argc, new_argv);
    return 0;
}
