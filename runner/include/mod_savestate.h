/*
 * mod_savestate.h — id-keyed per-mod save-state extension registry.
 *
 * savestate.c owns a fixed-layout struct covering CPU/RAM/PPU/mapper/APU
 * state. A mod that carries its own architectural state (e.g. a replacement
 * player controller with extra fields the guest RAM does not model) has
 * nowhere to put it without savestate.c knowing about that specific mod.
 *
 * This registry lets a mod claim a stable string id and a get/set pair.
 * savestate.c iterates registered hooks on save and looks hooks up by id on
 * load, without needing to know what any of them mean.
 *
 * Mirrors mod_function_hooks.h: always compiled, dependency-free, so linking
 * a title never depends on whether it opted into the mod runtime.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum payload size a hook may hand to/receive from savestate.c. Matches
 * the on-disk record's blob capacity (see savestate.c). */
#define NES_MOD_SAVESTATE_BLOB_CAP 512

/*
 * Serialize this mod's state into `buf` (capacity `cap`, always
 * NES_MOD_SAVESTATE_BLOB_CAP). Returns bytes written, or -1 if the state does
 * not fit in `cap`. Called during save; a -1 return excludes this mod's
 * record from the file rather than failing the whole save.
 */
typedef int (*NESModSavestateGet)(uint8_t *buf, int cap);

/*
 * Restore this mod's state from `buf` (`len` bytes, as produced by the get
 * callback that wrote this record). Called during load, after NES RAM/CPU/
 * PPU state has already been restored. Returns 1 on success.
 */
typedef int (*NESModSavestateSet)(const uint8_t *buf, int len);

/*
 * Register a mod's savestate get/set pair under a stable id. Call before
 * main() alongside the other plugin registrations. Re-registering the same
 * id updates the callbacks in place (a mod reloaded during development does
 * not need to track whether it already registered).
 *
 * Returns 1 on success; 0 if the id is empty, longer than 63 characters, the
 * table is full, or either callback is NULL.
 */
int nes_mod_register_savestate_hook(const char *id, NESModSavestateGet get,
                                    NESModSavestateSet set);

/* Number of registered hooks, for savestate.c to iterate on save. */
int nes_mod_savestate_hook_count(void);

/* Index-based accessors for iterating on save. `index` must be in
 * [0, nes_mod_savestate_hook_count()). Returns the hook's id and get
 * callback; never NULL for a valid index. */
const char *nes_mod_savestate_hook_id_at(int index);
NESModSavestateGet nes_mod_savestate_hook_get_at(int index);

/* Look up a registered hook's set callback by id, for restoring one loaded
 * record. Returns NULL if no hook is registered under `id` — the caller
 * should skip the record with a warning, not fail the load. */
NESModSavestateSet nes_mod_savestate_hook_find_set(const char *id);

#ifdef __cplusplus
}
#endif
