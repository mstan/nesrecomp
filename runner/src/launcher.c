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
#include "config.h"
#ifdef NESRECOMP_LAUNCHER
#include "launcher/launcher_capi.h"
#endif
#ifdef RECOMP_LAUNCHER
/* The shared recomp-ui Dear ImGui launcher (consumed as a git submodule;
 * recomp_ui.cmake defines RECOMP_LAUNCHER and adds its src/ to the include
 * path). Supersedes the in-tree RmlUi launcher above; the old gate is kept
 * side-by-side so a build defining NESRECOMP_LAUNCHER instead still works. */
#include "recomp_launcher.h"
#include "launcher_profile.h"
#include "save_ram.h"
#endif

/* Declared in main_runner.c */
void nesrecomp_runner_run(int argc, char **argv);

char g_exe_dir[260] = ".";
static int s_expected_process_exit = 0;

void nesrecomp_expect_process_exit(void) {
    s_expected_process_exit = 1;
}

/* ---- rom.cfg helpers ---- */

/* Build path: <exe_dir>/rom.cfg (next to the .AppImage on Linux). One
 * location only — no walk-up, matching config.ini resolution. */
static void get_rom_cfg_path(char *out, int max_len) {
    char dir[1024];
    nesrecomp_exe_dir(dir, sizeof(dir));
    snprintf(out, max_len, "%srom.cfg", dir);
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

#ifndef _WIN32
/* Run one shell-wrapped native chooser, read the selected path from its
 * stdout. Each command is gated on `command -v <tool>` so an absent tool
 * prints nothing and we fall through; a real selection prints one path and
 * exits 0. Returns 1 and fills `out` only on a real selection. */
static int run_picker_cmd(const char *cmd, char *out, int max_len) {
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    char buf[1024];
    buf[0] = '\0';
    char *got = fgets(buf, sizeof(buf), p);
    int rc = pclose(p);
    if (!got) return 0;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    if (rc != 0 || n == 0 || (int)n >= max_len) return 0;
    memcpy(out, buf, n + 1);
    return 1;
}
#endif

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
    /* Native graphical chooser, in preference order; each gated on
     * `command -v` so an absent tool falls through. rom.cfg and a
     * positional ROM arg remain fallbacks when none exist. No new
     * link-time deps — all via popen. */
    out[0] = '\0';
    static const char *const pickers[] = {
        "command -v zenity >/dev/null 2>&1 && "
        "zenity --file-selection --title='Select NES ROM' "
        "--file-filter='NES ROMs (.nes) | *.nes *.NES' "
        "--file-filter='All files | *' 2>/dev/null",
        "command -v kdialog >/dev/null 2>&1 && "
        "kdialog --getopenfilename \"${HOME:-/}\" "
        "'*.nes *.NES|NES ROMs' 2>/dev/null",
        "command -v qarma >/dev/null 2>&1 && "
        "qarma --file-selection --title='Select NES ROM' 2>/dev/null",
        "command -v osascript >/dev/null 2>&1 && "
        "osascript -e 'POSIX path of (choose file with prompt \"Select NES ROM\")' "
        "2>/dev/null",
    };
    for (size_t i = 0; i < sizeof(pickers) / sizeof(pickers[0]); i++)
        if (run_picker_cmd(pickers[i], out, max_len))
            return 1;
    fprintf(stderr,
            "[Launcher] No ROM specified and no graphical file chooser found "
            "(install zenity or kdialog).\nUsage: pass the ROM path as the first "
            "argument.\n");
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

#ifdef RECOMP_LAUNCHER
/* Read the iNES battery bit (header byte 6, bit 1) from a ROM file. The
 * recomp-ui launcher is console-agnostic and can't parse NES headers itself,
 * so the seam detects battery-backed SRAM here — matching main_runner.c's
 * s_rom_has_battery — to decide whether to show the SAVES panel. Returns 0 for
 * an absent/short/non-iNES file (no save UI, same as a non-battery cart). */
static int rom_has_battery(const char *path) {
    if (!path || !path[0]) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char header[16];
    size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (n < 16) return 0;
    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A)
        return 0;
    return (header[6] & 0x02) ? 1 : 0;
}
#endif

/* ---- main ---- */

/* ---- Crash handler (Windows SEH + VEH) ---- */
#ifdef _WIN32
#include <dbghelp.h>   /* StackWalk64 — link with dbghelp.lib */
#pragma comment(lib, "dbghelp.lib")

static void atexit_handler(void) {
    extern const char *g_recomp_stack[];
    extern int g_recomp_stack_top;
    extern uint64_t g_frame_count;
    if (s_expected_process_exit) return;
    /* Only log if the game exited unexpectedly (recomp stack still active) */
    if (g_recomp_stack_top > 0) {
        extern void nes_dump_dispatch_ring(void);
        printf("[EXIT] Unexpected exit at frame %llu, recomp stack (top=%d):\n",
               (unsigned long long)g_frame_count, g_recomp_stack_top);
        /* Native backtrace of the exit() caller chain (env NESRECOMP_EXIT_BT). */
        if (getenv("NESRECOMP_EXIT_BT")) {
            void *bt[40]; USHORT n = CaptureStackBackTrace(0, 40, bt, NULL);
            HANDLE proc = GetCurrentProcess(); SymInitialize(proc, NULL, TRUE);
            printf("[EXIT-BT] exit() call chain:\n");
            for (USHORT i = 0; i < n; i++) {
                char sb[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO *sym = (SYMBOL_INFO *)sb;
                sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255; DWORD64 disp = 0;
                if (SymFromAddr(proc, (DWORD64)bt[i], &disp, sym))
                    printf("  [%2u] %s+0x%llx\n", i, sym->Name, (unsigned long long)disp);
                else printf("  [%2u] %p\n", i, bt[i]);
            }
            fflush(stdout);
        }
        for (int i = g_recomp_stack_top - 1; i >= 0 && i >= g_recomp_stack_top - 20; i--)
            printf("  [%d] %s\n", i, g_recomp_stack[i] ? g_recomp_stack[i] : "?");
        nes_dump_dispatch_ring();
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

    /* Set g_exe_dir to the directory containing the executable (the folder
     * holding the .AppImage on Linux, via $APPIMAGE). */
    nesrecomp_exe_dir(g_exe_dir, sizeof(g_exe_dir));

    static char rom_path[512];
    uint32_t expected_crc = game_get_expected_crc32();

    /* Settings live in config.ini next to the exe (created with defaults if
     * absent). The GUI launcher edits them; the runner honors them. */
    config_load(config_path());

    int gui_resolved = 0;
#ifdef NESRECOMP_LAUNCHER
    {
        int have_positional = (argc >= 2 && argv[1][0] != '-');
        int force_launcher  = (argc >= 2 && strcmp(argv[1], "--launcher") == 0);
        int no_gui_env      = getenv("NESRECOMP_NO_LAUNCHER") != NULL;
        /* Show the GUI unless a ROM was given on the command line, the user asked
         * to skip it (and didn't pass --launcher to force it), or it's disabled. */
        int want_gui = !have_positional && !no_gui_env &&
                       (!g_nes_config.skip_launcher || force_launcher);
        if (want_gui) {
            char init_rom[512]; init_rom[0] = '\0';
            rom_cfg_read(init_rom, sizeof(init_rom));

            NesLauncherCSettings ls;
            memset(&ls, 0, sizeof(ls));
            ls.window_scale  = g_nes_config.window_scale;
            ls.fullscreen    = g_nes_config.fullscreen;
            ls.integer_scale = g_nes_config.integer_scale;
            ls.linear_filter = g_nes_config.linear_filter;
            ls.renderer      = g_nes_config.renderer;
            ls.widescreen    = g_nes_config.widescreen;
            ls.volume        = g_nes_config.volume;
            ls.player_src[0] = g_nes_config.player_src[0];
            ls.player_src[1] = g_nes_config.player_src[1];
            ls.deadzone[0]   = g_nes_config.deadzone[0];
            ls.deadzone[1]   = g_nes_config.deadzone[1];
            ls.skip_launcher = g_nes_config.skip_launcher;
            ls.hdpack_enabled = g_nes_config.hdpack_enabled;
            snprintf(ls.hdpack_dir, sizeof(ls.hdpack_dir), "%s", g_nes_config.hdpack_dir);

            NesLauncherCGameInfo gi;
            memset(&gi, 0, sizeof(gi));
            gi.name             = game_get_name();
            gi.region           = "NTSC-U (USA)";
            gi.expected_crc     = expected_crc;
            gi.has_expected_crc = expected_crc != 0;
            gi.mapper_board     = NULL;   /* launcher derives from the iNES mapper */
            gi.uses_sram        = 0;      /* battery bit auto-detected from the ROM */
            gi.save_basename    = game_get_name();
#ifdef NESRECOMP_GAME_WIDESCREEN
            gi.widescreen_supported = 1;  /* per-game opt-in (e.g. SMB) */
#else
            gi.widescreen_supported = 0;
#endif
            /* HD texture packs are supported by default for every game; a build
             * opts OUT with NESRECOMP_GAME_NO_HDPACK (e.g. a stock/unpatched
             * Zelda build that must not offer or load remaster packs). */
#ifdef NESRECOMP_GAME_NO_HDPACK
            gi.hdpack_supported = 0;
#else
            gi.hdpack_supported = 1;
#endif
#ifdef NESRECOMP_GAME_PASSWORD_SAVE
            /* Per-game password/mantra save (e.g. Faxanadu): the SAVES panel shows
             * the password text instead of binary SRAM. The file lives next to the
             * exe (same one the runtime auto-prefill reads/writes). */
            static char s_pw_save_path[600];
            snprintf(s_pw_save_path, sizeof(s_pw_save_path), "%s%s",
                     g_exe_dir, NESRECOMP_GAME_PASSWORD_SAVE);
            gi.password_save_path  = s_pw_save_path;
#  ifdef NESRECOMP_GAME_PASSWORD_SAVE_LABEL
            gi.password_save_label = NESRECOMP_GAME_PASSWORD_SAVE_LABEL;
#  else
            gi.password_save_label = "Password";
#  endif
#endif

            char win_title[96];
            snprintf(win_title, sizeof(win_title), "%s - NES Launcher",
                     gi.name ? gi.name : "NES");
            int act = nes_launcher_run_window(win_title, &ls, &gi, "launcher",
                                              init_rom, rom_path, sizeof(rom_path));
            if (act == 1) return 0;   /* user closed the launcher */
            if (act == 0) {
                g_nes_config.window_scale  = ls.window_scale;
                g_nes_config.fullscreen    = ls.fullscreen;
                g_nes_config.integer_scale = ls.integer_scale;
                g_nes_config.linear_filter = ls.linear_filter;
                g_nes_config.renderer      = ls.renderer;
                g_nes_config.widescreen    = ls.widescreen;
                g_nes_config.volume        = ls.volume;
                g_nes_config.player_src[0] = ls.player_src[0];
                g_nes_config.player_src[1] = ls.player_src[1];
                g_nes_config.deadzone[0]   = ls.deadzone[0];
                g_nes_config.deadzone[1]   = ls.deadzone[1];
                g_nes_config.skip_launcher = ls.skip_launcher;
                g_nes_config.hdpack_enabled = ls.hdpack_enabled;
                snprintf(g_nes_config.hdpack_dir, sizeof(g_nes_config.hdpack_dir), "%s", ls.hdpack_dir);
                config_save(config_path());
                if (rom_path[0]) { rom_cfg_write(rom_path); gui_resolved = 1; }
            }
            /* act == 2 (unavailable) -> fall through to the console resolver */
        }
    }
#endif

#ifdef RECOMP_LAUNCHER
    {
        int have_positional = (argc >= 2 && argv[1][0] != '-');
        int force_launcher  = (argc >= 2 && strcmp(argv[1], "--launcher") == 0);
        int no_gui_env      = getenv("NESRECOMP_NO_LAUNCHER") != NULL;
        int want_gui = !have_positional && !no_gui_env &&
                       (!g_nes_config.skip_launcher || force_launcher);
        if (want_gui) {
            char init_rom[512]; init_rom[0] = '\0';
            rom_cfg_read(init_rom, sizeof(init_rom));

            /* The recomp-ui settings struct shares every NES-mapped field name
             * with the config, but is a superset (PSX/SNES controls we leave
             * zero — the profile's capability flags hide those rows). */
            RecompLauncherCSettings ls;
            memset(&ls, 0, sizeof(ls));
            ls.window_scale   = g_nes_config.window_scale;
            ls.fullscreen     = g_nes_config.fullscreen;
            ls.integer_scale  = g_nes_config.integer_scale;
            ls.linear_filter  = g_nes_config.linear_filter;
            ls.renderer       = g_nes_config.renderer;
            ls.widescreen     = g_nes_config.widescreen;
            ls.enable_audio   = 1;    /* NES audio is always on; volume gates it */
            ls.volume         = g_nes_config.volume;
            ls.player_src[0]  = g_nes_config.player_src[0];
            ls.player_src[1]  = g_nes_config.player_src[1];
            ls.deadzone[0]    = g_nes_config.deadzone[0];
            ls.deadzone[1]    = g_nes_config.deadzone[1];
            ls.skip_launcher  = g_nes_config.skip_launcher;
            ls.hdpack_enabled = g_nes_config.hdpack_enabled;
            snprintf(ls.hdpack_dir, sizeof(ls.hdpack_dir), "%s", g_nes_config.hdpack_dir);

            /* Profile first (theme/platform/renderer labels/capabilities), then
             * the per-game specifics on top. */
            RecompLauncherCGameInfo gi;
            memset(&gi, 0, sizeof(gi));
            launcher_profile_apply("nes", &gi);
            gi.name             = game_get_name();
            gi.region           = "NTSC-U (USA)";
            gi.expected_crc     = expected_crc;
            gi.has_expected_crc = expected_crc != 0;
#ifdef NESRECOMP_GAME_PLAYERS
            gi.num_players      = NESRECOMP_GAME_PLAYERS;
#else
            gi.num_players      = 2;   /* most NES titles are 2-player */
#endif
#ifdef NESRECOMP_GAME_WIDESCREEN
            gi.widescreen_supported = 1;
#else
            gi.widescreen_supported = 0;
#endif
#ifdef NESRECOMP_GAME_NO_HDPACK
            gi.hdpack_supported = 0;
#else
            gi.hdpack_supported = 1;
#endif
#ifdef NESRECOMP_GAME_ZAPPER
            gi.zapper           = 1;   /* light-gun games: DuckHunt, Gumshoe */
#endif
#ifdef NESRECOMP_GAME_PASSWORD_SAVE
            /* Password/mantra save (e.g. Faxanadu): the SAVES row edits the
             * password text file next to the exe, not binary SRAM. */
            static char s_pw_save_path[600];
            snprintf(s_pw_save_path, sizeof(s_pw_save_path), "%s%s",
                     g_exe_dir, NESRECOMP_GAME_PASSWORD_SAVE);
            gi.password_save_path  = s_pw_save_path;
#  ifdef NESRECOMP_GAME_PASSWORD_SAVE_LABEL
            gi.password_save_label = NESRECOMP_GAME_PASSWORD_SAVE_LABEL;
#  else
            gi.password_save_label = "Password";
#  endif
#endif
            /* Battery SRAM: show the SAVES panel for battery-backed games. A
             * build declares itself battery-backed with NESRECOMP_GAME_BATTERY
             * (e.g. Zelda, Kirby) so the panel appears unconditionally — even on
             * the very first launch before any ROM has been picked. As a
             * fallback we also sniff the last-used ROM's iNES header, so an
             * unflagged build that has booted a battery ROM still shows it. Bind
             * only — no runtime activation (that happens in main_runner). */
#ifdef NESRECOMP_GAME_BATTERY
            int game_is_battery = 1;
#else
            int game_is_battery = 0;
#endif
            if (game_is_battery || rom_has_battery(init_rom)) {
                save_ram_ui_bind(game_get_name());
                gi.sram_path = save_ram_path();
            }
            /* Paths: exe-anchored, so the launcher and the runner agree on one
             * config.ini / keybinds.ini regardless of the cwd. */
            gi.config_path = config_path();
            static char s_keybinds_path[600];
            {
                char dir[512];
                nesrecomp_exe_dir(dir, sizeof(dir));
                snprintf(s_keybinds_path, sizeof(s_keybinds_path), "%skeybinds.ini", dir);
            }
            gi.keybinds_path = s_keybinds_path;

            char win_title[96];
            snprintf(win_title, sizeof(win_title), "%s - Launcher",
                     gi.name ? gi.name : "NES");
            /* assets_dir is resolved next to the exe by the capi shim; "." is a
             * harmless placeholder. */
            int act = recomp_launcher_run_window(win_title, &ls, &gi, ".",
                                                 init_rom, rom_path, sizeof(rom_path));
            if (act == 1) return 0;   /* user closed the launcher */
            if (act == 0) {
                g_nes_config.window_scale   = ls.window_scale;
                g_nes_config.fullscreen     = ls.fullscreen;
                g_nes_config.integer_scale  = ls.integer_scale;
                g_nes_config.linear_filter  = ls.linear_filter;
                g_nes_config.renderer       = ls.renderer;
                g_nes_config.widescreen     = ls.widescreen;
                g_nes_config.volume         = ls.volume;
                g_nes_config.player_src[0]  = ls.player_src[0];
                g_nes_config.player_src[1]  = ls.player_src[1];
                g_nes_config.deadzone[0]    = ls.deadzone[0];
                g_nes_config.deadzone[1]    = ls.deadzone[1];
                g_nes_config.skip_launcher  = ls.skip_launcher;
                g_nes_config.hdpack_enabled = ls.hdpack_enabled;
                snprintf(g_nes_config.hdpack_dir, sizeof(g_nes_config.hdpack_dir), "%s", ls.hdpack_dir);
                config_save(config_path());
                if (rom_path[0]) { rom_cfg_write(rom_path); gui_resolved = 1; }
            }
            /* act == 2 (unavailable) -> fall through to the console resolver */
        }
    }
#endif

    if (!gui_resolved)
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
