/*
 * keybinds.c — Configurable keyboard bindings for NES controllers
 *
 * Reads/writes an INI file next to the game executable.
 * Auto-generates with defaults if the file doesn't exist.
 */
#include "keybinds.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ── Default bindings ─────────────────────────────────────────────────────── */

static KeyBinds s_binds = {
    .p1 = {
        .a      = SDL_SCANCODE_Z,
        .b      = SDL_SCANCODE_X,
        .select = SDL_SCANCODE_TAB,
        .start  = SDL_SCANCODE_RETURN,
        .up     = SDL_SCANCODE_UP,
        .down   = SDL_SCANCODE_DOWN,
        .left   = SDL_SCANCODE_LEFT,
        .right  = SDL_SCANCODE_RIGHT,
    },
    .p2 = {
        .a      = SDL_SCANCODE_K,
        .b      = SDL_SCANCODE_L,
        .select = SDL_SCANCODE_RSHIFT,
        .start  = SDL_SCANCODE_BACKSLASH,
        .up     = SDL_SCANCODE_W,
        .down   = SDL_SCANCODE_S,
        .left   = SDL_SCANCODE_A,
        .right  = SDL_SCANCODE_D,
    },
    .zapper = {
        .mouse_enabled = 0,
        .crosshair = 0,
    },
};

/* ── Button name mapping ──────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    size_t offset;  /* offset into PlayerBinds */
} ButtonDef;

static const ButtonDef s_buttons[] = {
    { "a",      offsetof(PlayerBinds, a) },
    { "b",      offsetof(PlayerBinds, b) },
    { "select", offsetof(PlayerBinds, select) },
    { "start",  offsetof(PlayerBinds, start) },
    { "up",     offsetof(PlayerBinds, up) },
    { "down",   offsetof(PlayerBinds, down) },
    { "left",   offsetof(PlayerBinds, left) },
    { "right",  offsetof(PlayerBinds, right) },
    { NULL, 0 }
};

/* ── INI parsing helpers ──────────────────────────────────────────────────── */

static void trim(char *s) {
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    char *start = s;
    while (isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static SDL_Scancode name_to_scancode(const char *name) {
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN) return sc;
    /* Try common aliases */
    if (strcmp(name, "enter") == 0 || strcmp(name, "return") == 0) return SDL_SCANCODE_RETURN;
    if (strcmp(name, "tab") == 0) return SDL_SCANCODE_TAB;
    if (strcmp(name, "space") == 0) return SDL_SCANCODE_SPACE;
    if (strcmp(name, "lshift") == 0) return SDL_SCANCODE_LSHIFT;
    if (strcmp(name, "rshift") == 0) return SDL_SCANCODE_RSHIFT;
    if (strcmp(name, "backslash") == 0) return SDL_SCANCODE_BACKSLASH;
    if (strcmp(name, "escape") == 0) return SDL_SCANCODE_ESCAPE;
    return SDL_SCANCODE_UNKNOWN;
}

static const char *scancode_to_name(SDL_Scancode sc) {
    const char *name = SDL_GetScancodeName(sc);
    if (name && name[0]) return name;
    return "Unknown";
}

/* ── File I/O ─────────────────────────────────────────────────────────────── */

static char s_ini_path[512] = {0};

static void derive_ini_path(const char *exe_path) {
    if (!exe_path) {
        strcpy(s_ini_path, "keybinds.ini");
        return;
    }
    /* Find last separator */
    const char *slash = NULL, *p = exe_path;
    while (*p) { if (*p == '/' || *p == '\\') slash = p; p++; }
    if (slash) {
        size_t dir_len = (size_t)(slash - exe_path) + 1;
        if (dir_len + 13 < sizeof(s_ini_path)) {
            memcpy(s_ini_path, exe_path, dir_len);
            strcpy(s_ini_path + dir_len, "keybinds.ini");
        }
    } else {
        strcpy(s_ini_path, "keybinds.ini");
    }
}

static void write_player(FILE *f, const char *section, const PlayerBinds *pb) {
    fprintf(f, "[%s]\n", section);
    for (const ButtonDef *bd = s_buttons; bd->name; bd++) {
        SDL_Scancode sc = *(SDL_Scancode *)((char *)pb + bd->offset);
        fprintf(f, "%s = %s\n", bd->name, scancode_to_name(sc));
    }
    fprintf(f, "\n");
}

static void write_defaults(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# NES Controller Keybinds\n");
    fprintf(f, "# Edit key names to customize. Use SDL key names.\n");
    fprintf(f, "# Common keys: Z, X, Tab, Return, Up, Down, Left, Right\n");
    fprintf(f, "# A, B, C, ..., W, S, D, K, L, Space, Left Shift, Right Shift\n\n");
    write_player(f, "player1", &s_binds.p1);
    write_player(f, "player2", &s_binds.p2);
    fprintf(f, "[zapper]\n");
    fprintf(f, "# Set mouse = true to use the mouse as a Zapper light gun.\n");
    fprintf(f, "# Left click = trigger, mouse position = aim point.\n");
    fprintf(f, "mouse = false\n");
    fprintf(f, "# Set crosshair = true to show a crosshair at the aim point.\n");
    fprintf(f, "crosshair = false\n\n");
    fclose(f);
    printf("[Keybinds] Generated %s\n", path);
}

static void load_ini(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    PlayerBinds *current = NULL;
    int in_zapper = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;

        /* Section header */
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) *end = '\0';
            char *section = line + 1;
            in_zapper = 0;
            current = NULL;
            if (strcmp(section, "player1") == 0) current = &s_binds.p1;
            else if (strcmp(section, "player2") == 0) current = &s_binds.p2;
            else if (strcmp(section, "zapper") == 0) in_zapper = 1;
            continue;
        }

        /* key = value */
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        trim(key); trim(val);

        /* Convert key to lowercase for matching */
        for (char *c = key; *c; c++) *c = (char)tolower((unsigned char)*c);

        if (in_zapper) {
            /* Accept true/false/1/0/yes/no for boolean zapper keys */
            for (char *c = val; *c; c++) *c = (char)tolower((unsigned char)*c);
            int bval = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0 || strcmp(val, "yes") == 0);
            if (strcmp(key, "mouse") == 0)
                s_binds.zapper.mouse_enabled = bval;
            else if (strcmp(key, "crosshair") == 0)
                s_binds.zapper.crosshair = bval;
            continue;
        }

        if (!current) continue;

        for (const ButtonDef *bd = s_buttons; bd->name; bd++) {
            if (strcmp(key, bd->name) == 0) {
                SDL_Scancode sc = name_to_scancode(val);
                if (sc == SDL_SCANCODE_UNKNOWN) {
                    /* Try SDL's own lookup */
                    sc = SDL_GetScancodeFromName(val);
                }
                if (sc != SDL_SCANCODE_UNKNOWN) {
                    *(SDL_Scancode *)((char *)current + bd->offset) = sc;
                }
                break;
            }
        }
    }
    fclose(f);
    printf("[Keybinds] Loaded %s\n", path);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void keybinds_init(const char *exe_path) {
    derive_ini_path(exe_path);

    FILE *test = fopen(s_ini_path, "r");
    if (test) {
        fclose(test);
        load_ini(s_ini_path);
    } else {
        write_defaults(s_ini_path);
    }
}

const KeyBinds *keybinds_get(void) {
    return &s_binds;
}

uint8_t keybinds_read_player(const uint8_t *keys, int player) {
    const PlayerBinds *pb = (player == 1) ? &s_binds.p1 : &s_binds.p2;
    uint8_t btn = 0;
    if (keys[pb->a])      btn |= 0x80;
    if (keys[pb->b])      btn |= 0x40;
    if (keys[pb->select]) btn |= 0x20;
    if (keys[pb->start])  btn |= 0x10;
    if (keys[pb->up])     btn |= 0x08;
    if (keys[pb->down])   btn |= 0x04;
    if (keys[pb->left])   btn |= 0x02;
    if (keys[pb->right])  btn |= 0x01;
    return btn;
}

int keybinds_zapper_mouse(void) {
    return s_binds.zapper.mouse_enabled;
}

int keybinds_zapper_crosshair(void) {
    return s_binds.zapper.crosshair;
}
