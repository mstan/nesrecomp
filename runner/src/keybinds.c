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
        .select = SDL_SCANCODE_BACKSLASH,
        .start  = SDL_SCANCODE_RETURN,
        .up     = SDL_SCANCODE_UP,
        .down   = SDL_SCANCODE_DOWN,
        .left   = SDL_SCANCODE_LEFT,
        .right  = SDL_SCANCODE_RIGHT,
    },
    .zapper = {
        /* Default ON: a Zapper game (g_zapper_enabled) has no other input on a
         * PC, so the mouse IS the light gun.  Every consumer is gated on
         * g_zapper_enabled, so this is inert for non-Zapper games.  Users can
         * still set mouse/crosshair = false in keybinds.ini to override. */
        .mouse_enabled = 1,
        .crosshair = 1,
    },
    /* Default gamepad mapping is deliberately forgiving: the two right-hand
     * face buttons both act as A, the two left-hand both act as B, and the
     * left analog stick mirrors the d-pad. All of it is remappable below. */
    .pad1 = {
        .btn_mask = {
            [0] = (1u << SDL_CONTROLLER_BUTTON_A) | (1u << SDL_CONTROLLER_BUTTON_B), /* A */
            [1] = (1u << SDL_CONTROLLER_BUTTON_X) | (1u << SDL_CONTROLLER_BUTTON_Y), /* B */
            [2] = (1u << SDL_CONTROLLER_BUTTON_BACK),                                /* Select */
            [3] = (1u << SDL_CONTROLLER_BUTTON_START),                               /* Start */
            [4] = (1u << SDL_CONTROLLER_BUTTON_DPAD_UP),
            [5] = (1u << SDL_CONTROLLER_BUTTON_DPAD_DOWN),
            [6] = (1u << SDL_CONTROLLER_BUTTON_DPAD_LEFT),
            [7] = (1u << SDL_CONTROLLER_BUTTON_DPAD_RIGHT),
        },
        .deadzone = 16000,
        .analog_dpad = 1,
    },
    .pad2 = {
        .btn_mask = {
            [0] = (1u << SDL_CONTROLLER_BUTTON_A) | (1u << SDL_CONTROLLER_BUTTON_B),
            [1] = (1u << SDL_CONTROLLER_BUTTON_X) | (1u << SDL_CONTROLLER_BUTTON_Y),
            [2] = (1u << SDL_CONTROLLER_BUTTON_BACK),
            [3] = (1u << SDL_CONTROLLER_BUTTON_START),
            [4] = (1u << SDL_CONTROLLER_BUTTON_DPAD_UP),
            [5] = (1u << SDL_CONTROLLER_BUTTON_DPAD_DOWN),
            [6] = (1u << SDL_CONTROLLER_BUTTON_DPAD_LEFT),
            [7] = (1u << SDL_CONTROLLER_BUTTON_DPAD_RIGHT),
        },
        .deadzone = 16000,
        .analog_dpad = 1,
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

/* ── Gamepad button name <-> mask helpers ─────────────────────────────────── */

/* Parse a comma/space-separated list of SDL controller button names
 * (e.g. "a, b") into a bitmask of (1u << SDL_CONTROLLER_BUTTON_*). */
static uint32_t names_to_mask(const char *val) {
    char buf[160];
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char *c = buf; *c; c++) *c = (char)tolower((unsigned char)*c);
    uint32_t mask = 0;
    for (char *tok = strtok(buf, ", \t"); tok; tok = strtok(NULL, ", \t")) {
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(tok);
        if (b != SDL_CONTROLLER_BUTTON_INVALID && b < 32)
            mask |= (1u << b);
    }
    return mask;
}

/* Render a button mask back to a "a,b" style name list. */
static void mask_to_names(uint32_t mask, char *out, size_t n) {
    out[0] = '\0';
    for (int bit = 0; bit < SDL_CONTROLLER_BUTTON_MAX && bit < 32; bit++) {
        if (!(mask & (1u << bit))) continue;
        const char *nm = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)bit);
        if (!nm) continue;
        if (out[0]) strncat(out, ",", n - strlen(out) - 1);
        strncat(out, nm, n - strlen(out) - 1);
    }
    if (!out[0]) strncpy(out, "none", n - 1);
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

static void write_pad(FILE *f, const char *section, const GamepadBinds *gb) {
    fprintf(f, "[%s]\n", section);
    char names[160];
    for (const ButtonDef *bd = s_buttons; bd->name; bd++) {
        int idx = (int)(bd - s_buttons);  /* NES button index 0..7 */
        mask_to_names(gb->btn_mask[idx], names, sizeof(names));
        fprintf(f, "%s = %s\n", bd->name, names);
    }
    fprintf(f, "deadzone = %d\n", gb->deadzone);
    fprintf(f, "analog = %s\n", gb->analog_dpad ? "true" : "false");
    fprintf(f, "\n");
}

static void write_defaults(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# NES Controller Keybinds\n");
    fprintf(f, "# Edit key names to customize. Use SDL key names.\n");
    fprintf(f, "# Common keys: Z, X, Backslash, Return, Up, Down, Left, Right\n");
    fprintf(f, "# Tab and F1-F12 are reserved runtime hotkeys.\n\n");
    write_player(f, "player1", &s_binds.p1);
    fprintf(f, "[zapper]\n");
    fprintf(f, "# Mouse as the Zapper light gun (default on for Zapper games):\n");
    fprintf(f, "# left click = trigger, mouse position = aim.  Set false to disable.\n");
    fprintf(f, "mouse = %s\n", s_binds.zapper.mouse_enabled ? "true" : "false");
    fprintf(f, "# Show a crosshair at the aim point (and hide the OS cursor).\n");
    fprintf(f, "crosshair = %s\n\n", s_binds.zapper.crosshair ? "true" : "false");
    fprintf(f, "# Gamepad bindings (SDL game-controller button names).\n");
    fprintf(f, "# Values may list multiple buttons separated by commas, e.g. \"a, b\".\n");
    fprintf(f, "# Valid names: a b x y back start guide leftshoulder rightshoulder\n");
    fprintf(f, "#   leftstick rightstick dpup dpdown dpleft dpright (use \"none\" to unbind).\n");
    fprintf(f, "# deadzone: left-stick threshold 0-32767.  analog: stick also moves the d-pad.\n");
    fprintf(f, "# Names are positional (a=bottom, b=right, x=left, y=top on an Xbox pad).\n\n");
    write_pad(f, "gamepad1", &s_binds.pad1);
    write_pad(f, "gamepad2", &s_binds.pad2);
    fclose(f);
    printf("[Keybinds] Generated %s\n", path);
}

static void load_ini(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    PlayerBinds *current = NULL;
    GamepadBinds *cur_pad = NULL;
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
            cur_pad = NULL;
            if (strcmp(section, "player1") == 0) current = &s_binds.p1;
            /* Legacy [player2] keyboard sections are intentionally ignored.
             * P2 is now assigned explicitly to a gamepad or netplay peer. */
            else if (strcmp(section, "zapper") == 0) in_zapper = 1;
            else if (strcmp(section, "gamepad1") == 0) cur_pad = &s_binds.pad1;
            else if (strcmp(section, "gamepad2") == 0) cur_pad = &s_binds.pad2;
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

        if (cur_pad) {
            if (strcmp(key, "deadzone") == 0) {
                cur_pad->deadzone = atoi(val);
            } else if (strcmp(key, "analog") == 0) {
                char vb[16]; size_t i = 0;
                for (char *c = val; *c && i < sizeof(vb) - 1; c++) vb[i++] = (char)tolower((unsigned char)*c);
                vb[i] = '\0';
                cur_pad->analog_dpad =
                    (strcmp(vb, "true") == 0 || strcmp(vb, "1") == 0 || strcmp(vb, "yes") == 0);
            } else {
                for (const ButtonDef *bd = s_buttons; bd->name; bd++) {
                    if (strcmp(key, bd->name) == 0) {
                        cur_pad->btn_mask[(int)(bd - s_buttons)] = names_to_mask(val);
                        break;
                    }
                }
            }
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
    /* Tab became the global turbo hotkey. Migrate the former P1 Select default
     * in memory so existing generated keybinds.ini files adopt Backslash too. */
    if (s_binds.p1.select == SDL_SCANCODE_TAB) {
        s_binds.p1.select = SDL_SCANCODE_BACKSLASH;
        printf("[Keybinds] Migrated Player 1 Select from Tab to Backslash\n");
    }
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

static int is_runtime_hotkey(SDL_Scancode sc) {
    return sc == SDL_SCANCODE_TAB ||
           (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12);
}

uint8_t keybinds_read_player(const uint8_t *keys, int player) {
    if (player != 1) return 0;
    const PlayerBinds *pb = &s_binds.p1;
    uint8_t btn = 0;
    if (!is_runtime_hotkey(pb->a)      && keys[pb->a])      btn |= 0x80;
    if (!is_runtime_hotkey(pb->b)      && keys[pb->b])      btn |= 0x40;
    if (!is_runtime_hotkey(pb->select) && keys[pb->select]) btn |= 0x20;
    if (!is_runtime_hotkey(pb->start)  && keys[pb->start])  btn |= 0x10;
    if (!is_runtime_hotkey(pb->up)     && keys[pb->up])     btn |= 0x08;
    if (!is_runtime_hotkey(pb->down)   && keys[pb->down])   btn |= 0x04;
    if (!is_runtime_hotkey(pb->left)   && keys[pb->left])   btn |= 0x02;
    if (!is_runtime_hotkey(pb->right)  && keys[pb->right])  btn |= 0x01;
    return btn;
}

const GamepadBinds *keybinds_get_pad(int player) {
    return (player == 1) ? &s_binds.pad1 : &s_binds.pad2;
}

int keybinds_zapper_mouse(void) {
    return s_binds.zapper.mouse_enabled;
}

int keybinds_zapper_crosshair(void) {
    return s_binds.zapper.crosshair;
}
