/*
 * nes_session_config.h -- the session configuration seal
 * (recomp-ai-rules/NETPLAY.md §4: settle session-wide requirements up front;
 * session settlement is ephemeral).
 *
 * A game (or the engine) registers every simulation-affecting setting a
 * netplay match may vary as a key. The canonical text is "nes-session/1\n"
 * followed by "key=value\n" in registration order. A netplay launch APPLIES
 * the host's text on every peer before boot (never persisting it), the
 * rollback driver's mod-set handshake confirms every peer runs the same text,
 * and the offline values come back when the session ends. Everything not
 * registered here is not allowed to differ: mods are cleared for a match.
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef int  (*NesSessionGetFn)(char *out, int cap);       /* current value */
typedef int  (*NesSessionApplyFn)(const char *value);      /* 1 = applied */
typedef void (*NesSessionRestoreFn)(void);                 /* offline value */
int  nes_netplay_session_register(const char *key, NesSessionGetFn get,
                                  NesSessionApplyFn apply,
                                  NesSessionRestoreFn restore);
/* What the HOST proposes for a key (e.g. read from the offline mod
 * selection, since a match itself commits no mods); default = get. */
int  nes_netplay_session_set_offer(const char *key, NesSessionGetFn offer);
/* Game rule run after a whole text is applied (e.g. co-op excludes
 * widescreen). Deterministic: a function of the applied values only. */
typedef void (*NesSessionFinalizeFn)(void);
void nes_netplay_session_set_finalize(NesSessionFinalizeFn fn);
/* The offer text (the host's proposal; what a lobby publishes). */
int  nes_netplay_session_describe_offer(char *out, int cap);
/* Canonical text of the CURRENT settings. Returns its length. */
int  nes_netplay_session_describe(char *out, int cap);
/* Apply a host text; 0 with *why on an unknown key, a missing key or a
 * refused value. */
int  nes_netplay_session_apply(const char *text, char *why, int why_cap);
void nes_netplay_session_restore(void);
/* ';'-separated wire form <-> canonical text. */
void nes_netplay_session_to_wire(const char *text, char *out, int cap);
void nes_netplay_session_from_wire(const char *wire, char *out, int cap);

#ifdef __cplusplus
}
#endif
