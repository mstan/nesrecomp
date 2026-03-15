/*
 * game_extras.h — Game-specific hook interface for the runner.
 *
 * Each game provides exactly one extras.c in games/<name>/extras.c
 * that implements these functions. The runner calls them at the
 * appropriate points in the main loop.
 *
 * For games with no special behavior, provide empty implementations.
 * See games/super-mario-bros/extras.c for the stub template.
 */
#pragma once
#include <stdint.h>

/* Human-readable game name, shown in the window title */
const char *game_get_name(void);

/* Called once after ROM is loaded and runtime_init() completes */
void game_on_init(void);

/* Called every VBlank, before NMI runs */
void game_on_frame(uint64_t frame_count);

/* Called every VBlank, after NMI runs (nametable is up-to-date) */
void game_post_nmi(uint64_t frame_count);

/*
 * Handle a game-specific CLI argument.
 * key = "--flag", val = next argv (may be NULL if no following arg).
 * Returns 1 if the key was consumed (runner should skip val too if used),
 * 0 if not recognized.
 */
int game_handle_arg(const char *key, const char *val);

/* One-line usage string for game-specific args, or NULL if none */
const char *game_arg_usage(void);

/*
 * Expected CRC32 of the ROM file (entire .nes file, iNES header included).
 * The launcher checks this before starting the game and re-prompts if wrong.
 * Return 0 to skip verification.
 */
uint32_t game_get_expected_crc32(void);
