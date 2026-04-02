/*
 * debug_server.h — TCP debug server for NES recomp projects
 *
 * Non-blocking TCP server on localhost (default port 4370).
 * JSON-over-newline protocol, polled once per frame from the VBlank callback.
 * Includes a 36000-frame ring buffer for retroactive state queries.
 *
 * Modeled after segagenesisrecomp-v2/cmd_server and snesrecomp-v2/debug_server.
 */
#pragma once
#include <stdint.h>

/* ---- Ring buffer frame record ---- */

#define FRAME_HISTORY_CAP 36000   /* ~10 min @ 60fps */
#define MAX_FRAME_DIFFS   32

typedef struct {
    uint16_t addr;
    uint8_t  mine;
    uint8_t  theirs;
} FrameDiffEntry;

typedef struct {
    uint32_t frame_number;
    int      verify_pass;           /* 1=pass, 0=fail, -1=not checked */
    int      diff_count;            /* diverging bytes (verify mode) */

    /* CPU state */
    uint8_t  cpu_a, cpu_x, cpu_y, cpu_s, cpu_p;

    /* PPU state */
    uint8_t  ppuctrl, ppumask;
    uint8_t  ppuscroll_x, ppuscroll_y;

    /* Mapper + input */
    int      current_bank;
    uint8_t  controller_buttons;

    /* Game-specific (filled by game_fill_frame_record hook) */
    uint8_t  game_data[16];

    /* Zero page RAM snapshot ($00-$FF) for frame-level comparison */
    uint8_t  ram_zp[256];

    /* Divergence diffs (verify mode only) */
    FrameDiffEntry diffs[MAX_FRAME_DIFFS];

    /* Last recomp function name */
    char     last_func[32];
} NESFrameRecord;

/* ---- Public API ---- */

/* Initialize the server. Call once at startup (e.g., from game_on_init).
 * port=0 uses the default (4370). */
void debug_server_init(int port);

/* Poll for incoming connections and commands. Non-blocking.
 * Call once per frame (from game_on_frame). */
void debug_server_poll(void);

/* Record the current frame's state into the ring buffer.
 * Call after NMI runs (from game_post_nmi). */
void debug_server_record_frame(void);

/* Block while paused, polling TCP + SDL events.
 * Call from game_on_frame before NMI runs. */
void debug_server_wait_if_paused(void);

/* Graceful shutdown. Call at exit. */
void debug_server_shutdown(void);

/* Check if a TCP client is connected. */
int debug_server_is_connected(void);

/* ---- Watchpoint notifications (called from main loop) ---- */

/* Check all watchpoints against current RAM values.
 * Sends JSON notification for any changes. */
void debug_server_check_watchpoints(void);

/* ---- Input override ---- */

/* Returns >= 0 if the debug server wants to override controller input,
 * -1 if no override is active. */
int debug_server_get_input_override(void);

/* ---- Public send helpers (for game command handlers) ---- */

/* Send a complete JSON line to the connected client. */
void debug_server_send_line(const char *json);

/* Send a formatted JSON line (printf-style) to the connected client. */
void debug_server_send_fmt(const char *fmt, ...);
