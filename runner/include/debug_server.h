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
#include "mapper.h"

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

    /* ---- CPU state (exhaustive) ---- */
    uint8_t  cpu_a, cpu_x, cpu_y, cpu_s, cpu_p;
    uint8_t  cpu_n, cpu_v, cpu_d, cpu_i, cpu_z, cpu_c;  /* exploded flags */

    /* ---- PPU registers (exhaustive) ---- */
    uint8_t  ppuctrl, ppumask, ppustatus;
    uint8_t  oamaddr;
    uint8_t  ppuscroll_x, ppuscroll_y;
    uint16_t ppuaddr;               /* full VRAM address ($2006) */
    uint8_t  ppuaddr_latch;         /* $2006 write toggle (0=hi, 1=lo) */
    uint8_t  scroll_latch;          /* $2005 write toggle */
    uint8_t  ppudata_buf;           /* PPU read delay buffer */

    /* ---- Sprite-0 split state ---- */
    uint8_t  ppuscroll_x_hud, ppuscroll_y_hud, ppuctrl_hud;
    int      spr0_split_active;
    int      spr0_reads_ctr;

    /* ---- VBlank / timing state ---- */
    uint32_t ops_count;             /* cycle budget consumed this frame */
    int      vblank_depth;          /* NMI nesting depth */

    /* ---- Mapper (exhaustive) ---- */
    int      current_bank;
    MapperState mapper;

    /* ---- Controller state (exhaustive) ---- */
    uint8_t  controller1_buttons;
    uint8_t  controller2_buttons;
    uint8_t  ctrl1_shift;           /* shift register (button readout) */
    uint8_t  ctrl2_shift;
    uint8_t  ctrl1_strobe;

    /* ---- Game-specific (filled by game_fill_frame_record hook) ---- */
    uint8_t  game_data[32];

    /* ---- Full memory snapshots ---- */
    uint8_t  ram_full[0x0800];      /* 2KB work RAM ($0000-$07FF) */
    uint8_t  sram[0x2000];          /* 8KB SRAM ($6000-$7FFF) */
    uint8_t  chr_ram[0x2000];       /* 8KB CHR RAM ($0000-$1FFF PPU) */
    uint8_t  ppu_nt[0x1000];        /* 4KB nametable RAM */
    uint8_t  ppu_pal[0x20];         /* 32-byte palette */
    uint8_t  oam[0x100];            /* 256-byte OAM (sprite RAM) */

    /* Legacy note: ram_zp is now ram_full (first 256 bytes = zero page) */

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

/* ---- RAM Followers (write-level tracing) ---- */

/* Called from nes_write() when a followed address is written.
 * Records frame, old/new values, and call stack to a ring buffer.
 * Can optionally pause on a specific value (conditional breakpoint). */
void debug_server_notify_write(uint16_t addr, uint8_t old_val, uint8_t new_val);

/* ---- S-register change tracking ---- */

/* Check if S register changed since last call. If watch_s is enabled and
 * S changed, records the change with call stack to a ring buffer.
 * Call from maybe_trigger_vblank() for per-instruction tracking. */
void debug_server_check_s(void);

/* Returns 1 if any follower is watching this address. Used by nes_write()
 * to avoid the overhead of notify_write when no followers are active. */
int  debug_server_has_follower(uint16_t addr);

/* Register a follower programmatically (e.g., from game_on_init).
 * break_on_val: -1 = no break, 0-255 = pause when this value is written.
 * Returns slot index on success, -1 if full. */
int  debug_server_add_follower(uint16_t addr, int break_on_val);

/* ---- Input override ---- */

/* Returns >= 0 if the debug server wants to override controller input,
 * -1 if no override is active. */
int debug_server_get_input_override(void);

/* ---- Verify-mode result reporting ---- */

/* Called from a game's verify_mode after diffing native vs the oracle for
 * the current frame. The result is stashed in pending state and consumed
 * by the next debug_server_record_frame() call into that frame's record's
 * verify_pass / diff_count / diffs[] fields, regardless of which order the
 * caller invokes set_verify_result and record_frame in.
 *
 *   passed:     1 = no diffs, 0 = at least one diff
 *   diff_count: total number of diverging bytes (may exceed n_diffs)
 *   diffs:      first n_diffs entries (clipped to MAX_FRAME_DIFFS)
 *   n_diffs:    length of `diffs` array (0..MAX_FRAME_DIFFS) */
void debug_server_set_verify_result(int passed, int diff_count,
                                    const FrameDiffEntry *diffs, int n_diffs);

/* ---- Public send helpers (for game command handlers) ---- */

/* Send a complete JSON line to the connected client. */
void debug_server_send_line(const char *json);

/* Send a formatted JSON line (printf-style) to the connected client. */
void debug_server_send_fmt(const char *fmt, ...);
