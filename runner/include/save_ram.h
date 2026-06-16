/*
 * save_ram.h — generic battery-SRAM persistence for the NES runner.
 *
 * One shared mechanism for every game's saves: the 8 KB cartridge SRAM window
 * ($6000-$7FFF, `g_sram`) is mirrored to <exe_dir>/saves/<title>.srm. It is
 * loaded on boot, flushed (dirty-checked) periodically and on exit, and exposed
 * to the launcher's SAVES panel for import/clear.
 *
 * Activation:
 *   - Real battery games: the runner passes the iNES battery bit to
 *     save_ram_init(); persistence turns on automatically — no per-game code.
 *   - Synthetic-SRAM games (an enhancement for cartridges with no real battery,
 *     e.g. a password game that stashes its progress in the unused SRAM window):
 *     the game calls save_ram_request_enable() from game_on_init(). Default OFF.
 *
 * The launcher treats both identically: to the user it is simply "SRAM".
 */
#pragma once
#include <stddef.h>

/*
 * Opt into SRAM persistence even when the iNES header has no battery bit
 * (synthetic-SRAM enhancement). Call from a game's game_on_init(), which runs
 * before the runner calls save_ram_init().
 *   basename : save-file stem -> saves/<basename>.srm. NULL keeps the runner's
 *              default (the sanitized game name). Non-NULL also pins the filename
 *              for real battery games that want a specific stem.
 */
void save_ram_request_enable(const char *basename);

/*
 * Optional one-time migration from an older save location. If, at init, no save
 * exists yet at the canonical saves/<title>.srm path but a file exists at
 * legacy_abs, the legacy file is copied into place (and then loaded). Lets a game
 * that previously wrote its .srm elsewhere keep existing saves. Call from
 * game_on_init() (before the runner's save_ram_init()).
 */
void save_ram_set_legacy_path(const char *legacy_abs);

/*
 * Initialize persistence. Called once by the runner after game_on_init().
 *   default_title : fallback stem (sanitized game name) when none was pinned.
 *   battery_bit   : nonzero if the iNES header marks battery-backed SRAM.
 * Persistence activates when battery_bit is set OR a game requested it. On
 * activation: loads saves/<title>.srm into g_sram (if present) and registers an
 * atexit flush. A no-op (NONE backend) when neither condition holds.
 */
void save_ram_init(const char *default_title, int battery_bit);

/*
 * Per-VBlank dirty-checked flush; the runner calls this every frame. Internally
 * throttled, and a no-op when persistence is inactive or g_sram is unchanged
 * since the last flush.
 */
void save_ram_tick(void);

/* Force an immediate dirty-checked flush (used on exit and by import/clear). */
void save_ram_flush(void);

/*
 * Bind the save-file path for UI use WITHOUT activating runtime persistence.
 * The pre-boot launcher calls this so its SAVES panel (path/exists/size/import/
 * clear) operates on the exact same saves/<basename>.srm the runtime backend
 * will use — one source of truth for the file, no duplicated path logic. Does
 * not load g_sram, register an atexit flush, or start the periodic tick.
 */
void save_ram_ui_bind(const char *basename);

/* ---- Launcher / UI helpers ---- */

/* 1 if SRAM persistence is enabled for this game, else 0. */
int save_ram_active(void);

/* Absolute saves/<title>.srm path, or "" when inactive. */
const char *save_ram_path(void);

/* 1 if the .srm file currently exists on disk. */
int save_ram_exists(void);

/* Size of the .srm file in bytes, or 0 if absent/inactive. */
long save_ram_size(void);

/*
 * Import an external save: back up any existing .srm to <path>.bak, copy
 * src_path into place, and reload it into g_sram. Returns 1 on success.
 */
int save_ram_import(const char *src_path);

/*
 * Clear the save: back up the existing .srm to <path>.bak, delete it, and reset
 * g_sram to a fresh (all-0xFF) state. Returns 1 on success.
 */
int save_ram_clear(void);
