/*
 * nes_session_config.c -- the netplay session-configuration seal registry.
 * See include/nes_session_config.h. Always compiled (a game registers its
 * keys whether or not the netplay stack is linked); inert until a netplay
 * launch applies a host's image.
 */
#include "nes_session_config.h"

#include <stdio.h>
#include <string.h>

/* ── session configuration seal ───────────────────────────────────────── */

#define NES_SESSION_MAX_KEYS 12
#define NES_SESSION_HEADER "nes-session/1\n"

typedef struct {
    char key[32];
    NesSessionGetFn offer;
    NesSessionGetFn get;
    NesSessionApplyFn apply;
    NesSessionRestoreFn restore;
    int applied;
} SessionKey;

static SessionKey s_keys[NES_SESSION_MAX_KEYS];
static int s_key_count;
static NesSessionFinalizeFn s_finalize;

void nes_netplay_session_set_finalize(NesSessionFinalizeFn fn) { s_finalize = fn; }

int nes_netplay_session_set_offer(const char *key, NesSessionGetFn offer)
{
    int i;
    for (i = 0; i < s_key_count; ++i)
        if (!strcmp(s_keys[i].key, key)) { s_keys[i].offer = offer; return 1; }
    return 0;
}

int nes_netplay_session_describe_offer(char *out, int cap)
{
    int n, i;
    if (!out || cap <= 0) return 0;
    n = snprintf(out, (size_t)cap, "%s", NES_SESSION_HEADER);
    for (i = 0; i < s_key_count && n < cap; ++i) {
        char v[96] = "";
        (s_keys[i].offer ? s_keys[i].offer : s_keys[i].get)(v, (int)sizeof(v));
        n += snprintf(out + n, (size_t)(cap - n), "%s=%s\n", s_keys[i].key, v);
    }
    return n;
}

int nes_netplay_session_register(const char *key, NesSessionGetFn get,
                                 NesSessionApplyFn apply,
                                 NesSessionRestoreFn restore)
{
    int i;
    if (!key || !key[0] || strlen(key) >= sizeof(s_keys[0].key) || !get || !apply)
        return 0;
    for (i = 0; i < s_key_count; ++i)
        if (!strcmp(s_keys[i].key, key)) {
            s_keys[i].get = get;
            s_keys[i].apply = apply;
            s_keys[i].restore = restore;
            return 1;
        }
    if (s_key_count >= NES_SESSION_MAX_KEYS)
        return 0;
    snprintf(s_keys[s_key_count].key, sizeof(s_keys[0].key), "%s", key);
    s_keys[s_key_count].get = get;
    s_keys[s_key_count].apply = apply;
    s_keys[s_key_count].restore = restore;
    s_key_count++;
    return 1;
}

int nes_netplay_session_describe(char *out, int cap)
{
    int n, i;
    if (!out || cap <= 0) return 0;
    n = snprintf(out, (size_t)cap, "%s", NES_SESSION_HEADER);
    for (i = 0; i < s_key_count && n < cap; ++i) {
        char v[96] = "";
        s_keys[i].get(v, (int)sizeof(v));
        n += snprintf(out + n, (size_t)(cap - n), "%s=%s\n", s_keys[i].key, v);
    }
    return n;
}

int nes_netplay_session_apply(const char *text, char *why, int why_cap)
{
    const char *p;
    int i;
    if (why && why_cap) why[0] = 0;
    if (!text || strncmp(text, NES_SESSION_HEADER, strlen(NES_SESSION_HEADER)) != 0) {
        if (why) snprintf(why, (size_t)why_cap, "not a nes-session/1 configuration");
        return 0;
    }
    p = text + strlen(NES_SESSION_HEADER);
    /* Every key this build knows must be present -- a host that did not
     * settle a setting leaves each peer on its own value, which is exactly
     * the silent difference the seal exists to rule out. */
    for (i = 0; i < s_key_count; ++i) s_keys[i].applied = 0;
    while (*p) {
        char line[160], *eq;
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (nl ? 1 : 0);
        if (!line[0]) continue;
        eq = strchr(line, '=');
        if (!eq) {
            if (why) snprintf(why, (size_t)why_cap, "malformed line '%s'", line);
            return 0;
        }
        *eq = 0;
        for (i = 0; i < s_key_count; ++i)
            if (!strcmp(s_keys[i].key, line)) break;
        if (i == s_key_count) {
            if (why) snprintf(why, (size_t)why_cap, "unknown session key '%s'", line);
            return 0;
        }
        if (!s_keys[i].apply(eq + 1)) {
            if (why) snprintf(why, (size_t)why_cap, "cannot apply %s=%s", line, eq + 1);
            return 0;
        }
        s_keys[i].applied = 1;
    }
    for (i = 0; i < s_key_count; ++i)
        if (!s_keys[i].applied) {
            if (why) snprintf(why, (size_t)why_cap, "host did not settle '%s'", s_keys[i].key);
            return 0;
        }
    /* Cross-key rules (a game's exclusions) run once every key is in, so the
     * result is a function of the text alone, identical on every peer. */
    if (s_finalize) s_finalize();
    return 1;
}

void nes_netplay_session_restore(void)
{
    int i;
    for (i = 0; i < s_key_count; ++i)
        if (s_keys[i].restore) s_keys[i].restore();
}

void nes_netplay_session_to_wire(const char *text, char *out, int cap)
{
    int i = 0;
    if (!out || cap <= 0) return;
    for (; text && *text && i + 1 < cap; ++text)
        out[i++] = (*text == '\n') ? ';' : *text;
    out[i] = 0;
}

void nes_netplay_session_from_wire(const char *wire, char *out, int cap)
{
    int i = 0;
    if (!out || cap <= 0) return;
    for (; wire && *wire && i + 1 < cap; ++wire)
        out[i++] = (*wire == ';') ? '\n' : *wire;
    out[i] = 0;
}

