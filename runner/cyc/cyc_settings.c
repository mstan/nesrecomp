/*
 * cyc_settings.c - config.ini for the windowed cycle host (cyc_settings.h).
 */
#include "cyc_settings.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

void cyc_settings_default(CycSettings *s)
{
    memset(s, 0, sizeof(*s));
    s->window_scale = 3;
    s->fullscreen = 0;
    s->integer_scale = 1;
    s->linear_filter = 0;
    s->audio_enabled = 1;
    s->volume = 100;
    s->skip_launcher = 0;
    s->view_mode = 0;
    s->zapper_mouse = s->zapper_crosshair = 1;
    s->hdpack_enabled = 1;
    s->fds_hle = NES_FDS_HLE_ASK_NONE;
    cyc_bindings_default(&s->bind);
}

const char *cyc_settings_default_path(void)
{
    static char path[1024];
    char dir[900] = ".";
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof(dir));
    if (n == 0 || n >= sizeof(dir)) strcpy(dir, ".");
#else
    ssize_t n = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
    if (n > 0) dir[n] = 0; else strcpy(dir, ".");
#endif
    char *cut = strrchr(dir, '/'), *bcut = strrchr(dir, '\\');
    if (bcut > cut) cut = bcut;
    if (cut) *cut = 0;
    snprintf(path, sizeof(path), "%s/config.ini", dir);
    return path;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) ++s;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static bool eq_ci(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static bool parse_int(const char *v, int *out)
{
    char *end;
    long x = strtol(v, &end, 10);
    if (end == v || *trim(end)) return false;
    *out = (int)x;
    return true;
}

typedef enum { SEC_NONE, SEC_DISPLAY, SEC_AUDIO, SEC_INPUT, SEC_LAUNCHER, SEC_FDS, SEC_KEYS, SEC_PADS, SEC_GAME, SEC_ZAPPER } Section;

bool cyc_settings_load(CycSettings *s, const char *path, FILE *log, const CycSettingsGame *game)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[1024];
    Section sec = SEC_NONE;
    int player = -1;           /* in [Keyboard.PlayerN] / [Gamepad.PlayerN]; -1: shortcuts */
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        ++lineno;
        char *t = trim(line);
        if (!*t || *t == '#' || *t == ';') continue;
        if (*t == '[') {
            char *end = strchr(t, ']');
            if (end) *end = 0;
            const char *name = t + 1;
            sec = SEC_NONE;
            player = -1;
            if (eq_ci(name, "Display")) sec = SEC_DISPLAY;
            else if (eq_ci(name, "Zapper")) sec = SEC_ZAPPER;
            else if (eq_ci(name, "Audio")) sec = SEC_AUDIO;
            else if (eq_ci(name, "Input")) sec = SEC_INPUT;
            else if (eq_ci(name, "Launcher")) sec = SEC_LAUNCHER;
            else if (eq_ci(name, "FDS")) sec = SEC_FDS;
            else if (eq_ci(name, "Game")) sec = SEC_GAME;
            else if (!strncmp(name, "Keyboard.", 9) || !strncmp(name, "Gamepad.", 8)) {
                sec = name[0] == 'K' ? SEC_KEYS : SEC_PADS;
                const char *which = strchr(name, '.') + 1;
                if (eq_ci(which, "Shortcuts")) player = -1;
                else if (!strncmp(which, "Player", 6) && which[6] >= '1' && which[6] < '1' + CYC_INPUT_PLAYERS &&
                         !which[7]) player = which[6] - '1';
                else sec = SEC_NONE;
            }
            continue;
        }
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(t), *val = trim(eq + 1);
        int v = 0;
        bool bad = false;
        switch (sec) {
        case SEC_DISPLAY:
            if (eq_ci(key, "WindowScale")) bad = !parse_int(val, &v) || (s->window_scale = clampi(v, 1, 8), 0);
            else if (eq_ci(key, "Fullscreen")) bad = !parse_int(val, &v) || (s->fullscreen = clampi(v, 0, 2), 0);
            else if (eq_ci(key, "IntegerScale")) bad = !parse_int(val, &v) || (s->integer_scale = v != 0, 0);
            else if (eq_ci(key, "LinearFilter")) bad = !parse_int(val, &v) || (s->linear_filter = v != 0, 0);
            else if (eq_ci(key, "HdPackEnabled")) bad = !parse_int(val, &v) || (s->hdpack_enabled = v != 0, 0);
            else if (eq_ci(key, "HdPackDir")) snprintf(s->hdpack_dir,sizeof s->hdpack_dir,"%s",val);
            break;
        case SEC_AUDIO:
            if (eq_ci(key, "Volume")) bad = !parse_int(val, &v) || (s->volume = clampi(v, 0, 100), 0);
            else if (eq_ci(key, "Enabled")) bad = !parse_int(val, &v) || (s->audio_enabled = v != 0, 0);
            break;
        case SEC_LAUNCHER:
            if (eq_ci(key, "SkipLauncher")) bad = !parse_int(val, &v) || (s->skip_launcher = v != 0, 0);
            break;
        case SEC_INPUT:
            if (!strncmp(key, "Player", 6) && key[6] >= '1' && key[6] < '1' + CYC_INPUT_PLAYERS) {
                int p = key[6] - '1';
                const char *what = key + 7;
                if (eq_ci(what, "Source")) bad = !parse_int(val, &v) || (s->bind.source[p] = clampi(v, 0, 2), 0);
                else if (eq_ci(what, "Deadzone")) bad = !parse_int(val, &v) || (s->bind.deadzone[p] = clampi(v, 0, 100), 0);
                else if (eq_ci(what, "Device")) snprintf(s->bind.device[p], sizeof(s->bind.device[p]), "%s", val);
            }
            break;
        case SEC_GAME:
            if (eq_ci(key, "ViewMode")) bad = !parse_int(val, &v) || (s->view_mode = clampi(v, 0, 2), 0);
            else if (game && game->load) game->load(game->ctx, key, val);
            break;
        case SEC_ZAPPER:
            if (eq_ci(val, "true") || eq_ci(val, "on")) v = 1;
            else if (eq_ci(val, "false") || eq_ci(val, "off")) v = 0;
            else bad = !parse_int(val, &v);
            if (!bad && eq_ci(key, "Mouse")) { s->zapper_mouse = v != 0; s->zapper_keys |= 1; }
            else if (!bad && eq_ci(key, "Crosshair")) { s->zapper_crosshair = v != 0; s->zapper_keys |= 2; }
            break;
        case SEC_FDS: {
            if (eq_ci(key, "Bios")) {
                snprintf(s->fds_bios, sizeof(s->fds_bios), "%s", val);
                break;
            }
            unsigned n;
            const NesFdsHleAxis *ax = nes_fds_hle_axes(&n);
            for (unsigned i = 0; i < n; ++i) {
                if (!eq_ci(key, ax[i].key)) continue;
                int8_t *a = nes_fds_hle_ask_axis(&s->fds_hle, &ax[i]);
                if (eq_ci(val, "on") || eq_ci(val, "1")) *a = 1;
                else if (eq_ci(val, "off") || eq_ci(val, "0")) *a = 0;
                else if (eq_ci(val, "default") || !*val) *a = -1;
                else bad = true;
            }
            break;
        }
        case SEC_KEYS:
        case SEC_PADS: {
            CycBinding *target = NULL;
            if (player >= 0) {
                for (int i = 0; i < CYC_INPUT_BUTTONS; ++i)
                    if (eq_ci(key, cyc_button_key(i))) target = &s->bind.button[player][i];
            } else {
                for (int i = 0; i < CYC_SC_COUNT; ++i)
                    if (eq_ci(key, cyc_shortcut_key(i))) target = &s->bind.shortcut[i];
            }
            if (!target) break;
            if (sec == SEC_KEYS) bad = !cyc_key_parse(val, &v) || (target->key = v, 0);
            else bad = !cyc_pad_parse(val, &v) || (target->pad = v, 0);
            break;
        }
        default: break;
        }
        if (bad && log) fprintf(log, "%s:%d: cannot read %s = %s (kept the default)\n", path, lineno, key, val);
    }
    fclose(f);
    return true;
}

bool cyc_settings_save(const CycSettings *s, const char *path, const CycSettingsGame *game)
{
    /* Written whole to a temporary file, then moved over the old one. */
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "# NESRecomp cycle-backend settings: the launcher, the in-game menu and the\n"
               "# window write this file. Keys are SDL key and controller names.\n");
    fprintf(f, "[Display]\nWindowScale = %d\nFullscreen = %d\nIntegerScale = %d\nLinearFilter = %d\n",
            s->window_scale, s->fullscreen, s->integer_scale, s->linear_filter);
    fprintf(f,"HdPackEnabled = %d\nHdPackDir = %s\n",s->hdpack_enabled,s->hdpack_dir);
    fprintf(f, "[Audio]\nEnabled = %d\nVolume = %d\n", s->audio_enabled, s->volume);
    fprintf(f, "[Input]\n");
    for (int p = 0; p < CYC_INPUT_PLAYERS; ++p)
        fprintf(f, "Player%dSource = %d\nPlayer%dDevice = %s\nPlayer%dDeadzone = %d\n", p + 1, s->bind.source[p], p + 1,
                s->bind.device[p], p + 1, s->bind.deadzone[p]);
    fprintf(f, "[Launcher]\nSkipLauncher = %d\n", s->skip_launcher);
    fprintf(f, "[Zapper]\nMouse = %d\nCrosshair = %d\n", s->zapper_mouse, s->zapper_crosshair);
    fprintf(f, "[FDS]\n# on, off, or default (the game's own setting)\n");
    unsigned n;
    const NesFdsHleAxis *ax = nes_fds_hle_axes(&n);
    for (unsigned i = 0; i < n; ++i) {
        int8_t a = *nes_fds_hle_ask_axis((NesFdsHleAsk *)&s->fds_hle, &ax[i]);
        fprintf(f, "%s = %s\n", ax[i].key, a > 0 ? "on" : a == 0 ? "off" : "default");
    }
    fprintf(f, "# the FDS BIOS file, any name (checked by size and CRC); empty: the launcher asks for it\n"
               "Bios = %s\n", s->fds_bios);
    char text[96];
    for (int p = 0; p < CYC_INPUT_PLAYERS; ++p) {
        fprintf(f, "[Keyboard.Player%d]\n", p + 1);
        for (int i = 0; i < CYC_INPUT_BUTTONS; ++i) {
            cyc_key_text(s->bind.button[p][i].key, text, sizeof(text));
            fprintf(f, "%s = %s\n", cyc_button_key(i), text);
        }
        fprintf(f, "[Gamepad.Player%d]\n", p + 1);
        for (int i = 0; i < CYC_INPUT_BUTTONS; ++i) {
            cyc_pad_text(s->bind.button[p][i].pad, text, sizeof(text));
            fprintf(f, "%s = %s\n", cyc_button_key(i), text);
        }
    }
    fprintf(f, "[Keyboard.Shortcuts]\n");
    for (int i = 0; i < CYC_SC_COUNT; ++i) {
        cyc_key_text(s->bind.shortcut[i].key, text, sizeof(text));
        fprintf(f, "%s = %s\n", cyc_shortcut_key(i), text);
    }
    fprintf(f, "[Gamepad.Shortcuts]\n");
    for (int i = 0; i < CYC_SC_COUNT; ++i) {
        cyc_pad_text(s->bind.shortcut[i].pad, text, sizeof(text));
        fprintf(f, "%s = %s\n", cyc_shortcut_key(i), text);
    }
    fprintf(f, "[Game]\nViewMode = %d\n", s->view_mode);
    if (game && game->save) game->save(game->ctx, f);
    bool ok = fflush(f) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp); return false; }
#ifdef _WIN32
    ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    ok = rename(tmp, path) == 0;
#endif
    if (!ok) remove(tmp);
    return ok;
}
