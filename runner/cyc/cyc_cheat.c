/* cyc_cheat.c - scheduled RAM pokes and holds for QA routes (cyc_cheat.h). */
#include "cyc_cheat.h"
#include "cyc_mod.h"
#include "cyc_ring.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    long frame;
    CycCheat cheat;
} CheatEvent;

static CheatEvent *s_events;
static size_t s_event_count, s_event_cap, s_event_next;
static CycCheat s_holds[CYC_CHEAT_MAX_HOLDS];
static unsigned s_hold_count;

static const struct { const char *name; CycCheatMode mode; } KEYWORDS[] = {
    { "POKE", CYC_CHEAT_POKE },         { "HOLD_MAX", CYC_CHEAT_HOLD_MAX },
    { "HOLD_MIN", CYC_CHEAT_HOLD_MIN }, { "HOLD", CYC_CHEAT_HOLD },
    { "UNHOLD", CYC_CHEAT_UNHOLD },
};

static const struct { const char *text; CycCheatOp op; } OPS[] = {
    { "==", CYC_CHEAT_IF_EQ }, { "!=", CYC_CHEAT_IF_NE }, { "<=", CYC_CHEAT_IF_LE },
    { ">=", CYC_CHEAT_IF_GE }, { "<", CYC_CHEAT_IF_LT },  { ">", CYC_CHEAT_IF_GT },
    { "&", CYC_CHEAT_IF_ANY }, { "!&", CYC_CHEAT_IF_NONE },
};

static size_t word_len(const char *s)
{
    size_t n = 0;
    while (s[n] && (isalnum((unsigned char)s[n]) || s[n] == '_')) n++;
    return n;
}

static bool word_is(const char *s, size_t n, const char *name)
{
    if (strlen(name) != n) return false;
    for (size_t k = 0; k < n; k++)
        if (toupper((unsigned char)s[k]) != name[k]) return false;
    return true;
}

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static bool parse_hex(const char **ps, unsigned long limit, unsigned long *out)
{
    const char *s = *ps;
    if (*s == '$') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (!isxdigit((unsigned char)*s)) return false;
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s || v > limit) return false;
    *ps = end;
    *out = v;
    return true;
}

static bool at_end(const char *s)
{
    s = skip_ws(s);
    return !*s || *s == '#' || *s == '\r' || *s == '\n';
}

bool cyc_cheat_parse(const char *text, CycCheat *c, char *err, size_t err_len)
{
    memset(c, 0, sizeof *c);
    const char *s = skip_ws(text);
    size_t n = word_len(s);
    bool known = false;
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]) && !known; i++)
        if (word_is(s, n, KEYWORDS[i].name)) c->mode = KEYWORDS[i].mode, known = true;
    if (!known) { snprintf(err, err_len, "unknown cheat command"); return false; }
    s = skip_ws(s + n);
    if (c->mode == CYC_CHEAT_UNHOLD && word_is(s, word_len(s), "ALL")) {
        c->mode = CYC_CHEAT_UNHOLD_ALL;
        c->last = 0xFFFF;
        if (!at_end(s + 3)) { snprintf(err, err_len, "cheat: trailing text"); return false; }
        return true;
    }
    unsigned long a, b, v;
    if (!parse_hex(&s, 0xFFFF, &a)) { snprintf(err, err_len, "cheat: bad address"); return false; }
    b = a;
    if (*s == '-') {
        s++;
        if (!parse_hex(&s, 0xFFFF, &b) || b < a) { snprintf(err, err_len, "cheat: bad address range"); return false; }
    }
    /* Only memory cyc_mod_poke can store to: CPU RAM and cartridge/PRG RAM. */
    if (!((b < 0x2000) || (a >= 0x6000 && b < 0xE000))) {
        snprintf(err, err_len, "cheat: $%04lX-$%04lX is not RAM ($0000-$1FFF, $6000-$DFFF)", a, b);
        return false;
    }
    c->first = (uint16_t)a;
    c->last = (uint16_t)b;
    if (c->mode == CYC_CHEAT_UNHOLD) {
        if (!at_end(s)) { snprintf(err, err_len, "cheat: trailing text"); return false; }
        return true;
    }
    s = skip_ws(s);
    if (!parse_hex(&s, 0xFF, &v)) { snprintf(err, err_len, "cheat: bad byte value"); return false; }
    c->value = (uint8_t)v;
    s = skip_ws(s);
    if (word_is(s, word_len(s), "IF")) {
        s = skip_ws(s + 2);
        unsigned long ca, cv;
        if (!parse_hex(&s, 0xFFFF, &ca)) { snprintf(err, err_len, "cheat: bad IF address"); return false; }
        s = skip_ws(s);
        bool op = false;
        for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]) && !op; i++) {
            size_t len = strlen(OPS[i].text);
            if (!strncmp(s, OPS[i].text, len)) c->cond_op = OPS[i].op, s += len, op = true;
        }
        if (!op) { snprintf(err, err_len, "cheat: IF needs == != < <= > >= & !&"); return false; }
        s = skip_ws(s);
        if (!parse_hex(&s, 0xFF, &cv)) { snprintf(err, err_len, "cheat: bad IF byte"); return false; }
        c->has_cond = true;
        c->cond_addr = (uint16_t)ca;
        c->cond_value = (uint8_t)cv;
    }
    if (!at_end(s)) { snprintf(err, err_len, "cheat: trailing text"); return false; }
    return true;
}

bool cyc_cheat_schedule(long frame, const CycCheat *c)
{
    if (s_event_count == s_event_cap) {
        size_t cap = s_event_cap ? s_event_cap * 2 : 64;
        CheatEvent *grown = (CheatEvent *)realloc(s_events, cap * sizeof(*grown));
        if (!grown) return false;
        s_events = grown;
        s_event_cap = cap;
    }
    /* Frame order; commands of one frame keep their file order. */
    size_t i = s_event_count++;
    while (i > s_event_next && s_events[i - 1].frame > frame) { s_events[i] = s_events[i - 1]; --i; }
    s_events[i] = (CheatEvent){ frame, *c };
    return true;
}

static bool condition(const CycCheat *c)
{
    if (!c->has_cond) return true;
    uint8_t v = 0;
    if (!cyc_mod_peek_ok(c->cond_addr, &v)) return false;
    switch (c->cond_op) {
    case CYC_CHEAT_IF_EQ: return v == c->cond_value;
    case CYC_CHEAT_IF_NE: return v != c->cond_value;
    case CYC_CHEAT_IF_LT: return v < c->cond_value;
    case CYC_CHEAT_IF_LE: return v <= c->cond_value;
    case CYC_CHEAT_IF_GT: return v > c->cond_value;
    case CYC_CHEAT_IF_GE: return v >= c->cond_value;
    case CYC_CHEAT_IF_ANY: return (v & c->cond_value) != 0;
    case CYC_CHEAT_IF_NONE: return (v & c->cond_value) == 0;
    }
    return false;
}

/* The diagnostic replay (cyc_replay.h) records every byte a cheat changed. */
void (*cyc_cheat_change_hook)(uint16_t addr, uint8_t value);

static void write_byte(CycCheatMode mode, uint16_t addr, uint8_t value)
{
    uint8_t old = 0;
    if (!cyc_mod_peek_ok(addr, &old)) return;
    if (mode == CYC_CHEAT_HOLD_MAX && old <= value) return;
    if (mode == CYC_CHEAT_HOLD_MIN && old >= value) return;
    if (old == value) return;
    if (cyc_mod_poke(addr, value)) {
        cyc_ring_push(CYC_EV_CHEAT, addr, (uint32_t)mode << 16 | (uint32_t)old << 8 | value);
        if (cyc_cheat_change_hook) cyc_cheat_change_hook(addr, value);
    }
}

static void apply(const CycCheat *c)
{
    if (!condition(c)) return;
    for (uint32_t a = c->first; a <= c->last; a++) write_byte(c->mode, (uint16_t)a, c->value);
}

static void unhold(uint16_t first, uint16_t last)
{
    CycCheat kept[CYC_CHEAT_MAX_HOLDS];
    unsigned k = 0;
    for (unsigned i = 0; i < s_hold_count; i++) {
        const CycCheat *h = &s_holds[i];
        if (h->last < first || h->first > last) {
            kept[k++] = *h;
            continue;
        }
        /* Partial overlaps keep the uncovered ends. */
        if (h->first < first && k < CYC_CHEAT_MAX_HOLDS) {
            kept[k] = *h;
            kept[k++].last = (uint16_t)(first - 1);
        }
        if (h->last > last && k < CYC_CHEAT_MAX_HOLDS) {
            kept[k] = *h;
            kept[k++].first = (uint16_t)(last + 1);
        }
    }
    memcpy(s_holds, kept, k * sizeof(kept[0]));
    s_hold_count = k;
}

/* Update the hold table; true for a poke (the caller writes it). */
static bool adopt(const CycCheat *c)
{
    switch (c->mode) {
    case CYC_CHEAT_POKE: return true;
    case CYC_CHEAT_UNHOLD_ALL: s_hold_count = 0; return false;
    case CYC_CHEAT_UNHOLD: unhold(c->first, c->last); return false;
    default:
        unhold(c->first, c->last);   /* a new hold replaces what covered those bytes */
        if (s_hold_count < CYC_CHEAT_MAX_HOLDS) s_holds[s_hold_count++] = *c;
        else fprintf(stderr, "cheat: more than %d holds; $%04X ignored\n", CYC_CHEAT_MAX_HOLDS, c->first);
        return false;
    }
}

bool cyc_cheat_apply_now(const CycCheat *c)
{
    if (adopt(c)) apply(c);
    return true;
}

void cyc_cheat_seek(long frame)
{
    while (s_event_next < s_event_count && s_events[s_event_next].frame < frame)
        (void)adopt(&s_events[s_event_next++].cheat);
}

void cyc_cheat_frame(long frame)
{
    while (s_event_next < s_event_count && s_events[s_event_next].frame <= frame) {
        const CycCheat *c = &s_events[s_event_next++].cheat;
        if (adopt(c)) apply(c);
    }
    for (unsigned i = 0; i < s_hold_count; i++) apply(&s_holds[i]);
}

unsigned cyc_cheat_hold_count(void) { return s_hold_count; }

void cyc_cheat_describe(char *out, size_t n)
{
    static const char *const NAMES[] = { "poke", "hold", "hold_max", "hold_min" };
    static const char *const OPN[] = { "==", "!=", "<", "<=", ">", ">=", "&", "!&" };
    size_t at = 0;
    if (n) out[0] = 0;
    for (unsigned i = 0; i < s_hold_count && at + 48 < n; i++) {
        const CycCheat *h = &s_holds[i];
        at += (size_t)snprintf(out + at, n - at, "%s%s %04X-%04X %02X", i ? ", " : "",
                               NAMES[h->mode < 4 ? h->mode : 0], h->first, h->last, h->value);
        if (h->has_cond && at + 24 < n)
            at += (size_t)snprintf(out + at, n - at, " if %04X %s %02X", h->cond_addr,
                                   OPN[h->cond_op < 8 ? h->cond_op : 0], h->cond_value);
    }
}

void cyc_cheat_reset(void)
{
    free(s_events);
    s_events = NULL;
    s_event_count = s_event_cap = s_event_next = 0;
    s_hold_count = 0;
}
