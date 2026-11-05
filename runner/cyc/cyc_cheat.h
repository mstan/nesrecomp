/*
 * cyc_cheat.h - scheduled RAM pokes and holds for QA routes (dev surface).
 *
 * A QA route (a headless `--input` schedule, a window `--window-input`
 * schedule, or TCP) can write CPU RAM / cartridge RAM between frames:
 *
 *   <frame> POKE     <addr>[-<last>] <byte>   one write, before that frame runs
 *   <frame> HOLD     <addr>[-<last>] <byte>   write before every frame from then on
 *   <frame> HOLD_MAX <addr>[-<last>] <byte>   clamp: if RAM > byte, write byte (each frame)
 *   <frame> HOLD_MIN <addr>[-<last>] <byte>   clamp: if RAM < byte, write byte (each frame)
 *   <frame> UNHOLD   <addr>[-<last>]          drop holds covering those addresses
 *   <frame> UNHOLD   ALL                      drop every hold
 *
 * POKE and the HOLD forms take an optional condition, checked each time they
 * would write:  ... IF <addr> <op> <byte>   with op one of == != < <= > >=,
 * & (any of the bits set) or !& (none set). E.g. a one-hit boss once its
 * health bar has filled: HOLD_MAX 06C1 01 IF 00B1 >= 02.
 *
 * Numbers are hex (an optional `$` or `0x` prefix is accepted). Writes go
 * through cyc_mod_poke, so compiled RAM views see them as stores. Every write
 * that changes a byte leaves a CHEAT event in the always-on ring (addr = the
 * address, value = mode << 16 | old << 8 | new), so a route's assistance is
 * visible in the same history as the game's own events.
 *
 * The guest's instructions are untouched: cheats change data only, between
 * frames, identically for native and --interp-only runs, so native/interpreter
 * parity stays a valid check on an assisted route. They are refused during
 * online play (the rollback session owns the machine) and are not part of
 * save states (a route that loads a state re-applies the holds it schedules;
 * cyc_cheat_seek replays hold changes scheduled before the loaded frame and
 * skips its already-applied pokes).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CYC_CHEAT_POKE,
    CYC_CHEAT_HOLD,
    CYC_CHEAT_HOLD_MAX,
    CYC_CHEAT_HOLD_MIN,
    CYC_CHEAT_UNHOLD,
    CYC_CHEAT_UNHOLD_ALL,
} CycCheatMode;

typedef enum {
    CYC_CHEAT_IF_EQ, CYC_CHEAT_IF_NE, CYC_CHEAT_IF_LT, CYC_CHEAT_IF_LE,
    CYC_CHEAT_IF_GT, CYC_CHEAT_IF_GE, CYC_CHEAT_IF_ANY, CYC_CHEAT_IF_NONE,
} CycCheatOp;

typedef struct {
    CycCheatMode mode;
    uint16_t first, last;     /* inclusive address range */
    uint8_t value;
    bool has_cond;
    CycCheatOp cond_op;
    uint16_t cond_addr;
    uint8_t cond_value;
} CycCheat;

#define CYC_CHEAT_MAX_HOLDS 256

/* True when `text` (after the frame number) starts with a cheat keyword.
 * Inline so a host without the module (the TriCNES oracle) can refuse them. */
static inline bool cyc_cheat_is_command(const char *text)
{
    static const char *const WORDS[] = { "POKE", "HOLD", "HOLD_MAX", "HOLD_MIN", "UNHOLD" };
    while (*text == ' ' || *text == '\t') text++;
    size_t n = 0;
    while ((text[n] >= 'A' && text[n] <= 'Z') || (text[n] >= 'a' && text[n] <= 'z') || text[n] == '_') n++;
    for (size_t w = 0; w < sizeof(WORDS) / sizeof(WORDS[0]); w++) {
        size_t k = 0;
        while (WORDS[w][k] && k < n && (char)(text[k] & ~0x20) == WORDS[w][k]) k++;  /* '_' has no case bit */
        if (k == n && !WORDS[w][k]) return true;
    }
    return false;
}
/* Parse "MODE addr[-last] [byte] [IF addr op byte]"; false with a message in err. */
bool cyc_cheat_parse(const char *text, CycCheat *out, char *err, size_t err_len);
/* Queue a parsed command for the start of `frame` (frame order kept). */
bool cyc_cheat_schedule(long frame, const CycCheat *cheat);
/* Apply now (TCP): pokes write immediately; holds take effect from the next frame. */
bool cyc_cheat_apply_now(const CycCheat *cheat);
/* A state was loaded at `frame`: adopt the hold changes scheduled before it. */
void cyc_cheat_seek(long frame);
/* Before running `frame`: due scheduled commands, then every active hold. */
void cyc_cheat_frame(long frame);
/* Active holds (for status lines) and a reset. */
unsigned cyc_cheat_hold_count(void);
void cyc_cheat_describe(char *out, size_t n);
void cyc_cheat_reset(void);

#ifdef __cplusplus
}
#endif
