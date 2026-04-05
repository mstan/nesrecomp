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
#include "nes_runtime.h"

/* Declared in main_runner.c */
void nesrecomp_runner_run(int argc, char **argv);

char g_exe_dir[260] = ".";

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

    /* Skip 16-byte iNES header so different header variants of the
       same ROM (NES 2.0 vs iNES 1.0) produce the same CRC32. */
    size_t hdr = 16;
    if ((size_t)sz <= hdr) { free(data); return 0; }
    uint32_t actual = crc32_compute(data + hdr, (size_t)sz - hdr);
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

/* ---- Crash handler (Windows SEH + VEH) ---- */
#ifdef _WIN32
#include <dbghelp.h>   /* StackWalk64 — link with dbghelp.lib */
#pragma comment(lib, "dbghelp.lib")

static void atexit_handler(void) {
    extern const char *g_recomp_stack[];
    extern int g_recomp_stack_top;
    extern uint64_t g_frame_count;
    /* Only log if the game exited unexpectedly (recomp stack still active) */
    if (g_recomp_stack_top > 0) {
        printf("[EXIT] Unexpected exit at frame %llu, recomp stack (top=%d):\n",
               (unsigned long long)g_frame_count, g_recomp_stack_top);
        for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 20; i--)
            printf("  [%d] %s\n", i, g_recomp_stack[i] ? g_recomp_stack[i] : "?");
        fflush(stdout);
    }
}

static LONG WINAPI vectored_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    /* Skip benign exceptions (breakpoints, C++ exceptions, etc.) */
    if (code == EXCEPTION_BREAKPOINT || code == 0x406D1388 /* SetThreadName */
        || code == 0xE06D7363 /* C++ exception */)
        return EXCEPTION_CONTINUE_SEARCH;
    printf("\n[VEH] Exception 0x%08lX at %p\n", code, ep->ExceptionRecord->ExceptionAddress);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        printf("[VEH] Access violation: %s of %p\n",
               ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
               (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    if (code == EXCEPTION_STACK_OVERFLOW)
        printf("[VEH] STACK OVERFLOW detected\n");
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    /* Write crash info next to the exe */
    char path[512];
    GetModuleFileNameA(NULL, path, sizeof(path));
    char *s = strrchr(path, '\\');
    if (s) *(s + 1) = '\0';
    strcat(path, "crash_report.txt");

    FILE *f = fopen(path, "w");
    if (!f) f = stderr;

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    fprintf(f, "=== CRASH ===\n");
    fprintf(f, "Exception: 0x%08lX at %p\n", code, addr);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        const char *op = ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ";
        fprintf(f, "Access violation: %s of address %p\n",
                op, (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    CONTEXT *ctx = ep->ContextRecord;
    fprintf(f, "RIP=%p RSP=%p RBP=%p\n", (void *)ctx->Rip, (void *)ctx->Rsp, (void *)ctx->Rbp);
    fprintf(f, "RAX=%p RBX=%p RCX=%p RDX=%p\n",
            (void *)ctx->Rax, (void *)ctx->Rbx, (void *)ctx->Rcx, (void *)ctx->Rdx);

    /* Walk the native call stack */
    fprintf(f, "\nCall stack:\n");
    HANDLE proc = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymInitialize(proc, NULL, TRUE);
    STACKFRAME64 sf;
    memset(&sf, 0, sizeof(sf));
    sf.AddrPC.Offset    = ctx->Rip;
    sf.AddrPC.Mode      = AddrModeFlat;
    sf.AddrFrame.Offset = ctx->Rbp;
    sf.AddrFrame.Mode   = AddrModeFlat;
    sf.AddrStack.Offset = ctx->Rsp;
    sf.AddrStack.Mode   = AddrModeFlat;
    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &sf, ctx,
                         NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        char sym_buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)sym_buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym))
            fprintf(f, "  [%d] %s+0x%llx (%p)\n", i, sym->Name, (unsigned long long)disp, (void *)sf.AddrPC.Offset);
        else
            fprintf(f, "  [%d] %p\n", i, (void *)sf.AddrPC.Offset);
    }

    if (f != stderr) fclose(f);

    /* Also print to stdout for redirected output */
    printf("\n=== CRASH === Exception 0x%08lX at %p — see crash_report.txt\n", code, addr);
    fflush(stdout);

    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
    AddVectoredExceptionHandler(1, vectored_handler);
    atexit(atexit_handler);
#endif

    /* Set g_exe_dir to the directory containing the executable */
#ifdef _WIN32
    GetModuleFileNameA(NULL, g_exe_dir, sizeof(g_exe_dir));
    char *sep = strrchr(g_exe_dir, '\\');
    if (sep) *(sep + 1) = '\0';
#endif

    static char rom_path[512];
    uint32_t expected_crc = game_get_expected_crc32();

    if (argc >= 2 && argv[1][0] != '-') {
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

    /* Re-build argv so that argv[1] == rom_path for the runner.
     * If argv[1] was the ROM path, extra args start at index 2.
     * If argv[1] was a flag (starts with '-'), forward all args from index 1. */
    char *new_argv[64];
    int new_argc = 0;
    int extra_start = (argc >= 2 && argv[1][0] != '-') ? 2 : 1;
    new_argv[new_argc++] = argv[0];
    new_argv[new_argc++] = rom_path;
    for (int i = extra_start; i < argc && new_argc < 63; i++)
        new_argv[new_argc++] = argv[i];
    new_argv[new_argc] = NULL;

    nesrecomp_runner_run(new_argc, new_argv);
    return 0;
}
