/*
 * mod_function_hooks.h — trusted mod callbacks at recompiled 6502 function
 * entries.
 *
 * A launcher-toggleable mod needs to take over a specific guest subroutine
 * while it is enabled, and leave it completely alone while it is not.
 * `replace_func` in game.toml cannot express that: it is a compile-time
 * substitution that makes codegen drop the original body entirely, so there
 * is nothing left to fall back to.
 *
 * This is the runtime half of the alternative. A game lists entries in
 * game.toml:
 *
 *     [[mod_function_hook]]
 *     addr = 0xB0E9        # PlayerCtrlRoutine
 *
 * and codegen emits, as the first statement of that entry:
 *
 *     if (nes_mod_function_entry(0xB0E9u)) return;
 *
 * Nonzero means a mod handled the call and the original body is skipped;
 * zero — the default, with nothing registered — runs it unchanged. Because
 * the check precedes the stack-tracking push, a handling mod owns the whole
 * call including its frame, and needs no push/pop bookkeeping.
 *
 * Archives never supply native code: they select behavior by stable plugin
 * id, exactly as activation plugins do. This is deliberately NOT a general
 * per-instruction mod dispatcher.
 *
 * This module is always compiled, so generated code links whether or not the
 * game opted into the mod package runtime. With nothing registered the check
 * is a single predictable branch.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns nonzero to skip the original function body.
 * `addr` is the 6502 entry address, so one callback may serve several hooks.
 */
typedef int (*NESModFunctionEntryCallback)(uint16_t addr);

/*
 * Register a trusted implementation for one 6502 entry address. Call before
 * main() alongside the other plugin registrations. Hooks start DISABLED, so
 * registering one cannot change behavior until a mod activates it.
 *
 * Returns 1 on success; 0 if the id is empty, the table is full, or the id is
 * already registered against a different address or callback.
 */
int nes_mod_register_function_entry_plugin(const char *id, uint16_t addr,
                                           NESModFunctionEntryCallback cb);

/*
 * Enable or disable a registered hook by id. Activation plugins enable; reset
 * callbacks disable. Returns 1 when the id resolved.
 */
int nes_mod_set_function_hook_enabled(const char *id, int enabled);

/* Disable every registered hook. Cheap way for a reset callback to guarantee
 * stock behavior without naming each id. */
void nes_mod_disable_all_function_hooks(void);

/* 1 if the id is registered and currently enabled. */
int nes_mod_function_hook_enabled(const char *id);

/*
 * Called from generated code at an opted-in function entry. Returns nonzero
 * if an enabled hook handled the call and the original body must be skipped.
 */
int nes_mod_function_entry(uint16_t addr);

/* Number of times nes_mod_function_entry has reported "handled", for smoke
 * tests and for telling "the hook never fired" apart from "the hook fired and
 * did nothing". */
uint64_t nes_mod_function_hook_hits(void);

#ifdef __cplusplus
}
#endif
