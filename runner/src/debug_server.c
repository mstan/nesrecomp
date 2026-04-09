/*
 * debug_server.c — TCP debug server for NES recomp (shared runner)
 *
 * Single-threaded, non-blocking TCP server polled once per frame.
 * JSON-over-newline protocol on localhost:4370.
 *
 * Game-specific commands are dispatched via game_handle_debug_cmd() hook.
 * Game-specific frame data is filled via game_fill_frame_record() hook.
 *
 * Modeled after:
 *   segagenesisrecomp-v2/sonicthehedgehog/runner/cmd_server.c
 *   snesrecomp-v2/src/debug_server.c
 */
#include "debug_server.h"
#include "game_extras.h"
#include "nes_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

/* ---- Platform sockets ---- */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close closesocket
#  define SOCK_WOULDBLOCK WSAEWOULDBLOCK
   static int sock_error(void) { return WSAGetLastError(); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close close
#  define SOCK_WOULDBLOCK EWOULDBLOCK
   static int sock_error(void) { return errno; }
#endif

#include <SDL.h>  /* for SDL_PollEvent in pause loop */

/* ---- Server state ---- */
static sock_t s_listen  = SOCK_INVALID;
static sock_t s_client  = SOCK_INVALID;
static int    s_port    = 4370;

#define RECV_BUF_SIZE 8192
static char s_recv_buf[RECV_BUF_SIZE];
static int  s_recv_len = 0;

/* ---- Pause / step ---- */
static volatile int s_paused     = 0;
static int          s_step_count = 0;   /* frames remaining in step mode */
static uint32_t     s_run_to     = 0;   /* target frame for run_to_frame (0=disabled) */

/* ---- Input override ---- */
static int s_input_override = -1;  /* -1 = no override */

/* ---- Ring buffer (heap-allocated, ~540MB for full-state snapshots) ---- */
static NESFrameRecord *s_frame_history = NULL;
static uint64_t        s_history_count = 0;

/* ---- Verify-mode pending state ----
 * Caller (a game's verify_mode.c) calls debug_server_set_verify_result()
 * after running the diff for the current frame. record_frame() consumes
 * these and writes them into the new record at frame end. The deferred
 * pattern avoids any assumption about whether set_verify_result() runs
 * before or after the frame's record_frame() in the host loop. */
static int            s_pending_verify_set        = 0;
static int            s_pending_verify_pass       = -1;
static int            s_pending_verify_diff_count = 0;
static FrameDiffEntry s_pending_verify_diffs[MAX_FRAME_DIFFS];
static int            s_pending_verify_n_diffs    = 0;

/* ---- Watchpoints ---- */
#define MAX_WATCHPOINTS 8
typedef struct {
    uint32_t addr;
    uint8_t  prev_val;
    int      active;
} Watchpoint;
static Watchpoint s_watchpoints[MAX_WATCHPOINTS];

/* ---- RAM Followers (write-level tracing with call stack) ---- */
#define MAX_FOLLOWERS     8
#define FOLLOW_LOG_CAP    8192  /* entries per follower */

typedef struct {
    uint64_t frame;
    uint8_t  old_val;
    uint8_t  new_val;
    char     call_stack[128];   /* captured recomp stack snapshot */
} FollowEntry;

typedef struct {
    uint32_t     addr;
    int          active;
    int          break_on_val;  /* -1 = disabled, 0-255 = break when this value written */
    FollowEntry  log[FOLLOW_LOG_CAP];
    uint32_t     log_head;      /* next write position (circular) */
    uint32_t     log_count;     /* total writes recorded */
} Follower;

static Follower s_followers[MAX_FOLLOWERS];

/* ---- S-register change tracker ---- */
#define S_TRACK_LOG_CAP 4096

typedef struct {
    uint64_t frame;
    uint8_t  old_val;
    uint8_t  new_val;
    char     call_stack[128];
} STrackEntry;

static int          s_track_s_enabled = 0;
static uint8_t      s_track_s_prev = 0xFF;
static STrackEntry  s_track_s_log[S_TRACK_LOG_CAP];
static uint32_t     s_track_s_head = 0;
static uint32_t     s_track_s_count = 0;
static int          s_track_s_break_on_change = 0;  /* pause when S changes */
static uint64_t     s_track_s_frame_lo = 0;  /* only track in this frame range */
static uint64_t     s_track_s_frame_hi = UINT64_MAX;

/* ---- Recomp stack (extern if available) ---- */
#ifdef RECOMP_STACK_TRACKING
extern const char *g_recomp_stack[];
extern int         g_recomp_stack_top;
extern const char *g_last_recomp_func;
#else
static const char *g_last_recomp_func = "(no stack tracking)";
#endif

/* ---- Watchdog externs (defined in game's watchdog.c or extras.c) ---- */
/* Every game must define these three globals (use zero/NULL for stubs). */
extern int         g_watchdog_triggered;
extern uint32_t    g_watchdog_frame;
extern const char *g_watchdog_stack_dump;

/* ---- Platform helpers ---- */
static void set_nonblocking(sock_t s)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

/* ---- JSON helpers (hand-parsed, no library) ---- */

static const char *json_get_str(const char *json, const char *key,
                                 char *out, int out_sz)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < out_sz - 1)
            out[i++] = *p++;
        out[i] = '\0';
        return out;
    }
    /* Unquoted value (number etc) */
    {
        int i = 0;
        while (*p && *p != ',' && *p != '}' && *p != ' ' && i < out_sz - 1)
            out[i++] = *p++;
        out[i] = '\0';
        return out;
    }
}

static int json_get_int(const char *json, const char *key, int def)
{
    char buf[64];
    if (!json_get_str(json, key, buf, sizeof(buf))) return def;
    return atoi(buf);
}

static uint32_t hex_to_u32(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return (uint32_t)strtoul(s, NULL, 16);
}

/* ---- Send helpers (public API) ---- */

/* Temporarily switch socket to blocking, send all bytes, restore non-blocking.
 * This avoids WOULDBLOCK retry loops that can re-enter the game loop via
 * SDL_Delay and corrupt state (e.g. watchdog firing during send). */
static void send_all_blocking(sock_t sock, const char *data, int len)
{
#ifdef _WIN32
    u_long mode = 0;  /* blocking */
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
#endif

    int sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n > 0) { sent += n; continue; }
        break;  /* error — give up */
    }

#ifdef _WIN32
    mode = 1;  /* non-blocking */
    ioctlsocket(sock, FIONBIO, &mode);
#else
    fcntl(sock, F_SETFL, flags);
#endif
}

void debug_server_send_line(const char *json)
{
    if (s_client == SOCK_INVALID) return;
    int len = (int)strlen(json);
    send_all_blocking(s_client, json, len);
    send_all_blocking(s_client, "\n", 1);
}

void debug_server_send_fmt(const char *fmt, ...)
{
    char buf[16384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    debug_server_send_line(buf);
}

/* Internal short aliases */
#define send_line  debug_server_send_line
#define send_fmt   debug_server_send_fmt

static void send_ok(int id)
{
    send_fmt("{\"id\":%d,\"ok\":true}", id);
}

static void send_err(int id, const char *msg)
{
    send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"%s\"}", id, msg);
}

/* ---- RAM access helpers ---- */

/* Read a byte from the NES address space (mirroring g_ram/g_sram/PPU) */
static uint8_t read_byte(uint32_t addr)
{
    if (addr < 0x0800)
        return g_ram[addr];
    if (addr < 0x2000)
        return g_ram[addr & 0x07FF];  /* RAM mirrors */
    if (addr >= 0x6000 && addr < 0x8000)
        return g_sram[addr - 0x6000];
    /* PPU nametable */
    if (addr >= 0x2000 && addr < 0x3000)
        return g_ppu_nt[addr - 0x2000];
    /* PPU palette */
    if (addr >= 0x3F00 && addr < 0x3F20)
        return g_ppu_pal[addr - 0x3F00];
    /* OAM (via separate array) */
    if (addr >= 0xFE00 && addr < 0xFF00)
        return g_ppu_oam[addr - 0xFE00];
    return 0;
}

static void write_byte(uint32_t addr, uint8_t val)
{
    if (addr < 0x0800)
        g_ram[addr] = val;
    else if (addr < 0x2000)
        g_ram[addr & 0x07FF] = val;
    else if (addr >= 0x6000 && addr < 0x8000)
        g_sram[addr - 0x6000] = val;
}

/* ---- Command handlers ---- */

static void handle_ping(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%llu}",
             id, (unsigned long long)g_frame_count);
}

static void handle_frame(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%llu,\"last_func\":\"%s\"}",
             id, (unsigned long long)g_frame_count, g_last_recomp_func);
}

static void handle_get_registers(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"A\":\"0x%02X\",\"X\":\"0x%02X\",\"Y\":\"0x%02X\","
             "\"S\":\"0x%02X\",\"P\":\"0x%02X\","
             "\"N\":%d,\"V\":%d,\"D\":%d,\"I\":%d,\"Z\":%d,\"C\":%d,"
             "\"bank\":%d,\"frame\":%llu}",
             id, g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S, g_cpu.P,
             g_cpu.N, g_cpu.V, g_cpu.D, g_cpu.I, g_cpu.Z, g_cpu.C,
             g_current_bank, (unsigned long long)g_frame_count);
}

static void handle_read_ram(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 1);
    if (len < 1) len = 1;
    if (len > 256) len = 256;

    /* Build hex string */
    char hex[513];
    for (int i = 0; i < len; i++)
        snprintf(hex + i * 2, 3, "%02x", read_byte(addr + i));

    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"len\":%d,\"hex\":\"%s\"}",
             id, addr, len, hex);
}

static void handle_dump_ram(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 256);
    if (len < 1) len = 1;
    if (len > 8192) len = 8192;

    /* Send in chunks of 256 bytes */
    int offset = 0;
    while (offset < len) {
        int chunk = len - offset;
        if (chunk > 256) chunk = 256;
        char hex[513];
        for (int i = 0; i < chunk; i++)
            snprintf(hex + i * 2, 3, "%02x", read_byte(addr + offset + i));
        send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"offset\":%d,\"len\":%d,\"hex\":\"%s\"}",
                 id, addr + offset, offset, chunk, hex);
        offset += chunk;
    }
}

static void handle_write_ram(int id, const char *json)
{
    char addr_str[32], val_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    if (!json_get_str(json, "val", val_str, sizeof(val_str))) {
        send_err(id, "missing val");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    uint8_t val = (uint8_t)hex_to_u32(val_str);
    write_byte(addr, val);
    send_ok(id);
}

static void handle_read_ppu(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 1);
    if (len < 1) len = 1;
    if (len > 256) len = 256;

    char hex[513];
    for (int i = 0; i < len; i++) {
        uint32_t a = addr + i;
        uint8_t v = 0;
        if (a < 0x2000)
            v = g_chr_ram[a];
        else if (a < 0x3000)
            v = g_ppu_nt[a - 0x2000];
        else if (a >= 0x3F00 && a < 0x3F20)
            v = g_ppu_pal[a - 0x3F00];
        snprintf(hex + i * 2, 3, "%02x", v);
    }

    send_fmt("{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"len\":%d,\"hex\":\"%s\"}",
             id, addr, len, hex);
}

static void handle_mapper_state(int id, const char *json)
{
    (void)json;
    MapperState ms;
    mapper_get_state(&ms);
    send_fmt("{\"id\":%d,\"ok\":true,\"bank\":%d,"
             "\"type\":%d,\"mirror\":%d,"
             "\"mmc3_bank_sel\":%d,"
             "\"mmc3_regs\":[%d,%d,%d,%d,%d,%d,%d,%d],"
             "\"mmc3_irq_latch\":%d,\"mmc3_irq_counter\":%d,"
             "\"mmc3_irq_reload\":%d,\"mmc3_irq_enabled\":%d}",
             id, g_current_bank,
             ms.mapper_type, ms.mirroring,
             ms.mmc3_bank_select,
             ms.mmc3_regs[0], ms.mmc3_regs[1],
             ms.mmc3_regs[2], ms.mmc3_regs[3],
             ms.mmc3_regs[4], ms.mmc3_regs[5],
             ms.mmc3_regs[6], ms.mmc3_regs[7],
             ms.mmc3_irq_latch, ms.mmc3_irq_counter,
             ms.mmc3_irq_reload, ms.mmc3_irq_enabled);
}

static void handle_watch(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (!s_watchpoints[i].active) {
            s_watchpoints[i].addr = addr;
            s_watchpoints[i].prev_val = read_byte(addr);
            s_watchpoints[i].active = 1;
            send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"addr\":\"0x%04X\"}",
                     id, i, addr);
            return;
        }
    }
    send_err(id, "all watchpoint slots full (max 8)");
}

static void handle_unwatch(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (s_watchpoints[i].active && s_watchpoints[i].addr == addr) {
            s_watchpoints[i].active = 0;
            send_ok(id);
            return;
        }
    }
    send_err(id, "watchpoint not found");
}

static void handle_set_input(int id, const char *json)
{
    char val_str[32];
    if (!json_get_str(json, "buttons", val_str, sizeof(val_str))) {
        send_err(id, "missing buttons");
        return;
    }
    s_input_override = (int)hex_to_u32(val_str);
    send_ok(id);
}

static void handle_clear_input(int id, const char *json)
{
    (void)json;
    s_input_override = -1;
    send_ok(id);
}

static void handle_pause(int id, const char *json)
{
    (void)json;
    s_paused = 1;
    send_fmt("{\"id\":%d,\"ok\":true,\"paused\":true,\"frame\":%llu}",
             id, (unsigned long long)g_frame_count);
}

static void handle_continue(int id, const char *json)
{
    (void)json;
    s_paused = 0;
    s_step_count = 0;
    s_run_to = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"paused\":false}", id);
}

static void handle_step(int id, const char *json)
{
    int n = json_get_int(json, "count", 1);
    if (n < 1) n = 1;
    s_step_count = n;
    s_paused = 0;  /* unpause for N frames, then re-pause */
    send_fmt("{\"id\":%d,\"ok\":true,\"stepping\":%d}", id, n);
}

static void handle_run_to_frame(int id, const char *json)
{
    int target = json_get_int(json, "frame", 0);
    if (target <= (int)g_frame_count) {
        send_err(id, "target frame already passed");
        return;
    }
    s_run_to = (uint32_t)target;
    s_paused = 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"running_to\":%d}", id, target);
}

/* ---- Ring buffer queries ---- */

static void handle_history(int id, const char *json)
{
    (void)json;
    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;
    send_fmt("{\"id\":%d,\"ok\":true,\"count\":%llu,\"oldest\":%llu,\"newest\":%llu}",
             id,
             (unsigned long long)s_history_count,
             (unsigned long long)oldest,
             (unsigned long long)(s_history_count > 0 ? s_history_count - 1 : 0));
}

static void handle_get_frame(int id, const char *json)
{
    int f = json_get_int(json, "frame", -1);
    if (f < 0) { send_err(id, "missing frame"); return; }

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;
    if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
        send_err(id, "frame not in buffer");
        return;
    }

    uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
    const NESFrameRecord *r = &s_frame_history[idx];
    if (r->frame_number != (uint32_t)f) {
        send_err(id, "frame record mismatch");
        return;
    }

    /* Encode game_data as hex */
    char gd_hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(gd_hex + i * 2, 3, "%02x", r->game_data[i]);

    /* Encode zero page as hex */
    char zp_hex[513];
    for (int i = 0; i < 256; i++)
        snprintf(zp_hex + i * 2, 3, "%02x", r->ram_full[i]);

    /* Use malloc for the large response */
    char *buf = (char *)malloc(5120);
    if (!buf) { send_err(id, "alloc failed"); return; }
    snprintf(buf, 5120,
             "{\"id\":%d,\"ok\":true,"
             "\"frame\":%u,\"verify_pass\":%d,\"diff_count\":%d,"
             "\"cpu\":{\"A\":\"0x%02X\",\"X\":\"0x%02X\",\"Y\":\"0x%02X\","
                      "\"S\":\"0x%02X\",\"P\":\"0x%02X\","
                      "\"N\":%d,\"V\":%d,\"D\":%d,\"I\":%d,\"Z\":%d,\"C\":%d},"
             "\"ppu\":{\"ctrl\":\"0x%02X\",\"mask\":\"0x%02X\",\"status\":\"0x%02X\","
                      "\"oamaddr\":\"0x%02X\","
                      "\"scroll_x\":%d,\"scroll_y\":%d,"
                      "\"ppuaddr\":\"0x%04X\",\"addr_latch\":%d,\"scroll_latch\":%d,"
                      "\"data_buf\":\"0x%02X\","
                      "\"hud_sx\":%d,\"hud_sy\":%d,\"hud_ctrl\":\"0x%02X\","
                      "\"spr0_active\":%d,\"spr0_reads\":%d},"
             "\"timing\":{\"ops_count\":%u,\"vblank_depth\":%d},"
             "\"bank\":%d,"
             "\"mapper\":{\"type\":%d,\"shift_reg\":%d,\"shift_count\":%d,"
                         "\"ctrl\":%d,\"chr0\":%d,\"chr1\":%d,\"prg_reg\":%d,\"mirror\":%d,"
                         "\"mmc3_bank_sel\":%d,"
                         "\"mmc3_regs\":[%d,%d,%d,%d,%d,%d,%d,%d],"
                         "\"mmc3_irq_latch\":%d,\"mmc3_irq_counter\":%d,"
                         "\"mmc3_irq_reload\":%d,\"mmc3_irq_enabled\":%d},"
             "\"buttons1\":\"0x%02X\",\"buttons2\":\"0x%02X\","
             "\"ctrl_shift1\":\"0x%02X\",\"ctrl_shift2\":\"0x%02X\",\"ctrl_strobe\":%d,"
             "\"game_data\":\"%s\","
             "\"ram_zp\":\"%s\","
             "\"last_func\":\"%s\"}",
             id, r->frame_number, r->verify_pass, r->diff_count,
             r->cpu_a, r->cpu_x, r->cpu_y, r->cpu_s, r->cpu_p,
             r->cpu_n, r->cpu_v, r->cpu_d, r->cpu_i, r->cpu_z, r->cpu_c,
             r->ppuctrl, r->ppumask, r->ppustatus, r->oamaddr,
             r->ppuscroll_x, r->ppuscroll_y,
             r->ppuaddr, r->ppuaddr_latch, r->scroll_latch,
             r->ppudata_buf,
             r->ppuscroll_x_hud, r->ppuscroll_y_hud, r->ppuctrl_hud,
             r->spr0_split_active, r->spr0_reads_ctr,
             r->ops_count, r->vblank_depth,
             r->current_bank,
             r->mapper.mapper_type, r->mapper.shift_reg, r->mapper.shift_count,
             r->mapper.ctrl, r->mapper.chr0, r->mapper.chr1, r->mapper.prg_reg,
             r->mapper.mirroring,
             r->mapper.mmc3_bank_select,
             r->mapper.mmc3_regs[0], r->mapper.mmc3_regs[1],
             r->mapper.mmc3_regs[2], r->mapper.mmc3_regs[3],
             r->mapper.mmc3_regs[4], r->mapper.mmc3_regs[5],
             r->mapper.mmc3_regs[6], r->mapper.mmc3_regs[7],
             r->mapper.mmc3_irq_latch, r->mapper.mmc3_irq_counter,
             r->mapper.mmc3_irq_reload, r->mapper.mmc3_irq_enabled,
             r->controller1_buttons, r->controller2_buttons,
             r->ctrl1_shift, r->ctrl2_shift, r->ctrl1_strobe,
             gd_hex, zp_hex,
             r->last_func);
    send_line(buf);
    free(buf);
}

static void handle_frame_range(int id, const char *json)
{
    int start = json_get_int(json, "start", -1);
    int end   = json_get_int(json, "end", -1);
    if (start < 0 || end < 0) { send_err(id, "missing start/end"); return; }
    if (end - start + 1 > 200) { send_err(id, "max 200 frames per request"); return; }

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;

    /* Build response in a large buffer */
    char *buf = (char *)malloc(200 * 256 + 256);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 64, "{\"id\":%d,\"ok\":true,\"frames\":[", id);

    int first = 1;
    for (int f = start; f <= end; f++) {
        if (!first) buf[pos++] = ',';
        first = 0;

        if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
            pos += snprintf(buf + pos, 128,
                "{\"frame\":%d,\"available\":false}", f);
            continue;
        }

        uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
        const NESFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number != (uint32_t)f) {
            pos += snprintf(buf + pos, 128,
                "{\"frame\":%d,\"available\":false}", f);
            continue;
        }

        /* Encode game_data as hex */
        char gd_hex[33];
        for (int i = 0; i < 16; i++)
            snprintf(gd_hex + i * 2, 3, "%02x", r->game_data[i]);

        pos += snprintf(buf + pos, 256,
            "{\"frame\":%u,\"verify\":%d,\"bank\":%d,\"btn\":\"0x%02X\","
            "\"game_data\":\"%s\"}",
            r->frame_number, r->verify_pass,
            r->current_bank, r->controller1_buttons,
            gd_hex);
    }

    pos += snprintf(buf + pos, 8, "]}");
    send_line(buf);
    free(buf);
}

static void handle_frame_timeseries(int id, const char *json)
{
    int start = json_get_int(json, "start", -1);
    int end   = json_get_int(json, "end", -1);
    if (start < 0 || end < 0) { send_err(id, "missing start/end"); return; }
    if (end - start + 1 > 200) { send_err(id, "max 200 frames per request"); return; }

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;

    /* Build compact JSON array */
    char *buf = (char *)malloc(200 * 320 + 256);
    if (!buf) { send_err(id, "alloc failed"); return; }

    int pos = snprintf(buf, 64, "{\"id\":%d,\"ok\":true,\"ts\":[", id);

    int first = 1;
    for (int f = start; f <= end; f++) {
        if (!first) buf[pos++] = ',';
        first = 0;

        if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
            pos += snprintf(buf + pos, 32, "null");
            continue;
        }

        uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
        const NESFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number != (uint32_t)f) {
            pos += snprintf(buf + pos, 32, "null");
            continue;
        }

        /* Encode game_data as hex */
        char gd_hex[33];
        for (int i = 0; i < 16; i++)
            snprintf(gd_hex + i * 2, 3, "%02x", r->game_data[i]);

        pos += snprintf(buf + pos, 320,
            "{\"f\":%u,\"v\":%d,\"a\":%d,\"x\":%d,\"y\":%d,"
            "\"ctrl\":%d,\"mask\":%d,\"sx\":%d,\"sy\":%d,"
            "\"bk\":%d,\"btn\":%d,\"gd\":\"%s\"}",
            r->frame_number, r->verify_pass,
            r->cpu_a, r->cpu_x, r->cpu_y,
            r->ppuctrl, r->ppumask, r->ppuscroll_x, r->ppuscroll_y,
            r->current_bank, r->controller1_buttons,
            gd_hex);
    }

    pos += snprintf(buf + pos, 8, "]}");
    send_line(buf);
    free(buf);
}

static void handle_first_failure(int id, const char *json)
{
    (void)json;
    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;

    for (uint64_t f = oldest; f < s_history_count; f++) {
        uint32_t idx = (uint32_t)(f % FRAME_HISTORY_CAP);
        const NESFrameRecord *r = &s_frame_history[idx];
        if (r->frame_number == (uint32_t)f && r->verify_pass == 0) {
            send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%u,\"diff_count\":%d}",
                     id, r->frame_number, r->diff_count);
            return;
        }
    }
    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":-1,\"message\":\"no failures found\"}", id);
}

static void handle_ppu_state(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"ppuctrl\":\"0x%02X\",\"ppumask\":\"0x%02X\","
             "\"ppustatus\":\"0x%02X\","
             "\"scroll_x\":%d,\"scroll_y\":%d,"
             "\"spr0_split\":%d,\"spr0_reads\":%d,"
             "\"chr_is_rom\":%d}",
             id, g_ppuctrl, g_ppumask, g_ppustatus,
             g_ppuscroll_x, g_ppuscroll_y,
             g_spr0_split_active, g_spr0_reads_ctr,
             g_chr_is_rom);
}

#ifdef RECOMP_STACK_TRACKING
static void handle_call_stack(int id, const char *json)
{
    (void)json;
    /* Dynamic alloc: each entry ~25 chars avg, plus JSON overhead */
    int top = g_recomp_stack_top;
    int bufsz = 256 + top * 30;
    if (bufsz < 2048) bufsz = 2048;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { send_err(id, "OOM"); return; }

    int pos = snprintf(buf, bufsz,
        "{\"id\":%d,\"ok\":true,\"depth\":%d,\"stack\":[",
        id, top);

    for (int i = top - 1; i >= 0; i--) {
        if (i < top - 1 && pos < bufsz - 1) buf[pos++] = ',';
        pos += snprintf(buf + pos, bufsz - pos,
            "\"%s\"", g_recomp_stack[i] ? g_recomp_stack[i] : "(null)");
        if (pos >= bufsz - 2) break;
    }
    pos += snprintf(buf + pos, bufsz - pos, "]}");
    send_line(buf);
    free(buf);
}
#endif

static void handle_watchdog_status(int id, const char *json)
{
    (void)json;
    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"triggered\":%d,\"frame\":%u,\"stack_dump\":\"%s\"}",
             id, g_watchdog_triggered, (unsigned)g_watchdog_frame,
             g_watchdog_stack_dump ? g_watchdog_stack_dump : "");
}

static void handle_dispatch_miss_info(int id, const char *json)
{
    (void)json;
    extern uint32_t g_miss_count_any;
    extern uint16_t g_miss_last_addr;
    extern uint64_t g_miss_last_frame;
    extern int      g_miss_last_bank;
    extern char     g_miss_last_caller[64];
    extern char     g_miss_last_stack2[64];
    extern uint8_t  g_miss_last_sp;
    extern uint8_t  g_miss_last_stack_bytes[16];
    extern uint16_t g_miss_unique_addrs[];
    extern int      g_miss_unique_count;

    /* Build hex string for stack bytes */
    char stack_hex[48];
    for (int i = 0; i < 16; i++)
        snprintf(stack_hex + i*3, 4, "%02X ", g_miss_last_stack_bytes[i]);
    if (stack_hex[0]) stack_hex[47] = '\0';

    /* Build unique misses array */
    char unique_buf[256];
    int pos = 0;
    unique_buf[pos++] = '[';
    for (int i = 0; i < g_miss_unique_count; i++) {
        if (i) unique_buf[pos++] = ',';
        pos += snprintf(unique_buf + pos, sizeof(unique_buf) - pos,
                        "\"$%04X\"", g_miss_unique_addrs[i]);
    }
    unique_buf[pos++] = ']';
    unique_buf[pos] = '\0';

    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"total_misses\":%u,"
             "\"last_addr\":\"$%04X\","
             "\"last_bank\":%d,"
             "\"last_frame\":%llu,"
             "\"last_caller\":\"%s\","
             "\"last_stack2\":\"%s\","
             "\"last_sp\":\"$%02X\","
             "\"stack_bytes\":\"%s\","
             "\"unique_misses\":%s}",
             id,
             (unsigned)g_miss_count_any,
             g_miss_last_addr,
             g_miss_last_bank,
             (unsigned long long)g_miss_last_frame,
             g_miss_last_caller,
             g_miss_last_stack2,
             g_miss_last_sp,
             stack_hex,
             unique_buf);
}

static void handle_quit(int id, const char *json)
{
    (void)json;
    send_ok(id);
    debug_server_shutdown();
    exit(0);
}

/* ---- Time-travel: read from historical frame snapshots ---- */

static uint8_t frame_read_byte(const NESFrameRecord *r, uint32_t addr)
{
    /* CPU address space */
    if (addr < 0x0800) return r->ram_full[addr];
    if (addr < 0x2000) return r->ram_full[addr & 0x07FF];  /* RAM mirrors */
    if (addr >= 0x6000 && addr < 0x8000) return r->sram[addr - 0x6000];
    /* PPU address space (use addr >= 0x10000 for PPU, or standard ranges) */
    if (addr >= 0x2000 && addr < 0x3000) return r->ppu_nt[addr - 0x2000];
    if (addr >= 0x3F00 && addr < 0x3F20) return r->ppu_pal[addr - 0x3F00];
    /* OAM via special range */
    if (addr >= 0xFE00 && addr < 0xFF00) return r->oam[addr - 0xFE00];
    /* CHR RAM via PPU range 0x0000-0x1FFF (use 0x10000+ to disambiguate from CPU) */
    if (addr >= 0x10000 && addr < 0x12000) return r->chr_ram[addr - 0x10000];
    /* Also support raw CHR range if addr < 0x2000 and > 0x07FF — ambiguous, prefer PPU */
    return 0;
}

static void handle_read_frame_ram(int id, const char *json)
{
    int f = json_get_int(json, "frame", -1);
    if (f < 0) { send_err(id, "missing frame"); return; }

    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr"); return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int len = json_get_int(json, "len", 1);
    if (len < 1) len = 1;
    if (len > 256) len = 256;

    if (!s_frame_history) { send_err(id, "ring buffer not allocated"); return; }

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;
    if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
        send_err(id, "frame not in buffer"); return;
    }

    uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
    const NESFrameRecord *r = &s_frame_history[idx];

    /* Build hex string */
    char hex[513];
    for (int i = 0; i < len; i++)
        snprintf(hex + i*2, 3, "%02x", frame_read_byte(r, addr + i));

    send_fmt("{\"id\":%d,\"ok\":true,\"frame\":%d,\"addr\":\"0x%04X\",\"len\":%d,\"hex\":\"%s\"}",
             id, f, addr, len, hex);
}

static void handle_restore_frame(int id, const char *json)
{
    int f = json_get_int(json, "frame", -1);
    if (f < 0) { send_err(id, "missing frame"); return; }
    if (!s_frame_history) { send_err(id, "ring buffer not allocated"); return; }

    uint64_t oldest = (s_history_count > FRAME_HISTORY_CAP)
                    ? s_history_count - FRAME_HISTORY_CAP : 0;
    if ((uint64_t)f < oldest || (uint64_t)f >= s_history_count) {
        send_err(id, "frame not in buffer"); return;
    }

    uint32_t idx = (uint32_t)f % FRAME_HISTORY_CAP;
    const NESFrameRecord *r = &s_frame_history[idx];

    /* ---- Restore CPU (exhaustive) ---- */
    g_cpu.A = r->cpu_a;
    g_cpu.X = r->cpu_x;
    g_cpu.Y = r->cpu_y;
    g_cpu.S = r->cpu_s;
    g_cpu.P = r->cpu_p;
    g_cpu.N = r->cpu_n; g_cpu.V = r->cpu_v; g_cpu.D = r->cpu_d;
    g_cpu.I = r->cpu_i; g_cpu.Z = r->cpu_z; g_cpu.C = r->cpu_c;

    /* ---- Restore PPU registers (exhaustive) ---- */
    g_ppuctrl     = r->ppuctrl;
    g_ppumask     = r->ppumask;
    g_ppustatus   = r->ppustatus;
    g_oamaddr     = r->oamaddr;
    g_ppuscroll_x = r->ppuscroll_x;
    g_ppuscroll_y = r->ppuscroll_y;
    runtime_set_ppuaddr(r->ppuaddr);
    runtime_set_latch_state(r->ppuaddr_latch, r->scroll_latch);
    runtime_set_ppudata_buf(r->ppudata_buf);

    /* ---- Restore sprite-0 split state ---- */
    g_ppuscroll_x_hud  = r->ppuscroll_x_hud;
    g_ppuscroll_y_hud  = r->ppuscroll_y_hud;
    g_ppuctrl_hud      = r->ppuctrl_hud;
    g_spr0_split_active = r->spr0_split_active;
    g_spr0_reads_ctr    = r->spr0_reads_ctr;

    /* ---- Restore VBlank / timing state ---- */
    runtime_set_vblank_state(r->ops_count, r->vblank_depth);

    /* ---- Restore mapper (exhaustive) ---- */
    g_current_bank = r->current_bank;
    mapper_set_state(&r->mapper);

    /* ---- Restore controller state ---- */
    g_controller1_buttons = r->controller1_buttons;
    g_controller2_buttons = r->controller2_buttons;
    runtime_set_controller_shift(r->ctrl1_shift, r->ctrl2_shift, r->ctrl1_strobe);

    /* ---- Restore full memory state ---- */
    memcpy(g_ram,     r->ram_full, sizeof(r->ram_full));
    memcpy(g_sram,    r->sram,     sizeof(r->sram));
    memcpy(g_chr_ram, r->chr_ram,  sizeof(r->chr_ram));
    memcpy(g_ppu_nt,  r->ppu_nt,   sizeof(r->ppu_nt));
    memcpy(g_ppu_pal, r->ppu_pal,  sizeof(r->ppu_pal));
    memcpy(g_ppu_oam, r->oam,      sizeof(r->oam));

    /* Reset frame counter to the restored frame */
    g_frame_count = (uint64_t)f;

    send_fmt("{\"id\":%d,\"ok\":true,\"restored_frame\":%d}", id, f);
}

/* ---- Follow commands ---- */

static void handle_follow(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int brk = json_get_int(json, "break_on", -1);

    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (!s_followers[i].active) {
            s_followers[i].addr = addr;
            s_followers[i].active = 1;
            s_followers[i].break_on_val = brk;
            s_followers[i].log_head = 0;
            s_followers[i].log_count = 0;
            send_fmt("{\"id\":%d,\"ok\":true,\"slot\":%d,\"addr\":\"0x%04X\","
                     "\"break_on\":%d}", id, i, addr, brk);
            return;
        }
    }
    send_err(id, "all follower slots full (max 8)");
}

static void handle_unfollow(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (s_followers[i].active && s_followers[i].addr == addr) {
            s_followers[i].active = 0;
            send_ok(id);
            return;
        }
    }
    send_err(id, "no follower at that address");
}

static void handle_follow_history(int id, const char *json)
{
    char addr_str[32];
    if (!json_get_str(json, "addr", addr_str, sizeof(addr_str))) {
        send_err(id, "missing addr");
        return;
    }
    uint32_t addr = hex_to_u32(addr_str);
    int limit = json_get_int(json, "limit", 50);
    if (limit > FOLLOW_LOG_CAP) limit = FOLLOW_LOG_CAP;

    Follower *f = NULL;
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (s_followers[i].active && s_followers[i].addr == addr) {
            f = &s_followers[i];
            break;
        }
    }
    if (!f) { send_err(id, "no follower at that address"); return; }

    /* Build JSON array of recent entries */
    char buf[16384];
    int pos = snprintf(buf, sizeof(buf),
        "{\"id\":%d,\"ok\":true,\"addr\":\"0x%04X\",\"total\":%u,\"entries\":[",
        id, addr, f->log_count);

    uint32_t avail = f->log_count < FOLLOW_LOG_CAP ? f->log_count : FOLLOW_LOG_CAP;
    uint32_t start = (avail > (uint32_t)limit) ? avail - (uint32_t)limit : 0;

    int first = 1;
    for (uint32_t j = start; j < avail && pos < (int)sizeof(buf) - 256; j++) {
        uint32_t idx = (f->log_head - avail + j) % FOLLOW_LOG_CAP;
        FollowEntry *e = &f->log[idx];
        if (!first) buf[pos++] = ',';
        first = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"f\":%llu,\"old\":\"0x%02X\",\"new\":\"0x%02X\",\"stack\":\"%s\"}",
            (unsigned long long)e->frame, e->old_val, e->new_val, e->call_stack);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    debug_server_send_line(buf);
}

/* ---- S-register tracking commands ---- */

static void handle_watch_s(int id, const char *json)
{
    int brk = json_get_int(json, "break_on_change", 0);
    int frame_lo = json_get_int(json, "frame_lo", 0);
    int frame_hi = json_get_int(json, "frame_hi", 999999999);
    s_track_s_enabled = 1;
    s_track_s_prev = g_cpu.S;
    s_track_s_head = 0;
    s_track_s_count = 0;
    s_track_s_break_on_change = brk;
    s_track_s_frame_lo = (uint64_t)frame_lo;
    s_track_s_frame_hi = (uint64_t)frame_hi;
    send_fmt("{\"id\":%d,\"ok\":true,\"watching_s\":true,\"initial_s\":\"0x%02X\","
             "\"break_on_change\":%d,\"frame_lo\":%d,\"frame_hi\":%d}",
             id, g_cpu.S, brk, frame_lo, frame_hi);
}

static void handle_unwatch_s(int id, const char *json)
{
    (void)json;
    s_track_s_enabled = 0;
    send_ok(id);
}

static void handle_watch_s_history(int id, const char *json)
{
    int limit = json_get_int(json, "limit", 64);
    int offset = json_get_int(json, "offset", -1); /* -1 = last N entries */
    if (limit > (int)S_TRACK_LOG_CAP) limit = S_TRACK_LOG_CAP;

    char buf[32768];
    int pos = snprintf(buf, sizeof(buf),
        "{\"id\":%d,\"ok\":true,\"total\":%u,\"entries\":[",
        id, s_track_s_count);

    uint32_t avail = s_track_s_count < S_TRACK_LOG_CAP ? s_track_s_count : S_TRACK_LOG_CAP;
    uint32_t start;
    if (offset >= 0) {
        start = (uint32_t)offset;
        if (start >= avail) start = avail;
    } else {
        start = (avail > (uint32_t)limit) ? avail - (uint32_t)limit : 0;
    }

    int first = 1;
    for (uint32_t j = start; j < avail && pos < (int)sizeof(buf) - 256; j++) {
        uint32_t idx = (s_track_s_head - avail + j) % S_TRACK_LOG_CAP;
        STrackEntry *e = &s_track_s_log[idx];
        if (!first) buf[pos++] = ',';
        first = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"f\":%llu,\"old\":\"0x%02X\",\"new\":\"0x%02X\",\"stack\":\"%s\"}",
            (unsigned long long)e->frame, e->old_val, e->new_val, e->call_stack);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    debug_server_send_line(buf);
}

/* ---- Screenshot ---- */

static void handle_screenshot(int id, const char *json)
{
    char path[256];
    if (!json_get_str(json, "path", path, sizeof(path))) {
        snprintf(path, sizeof(path), "screenshot_%04llu.png",
                 (unsigned long long)g_frame_count);
    }
    runner_screenshot(path);
    send_fmt("{\"id\":%d,\"ok\":true,\"path\":\"%s\"}", id, path);
}

/* ---- Command dispatch ---- */

typedef void (*CmdHandler)(int id, const char *json);
typedef struct {
    const char *name;
    CmdHandler  handler;
} CmdEntry;

static const CmdEntry s_commands[] = {
    { "ping",              handle_ping },
    { "frame",             handle_frame },
    { "get_registers",     handle_get_registers },
    { "read_ram",          handle_read_ram },
    { "dump_ram",          handle_dump_ram },
    { "write_ram",         handle_write_ram },
    { "read_ppu",          handle_read_ppu },
    { "mapper_state",      handle_mapper_state },
    { "watch",             handle_watch },
    { "unwatch",           handle_unwatch },
    { "follow",            handle_follow },
    { "unfollow",          handle_unfollow },
    { "follow_history",    handle_follow_history },
    { "watch_s",           handle_watch_s },
    { "unwatch_s",         handle_unwatch_s },
    { "watch_s_history",   handle_watch_s_history },
    { "read_frame_ram",    handle_read_frame_ram },
    { "restore_frame",     handle_restore_frame },
    { "set_input",         handle_set_input },
    { "clear_input",       handle_clear_input },
    { "pause",             handle_pause },
    { "continue",          handle_continue },
    { "step",              handle_step },
    { "run_to_frame",      handle_run_to_frame },
    { "history",           handle_history },
    { "get_frame",         handle_get_frame },
    { "frame_range",       handle_frame_range },
    { "frame_timeseries",  handle_frame_timeseries },
    { "first_failure",     handle_first_failure },
    { "ppu_state",         handle_ppu_state },
    { "watchdog_status",   handle_watchdog_status },
#ifdef RECOMP_STACK_TRACKING
    { "call_stack",        handle_call_stack },
#endif
    { "dispatch_miss_info", handle_dispatch_miss_info },
    { "quit",              handle_quit },
    { "screenshot",        handle_screenshot },
    { NULL, NULL }
};

static void process_command(const char *line)
{
    /* Extract command name and id from JSON */
    char cmd[64];
    if (!json_get_str(line, "cmd", cmd, sizeof(cmd))) {
        /* Maybe it's a bare command name (not JSON) */
        strncpy(cmd, line, sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';
        /* Strip trailing whitespace */
        int len = (int)strlen(cmd);
        while (len > 0 && (cmd[len-1] == '\r' || cmd[len-1] == ' '))
            cmd[--len] = '\0';
    }

    int id = json_get_int(line, "id", 0);

    /* Try game-specific command handler first (allows overriding built-ins) */
    if (game_handle_debug_cmd(cmd, id, line))
        return;

    for (const CmdEntry *e = s_commands; e->name; e++) {
        if (strcmp(cmd, e->name) == 0) {
            e->handler(id, line);
            return;
        }
    }

    send_err(id, "unknown command");
}

/* ---- Public API ---- */

void debug_server_init(int port)
{
    if (port > 0) s_port = port;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen == SOCK_INVALID) {
        fprintf(stderr, "[debug] Failed to create socket\n");
        return;
    }

    int yes = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)s_port);

    if (bind(s_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[debug] Failed to bind port %d\n", s_port);
        sock_close(s_listen);
        s_listen = SOCK_INVALID;
        return;
    }

    listen(s_listen, 1);
    set_nonblocking(s_listen);

    /* Initialize ring buffer (heap-allocated for full-state snapshots) */
    if (!s_frame_history) {
        size_t sz = sizeof(NESFrameRecord) * FRAME_HISTORY_CAP;
        s_frame_history = (NESFrameRecord *)calloc(FRAME_HISTORY_CAP, sizeof(NESFrameRecord));
        if (!s_frame_history) {
            fprintf(stderr, "[debug] Failed to allocate ring buffer (%zu MB)\n", sz / (1024*1024));
            return;
        }
        fprintf(stderr, "[debug] Ring buffer allocated: %zu MB (%d frames × %zu bytes)\n",
                sz / (1024*1024), FRAME_HISTORY_CAP, sizeof(NESFrameRecord));
    }
    s_history_count = 0;

    /* Initialize watchpoints */
    memset(s_watchpoints, 0, sizeof(s_watchpoints));

    fprintf(stderr, "[debug] TCP server listening on 127.0.0.1:%d\n", s_port);
}

void debug_server_poll(void)
{
    if (s_listen == SOCK_INVALID) return;

    /* Accept new client if none connected */
    if (s_client == SOCK_INVALID) {
        struct sockaddr_in caddr;
        int clen = sizeof(caddr);
        sock_t c = accept(s_listen, (struct sockaddr *)&caddr, &clen);
        if (c != SOCK_INVALID) {
            s_client = c;
            set_nonblocking(s_client);
            s_recv_len = 0;
            fprintf(stderr, "[debug] Client connected\n");
        }
        return;  /* no client yet, nothing to poll */
    }

    /* Receive data */
    int space = RECV_BUF_SIZE - s_recv_len - 1;
    if (space > 0) {
        int n = recv(s_client, s_recv_buf + s_recv_len, space, 0);
        if (n > 0) {
            s_recv_len += n;
            s_recv_buf[s_recv_len] = '\0';
        } else if (n == 0) {
            /* Client disconnected */
            fprintf(stderr, "[debug] Client disconnected\n");
            sock_close(s_client);
            s_client = SOCK_INVALID;
            s_paused = 0;
            s_input_override = -1;
            return;
        } else {
            int err = sock_error();
#ifdef _WIN32
            if (err != WSAEWOULDBLOCK) {
#else
            if (err != EAGAIN && err != EWOULDBLOCK) {
#endif
                fprintf(stderr, "[debug] recv error %d, dropping client\n", err);
                sock_close(s_client);
                s_client = SOCK_INVALID;
                s_paused = 0;
                s_input_override = -1;
                return;
            }
        }
    }

    /* Process complete lines */
    char *nl;
    while ((nl = strchr(s_recv_buf, '\n')) != NULL) {
        *nl = '\0';
        /* Also strip \r */
        if (nl > s_recv_buf && *(nl - 1) == '\r')
            *(nl - 1) = '\0';
        if (s_recv_buf[0] != '\0')
            process_command(s_recv_buf);
        int consumed = (int)(nl - s_recv_buf) + 1;
        s_recv_len -= consumed;
        memmove(s_recv_buf, nl + 1, s_recv_len + 1);
    }
}

void debug_server_record_frame(void)
{
    if (!s_frame_history) return;

    uint32_t idx = (uint32_t)(g_frame_count % FRAME_HISTORY_CAP);
    NESFrameRecord *r = &s_frame_history[idx];

    r->frame_number = (uint32_t)g_frame_count;

    /* ---- Verify-mode result (set by a game's verify_mode via setter) ---- */
    if (s_pending_verify_set) {
        r->verify_pass = s_pending_verify_pass;
        r->diff_count  = s_pending_verify_diff_count;
        int n = s_pending_verify_n_diffs;
        if (n > MAX_FRAME_DIFFS) n = MAX_FRAME_DIFFS;
        memset(r->diffs, 0, sizeof(r->diffs));
        for (int i = 0; i < n; i++) r->diffs[i] = s_pending_verify_diffs[i];
        s_pending_verify_set        = 0;
        s_pending_verify_pass       = -1;
        s_pending_verify_diff_count = 0;
        s_pending_verify_n_diffs    = 0;
    } else {
        r->verify_pass = -1;
        r->diff_count  = 0;
        memset(r->diffs, 0, sizeof(r->diffs));
    }

    /* ---- CPU state (exhaustive) ---- */
    r->cpu_a = g_cpu.A;
    r->cpu_x = g_cpu.X;
    r->cpu_y = g_cpu.Y;
    r->cpu_s = g_cpu.S;
    r->cpu_p = (uint8_t)((g_cpu.N<<7)|(g_cpu.V<<6)|(1<<5)|
                          (g_cpu.D<<3)|(g_cpu.I<<2)|(g_cpu.Z<<1)|g_cpu.C);
    r->cpu_n = g_cpu.N; r->cpu_v = g_cpu.V; r->cpu_d = g_cpu.D;
    r->cpu_i = g_cpu.I; r->cpu_z = g_cpu.Z; r->cpu_c = g_cpu.C;

    /* ---- PPU registers (exhaustive) ---- */
    r->ppuctrl     = g_ppuctrl;
    r->ppumask     = g_ppumask;
    r->ppustatus   = g_ppustatus;
    r->oamaddr     = g_oamaddr;
    r->ppuscroll_x = g_ppuscroll_x;
    r->ppuscroll_y = g_ppuscroll_y;
    r->ppuaddr     = runtime_get_ppuaddr();
    {
        uint8_t al, sl;
        runtime_get_latch_state(&al, &sl);
        r->ppuaddr_latch = al;
        r->scroll_latch  = sl;
    }
    r->ppudata_buf = runtime_get_ppudata_buf();

    /* ---- Sprite-0 split state ---- */
    r->ppuscroll_x_hud = g_ppuscroll_x_hud;
    r->ppuscroll_y_hud = g_ppuscroll_y_hud;
    r->ppuctrl_hud     = g_ppuctrl_hud;
    r->spr0_split_active = g_spr0_split_active;
    r->spr0_reads_ctr    = g_spr0_reads_ctr;

    /* ---- VBlank / timing state ---- */
    {
        uint32_t ops; int depth;
        runtime_get_vblank_state(&ops, &depth);
        r->ops_count    = ops;
        r->vblank_depth = depth;
    }

    /* ---- Mapper (exhaustive) ---- */
    r->current_bank = g_current_bank;
    mapper_get_state(&r->mapper);

    /* ---- Controller state (exhaustive) ---- */
    r->controller1_buttons = g_controller1_buttons;
    r->controller2_buttons = g_controller2_buttons;
    runtime_get_controller_shift(&r->ctrl1_shift, &r->ctrl2_shift, &r->ctrl1_strobe);

    /* ---- Full memory snapshots ---- */
    memcpy(r->ram_full, g_ram,     0x0800);
    memcpy(r->sram,     g_sram,    0x2000);
    memcpy(r->chr_ram,  g_chr_ram, 0x2000);
    memcpy(r->ppu_nt,   g_ppu_nt,  0x1000);
    memcpy(r->ppu_pal,  g_ppu_pal, 0x20);
    memcpy(r->oam,      g_ppu_oam, 0x100);

    /* Game-specific data (filled by game hook) */
    memset(r->game_data, 0, sizeof(r->game_data));
    game_fill_frame_record(r);

    /* Last function */
#ifdef RECOMP_STACK_TRACKING
    strncpy(r->last_func, g_last_recomp_func ? g_last_recomp_func : "(none)",
            sizeof(r->last_func) - 1);
    r->last_func[sizeof(r->last_func) - 1] = '\0';
#else
    strcpy(r->last_func, "(no tracking)");
#endif

    s_history_count = g_frame_count + 1;

    /* Step mode: count down and re-pause */
    if (s_step_count > 0) {
        s_step_count--;
        if (s_step_count == 0) {
            s_paused = 1;
            send_fmt("{\"event\":\"step_done\",\"frame\":%llu}",
                     (unsigned long long)g_frame_count);
        }
    }

    /* Run-to-frame: pause when target reached */
    if (s_run_to > 0 && g_frame_count >= s_run_to) {
        s_paused = 1;
        s_run_to = 0;
        send_fmt("{\"event\":\"run_to_done\",\"frame\":%llu}",
                 (unsigned long long)g_frame_count);
    }
}

void debug_server_wait_if_paused(void)
{
    while (s_paused) {
        /* Keep polling TCP commands (so "continue" can arrive) */
        debug_server_poll();

        /* Pump SDL events (so window doesn't freeze) */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) exit(0);
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) exit(0);
        }

        SDL_Delay(5);  /* 5ms sleep to avoid busy loop */
    }
}

void debug_server_check_watchpoints(void)
{
    if (s_client == SOCK_INVALID) return;

    for (int i = 0; i < MAX_WATCHPOINTS; i++) {
        if (!s_watchpoints[i].active) continue;
        uint8_t cur = read_byte(s_watchpoints[i].addr);
        if (cur != s_watchpoints[i].prev_val) {
            send_fmt("{\"event\":\"watchpoint\","
                     "\"addr\":\"0x%04X\",\"old\":\"0x%02X\",\"new\":\"0x%02X\","
                     "\"frame\":%llu}",
                     s_watchpoints[i].addr,
                     s_watchpoints[i].prev_val, cur,
                     (unsigned long long)g_frame_count);
            s_watchpoints[i].prev_val = cur;
        }
    }
}

/* ---- Follower: write-level RAM tracing ---- */

static void capture_stack(char *out, int out_sz)
{
#ifdef RECOMP_STACK_TRACKING
    if (out_sz < 2) return;
    out[0] = '\0';
    int top = g_recomp_stack_top;
    if (top <= 0 || top > 64) {
        snprintf(out, out_sz, "(empty)");
        return;
    }
    int pos = 0;
    for (int i = top - 1; i >= 0 && pos < out_sz - 2; i--) {
        if (i < top - 1)
            out[pos++] = '<';
        const char *fn = g_recomp_stack[i];
        if (!fn) { fn = "?"; }
        while (*fn && pos < out_sz - 2)
            out[pos++] = *fn++;
    }
    out[pos] = '\0';
#else
    snprintf(out, out_sz, "(no tracking)");
#endif
}

void debug_server_notify_write(uint16_t addr, uint8_t old_val, uint8_t new_val)
{
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (!s_followers[i].active) continue;
        if (s_followers[i].addr != (uint32_t)addr) continue;

        /* Record to ring buffer */
        uint32_t idx = s_followers[i].log_head % FOLLOW_LOG_CAP;
        FollowEntry *e = &s_followers[i].log[idx];
        e->frame   = g_frame_count;
        e->old_val = old_val;
        e->new_val = new_val;
        capture_stack(e->call_stack, sizeof(e->call_stack));
        s_followers[i].log_head++;
        s_followers[i].log_count++;
        if (addr == 0xFF && s_followers[i].log_count <= 3) {
            fprintf(stderr, "[FOLLOW] slot=%d addr=%04X count=%u head=%u stack=%s\n",
                    i, addr, s_followers[i].log_count, s_followers[i].log_head,
                    e->call_stack);
        }

        /* Conditional break */
        if (s_followers[i].break_on_val >= 0 &&
            new_val == (uint8_t)s_followers[i].break_on_val) {
            s_paused = 1;
            send_fmt("{\"event\":\"follow_break\","
                     "\"addr\":\"0x%04X\",\"old\":\"0x%02X\",\"new\":\"0x%02X\","
                     "\"frame\":%llu,\"stack\":\"%s\"}",
                     addr, old_val, new_val,
                     (unsigned long long)g_frame_count,
                     e->call_stack);
        }
    }
}

void debug_server_check_s(void)
{
    if (!s_track_s_enabled) return;
    if (g_cpu.S == s_track_s_prev) return;

    uint8_t prev = s_track_s_prev;
    s_track_s_prev = g_cpu.S;

    /* Only record in the specified frame range */
    if (g_frame_count < s_track_s_frame_lo || g_frame_count > s_track_s_frame_hi)
        return;

    int diff = (int)g_cpu.S - (int)prev;
    if (diff < 0) diff = -diff;

    /* S change in range — record it */
    uint32_t idx = s_track_s_head % S_TRACK_LOG_CAP;
    STrackEntry *e = &s_track_s_log[idx];
    e->frame   = g_frame_count;
    e->old_val = prev;
    e->new_val = g_cpu.S;
    capture_stack(e->call_stack, sizeof(e->call_stack));
    s_track_s_head++;
    s_track_s_count++;

    /* Break on large changes (> 10 in either direction) */
    if (s_track_s_break_on_change && diff > 10) {
        s_paused = 1;
        send_fmt("{\"event\":\"s_change_break\","
                 "\"old\":\"0x%02X\",\"new\":\"0x%02X\","
                 "\"frame\":%llu,\"stack\":\"%s\"}",
                 prev, g_cpu.S,
                 (unsigned long long)g_frame_count,
                 e->call_stack);
    }
}

int debug_server_has_follower(uint16_t addr)
{
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (s_followers[i].active && s_followers[i].addr == (uint32_t)addr)
            return 1;
    }
    return 0;
}

int debug_server_add_follower(uint16_t addr, int break_on_val)
{
    for (int i = 0; i < MAX_FOLLOWERS; i++) {
        if (!s_followers[i].active) {
            s_followers[i].addr = (uint32_t)addr;
            s_followers[i].active = 1;
            s_followers[i].break_on_val = break_on_val;
            s_followers[i].log_head = 0;
            s_followers[i].log_count = 0;
            return i;
        }
    }
    return -1;
}

void debug_server_shutdown(void)
{
    if (s_client != SOCK_INVALID) {
        sock_close(s_client);
        s_client = SOCK_INVALID;
    }
    if (s_listen != SOCK_INVALID) {
        sock_close(s_listen);
        s_listen = SOCK_INVALID;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    fprintf(stderr, "[debug] Server shut down\n");
}

int debug_server_is_connected(void)
{
    return s_client != SOCK_INVALID;
}

int debug_server_get_input_override(void)
{
    return s_input_override;
}

void debug_server_set_verify_result(int passed, int diff_count,
                                    const FrameDiffEntry *diffs, int n_diffs)
{
    s_pending_verify_set        = 1;
    s_pending_verify_pass       = passed ? 1 : 0;
    s_pending_verify_diff_count = diff_count;
    if (n_diffs < 0) n_diffs = 0;
    if (n_diffs > MAX_FRAME_DIFFS) n_diffs = MAX_FRAME_DIFFS;
    s_pending_verify_n_diffs = n_diffs;
    if (diffs && n_diffs > 0)
        memcpy(s_pending_verify_diffs, diffs, n_diffs * sizeof(FrameDiffEntry));
}
