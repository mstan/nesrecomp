/*
 * cyc_host_extras.h - what a game adds to the cycle host.
 *
 * A game project passes its own sources to nesrecomp_add_cycle_game(...
 * HOST_EXTRAS <files>) and defines cyc_host_extras() there; without it the
 * host links cyc_host_extras_none.c, which returns NULL. Everything is
 * optional: leave a field NULL/0 and the host does what it does today. The
 * headless host uses the fields marked (both); the window uses all of them.
 *
 *   present      The picture the window shows. Default: the engine's
 *                (cyc_render.h: the machine's 256x240 frame, or a game
 *                compositor's picture at cyc_video_width()). A game that draws
 *                everything itself returns its own ARGB8888 buffer; the width
 *                may change from frame to frame and the window follows it. It
 *                is presentation: screenshots from the window use it,
 *                headless --hash-out / --screenshot / --frame-log never do.
 *
 *   view_modes   RECOMP_RUNTIME_UI_VIEW_MODE_* bits (recomp_runtime_ui.h) the
 *                game can present; the runtime menu then shows the standard
 *                "View mode" row, and get/set_view_mode read and apply it
 *                live. The value persists in config.ini [Game] ViewMode.
 *
 *   menu_items   Extra rows for recomp-ui's runtime menu (struct
 *                RecompRuntimeUiItem, recomp_runtime_ui.h), in the game's own
 *                sections; keys must not start with "cyc.". The host routes
 *                every callback for these keys to menu_callbacks (whose
 *                context is its own). Rows need a recomp-ui build.
 *
 *   load_setting / save_settings
 *                The game's own persistent values, in config.ini [Game]: the
 *                host calls load_setting for every key of that section at
 *                start, and save_settings (write "Key = value" lines) whenever
 *                it saves.
 *
 *   power_on     (both) The machine was just powered on, before its first
 *                frame (and after mods activated): reset the game's own state.
 *
 *   frame_begin  (both) A frame is about to run (before cyc_run_frame).
 *
 *   frame_end    (both) The machine finished a frame (cyc_run_frame), before
 *                anything is presented: the place for a game's per-frame work
 *                (caches, simulation) that must happen whether or not the
 *                frame is shown. The frame's picture is already current: a
 *                cyc_render_present here composes the one the window shows
 *                next (a probe measuring the composed picture every frame).
 *
 *   options / option
 *                (both) Developer command-line options of the game's own, as
 *                "--name value" (takes_value) or "--name". The host lists them
 *                in its usage, parses them with its own, and passes each to
 *                option() once mods have activated (so an option overrides a
 *                saved mod selection); option() returns false to reject the
 *                value, which stops the program (exit 2).
 *
 *   tcp_setup    Register the game's own TCP debug commands (cyc_tcp.h
 *                cyc_tcp_register) when the window starts its server.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

struct RecompRuntimeUiItem;
struct RecompRuntimeUiCallbacks;

typedef struct {
    const char *name;         /* "--widescreen" */
    bool        takes_value;
    const char *help;         /* one line for the usage */
} CycHostOption;

typedef struct CycHostExtras {
    void *ctx;
    const uint32_t *(*present)(void *ctx, int *width, int *height);
    unsigned view_modes;
    int  (*get_view_mode)(void *ctx);
    bool (*set_view_mode)(void *ctx, int mode);
    const struct RecompRuntimeUiItem *menu_items;
    size_t menu_item_count;
    const struct RecompRuntimeUiCallbacks *menu_callbacks;
    void (*load_setting)(void *ctx, const char *key, const char *value);
    void (*save_settings)(void *ctx, FILE *f);
    void (*power_on)(void *ctx);
    void (*frame_begin)(void *ctx);
    void (*frame_end)(void *ctx);
    const CycHostOption *options;
    size_t option_count;
    bool (*option)(void *ctx, const char *name, const char *value);
    void (*tcp_setup)(void *ctx);
} CycHostExtras;

const CycHostExtras *cyc_host_extras(void);

/* The host's --no-save policy also applies to game-owned password sidecars.
 * Available before the launcher opens and before power_on / option callbacks.
 * Explicit command-line passwords may still be used without loading a save. */
bool cyc_host_saves_enabled(void);

#ifdef __cplusplus
}
#endif
