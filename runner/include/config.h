/*
 * config.h — persistent launcher/runtime settings (config.ini next to the exe).
 *
 * Seeded with sensible defaults, overlaid by config.ini if present, edited by the
 * pre-boot GUI launcher, and read by main_runner to size the window, pick render
 * scaling, gate audio, and route per-player input. One shared global, g_nes_config.
 */
#pragma once

#include <stddef.h>  /* size_t */

typedef struct {
    /* Display */
    int window_scale;     /* 1..N integer scale (native 256x240); default 3 */
    int fullscreen;       /* 0 windowed, 1 borderless-desktop */
    int integer_scale;    /* snap to whole-pixel multiples; default 1 */
    int linear_filter;    /* bilinear (1) vs nearest (0); default 0 */
    int renderer;         /* game output: 0 accelerated, 1 software; default 0 */
    int widescreen;       /* experimental 16:9 (per-game; SMB); default 0 */

    /* Audio (always on) */
    int volume;           /* 0..100; default 100 */

    /* Input (2 players): 0 none, 1 keyboard, 2 gamepad */
    int player_src[2];    /* default { 1 keyboard, 2 gamepad } */
    int deadzone[2];      /* 0..100 percent; default { 30, 30 } */

    /* Launcher behaviour */
    int skip_launcher;    /* boot straight to the game; default 0 */
} NesConfig;

extern NesConfig g_nes_config;

/* Reset *c to built-in defaults. */
void config_set_defaults(NesConfig *c);

/* Directory containing the running executable, WITH trailing separator,
 * into `out`. On an AppImage, /proc/self/exe points into the read-only
 * squashfs mount, so $APPIMAGE (the .AppImage's path, exported by the
 * AppImage runtime) is preferred on Linux — config.ini / rom.cfg / saves
 * then anchor next to the .AppImage, as they do next to the .exe on
 * Windows. Falls back to "./" when the path can't be determined. */
void nesrecomp_exe_dir(char *out, size_t max_len);

/* Absolute <exe_dir>/config.ini path (static buffer). */
const char *config_path(void);

/* Load config.ini at `path` into g_nes_config (defaults first, then overlay any
 * keys found). Missing/unreadable file => pure defaults. */
void config_load(const char *path);

/* Write g_nes_config to config.ini at `path`. */
void config_save(const char *path);
