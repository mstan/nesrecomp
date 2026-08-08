/*
 * mod_savestate.c — registry for id-keyed per-mod save-state extensions. See
 * mod_savestate.h for the contract.
 *
 * Kept small and dependency-free like mod_function_hooks.c: it must link in
 * every configuration, including titles that never enable the mod package
 * runtime.
 */
#include "mod_savestate.h"

#include <string.h>

#define NES_MOD_MAX_SAVESTATE_HOOKS 16
#define NES_MOD_SAVESTATE_ID_CAP 64  /* 63 chars + NUL, matches the on-disk id field */

typedef struct {
    char                id[NES_MOD_SAVESTATE_ID_CAP];
    NESModSavestateGet  get;
    NESModSavestateSet  set;
} SavestateHook;

static SavestateHook s_hooks[NES_MOD_MAX_SAVESTATE_HOOKS];
static int           s_count = 0;

int nes_mod_register_savestate_hook(const char *id, NESModSavestateGet get,
                                    NESModSavestateSet set) {
    if (!id || !id[0] || !get || !set) return 0;
    if (strlen(id) >= NES_MOD_SAVESTATE_ID_CAP) return 0;

    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_hooks[i].id, id) != 0) continue;
        /* Re-registration updates in place rather than rejecting — a
         * reloaded mod does not need to track whether it already ran. */
        s_hooks[i].get = get;
        s_hooks[i].set = set;
        return 1;
    }
    if (s_count >= NES_MOD_MAX_SAVESTATE_HOOKS) return 0;

    strncpy(s_hooks[s_count].id, id, NES_MOD_SAVESTATE_ID_CAP - 1);
    s_hooks[s_count].id[NES_MOD_SAVESTATE_ID_CAP - 1] = '\0';
    s_hooks[s_count].get = get;
    s_hooks[s_count].set = set;
    s_count++;
    return 1;
}

int nes_mod_savestate_hook_count(void) { return s_count; }

const char *nes_mod_savestate_hook_id_at(int index) {
    if (index < 0 || index >= s_count) return NULL;
    return s_hooks[index].id;
}

NESModSavestateGet nes_mod_savestate_hook_get_at(int index) {
    if (index < 0 || index >= s_count) return NULL;
    return s_hooks[index].get;
}

NESModSavestateSet nes_mod_savestate_hook_find_set(const char *id) {
    if (!id) return NULL;
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_hooks[i].id, id) == 0) return s_hooks[i].set;
    return NULL;
}
