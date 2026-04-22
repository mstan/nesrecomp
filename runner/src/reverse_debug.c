/*
 * reverse_debug.c — Tier 1 / 1.5 / 2 / 2.5 / 3 reverse-debugger hooks.
 *
 * See nesrecomp/REVERSE_DEBUGGER.md.
 *
 * Only compiles when NESRECOMP_REVERSE_DEBUG=1.
 */
#include "reverse_debug.h"

#if NESRECOMP_REVERSE_DEBUG

#include "debug_server.h"
#include "nes_runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Tunables ---- */
#define RDB_STORE_RING_SIZE    (1u << 20)   /* 1 M — Tier 1 */
#define RDB_CALL_RING_SIZE     (1u << 16)   /* 64 k — Tier 1.5 */
#define RDB_BLOCK_RING_SIZE    (1u << 18)   /* 256 k — Tier 2 */
#define RDB_MAX_RANGES         8
#define RDB_MAX_BREAKS         16
#define RDB_MAX_WATCHES        16
#define RDB_ANCHOR_COUNT       64            /* Tier 3 */

/* ============================================================= */
/* Globals                                                        */
/* ============================================================= */

uint16_t          g_rdb_current_func = 0;
volatile int      g_rdb_paused       = 0;

static uint64_t   s_block_counter    = 0;   /* global monotonic block index */

/* ---- Tier 1 ring ---- */
typedef struct {
    uint64_t block_idx;   /* s_block_counter at the moment of the store */
    uint32_t frame;
    uint16_t addr;
    uint16_t pc_hint;
    uint16_t func;
    uint8_t  val;
    uint8_t  _pad;
} RdbStoreEntry;
static RdbStoreEntry s_store_ring[RDB_STORE_RING_SIZE];
static uint64_t s_store_widx = 0, s_store_count = 0;

typedef struct { uint16_t lo, hi; } RdbRange;
static RdbRange s_store_ranges[RDB_MAX_RANGES];
static int s_store_nranges = 0;

/* ---- Tier 1.5 ring ---- */
typedef struct {
    uint32_t frame;
    uint16_t func;
    uint16_t caller;
} RdbCallEntry;
static RdbCallEntry s_call_ring[RDB_CALL_RING_SIZE];
static uint64_t s_call_widx = 0, s_call_count = 0;
static volatile int s_call_trace_active = 0;

/* ---- Tier 2 ring ---- */
typedef struct {
    uint32_t frame;
    uint16_t pc;
    uint16_t func;
    uint8_t  a, x, y, p;
} RdbBlockEntry;
static RdbBlockEntry s_block_ring[RDB_BLOCK_RING_SIZE];
static uint64_t s_block_widx = 0, s_block_count = 0;
static volatile int s_block_trace_active = 0;

static RdbRange s_block_pc_ranges[RDB_MAX_RANGES];
static int s_block_pc_nranges = 0;

/* ---- Tier 2.5 ---- */
static uint16_t s_breaks[RDB_MAX_BREAKS];
static int      s_break_count = 0;
static volatile int s_break_step_block = 0;

typedef struct { uint16_t addr; int16_t match; } RdbWatch;
static RdbWatch s_watches[RDB_MAX_WATCHES];
static int      s_watch_count = 0;

static volatile uint16_t s_parked_pc         = 0;
static volatile uint16_t s_parked_func       = 0;
static volatile uint16_t s_parked_watch_addr = 0;
static volatile uint8_t  s_parked_watch_val  = 0;
static volatile uint8_t  s_parked_reason     = 0;  /* 1=break, 2=watch, 3=step */

/* ---- Tier 3 native anchors ---- */
typedef struct {
    uint64_t block_idx;
    uint32_t frame;
    uint8_t  wram[0x0800];
} RdbAnchor;
static RdbAnchor s_anchors[RDB_ANCHOR_COUNT];
static int       s_anchor_widx     = 0;
static int       s_anchor_count    = 0;
static volatile int s_anchor_active = 0;
static uint32_t  s_anchor_interval = 4096;

/* ============================================================= */
/* Helpers                                                        */
/* ============================================================= */

static inline int in_any_range(const RdbRange *r, int n, uint16_t addr) {
    for (int i = 0; i < n; i++)
        if (addr >= r[i].lo && addr <= r[i].hi) return 1;
    return 0;
}

static int json_field(const char *json, const char *key, char *out, int cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    int i = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ' ' && i < cap - 1)
            out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

static int json_u32(const char *json, const char *key, uint32_t *out) {
    char buf[32];
    if (!json_field(json, key, buf, sizeof(buf))) return 0;
    const char *s = buf;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        *out = (uint32_t)strtoul(s + 2, NULL, 16);
    else if (strchr(buf, 'x') || strchr(buf, 'X'))
        *out = (uint32_t)strtoul(buf, NULL, 16);
    else
        *out = (uint32_t)strtoul(buf, NULL, 0);
    return 1;
}

static int json_i32(const char *json, const char *key, int32_t *out) {
    uint32_t u;
    if (!json_u32(json, key, &u)) return 0;
    *out = (int32_t)u;
    return 1;
}

/* ============================================================= */
/* Tier 1 — bus-write hook                                        */
/* ============================================================= */

static void park_on_watch_hit(uint16_t addr, uint8_t val) {
    s_parked_reason     = 2;
    s_parked_watch_addr = addr;
    s_parked_watch_val  = val;
    s_parked_func       = g_rdb_current_func;
    s_parked_pc         = 0;
    g_rdb_paused        = 1;
    rdb_wait_if_parked();
}

void rdb_store8(uint16_t pc_hint, uint16_t addr, uint8_t val) {
    nes_write(addr, val);

    /* Two reasons to record: (a) addr is in a user-armed range for
     * targeted inspection, or (b) anchors are armed AND addr is in
     * WRAM ($0000-$07FF), so rdb_wram_at_block can replay stores
     * forward from the nearest anchor. Each store records once. */
    int record = s_store_nranges && in_any_range(s_store_ranges, s_store_nranges, addr);
    if (!record && s_anchor_active && addr <= 0x07FF) record = 1;
    if (record) {
        uint64_t idx = s_store_widx++ & (RDB_STORE_RING_SIZE - 1);
        RdbStoreEntry *e = &s_store_ring[idx];
        e->block_idx = s_block_counter;
        e->frame     = (uint32_t)g_frame_count;
        e->addr      = addr;
        e->pc_hint   = pc_hint;
        e->func      = g_rdb_current_func;
        e->val       = val;
        e->_pad      = 0;
        if (s_store_count < RDB_STORE_RING_SIZE) s_store_count++;
    }

    if (s_watch_count) {
        for (int i = 0; i < s_watch_count; i++) {
            if (s_watches[i].addr != addr) continue;
            if (s_watches[i].match >= 0 && (uint8_t)s_watches[i].match != val) continue;
            park_on_watch_hit(addr, val);
            break;
        }
    }
}

/* ============================================================= */
/* Tier 1.5 — call ring                                           */
/* ============================================================= */

void rdb_on_call(uint16_t func_pc) {
    if (!s_call_trace_active) return;
    uint64_t idx = s_call_widx++ & (RDB_CALL_RING_SIZE - 1);
    RdbCallEntry *e = &s_call_ring[idx];
    e->frame  = (uint32_t)g_frame_count;
    e->func   = func_pc;
    uint8_t s_ptr = g_cpu.S;
    uint8_t lo = g_ram[0x100 + ((uint8_t)(s_ptr + 1))];
    uint8_t hi = g_ram[0x100 + ((uint8_t)(s_ptr + 2))];
    uint16_t ret = (uint16_t)lo | ((uint16_t)hi << 8);
    e->caller = (uint16_t)(ret + 1);
    if (s_call_count < RDB_CALL_RING_SIZE) s_call_count++;
}

/* ============================================================= */
/* Tier 2 — block hook                                            */
/* ============================================================= */

static inline void maybe_anchor(void) {
    if (!s_anchor_active) return;
    if ((s_block_counter % s_anchor_interval) != 0) return;
    int idx = s_anchor_widx % RDB_ANCHOR_COUNT;
    s_anchors[idx].block_idx = s_block_counter;
    s_anchors[idx].frame     = (uint32_t)g_frame_count;
    memcpy(s_anchors[idx].wram, g_ram, 0x0800);
    s_anchor_widx++;
    if (s_anchor_count < RDB_ANCHOR_COUNT) s_anchor_count++;
}

static void park_on_break(uint16_t pc, uint8_t reason) {
    s_parked_reason     = reason;
    s_parked_pc         = pc;
    s_parked_func       = g_rdb_current_func;
    s_parked_watch_addr = 0;
    s_parked_watch_val  = 0;
    g_rdb_paused        = 1;
    rdb_wait_if_parked();
}

void rdb_on_block(uint16_t pc) {
    s_block_counter++;
    maybe_anchor();

    if (s_block_trace_active &&
        (s_block_pc_nranges == 0 ||
         in_any_range(s_block_pc_ranges, s_block_pc_nranges, pc))) {
        uint64_t idx = s_block_widx++ & (RDB_BLOCK_RING_SIZE - 1);
        RdbBlockEntry *e = &s_block_ring[idx];
        e->frame = (uint32_t)g_frame_count;
        e->pc    = pc;
        e->func  = g_rdb_current_func;
        e->a     = g_cpu.A;
        e->x     = g_cpu.X;
        e->y     = g_cpu.Y;
        e->p     = (uint8_t)((g_cpu.N << 7) | (g_cpu.V << 6) | 0x20 |
                             (g_cpu.D << 3) | (g_cpu.I << 2) |
                             (g_cpu.Z << 1) |  g_cpu.C);
        if (s_block_count < RDB_BLOCK_RING_SIZE) s_block_count++;
    }

    if (s_break_step_block) {
        s_break_step_block = 0;
        park_on_break(pc, 3);
        return;
    }

    if (s_break_count) {
        for (int i = 0; i < s_break_count; i++) {
            if (s_breaks[i] == pc) { park_on_break(pc, 1); break; }
        }
    }
}

/* ============================================================= */
/* Park spin                                                      */
/* ============================================================= */

void rdb_wait_if_parked(void) {
    while (g_rdb_paused) {
        debug_server_poll();
    }
}

/* ============================================================= */
/* TCP commands                                                   */
/* ============================================================= */

/* ---- Tier 1 / status ---- */
static void cmd_rdb_status(int id, const char *json) {
    (void)json;
    char ranges[256]; int p = 0;
    p += snprintf(ranges + p, sizeof(ranges) - p, "[");
    for (int i = 0; i < s_store_nranges; i++)
        p += snprintf(ranges + p, sizeof(ranges) - p,
                      "%s{\"lo\":\"0x%04X\",\"hi\":\"0x%04X\"}",
                      i ? "," : "", s_store_ranges[i].lo, s_store_ranges[i].hi);
    p += snprintf(ranges + p, sizeof(ranges) - p, "]");
    debug_server_send_fmt(
        "{\"id\":%d,\"ok\":true,\"store_count\":%llu,\"store_ranges\":%s,"
        "\"call_count\":%llu,\"call_trace\":%d,"
        "\"block_count\":%llu,\"block_trace\":%d,"
        "\"break_count\":%d,\"watch_count\":%d,"
        "\"anchor_active\":%d,\"anchor_interval\":%u,\"anchor_count\":%d,"
        "\"block_idx\":%llu,\"current_func\":\"0x%04X\",\"paused\":%d}",
        id,
        (unsigned long long)s_store_count, ranges,
        (unsigned long long)s_call_count, s_call_trace_active,
        (unsigned long long)s_block_count, s_block_trace_active,
        s_break_count, s_watch_count,
        s_anchor_active, (unsigned)s_anchor_interval, s_anchor_count,
        (unsigned long long)s_block_counter, g_rdb_current_func, g_rdb_paused);
}

static void cmd_rdb_range(int id, const char *json) {
    uint32_t lo = 0, hi = 0;
    if (!json_u32(json, "lo", &lo) || !json_u32(json, "hi", &hi)) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"usage: rdb_range lo hi\"}", id);
        return;
    }
    if (s_store_nranges >= RDB_MAX_RANGES) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"store range table full\"}", id);
        return;
    }
    s_store_ranges[s_store_nranges].lo = (uint16_t)lo;
    s_store_ranges[s_store_nranges].hi = (uint16_t)hi;
    s_store_nranges++;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"nranges\":%d}", id, s_store_nranges);
}
static void cmd_rdb_range_clear(int id, const char *json) {
    (void)json; s_store_nranges = 0;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true}", id);
}
static void cmd_rdb_reset(int id, const char *json) {
    (void)json; s_store_widx = 0; s_store_count = 0;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true}", id);
}
static void cmd_rdb_count(int id, const char *json) {
    (void)json;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"count\":%llu,\"write_idx\":%llu}",
                          id, (unsigned long long)s_store_count, (unsigned long long)s_store_widx);
}
static void cmd_rdb_dump(int id, const char *json) {
    uint32_t start = 0, max_n = 256;
    json_u32(json, "start", &start);
    json_u32(json, "max", &max_n);
    if (max_n == 0 || max_n > 4096) max_n = 256;
    uint64_t oldest = (s_store_count < RDB_STORE_RING_SIZE) ? 0 : (s_store_widx - RDB_STORE_RING_SIZE);
    if (start >= s_store_count) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"count\":%llu,\"returned\":0,\"entries\":[]}",
                              id, (unsigned long long)s_store_count);
        return;
    }
    uint64_t want = max_n;
    if (start + want > s_store_count) want = s_store_count - start;
    size_t bufsz = (size_t)want * 128 + 512;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"oom\"}", id); return; }
    int pos = 0;
    pos += snprintf(buf + pos, bufsz - pos, "[");
    for (uint64_t i = 0; i < want; i++) {
        uint64_t gidx = oldest + start + i;
        const RdbStoreEntry *e = &s_store_ring[gidx & (RDB_STORE_RING_SIZE - 1)];
        pos += snprintf(buf + pos, bufsz - pos,
                        "%s{\"i\":%llu,\"block\":%llu,\"frame\":%u,\"addr\":\"0x%04X\",\"val\":\"0x%02X\",\"pc\":\"0x%04X\",\"func\":\"0x%04X\"}",
                        i ? "," : "", (unsigned long long)gidx,
                        (unsigned long long)e->block_idx,
                        e->frame, e->addr, e->val, e->pc_hint, e->func);
    }
    pos += snprintf(buf + pos, bufsz - pos, "]");
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"count\":%llu,\"returned\":%llu,\"entries\":%s}",
                          id, (unsigned long long)s_store_count, (unsigned long long)want, buf);
    free(buf);
}

/* ---- Tier 1.5 ---- */
static void cmd_trace_calls(int id, const char *json) {
    (void)json; s_call_trace_active = 1;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true}", id);
}
static void cmd_trace_calls_reset(int id, const char *json) {
    (void)json; s_call_trace_active = 0; s_call_widx = 0; s_call_count = 0;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true}", id);
}
static void cmd_get_call_trace(int id, const char *json) {
    uint32_t max_n = 256, from = 0, to = 0xFFFF;
    json_u32(json, "max", &max_n);
    json_u32(json, "from", &from);
    json_u32(json, "to", &to);
    if (max_n == 0 || max_n > 4096) max_n = 256;
    uint64_t oldest = (s_call_count < RDB_CALL_RING_SIZE) ? 0 : (s_call_widx - RDB_CALL_RING_SIZE);
    size_t bufsz = (size_t)max_n * 128 + 512;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"oom\"}", id); return; }
    int pos = 0; pos += snprintf(buf + pos, bufsz - pos, "[");
    uint32_t emitted = 0;
    for (uint64_t i = 0; i < s_call_count && emitted < max_n; i++) {
        uint64_t gidx = oldest + i;
        const RdbCallEntry *e = &s_call_ring[gidx & (RDB_CALL_RING_SIZE - 1)];
        if (e->func < from || e->func > to) continue;
        pos += snprintf(buf + pos, bufsz - pos,
                        "%s{\"i\":%llu,\"frame\":%u,\"func\":\"0x%04X\",\"caller\":\"0x%04X\"}",
                        emitted ? "," : "",
                        (unsigned long long)gidx, e->frame, e->func, e->caller);
        emitted++;
    }
    pos += snprintf(buf + pos, bufsz - pos, "]");
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"count\":%llu,\"returned\":%u,\"entries\":%s}",
                          id, (unsigned long long)s_call_count, emitted, buf);
    free(buf);
}

/* ---- Tier 2 ---- */
static void cmd_trace_blocks(int id, const char *json) {
    (void)json; s_block_trace_active = 1;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true}", id);
}
static void cmd_trace_blocks_reset(int id, const char *json) {
    (void)json; s_block_trace_active = 0; s_block_widx = 0; s_block_count = 0;
    s_block_pc_nranges = 0;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true}", id);
}
static void cmd_trace_blocks_range(int id, const char *json) {
    uint32_t lo = 0, hi = 0;
    if (!json_u32(json, "lo", &lo) || !json_u32(json, "hi", &hi)) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"usage: trace_blocks_range lo hi\"}", id);
        return;
    }
    if (s_block_pc_nranges >= RDB_MAX_RANGES) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"block range table full\"}", id);
        return;
    }
    s_block_pc_ranges[s_block_pc_nranges].lo = (uint16_t)lo;
    s_block_pc_ranges[s_block_pc_nranges].hi = (uint16_t)hi;
    s_block_pc_nranges++;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"nranges\":%d}", id, s_block_pc_nranges);
}
static void cmd_get_block_trace(int id, const char *json) {
    uint32_t max_n = 256, from = 0, to = 0xFFFF;
    json_u32(json, "max", &max_n);
    json_u32(json, "from", &from);
    json_u32(json, "to", &to);
    if (max_n == 0 || max_n > 4096) max_n = 256;
    uint64_t oldest = (s_block_count < RDB_BLOCK_RING_SIZE) ? 0 : (s_block_widx - RDB_BLOCK_RING_SIZE);
    size_t bufsz = (size_t)max_n * 144 + 512;
    char *buf = (char *)malloc(bufsz);
    if (!buf) { debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"oom\"}", id); return; }
    int pos = 0; pos += snprintf(buf + pos, bufsz - pos, "[");
    uint32_t emitted = 0;
    for (uint64_t i = 0; i < s_block_count && emitted < max_n; i++) {
        uint64_t gidx = oldest + i;
        const RdbBlockEntry *e = &s_block_ring[gidx & (RDB_BLOCK_RING_SIZE - 1)];
        if (e->pc < from || e->pc > to) continue;
        pos += snprintf(buf + pos, bufsz - pos,
            "%s{\"i\":%llu,\"frame\":%u,\"pc\":\"0x%04X\",\"func\":\"0x%04X\","
            "\"a\":\"0x%02X\",\"x\":\"0x%02X\",\"y\":\"0x%02X\",\"p\":\"0x%02X\"}",
            emitted ? "," : "", (unsigned long long)gidx,
            e->frame, e->pc, e->func, e->a, e->x, e->y, e->p);
        emitted++;
    }
    pos += snprintf(buf + pos, bufsz - pos, "]");
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"count\":%llu,\"returned\":%u,\"entries\":%s}",
                          id, (unsigned long long)s_block_count, emitted, buf);
    free(buf);
}

/* ---- Tier 2.5 breakpoints ---- */
static void cmd_rdb_break(int id, const char *json) {
    uint32_t pc = 0;
    if (!json_u32(json, "pc", &pc)) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"usage: rdb_break pc\"}", id);
        return;
    }
    if (s_break_count >= RDB_MAX_BREAKS) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"break table full\"}", id);
        return;
    }
    s_breaks[s_break_count++] = (uint16_t)pc;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"count\":%d}", id, s_break_count);
}
static void cmd_rdb_break_clear(int id, const char *json) {
    (void)json; s_break_count = 0;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true}", id);
}
static void cmd_rdb_break_list(int id, const char *json) {
    (void)json;
    char buf[512]; int p = 0;
    p += snprintf(buf + p, sizeof(buf) - p, "[");
    for (int i = 0; i < s_break_count; i++)
        p += snprintf(buf + p, sizeof(buf) - p, "%s\"0x%04X\"", i ? "," : "", s_breaks[i]);
    p += snprintf(buf + p, sizeof(buf) - p, "]");
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"breaks\":%s}", id, buf);
}
static void cmd_rdb_break_continue(int id, const char *json) {
    (void)json; g_rdb_paused = 0;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true}", id);
}
static void cmd_rdb_step_block(int id, const char *json) {
    (void)json; s_break_step_block = 1; g_rdb_paused = 0;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true}", id);
}

/* ---- Tier 2.5 watchpoints ---- */
static void cmd_rdb_watch_add(int id, const char *json) {
    uint32_t addr = 0;
    int32_t  match = -1;
    if (!json_u32(json, "addr", &addr)) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"usage: rdb_watch_add addr [val]\"}", id);
        return;
    }
    json_i32(json, "val", &match);
    if (s_watch_count >= RDB_MAX_WATCHES) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"watch table full\"}", id);
        return;
    }
    s_watches[s_watch_count].addr  = (uint16_t)addr;
    s_watches[s_watch_count].match = (int16_t)match;
    s_watch_count++;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"count\":%d}", id, s_watch_count);
}
static void cmd_rdb_watch_clear(int id, const char *json) {
    (void)json; s_watch_count = 0;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true}", id);
}
static void cmd_rdb_watch_list(int id, const char *json) {
    (void)json;
    char buf[512]; int p = 0;
    p += snprintf(buf + p, sizeof(buf) - p, "[");
    for (int i = 0; i < s_watch_count; i++)
        p += snprintf(buf + p, sizeof(buf) - p,
                      "%s{\"addr\":\"0x%04X\",\"val\":%d}",
                      i ? "," : "", s_watches[i].addr, s_watches[i].match);
    p += snprintf(buf + p, sizeof(buf) - p, "]");
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"watches\":%s}", id, buf);
}
static void cmd_rdb_watch_continue(int id, const char *json) {
    (void)json; g_rdb_paused = 0;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true}", id);
}
static void cmd_rdb_parked(int id, const char *json) {
    (void)json;
    debug_server_send_fmt(
        "{\"id\":%d,\"ok\":true,\"paused\":%d,\"reason\":%d,\"pc\":\"0x%04X\","
        "\"func\":\"0x%04X\",\"watch_addr\":\"0x%04X\",\"watch_val\":\"0x%02X\"}",
        id, g_rdb_paused, s_parked_reason, s_parked_pc, s_parked_func,
        s_parked_watch_addr, s_parked_watch_val);
}

/* ---- Tier 3 anchors ---- */
static void cmd_rdb_anchor_on(int id, const char *json) {
    uint32_t interval = 0;
    if (json_u32(json, "interval", &interval) && interval >= 64)
        s_anchor_interval = interval;
    s_anchor_active = 1;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true,\"interval\":%u}", id, (unsigned)s_anchor_interval);
}
static void cmd_rdb_anchor_off(int id, const char *json) {
    (void)json; s_anchor_active = 0;
    debug_server_send_fmt("{\"id\":%d,\"ok\":true}", id);
}
static void cmd_rdb_anchor_status(int id, const char *json) {
    (void)json;
    debug_server_send_fmt(
        "{\"id\":%d,\"ok\":true,\"active\":%d,\"count\":%d,\"interval\":%u,\"block_idx\":%llu}",
        id, s_anchor_active, s_anchor_count, (unsigned)s_anchor_interval,
        (unsigned long long)s_block_counter);
}
static void cmd_rdb_wram_at_block(int id, const char *json) {
    uint64_t target = 0;
    uint32_t tmp = 0;
    if (!json_u32(json, "block", &tmp)) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"usage: rdb_wram_at_block block\"}", id);
        return;
    }
    target = (uint64_t)tmp;
    if (target > s_block_counter) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"block in the future\"}", id);
        return;
    }

    /* Find nearest prior anchor (greatest block_idx <= target). */
    int best = -1;
    uint64_t best_idx = 0;
    for (int i = 0; i < s_anchor_count; i++) {
        int slot = (s_anchor_widx - 1 - i);
        while (slot < 0) slot += RDB_ANCHOR_COUNT;
        slot %= RDB_ANCHOR_COUNT;
        if (s_anchors[slot].block_idx <= target) {
            if (best < 0 || s_anchors[slot].block_idx > best_idx) {
                best = slot;
                best_idx = s_anchors[slot].block_idx;
            }
        }
    }
    if (best < 0) {
        debug_server_send_fmt("{\"id\":%d,\"ok\":false,\"error\":\"no prior anchor covers this block\"}", id);
        return;
    }

    /* Reconstruct: start from anchor snapshot, walk store ring forward
     * replaying every entry whose block_idx is in (anchor_block, target]
     * and whose addr is WRAM ($0000-$07FF). */
    static uint8_t wram[0x0800];
    memcpy(wram, s_anchors[best].wram, 0x0800);

    uint64_t oldest = (s_store_count < RDB_STORE_RING_SIZE)
                         ? 0 : (s_store_widx - RDB_STORE_RING_SIZE);
    /* If oldest stored block_idx is already past anchor_block, the
     * store ring has wrapped through the replay window — reconstruction
     * would be incomplete. Detect this up-front. */
    int store_ring_wrapped_past_anchor = 0;
    if (s_store_count > 0) {
        const RdbStoreEntry *first = &s_store_ring[oldest & (RDB_STORE_RING_SIZE - 1)];
        if (first->block_idx > s_anchors[best].block_idx)
            store_ring_wrapped_past_anchor = 1;
    }

    uint64_t replayed = 0;
    for (uint64_t i = 0; i < s_store_count; i++) {
        uint64_t gidx = oldest + i;
        const RdbStoreEntry *e = &s_store_ring[gidx & (RDB_STORE_RING_SIZE - 1)];
        if (e->block_idx <= s_anchors[best].block_idx) continue;
        if (e->block_idx > target) break;   /* ring is chronological */
        if (e->addr > 0x07FF) continue;
        wram[e->addr] = e->val;
        replayed++;
    }

    static char hex[0x0800 * 2 + 16];
    static const char HX[] = "0123456789ABCDEF";
    for (int i = 0; i < 0x0800; i++) {
        hex[i * 2]     = HX[(wram[i] >> 4) & 0xF];
        hex[i * 2 + 1] = HX[wram[i] & 0xF];
    }
    hex[0x0800 * 2] = '\0';

    debug_server_send_fmt(
        "{\"id\":%d,\"ok\":true,\"anchor_block\":%llu,\"anchor_frame\":%u,"
        "\"target_block\":%llu,\"replayed\":%llu,\"store_ring_wrapped\":%d,"
        "\"hex\":\"%s\"}",
        id, (unsigned long long)s_anchors[best].block_idx,
        s_anchors[best].frame, (unsigned long long)target,
        (unsigned long long)replayed, store_ring_wrapped_past_anchor, hex);
}

/* ============================================================= */
/* Dispatcher                                                     */
/* ============================================================= */

int rdb_handle_cmd(const char *cmd, int id, const char *json) {
    if (strcmp(cmd, "rdb_status")       == 0) { cmd_rdb_status(id, json);       return 1; }
    if (strcmp(cmd, "rdb_range")        == 0) { cmd_rdb_range(id, json);        return 1; }
    if (strcmp(cmd, "rdb_range_clear")  == 0) { cmd_rdb_range_clear(id, json);  return 1; }
    if (strcmp(cmd, "rdb_reset")        == 0) { cmd_rdb_reset(id, json);        return 1; }
    if (strcmp(cmd, "rdb_count")        == 0) { cmd_rdb_count(id, json);        return 1; }
    if (strcmp(cmd, "rdb_dump")         == 0) { cmd_rdb_dump(id, json);         return 1; }
    if (strcmp(cmd, "trace_calls")       == 0) { cmd_trace_calls(id, json);       return 1; }
    if (strcmp(cmd, "trace_calls_reset") == 0) { cmd_trace_calls_reset(id, json); return 1; }
    if (strcmp(cmd, "get_call_trace")    == 0) { cmd_get_call_trace(id, json);    return 1; }
    if (strcmp(cmd, "trace_blocks")       == 0) { cmd_trace_blocks(id, json);       return 1; }
    if (strcmp(cmd, "trace_blocks_reset") == 0) { cmd_trace_blocks_reset(id, json); return 1; }
    if (strcmp(cmd, "trace_blocks_range") == 0) { cmd_trace_blocks_range(id, json); return 1; }
    if (strcmp(cmd, "get_block_trace")    == 0) { cmd_get_block_trace(id, json);    return 1; }
    if (strcmp(cmd, "rdb_break")          == 0) { cmd_rdb_break(id, json);          return 1; }
    if (strcmp(cmd, "rdb_break_clear")    == 0) { cmd_rdb_break_clear(id, json);    return 1; }
    if (strcmp(cmd, "rdb_break_list")     == 0) { cmd_rdb_break_list(id, json);     return 1; }
    if (strcmp(cmd, "rdb_break_continue") == 0) { cmd_rdb_break_continue(id, json); return 1; }
    if (strcmp(cmd, "rdb_step_block")     == 0) { cmd_rdb_step_block(id, json);     return 1; }
    if (strcmp(cmd, "rdb_watch_add")      == 0) { cmd_rdb_watch_add(id, json);      return 1; }
    if (strcmp(cmd, "rdb_watch_clear")    == 0) { cmd_rdb_watch_clear(id, json);    return 1; }
    if (strcmp(cmd, "rdb_watch_list")     == 0) { cmd_rdb_watch_list(id, json);     return 1; }
    if (strcmp(cmd, "rdb_watch_continue") == 0) { cmd_rdb_watch_continue(id, json); return 1; }
    if (strcmp(cmd, "rdb_parked")         == 0) { cmd_rdb_parked(id, json);         return 1; }
    if (strcmp(cmd, "rdb_anchor_on")      == 0) { cmd_rdb_anchor_on(id, json);      return 1; }
    if (strcmp(cmd, "rdb_anchor_off")     == 0) { cmd_rdb_anchor_off(id, json);     return 1; }
    if (strcmp(cmd, "rdb_anchor_status")  == 0) { cmd_rdb_anchor_status(id, json);  return 1; }
    if (strcmp(cmd, "rdb_wram_at_block")  == 0) { cmd_rdb_wram_at_block(id, json);  return 1; }
    return 0;
}

void rdb_init(void) {
    /* Rings are BSS. Kept for future setup. */
}

#endif /* NESRECOMP_REVERSE_DEBUG */
