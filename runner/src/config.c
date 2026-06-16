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
#endif

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
#ifdef _WIN32
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        char *sep = strrchr(exe, '\\');
        if (!sep) sep = strrchr(exe, '/');
        if (sep) { *(sep + 1) = '\0'; snprintf(path, sizeof(path), "%sconfig.ini", exe); return path; }
    }
#endif
    snprintf(path, sizeof(path), "config.ini");
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
