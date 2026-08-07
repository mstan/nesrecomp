/*
 * mod_function_hooks.c — registry for trusted mod callbacks at recompiled
 * 6502 function entries. See mod_function_hooks.h for the contract.
 *
 * Kept deliberately small and dependency-free: generated code calls into it,
 * so it must link in every configuration, including titles that never enable
 * the mod package runtime.
 */
#include "mod_function_hooks.h"

#include <string.h>

#define NES_MOD_MAX_FUNCTION_HOOKS 32

typedef struct {
    const char                 *id;
    uint16_t                    addr;
    int                         enabled;
    NESModFunctionEntryCallback callback;
} FunctionHook;

static FunctionHook s_hooks[NES_MOD_MAX_FUNCTION_HOOKS];
static int          s_count = 0;

/* Number of ENABLED hooks. The generated-code path checks this first, so a
 * build with hooks registered but none active pays one load and one branch. */
static int      s_active = 0;
static uint64_t s_hits = 0;

int nes_mod_register_function_entry_plugin(const char *id, uint16_t addr,
                                           NESModFunctionEntryCallback cb) {
    if (!id || !id[0] || !cb) return 0;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_hooks[i].id, id) != 0) continue;
        /* Idempotent re-registration is fine; a conflicting claim is not. */
        return (s_hooks[i].addr == addr && s_hooks[i].callback == cb);
    }
    if (s_count >= NES_MOD_MAX_FUNCTION_HOOKS) return 0;
    s_hooks[s_count].id       = id;
    s_hooks[s_count].addr     = addr;
    s_hooks[s_count].enabled  = 0;   /* registering must never change behavior */
    s_hooks[s_count].callback = cb;
    s_count++;
    return 1;
}

int nes_mod_set_function_hook_enabled(const char *id, int enabled) {
    if (!id) return 0;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_hooks[i].id, id) != 0) continue;
        const int want = enabled ? 1 : 0;
        if (s_hooks[i].enabled != want) {
            s_hooks[i].enabled = want;
            s_active += want ? 1 : -1;
        }
        return 1;
    }
    return 0;
}

void nes_mod_disable_all_function_hooks(void) {
    for (int i = 0; i < s_count; i++) s_hooks[i].enabled = 0;
    s_active = 0;
}

int nes_mod_function_hook_enabled(const char *id) {
    if (!id) return 0;
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_hooks[i].id, id) == 0) return s_hooks[i].enabled;
    return 0;
}

int nes_mod_function_entry(uint16_t addr) {
    if (s_active == 0) return 0;
    for (int i = 0; i < s_count; i++) {
        if (!s_hooks[i].enabled || s_hooks[i].addr != addr) continue;
        if (s_hooks[i].callback(addr)) { s_hits++; return 1; }
        /* A hook that declines this call falls through to the original body
         * rather than blocking later hooks on the same address; the first one
         * that handles it wins. */
    }
    return 0;
}

uint64_t nes_mod_function_hook_hits(void) { return s_hits; }
