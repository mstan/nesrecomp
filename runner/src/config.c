/*
 * config.c — config.ini load/save (see config.h).
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <unistd.h>
#else
#  include <unistd.h>
#endif

void nesrecomp_exe_dir(char *out, size_t max_len) {
    char exe[1024];
    int got = 0;
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
    got = (n > 0 && n < sizeof(exe));
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)sizeof(exe);
    got = (_NSGetExecutablePath(exe, &size) == 0);
#else /* Linux and other Unix */
    /* Inside an AppImage /proc/self/exe is the read-only mount, not where
     * the user's .AppImage + config.ini live; $APPIMAGE is the .AppImage. */
    const char *appimg = getenv("APPIMAGE");
    if (appimg && appimg[0] && strlen(appimg) < sizeof(exe)) {
        strcpy(exe, appimg);
        got = 1;
    } else {
        ssize_t r = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (r > 0) { exe[r] = '\0'; got = 1; }
    }
#endif
    if (got) {
        char *sep = strrchr(exe, '/');
        char *bsep = strrchr(exe, '\\');
        if (bsep && (!sep || bsep > sep)) sep = bsep;
        if (sep) { sep[1] = '\0'; snprintf(out, max_len, "%s", exe); return; }
    }
    snprintf(out, max_len, "./");
}

/* Defaults: P1 keyboard, P2 gamepad; 3x window; nearest, integer-scaled. */
NesConfig g_nes_config = {
    /*window_scale*/ 3, /*fullscreen*/ 0, /*integer_scale*/ 1, /*linear_filter*/ 0,
    /*renderer*/ 0, /*widescreen*/ 0,
    /*volume*/ 100,
    /*player_src*/ { 1, 2 }, /*deadzone*/ { 30, 30 },
    /*skip_launcher*/ 0,
};

void config_set_defaults(NesConfig *c) {
    NesConfig d = {
        3, 0, 1, 0,
        0, 0,
        100,
        { 1, 2 }, { 30, 30 },
        0,
    };
    if (c) *c = d;
}

const char *config_path(void) {
    static char path[1024];
    char dir[1024];
    /* config.ini lives next to the exe (next to the .AppImage on Linux) —
     * one location, no directory walk-up, no alternate-name search. */
    nesrecomp_exe_dir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%sconfig.ini", dir);
    return path;
}

/* trim leading/trailing whitespace in place; returns s. */
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void config_load(const char *path) {
    config_set_defaults(&g_nes_config);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '\0' || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s);
        int   val = atoi(trim(eq + 1));
        if      (!strcmp(key, "WindowScale"))   g_nes_config.window_scale  = clampi(val, 1, 8);
        else if (!strcmp(key, "Fullscreen"))    g_nes_config.fullscreen    = val ? 1 : 0;
        else if (!strcmp(key, "IntegerScale"))  g_nes_config.integer_scale = val ? 1 : 0;
        else if (!strcmp(key, "LinearFilter"))  g_nes_config.linear_filter = val ? 1 : 0;
        else if (!strcmp(key, "Renderer"))      g_nes_config.renderer      = clampi(val, 0, 1);
        else if (!strcmp(key, "Widescreen"))    g_nes_config.widescreen    = val ? 1 : 0;
        else if (!strcmp(key, "Volume"))        g_nes_config.volume        = clampi(val, 0, 100);
        else if (!strcmp(key, "Player1Source")) g_nes_config.player_src[0] = clampi(val, 0, 2);
        else if (!strcmp(key, "Player2Source")) g_nes_config.player_src[1] = clampi(val, 0, 2);
        else if (!strcmp(key, "Player1Deadzone")) g_nes_config.deadzone[0] = clampi(val, 0, 100);
        else if (!strcmp(key, "Player2Deadzone")) g_nes_config.deadzone[1] = clampi(val, 0, 100);
        else if (!strcmp(key, "SkipLauncher"))  g_nes_config.skip_launcher = val ? 1 : 0;
    }
    fclose(f);
}

void config_save(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[Config] Cannot write %s\n", path); return; }
    const NesConfig *c = &g_nes_config;
    fprintf(f, "# NESRecomp settings — edited by the launcher's Settings view.\n");
    fprintf(f, "[Display]\n");
    fprintf(f, "WindowScale = %d\n",   c->window_scale);
    fprintf(f, "Fullscreen = %d\n",    c->fullscreen);
    fprintf(f, "IntegerScale = %d\n",  c->integer_scale);
    fprintf(f, "LinearFilter = %d\n",  c->linear_filter);
    fprintf(f, "Renderer = %d\n",      c->renderer);
    fprintf(f, "Widescreen = %d\n",    c->widescreen);
    fprintf(f, "[Audio]\n");
    fprintf(f, "Volume = %d\n",        c->volume);
    fprintf(f, "[Input]\n");
    fprintf(f, "Player1Source = %d\n", c->player_src[0]);
    fprintf(f, "Player2Source = %d\n", c->player_src[1]);
    fprintf(f, "Player1Deadzone = %d\n", c->deadzone[0]);
    fprintf(f, "Player2Deadzone = %d\n", c->deadzone[1]);
    fprintf(f, "[Launcher]\n");
    fprintf(f, "SkipLauncher = %d\n",  c->skip_launcher);
    fclose(f);
}
