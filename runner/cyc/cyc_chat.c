/*
 * cyc_chat.c - chat over the game and chat effects on it (cyc_chat.h).
 */
#include "cyc_chat.h"

#include "cyc_mod.h"
#include "cyc_tcp.h"

#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyc_chat_font.inc"

#define MSG_MAX   48
#define PAD_MAX   8
#define RAM_MAX   16
#define ROWS_MAX  16
#define LINE_H    10                 /* chat pixels per panel row: 8 of glyph, 2 of gap */
#define FADE_MS   1000

typedef struct {
    char     user[32];
    char     text[256];
    uint32_t color;
    uint32_t born, life;
} Msg;

enum { PAD_PRESS, PAD_TAP, PAD_BLOCK, PAD_INVERT };
typedef struct {
    bool    on;
    int     mode;
    uint8_t buttons;
    long    left;
    char    label[40];
} PadFx;

enum { RAM_SET, RAM_ADD, RAM_OR, RAM_AND };
typedef struct {
    bool     on;
    uint16_t addr;
    int      op, value, lo, hi;
    long     left, every, tick;
    long     if_addr, if_eq;
    char     label[40];
} RamFx;

static Msg s_msg[MSG_MAX];
static int s_msg_next, s_msg_count;  /* ring; the newest is at s_msg_next - 1 */
static struct { char text[96]; uint32_t color, born, life; } s_banner;
#define POLL_MAX 5
static struct {
    char     title[32], option[POLL_MAX][24];
    int      votes[POLL_MAX], count, winner, y;
    bool     countdown;
    uint32_t born, life;
} s_poll;
static PadFx s_pad[PAD_MAX];
static RamFx s_ram[RAM_MAX];
/* offset: percent of the window height between the panel and its edge */
static struct { bool show, right, top; int lines, scale, offset; uint32_t ms; } s_cfg = { true, false, false, 8, 0, 3, 20000 };

static SDL_Texture *s_tex;
static int          s_tex_w, s_tex_h;
static uint32_t    *s_px;
static size_t       s_px_len;

/* ---- effects ---- */

static uint8_t swap_bits(uint8_t pad, uint8_t a, uint8_t b)
{
    bool ha = pad & a, hb = pad & b;
    pad &= (uint8_t)~(a | b);
    return (uint8_t)(pad | (ha ? b : 0) | (hb ? a : 0));
}

static void ram_store(RamFx *fx)
{
    uint8_t cond;
    if (fx->if_addr >= 0 && (!cyc_mod_peek_ok((uint16_t)fx->if_addr, &cond) || cond != (uint8_t)fx->if_eq)) return;
    int v = cyc_mod_peek(fx->addr);
    switch (fx->op) {
    case RAM_SET: v = fx->value; break;
    case RAM_ADD:
        v += fx->value;
        if (v < fx->lo) v = fx->lo;
        if (v > fx->hi) v = fx->hi;
        break;
    case RAM_OR: v |= fx->value; break;
    default: v &= fx->value; break;
    }
    cyc_mod_poke(fx->addr, (uint8_t)v);
}

uint8_t cyc_chat_frame(uint8_t pad)
{
    for (int i = 0; i < RAM_MAX; ++i) {
        RamFx *fx = &s_ram[i];
        if (!fx->on) continue;
        if (fx->tick++ % fx->every == 0) ram_store(fx);
        if (--fx->left <= 0) fx->on = false;
    }
    for (int i = 0; i < PAD_MAX; ++i) {
        PadFx *fx = &s_pad[i];
        if (!fx->on) continue;
        switch (fx->mode) {
        case PAD_PRESS: pad |= fx->buttons; break;
        case PAD_TAP: if ((fx->left / 8) % 2 == 0) pad |= fx->buttons; break;
        case PAD_BLOCK: pad &= (uint8_t)~fx->buttons; break;
        default:
            if (fx->buttons & 0x03) pad = swap_bits(pad, 0x01, 0x02);
            if (fx->buttons & 0x0C) pad = swap_bits(pad, 0x04, 0x08);
            break;
        }
        if (--fx->left <= 0) fx->on = false;
    }
    return pad;
}

/* The slot an effect goes in: the active one with its label, else a free one. */
static int pad_slot(const char *label)
{
    for (int i = 0; label[0] && i < PAD_MAX; ++i)
        if (s_pad[i].on && !strcmp(s_pad[i].label, label)) return i;
    for (int i = 0; i < PAD_MAX; ++i)
        if (!s_pad[i].on) return i;
    return -1;
}

static int ram_slot(const char *label)
{
    for (int i = 0; label[0] && i < RAM_MAX; ++i)
        if (s_ram[i].on && !strcmp(s_ram[i].label, label)) return i;
    for (int i = 0; i < RAM_MAX; ++i)
        if (!s_ram[i].on) return i;
    return -1;
}

/* ---- requests ---- */

static uint32_t color_field(const char *line, const char *key, uint32_t fallback)
{
    char hex[16];
    if (!cyc_tcp_str(line, key, hex, sizeof(hex))) return fallback;
    const char *p = hex[0] == '#' ? hex + 1 : hex;
    char *end;
    unsigned long v = strtoul(p, &end, 16);
    return end != p && !*end && v <= 0xFFFFFFu ? (uint32_t)v : fallback;
}

/* Printable ASCII only: the font has nothing else (the bridge transliterates). */
static void printable(char *s)
{
    for (; *s; ++s)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) *s = '?';
}

static void tcp_chat(int id, const char *line)
{
    Msg m;
    memset(&m, 0, sizeof(m));
    if (!cyc_tcp_str(line, "text", m.text, sizeof(m.text))) {
        cyc_tcp_err(id, "chat: user, text, color (RRGGBB), ms");
        return;
    }
    cyc_tcp_str(line, "user", m.user, sizeof(m.user));
    printable(m.user);
    printable(m.text);
    m.color = color_field(line, "color", 0x9FA8FF);
    long ms = (long)s_cfg.ms;
    cyc_tcp_long(line, "ms", &ms);
    m.born = SDL_GetTicks();
    m.life = (uint32_t)(ms < 500 ? 500 : ms > 600000 ? 600000 : ms);
    s_msg[s_msg_next] = m;
    s_msg_next = (s_msg_next + 1) % MSG_MAX;
    if (s_msg_count < MSG_MAX) s_msg_count++;
    cyc_tcp_ok(id, NULL);
}

static void tcp_banner(int id, const char *line)
{
    if (!cyc_tcp_str(line, "text", s_banner.text, sizeof(s_banner.text))) {
        cyc_tcp_err(id, "chat_banner: text, color (RRGGBB), ms");
        return;
    }
    printable(s_banner.text);
    s_banner.color = color_field(line, "color", 0xFFD25A);
    long ms = 3000;
    cyc_tcp_long(line, "ms", &ms);
    s_banner.born = SDL_GetTicks();
    s_banner.life = (uint32_t)(ms < 500 ? 500 : ms > 60000 ? 60000 : ms);
    cyc_tcp_ok(id, NULL);
}

/* `options` and `votes` are '|'-separated lists ("STAR|MOON", "3|1"); a
 * request without options takes the poll down. */
static void tcp_poll(int id, const char *line)
{
    char options[POLL_MAX * 24 + 8] = "", votes[POLL_MAX * 8 + 8] = "";
    cyc_tcp_str(line, "options", options, sizeof(options));
    cyc_tcp_str(line, "votes", votes, sizeof(votes));
    s_poll.count = 0;
    if (!options[0]) { cyc_tcp_ok(id, NULL); return; }
    char *o = options, *v = votes;
    while (o && s_poll.count < POLL_MAX) {
        char *bar = strchr(o, '|');
        if (bar) *bar = 0;
        snprintf(s_poll.option[s_poll.count], sizeof(s_poll.option[0]), "%s", o);
        printable(s_poll.option[s_poll.count]);
        long n = v && *v ? strtol(v, &v, 10) : 0;
        if (v && *v == '|') ++v;
        s_poll.votes[s_poll.count++] = n < 0 ? 0 : n > 99999 ? 99999 : (int)n;
        o = bar ? bar + 1 : NULL;
    }
    if (!cyc_tcp_str(line, "title", s_poll.title, sizeof(s_poll.title))) snprintf(s_poll.title, sizeof(s_poll.title), "VOTE");
    printable(s_poll.title);
    long ms = 10000, winner = -1, y = 20;
    bool countdown = true;
    cyc_tcp_long(line, "ms", &ms);
    cyc_tcp_long(line, "winner", &winner);
    cyc_tcp_long(line, "y", &y);
    cyc_tcp_bool(line, "countdown", &countdown);
    s_poll.winner = winner >= 0 && winner < s_poll.count ? (int)winner : -1;
    s_poll.y = y < 0 ? 0 : y > 90 ? 90 : (int)y;
    s_poll.countdown = countdown;
    s_poll.born = SDL_GetTicks();
    s_poll.life = (uint32_t)(ms < 500 ? 500 : ms > 600000 ? 600000 : ms);
    cyc_tcp_ok(id, NULL);
}

static void tcp_pad(int id, const char *line)
{
    char mode[16] = "press", label[40] = "";
    long buttons = 0, frames = 60;
    cyc_tcp_str(line, "mode", mode, sizeof(mode));
    cyc_tcp_str(line, "label", label, sizeof(label));
    printable(label);
    cyc_tcp_long(line, "frames", &frames);
    int m = !strcmp(mode, "press") ? PAD_PRESS : !strcmp(mode, "tap") ? PAD_TAP
          : !strcmp(mode, "block") ? PAD_BLOCK : !strcmp(mode, "invert") ? PAD_INVERT : -1;
    if (!cyc_tcp_long(line, "buttons", &buttons) || buttons <= 0 || buttons > 0xFF || m < 0 || frames < 1) {
        cyc_tcp_err(id, "chat_pad: buttons (A = 0x80 ... Right = 0x01), mode press|tap|block|invert, frames, label");
        return;
    }
    int i = pad_slot(label);
    if (i < 0) { cyc_tcp_err(id, "chat_pad: every input effect slot is busy"); return; }
    PadFx *fx = &s_pad[i];
    fx->on = true;
    fx->mode = m;
    fx->buttons = (uint8_t)buttons;
    fx->left = frames > 36000 ? 36000 : frames;
    snprintf(fx->label, sizeof(fx->label), "%s", label);
    char f[32];
    snprintf(f, sizeof(f), "\"slot\":%d", i);
    cyc_tcp_ok(id, f);
}

static void tcp_ram(int id, const char *line)
{
    RamFx fx;
    memset(&fx, 0, sizeof(fx));
    long addr = -1, value = 0, lo = 0, hi = 255, frames = 0, every = 1, if_addr = -1, if_eq = 0;
    int op = -1;
    if (cyc_tcp_long(line, "set", &value)) op = RAM_SET;
    else if (cyc_tcp_long(line, "add", &value)) op = RAM_ADD;
    else if (cyc_tcp_long(line, "or", &value)) op = RAM_OR;
    else if (cyc_tcp_long(line, "and", &value)) op = RAM_AND;
    cyc_tcp_long(line, "min", &lo);
    cyc_tcp_long(line, "max", &hi);
    cyc_tcp_long(line, "frames", &frames);
    cyc_tcp_long(line, "every", &every);
    bool cond = cyc_tcp_long(line, "if_addr", &if_addr);
    if (cond && !cyc_tcp_long(line, "if_eq", &if_eq)) if_addr = -2;
    cyc_tcp_str(line, "label", fx.label, sizeof(fx.label));
    printable(fx.label);
    uint8_t probe;
    if (!cyc_tcp_long(line, "addr", &addr) || addr < 0 || addr > 0xFFFF || op < 0 || value < -255 || value > 255 ||
        lo < 0 || hi > 255 || lo > hi || frames < 0 || every < 1 || if_addr < -1 || if_addr > 0xFFFF ||
        !cyc_mod_peek_ok((uint16_t)addr, &probe)) {
        cyc_tcp_err(id, "chat_ram: addr (RAM), set|add|or|and, min, max, frames, every, if_addr + if_eq, label");
        return;
    }
    fx.addr = (uint16_t)addr;
    fx.op = op;
    fx.value = (int)value;
    fx.lo = (int)lo;
    fx.hi = (int)hi;
    fx.every = every;
    fx.if_addr = if_addr;
    fx.if_eq = if_eq;
    if (!frames) {                          /* once, now: requests are read between frames */
        ram_store(&fx);
        cyc_tcp_ok(id, NULL);
        return;
    }
    int i = ram_slot(fx.label);
    if (i < 0) { cyc_tcp_err(id, "chat_ram: every RAM effect slot is busy"); return; }
    fx.on = true;
    fx.left = frames > 36000 ? 36000 : frames;
    s_ram[i] = fx;
    char f[32];
    snprintf(f, sizeof(f), "\"slot\":%d", i);
    cyc_tcp_ok(id, f);
}

/* One of the program's own routines, between frames, its RAM effects kept
 * (cyc_mod_call_commit): what a RAM effect alone would leave half done. */
static void tcp_call(int id, const char *line)
{
    long addr = -1, a = 0, x = 0, y = 0;
    cyc_tcp_long(line, "a", &a);
    cyc_tcp_long(line, "x", &x);
    cyc_tcp_long(line, "y", &y);
    if (!cyc_tcp_long(line, "addr", &addr) || addr < 0 || addr > 0xFFFF) {
        cyc_tcp_err(id, "chat_call: addr, a, x, y");
        return;
    }
    CycModRegs r;
    cyc_mod_regs(&r);
    r.a = (uint8_t)a;
    r.x = (uint8_t)x;
    r.y = (uint8_t)y;
    if (!cyc_mod_call_commit((uint16_t)addr, &r)) { cyc_tcp_err(id, "chat_call: the routine did not return"); return; }
    cyc_tcp_ok(id, NULL);
}

static void tcp_config(int id, const char *line)
{
    char side[16], valign[16];
    long lines = s_cfg.lines, ms = (long)s_cfg.ms, scale = s_cfg.scale, offset = s_cfg.offset;
    cyc_tcp_bool(line, "show", &s_cfg.show);
    if (cyc_tcp_str(line, "side", side, sizeof(side))) s_cfg.right = !strcmp(side, "right");
    if (cyc_tcp_str(line, "valign", valign, sizeof(valign))) s_cfg.top = !strcmp(valign, "top");
    cyc_tcp_long(line, "lines", &lines);
    cyc_tcp_long(line, "ms", &ms);
    cyc_tcp_long(line, "scale", &scale);
    cyc_tcp_long(line, "offset", &offset);
    s_cfg.lines = lines < 1 ? 1 : lines > ROWS_MAX ? ROWS_MAX : (int)lines;
    s_cfg.ms = (uint32_t)(ms < 500 ? 500 : ms > 600000 ? 600000 : ms);
    s_cfg.scale = scale < 0 ? 0 : scale > 8 ? 8 : (int)scale;
    s_cfg.offset = offset < 0 ? 0 : offset > 80 ? 80 : (int)offset;
    char f[192];
    snprintf(f, sizeof(f), "\"show\":%s,\"side\":\"%s\",\"valign\":\"%s\",\"offset\":%d,\"lines\":%d,\"ms\":%u,\"scale\":%d",
             s_cfg.show ? "true" : "false", s_cfg.right ? "right" : "left", s_cfg.top ? "top" : "bottom", s_cfg.offset,
             s_cfg.lines, (unsigned)s_cfg.ms, s_cfg.scale);
    cyc_tcp_ok(id, f);
}

static void tcp_clear(int id, const char *line)
{
    (void)line;
    s_msg_count = s_msg_next = 0;
    s_banner.life = 0;
    s_poll.count = 0;
    memset(s_pad, 0, sizeof(s_pad));
    memset(s_ram, 0, sizeof(s_ram));
    cyc_tcp_ok(id, NULL);
}

static void tcp_state(int id, const char *line)
{
    (void)line;
    uint32_t now = SDL_GetTicks();
    int shown = 0, pads = 0, rams = 0;
    for (int i = 0; i < s_msg_count; ++i)
        if (now - s_msg[i].born < s_msg[i].life) shown++;
    for (int i = 0; i < PAD_MAX; ++i) pads += s_pad[i].on;
    for (int i = 0; i < RAM_MAX; ++i) rams += s_ram[i].on;
    char f[160];
    snprintf(f, sizeof(f), "\"messages\":%d,\"banner\":%s,\"pad_effects\":%d,\"ram_effects\":%d,\"show\":%s", shown,
             s_banner.life && now - s_banner.born < s_banner.life ? "true" : "false", pads, rams,
             s_cfg.show ? "true" : "false");
    cyc_tcp_ok(id, f);
}

void cyc_chat_register_tcp(void)
{
    cyc_tcp_register("chat", "a chat line over the game: user, text, color (RRGGBB), ms", tcp_chat);
    cyc_tcp_register("chat_banner", "a big line across the picture: text, color, ms", tcp_banner);
    cyc_tcp_register("chat_poll", "a poll over the game: title, options a|b|c, votes 1|0|2, ms, countdown, winner, y (%)", tcp_poll);
    cyc_tcp_register("chat_pad", "player 1 input effect: buttons, mode press|tap|block|invert, frames, label", tcp_pad);
    cyc_tcp_register("chat_ram", "RAM effect: addr, set|add|or|and, min, max, frames, every, if_addr, if_eq, label", tcp_ram);
    cyc_tcp_register("chat_call", "run a routine of the program now and keep its RAM effects: addr, a, x, y", tcp_call);
    cyc_tcp_register("chat_config", "the chat panel: show, side left|right, valign top|bottom, offset (% of height), lines, ms, scale", tcp_config);
    cyc_tcp_register("chat_clear", "drop chat lines, the banner and every chat effect", tcp_clear);
    cyc_tcp_register("chat_state", "chat lines up, banner, active effects", tcp_state);
}

/* ---- drawing, into a buffer of chat pixels scaled up by whole window pixels ---- */

static void blend(int x, int y, uint32_t color, unsigned alpha)
{
    if (x < 0 || y < 0 || x >= s_tex_w || y >= s_tex_h || !alpha) return;
    uint32_t *p = &s_px[(size_t)y * (size_t)s_tex_w + (size_t)x];
    unsigned da = *p >> 24;
    if (alpha >= 255 || !da) { *p = (uint32_t)alpha << 24 | color; return; }
    unsigned oa = alpha + da * (255 - alpha) / 255, r = 0;
    for (int s = 0; s < 24; s += 8) {
        unsigned sc = (color >> s) & 255, dc = (*p >> s) & 255;
        r |= ((sc * alpha + dc * da * (255 - alpha) / 255) / oa) << s;
    }
    *p = (uint32_t)oa << 24 | r;
}

static void box(int x, int y, int w, int h, uint32_t color, unsigned alpha)
{
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx) blend(xx, yy, color, alpha);
}

static void glyph(int x, int y, int zoom, char c, uint32_t color, unsigned alpha)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *g = CHAT_FONT[c - 0x20];
    for (int r = 0; r < 8; ++r)
        for (int b = 0; b < 8; ++b)
            if (g[r] >> b & 1) box(x + b * zoom, y + r * zoom, zoom, zoom, color, alpha);
}

/* Text with a one-pixel dark edge below and right (the panel's rows) or all
 * round (the banner, which sits on the picture itself). */
static void text(int x, int y, int zoom, const char *s, int n, uint32_t color, unsigned alpha, bool outline)
{
    for (int i = 0; i < n; ++i) {
        int gx = x + i * 8 * zoom;
        if (outline) {
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx || dy) glyph(gx + dx * zoom, y + dy * zoom, zoom, s[i], 0x000000, alpha);
        } else {
            glyph(gx + 1, y + 1, zoom, s[i], 0x000000, alpha * 3 / 4);
        }
        glyph(gx, y, zoom, s[i], color, alpha);
    }
}

static unsigned fade(uint32_t now, uint32_t born, uint32_t life)
{
    uint32_t age = now - born;
    if (age >= life) return 0;
    uint32_t left = life - age;
    return left >= FADE_MS ? 255 : 255 * left / FADE_MS;
}

typedef struct { const Msg *m; int at, len; unsigned alpha; } Row;

/* "user: text" word-wrapped into rows of `cols`, newest message at the bottom. */
static int wrap(const Msg *m, int cols, unsigned alpha, Row *out, int max)
{
    char all[300];
    int n = snprintf(all, sizeof(all), m->user[0] ? "%s: %s" : "%s%s", m->user, m->text);
    if (n < 0) return 0;
    if (n >= (int)sizeof(all)) n = (int)sizeof(all) - 1;
    Row rows[40];
    int count = 0;
    for (int at = 0; at < n && count < 40;) {
        int len = n - at;
        if (len > cols) {
            len = cols;
            for (int k = cols; k > cols / 2; --k)
                if (all[at + k] == ' ') { len = k; break; }
        }
        rows[count++] = (Row){ m, at, len, alpha };
        at += len;
        while (at < n && all[at] == ' ') ++at;
    }
    if (count > max) count = max;            /* a very long message keeps its start */
    memcpy(out, rows, sizeof(Row) * (size_t)count);
    return count;
}

static void draw_panel(uint32_t now, int lw, int lh)
{
    int cols = lw * 2 / 5 / 8;
    if (cols < 16) cols = 16;
    if (cols > 48) cols = 48;
    Row rows[ROWS_MAX];
    int used = 0;
    /* newest first, so the newest message always has room */
    for (int k = 0; k < s_msg_count && used < s_cfg.lines; ++k) {
        const Msg *m = &s_msg[(s_msg_next - 1 - k + MSG_MAX) % MSG_MAX];
        unsigned a = fade(now, m->born, m->life);
        if (!a) continue;
        Row mine[ROWS_MAX];
        int n = wrap(m, cols, a, mine, s_cfg.lines - used);
        memmove(rows + n, rows, sizeof(Row) * (size_t)used);
        memcpy(rows, mine, sizeof(Row) * (size_t)n);
        used += n;
    }
    if (!used) return;
    int edge = lh * s_cfg.offset / 100 + 2;
    int x0 = s_cfg.right ? lw - 6 - cols * 8 - 4 : 6, y = s_cfg.top ? edge : lh - edge - used * LINE_H;
    for (int i = 0; i < used; ++i, y += LINE_H) {
        const Row *r = &rows[i];
        const Msg *m = r->m;
        char all[300];
        snprintf(all, sizeof(all), m->user[0] ? "%s: %s" : "%s%s", m->user, m->text);
        int name = m->user[0] ? (int)strlen(m->user) + 1 : 0;
        box(x0 - 2, y - 1, r->len * 8 + 4, LINE_H, 0x0C0C14, r->alpha * 150 / 255);
        int split = name - r->at;
        if (split < 0) split = 0;
        if (split > r->len) split = r->len;
        text(x0, y, 1, all + r->at, split, m->color, r->alpha, false);
        text(x0 + split * 8, y, 1, all + r->at + split, r->len - split, m->user[0] ? 0xF0F0F0 : m->color, r->alpha, false);
    }
}

static void draw_effects(int lw)
{
    int y = 4;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < (pass ? RAM_MAX : PAD_MAX); ++i) {
            bool on = pass ? s_ram[i].on : s_pad[i].on;
            const char *label = pass ? s_ram[i].label : s_pad[i].label;
            long left = pass ? s_ram[i].left : s_pad[i].left;
            if (!on || !label[0]) continue;
            char row[64];
            int n = snprintf(row, sizeof(row), "%s %lds", label, (left + 59) / 60);
            if (n < 0) continue;
            if (n >= (int)sizeof(row)) n = (int)sizeof(row) - 1;
            int x = lw - 6 - n * 8;
            box(x - 3, y - 2, n * 8 + 6, LINE_H + 2, 0x241A04, 190);
            text(x, y, 1, row, n, 0xFFD25A, 255, false);
            y += LINE_H + 3;
        }
}

static void draw_banner(uint32_t now, int lw, int lh)
{
    unsigned a = s_banner.life ? fade(now, s_banner.born, s_banner.life) : 0;
    if (!a) return;
    int n = (int)strlen(s_banner.text), zoom = 2;
    while (zoom > 1 && n * 8 * zoom > lw - 8) --zoom;
    if (n * 8 * zoom > lw - 8) n = (lw - 8) / (8 * zoom);
    int x = (lw - n * 8 * zoom) / 2, y = lh / 3 - 4 * zoom;
    text(x, y, zoom, s_banner.text, n, s_banner.color, a, true);
}

/* The poll: a title with its countdown, then one row per option with its
 * number, a bar for its share of the votes and its count; the winner lit. */
static void draw_poll(uint32_t now, int lw, int lh)
{
    unsigned a = s_poll.life ? fade(now, s_poll.born, s_poll.life) : 0;
    if (!a || !s_poll.count) return;
    const int cols = 30, w = cols * 8 + 8;
    int total = 0, best = 0;
    for (int i = 0; i < s_poll.count; ++i) {
        total += s_poll.votes[i];
        if (s_poll.votes[i] > best) best = s_poll.votes[i];
    }
    int x = (lw - w) / 2, y = lh * s_poll.y / 100;
    box(x, y, w, (s_poll.count + 1) * (LINE_H + 2) + 6, 0x0C0C14, a * 190 / 255);
    char row[64];
    int n;
    if (s_poll.countdown) {
        uint32_t left = s_poll.life - (now - s_poll.born);
        n = snprintf(row, sizeof(row), "%.22s %us", s_poll.title, (unsigned)((left + 999) / 1000));
    } else {
        n = snprintf(row, sizeof(row), "%.29s", s_poll.title);
    }
    text(x + 4, y + 3, 1, row, n < cols ? n : cols, 0xFFD25A, a, false);
    for (int i = 0; i < s_poll.count; ++i) {
        int ry = y + 3 + (i + 1) * (LINE_H + 2);
        bool lit = s_poll.winner == i || (s_poll.winner < 0 && best && s_poll.votes[i] == best);
        int bar = total ? s_poll.votes[i] * (w - 8) / total : 0;
        box(x + 4, ry - 1, bar, LINE_H, s_poll.winner == i ? 0x3C8C3C : 0x2A3A6A, a * 220 / 255);
        n = snprintf(row, sizeof(row), "%d %-20.20s %4d", i + 1, s_poll.option[i], s_poll.votes[i]);
        text(x + 4, ry, 1, row, n < cols ? n : cols, lit ? 0xFFFFFF : 0xB4B4C8, a, false);
    }
}

static bool anything(uint32_t now)
{
    if (s_banner.life && now - s_banner.born < s_banner.life) return true;
    if (s_poll.life && s_poll.count && now - s_poll.born < s_poll.life) return true;
    for (int i = 0; i < PAD_MAX; ++i)
        if (s_pad[i].on && s_pad[i].label[0]) return true;
    for (int i = 0; i < RAM_MAX; ++i)
        if (s_ram[i].on && s_ram[i].label[0]) return true;
    if (!s_cfg.show) return false;
    for (int i = 0; i < s_msg_count; ++i)
        if (now - s_msg[i].born < s_msg[i].life) return true;
    return false;
}

void cyc_chat_draw(SDL_Renderer *ren)
{
    uint32_t now = SDL_GetTicks();
    if (!anything(now)) return;
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    int zoom = s_cfg.scale ? s_cfg.scale : (oh + 180) / 360;
    if (zoom < 1) zoom = 1;
    int lw = ow / zoom, lh = oh / zoom;
    if (lw < 64 || lh < 64) return;
    if (!s_tex || lw != s_tex_w || lh != s_tex_h) {
        if (s_tex) SDL_DestroyTexture(s_tex);
        s_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, lw, lh);
        if (!s_tex) return;
        SDL_SetTextureBlendMode(s_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(s_tex, SDL_ScaleModeNearest);
        s_tex_w = lw;
        s_tex_h = lh;
    }
    size_t need = (size_t)lw * (size_t)lh;
    if (need > s_px_len) {
        free(s_px);
        s_px = (uint32_t *)malloc(need * 4);
        s_px_len = s_px ? need : 0;
        if (!s_px) return;
    }
    memset(s_px, 0, need * 4);
    if (s_cfg.show) draw_panel(now, lw, lh);
    draw_effects(lw);
    draw_poll(now, lw, lh);
    draw_banner(now, lw, lh);
    SDL_UpdateTexture(s_tex, NULL, s_px, lw * 4);

    int llw = 0, llh = 0;
    SDL_RenderGetLogicalSize(ren, &llw, &llh);
    SDL_bool integer = SDL_RenderGetIntegerScale(ren);
    SDL_RenderSetLogicalSize(ren, 0, 0);                  /* window pixels */
    SDL_RenderSetViewport(ren, NULL);
    SDL_RenderSetScale(ren, 1.0f, 1.0f);
    SDL_Rect dst = { 0, 0, lw * zoom, lh * zoom };
    SDL_RenderCopy(ren, s_tex, NULL, &dst);
    SDL_RenderSetLogicalSize(ren, llw, llh);              /* back to the picture's coordinates */
    SDL_RenderSetIntegerScale(ren, integer);
}
