/*
 * cyc_settings.h - the windowed cycle host's persistent settings: config.ini
 * next to the executable.
 *
 * The keys nesrecomp's other runtime and recomp-ui's NES launcher already use
 * keep their names ([Display] WindowScale, Fullscreen, IntegerScale,
 * LinearFilter; [Audio] Volume; [Input] PlayerNSource / PlayerNDevice /
 * PlayerNDeadzone; [Launcher] SkipLauncher). The cycle host adds:
 *
 *   [Audio]    Enabled
 *   [FDS]      one key per HLE axis (common/nes_fds_hle.h nes_fds_hle_axes():
 *              AutoSwap, FastLoad, ...): on, off or default (game.toml decides);
 *              Bios = the FDS BIOS file the player chose (the launcher's
 *              Select BIOS...), empty to look for one (cyc_fds_bios.h)
 *   [Keyboard.PlayerN] / [Gamepad.PlayerN]   a = Z ... (cyc_input.h names)
 *   [Keyboard.Shortcuts] / [Gamepad.Shortcuts]  disk = D, menu = Escape ...
 *   [Game]     ViewMode (a game's view modes, cyc_host_extras.h), then the
 *              game's own keys (CycSettingsGame)
 *
 * Headless runs never read or write it (runs stay reproducible); only the
 * window does. A key the file lacks keeps its default; an unreadable value is
 * reported and keeps its default; saving rewrites the whole file.
 */
#pragma once
#include "cyc_input.h"
#include "../../common/nes_fds_hle.h"
#include <stdbool.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct CycSettings {
    int  window_scale;      /* 1..8 */
    int  fullscreen;        /* 0 off, 1 borderless, 2 exclusive */
    int  integer_scale;     /* bool */
    int  linear_filter;     /* bool */
    int  audio_enabled;     /* bool */
    int  volume;            /* 0..100 */
    int  skip_launcher;     /* bool */
    int  view_mode;         /* RECOMP_RUNTIME_UI_VIEW_* of a game that has view modes; 0 native */
    int  zapper_mouse, zapper_crosshair; /* [Zapper], booleans (both default on) */
    unsigned zapper_keys;   /* presence bits for migration from legacy keybinds.ini */
    int hdpack_enabled;
    char hdpack_dir[512];   /* existing [Display] HD pack settings */
    NesFdsHleAsk fds_hle;   /* the player's saved HLE choices (the plan's "settings" source) */
    char fds_bios[512];     /* [FDS] Bios: the player's FDS BIOS file; "" looks for one */
    CycBindings  bind;
} CycSettings;

/* A game's own [Game] keys (cyc_host_extras.h load_setting / save_settings). */
typedef struct {
    void *ctx;
    void (*load)(void *ctx, const char *key, const char *value);
    void (*save)(void *ctx, FILE *f);
} CycSettingsGame;

void cyc_settings_default(CycSettings *s);
/* Overlay `path` onto *s. Returns false if the file does not exist (s keeps
 * what it had). Warnings for unreadable values go to `log` (may be NULL);
 * `game` (may be NULL) gets every other [Game] key. */
bool cyc_settings_load(CycSettings *s, const char *path, FILE *log, const CycSettingsGame *game);
/* Written whole to a temporary file, then moved over `path`. */
bool cyc_settings_save(const CycSettings *s, const char *path, const CycSettingsGame *game);
/* <exe dir>/config.ini */
const char *cyc_settings_default_path(void);

#ifdef __cplusplus
}
#endif
