/*
 * input_script.h — NES input script recording and playback
 *
 * Script format (plain text):
 *   WAIT <frames>
 *   HOLD <BUTTON> [player] # A B SELECT START UP DOWN LEFT RIGHT; player 1..4, default 1
 *   RELEASE <BUTTON> [player]
 *   TURBO ON|OFF           # toggle fast-forward (skip 60Hz delay)
 *   SCREENSHOT [filename]  # absolute path as-is; else $NESRECOMP_SHOT_DIR/filename
 *                          # (default dir C:/temp; name nes_script_NNN.png)
 *   KEY_TAP KP8            # enqueue one host keyboard down/up pair
 *   LOG <message>
 *   EXIT [code]
 *   WAIT_RAM8 <hex_addr> <hex_value>    # block until g_ram[addr]==value (30s timeout)
 *   WAIT_FOREIGN_STATE <state> [frame]  # block until active controller reports state
 *                                        # and optional exact public frame (30s timeout)
 *   ASSERT_RAM8 <hex_addr> <hex_value> [msg]
 *
 * A wait timeout or assert failure marks the script desynced: a run that would
 * exit 0 (EXIT 0 or end of script) exits 3 instead. Explicit nonzero EXIT
 * codes are kept.
 */
#pragma once
#include <stdint.h>

/* Set by script TURBO ON/OFF — main_runner checks this to skip 60Hz delay */
extern int g_turbo;

int  script_load(const char *path);
void script_tick(uint64_t frame, const uint8_t *ram);
int  script_get_player_buttons(int player);
int  script_get_buttons(void);   /* -1 = no override; else returns button byte */
int  script_check_exit(void);    /* -1 = still running; else exit code */
/* Returns 1 and fills buf with the resolved path if a screenshot was requested this frame */
int  script_wants_screenshot(char *buf, int buflen);
/* Set a prefix for auto-named screenshots (e.g. "native_" or "emu_") */
void script_set_screenshot_prefix(const char *prefix);

/* Returns 1 if the script is currently overriding the zapper trigger */
int  script_has_trigger_override(void);

/* Recording — call once per frame BEFORE applying script override */
void record_open(const char *path);
void record_tick(uint64_t frame, uint8_t buttons, int turbo);
/* Emit LOAD_STATE into the recording at the current frame */
void record_loadstate(uint64_t frame, const char *path);
/* Re-sync frame baseline after savestate_load() changes g_frame_count */
void record_sync_frame(uint64_t frame);
/* Flush final WAIT + EXIT 0 — registered automatically via atexit */
void record_close(void);
