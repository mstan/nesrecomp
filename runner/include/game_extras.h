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
 * Expected CRC32 of the ROM data (16-byte iNES header excluded).
 * The launcher checks this before starting the game and re-prompts if wrong.
 * Return 0 to skip verification.
 */
uint32_t game_get_expected_crc32(void);

/*
 * Called when call_by_address has no entry for the given address.
 * The game can handle the call (e.g. SRAM code remapping) and return 1,
 * or return 0 to fall through to the dispatch miss log.
 */
int game_dispatch_override(uint16_t addr);

/*
 * RAM read hook — called from generated code when a ram_read_hook address
 * is read via absolute addressing mode. Allows game-specific extras.c to
 * adjust the value per call-site (identified by the 6502 PC).
 * Default: return val unchanged.
 *
 * pc   = 6502 program counter of the reading instruction
 * addr = RAM address being read
 * val  = raw value from nes_read()
 * Returns: (possibly adjusted) value to use
 */
uint8_t game_ram_read_hook(uint16_t pc, uint16_t addr, uint8_t val);

/*
 * Run the NMI handler for one frame. Default implementation calls func_NMI().
 * Override in extras.c to inject verify mode (dual-execution comparison).
 */
void game_run_nmi(void);

/*
 * Run the main game loop. Default implementation calls func_RESET() (never returns).
 * Override in extras.c for emulated mode (FCEUX drives the main loop instead).
 * When this function returns, the runner exits.
 */
void game_run_main(void);

/* Fill game-specific data in the debug frame record.
 * Called each frame from debug_server_record_frame().
 * Cast record to NESFrameRecord* and write up to 16 bytes into game_data[]. */
void game_fill_frame_record(void *record);

/* Handle a game-specific TCP debug command.
 * Returns 1 if handled, 0 if not recognized.
 * Use debug_server_send_fmt() to send responses. */
int game_handle_debug_cmd(const char *cmd, int id, const char *json);
