/*
 * cyc_session.h - what both cycle hosts (headless cyc_host.c, the window
 * cyc_sdl.c) do around the machine for a game's own additions:
 *
 *   - the game's extras (cyc_host_extras.h) and its developer command-line
 *     options, parsed with the host's and applied after mods activate;
 *   - the mod package runtime (runner/include/mod_runtime.h) for a game built
 *     with nesrecomp_add_cycle_game(... MODS GAME_ID <id>): the catalog under
 *     <exe>/mods in the window (the launcher's Mods screen edits it), and only
 *     with --mods-root DIR headless (runs stay reproducible without it); PLAY
 *     commits the selection against the chosen image and activates the
 *     selected trusted plugins;
 *   - binding the plugins that registered at hook sites (cyc_hooks.h), which
 *     refuses to start on a registration no site declares.
 *
 * Without mods and extras every call here does nothing.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "cyc_host_extras.h"

#ifdef __cplusplus
extern "C" {
#endif

const CycHostExtras *cyc_session_extras(void);

/* argv[*i] is one of the game's options: take it (and its value, advancing
 * *i) and return true; *bad when its value is missing. */
bool cyc_session_take_option(int argc, char **argv, int *i, bool *bad);
void cyc_session_print_options(FILE *f);

/* The mod catalog (root NULL: none this run). False only for a catalog that
 * cannot be read. */
bool cyc_session_mods_init(const char *root, char *err, size_t err_len);
bool cyc_session_mods_enabled(void);
/* recomp-ui's RecompLauncherCModProvider for the launcher, or NULL. */
const void *cyc_session_mods_provider(void);
/* Commit the selection for this image and activate its plugins (reset
 * callbacks first). */
bool cyc_session_mods_start(const char *image_path, char *err, size_t err_len);
/* Activate the committed plugins again after the selection changed in the
 * running game (the runtime menu's Mods rows). */
void cyc_session_mods_reapply(void);

/* After power-on, before the first frame: bind hook plugins, apply the game's
 * command-line options, the game's power_on. False: do not run. */
bool cyc_session_start(void);
/* Before every frame: the game's frame_begin. */
void cyc_session_frame_begin(void);
void cyc_session_input(uint8_t buttons[2]);
void cyc_session_event(const void *event, int player);
/* After every frame: a new picture to present, then the game's frame_end. */
void cyc_session_frame_end(void);
/* After a save state replaced the machine. */
void cyc_session_state_loaded(void);

#ifdef __cplusplus
}
#endif
