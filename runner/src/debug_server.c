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

/* ---- Ring buffer ---- */
static NESFrameRecord s_frame_history[FRAME_HISTORY_CAP];
static uint64_t       s_history_count = 0;

/* ---- Watchpoints ---- */
#define MAX_WATCHPOINTS 8
typedef struct {
    uint32_t addr;
    uint8_t  prev_val;
    int      active;
} Watchpoint;
static Watchpoint s_watchpoints[MAX_WATCHPOINTS];

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

void debug_server_send_line(const char *json)
{
    if (s_client == SOCK_INVALID) return;
    int len = (int)strlen(json);
    send(s_client, json, len, 0);
    send(s_client, "\n", 1, 0);
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
    send_fmt("{\"id\":%d,\"ok\":true,\"bank\":%d}",
             id, g_current_bank);
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

    send_fmt("{\"id\":%d,\"ok\":true,"
             "\"frame\":%u,\"verify_pass\":%d,\"diff_count\":%d,"
             "\"cpu\":{\"A\":\"0x%02X\",\"X\":\"0x%02X\",\"Y\":\"0x%02X\",\"S\":\"0x%02X\"},"
             "\"ppu\":{\"ctrl\":\"0x%02X\",\"mask\":\"0x%02X\",\"scroll_x\":%d,\"scroll_y\":%d},"
             "\"bank\":%d,\"buttons\":\"0x%02X\","
             "\"game_data\":\"%s\","
             "\"last_func\":\"%s\"}",
             id, r->frame_number, r->verify_pass, r->diff_count,
             r->cpu_a, r->cpu_x, r->cpu_y, r->cpu_s,
             r->ppuctrl, r->ppumask, r->ppuscroll_x, r->ppuscroll_y,
             r->current_bank, r->controller_buttons,
             gd_hex,
             r->last_func);
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
            r->current_bank, r->controller_buttons,
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
            r->current_bank, r->controller_buttons,
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
    char buf[2048];
    int pos = snprintf(buf, sizeof(buf),
        "{\"id\":%d,\"ok\":true,\"depth\":%d,\"stack\":[",
        id, g_recomp_stack_top);

    for (int i = g_recomp_stack_top - 1; i >= 0; i--) {
        if (i < g_recomp_stack_top - 1) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "\"%s\"", g_recomp_stack[i] ? g_recomp_stack[i] : "(null)");
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    send_line(buf);
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

static void handle_quit(int id, const char *json)
{
    (void)json;
    send_ok(id);
    debug_server_shutdown();
    exit(0);
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
    { "quit",              handle_quit },
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

    for (const CmdEntry *e = s_commands; e->name; e++) {
        if (strcmp(cmd, e->name) == 0) {
            e->handler(id, line);
            return;
        }
    }

    /* Try game-specific command handler */
    if (game_handle_debug_cmd(cmd, id, line))
        return;

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

    /* Initialize ring buffer */
    memset(s_frame_history, 0, sizeof(s_frame_history));
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
    uint32_t idx = (uint32_t)(g_frame_count % FRAME_HISTORY_CAP);
    NESFrameRecord *r = &s_frame_history[idx];

    r->frame_number = (uint32_t)g_frame_count;
    r->verify_pass  = -1;  /* not checked unless verify mode is active */
    r->diff_count   = 0;

    /* CPU state */
    r->cpu_a = g_cpu.A;
    r->cpu_x = g_cpu.X;
    r->cpu_y = g_cpu.Y;
    r->cpu_s = g_cpu.S;
    r->cpu_p = (uint8_t)((g_cpu.N<<7)|(g_cpu.V<<6)|(1<<5)|
                          (g_cpu.D<<3)|(g_cpu.I<<2)|(g_cpu.Z<<1)|g_cpu.C);

    /* PPU state */
    r->ppuctrl    = g_ppuctrl;
    r->ppumask    = g_ppumask;
    r->ppuscroll_x = g_ppuscroll_x;
    r->ppuscroll_y = g_ppuscroll_y;

    /* Mapper + input */
    r->current_bank       = g_current_bank;
    r->controller_buttons = g_controller1_buttons;

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
