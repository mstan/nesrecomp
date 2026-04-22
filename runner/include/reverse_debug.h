/*
 * reverse_debug.h — Tier 1 / 1.5 / 2 / 2.5 / 3 reverse-debugger hooks.
 *
 * See nesrecomp/REVERSE_DEBUGGER.md for the full tier design.
 *
 * When NESRECOMP_REVERSE_DEBUG is 0, every macro in this header expands
 * to zero-cost / direct-call equivalents — byte-identical codegen.
 * When 1, the generator emits RDB_STORE8 / RDB_BLOCK_HOOK / rdb_on_call
 * hooks that feed ring buffers and park primitives, dumped via TCP.
 */
#ifndef NESRECOMP_REVERSE_DEBUG_H
#define NESRECOMP_REVERSE_DEBUG_H

#include <stdint.h>

#ifndef NESRECOMP_REVERSE_DEBUG
#define NESRECOMP_REVERSE_DEBUG 0
#endif

/* Always visible: nes_write is the underlying bus path. */
void nes_write(uint16_t addr, uint8_t val);

#if NESRECOMP_REVERSE_DEBUG

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Tier 1 ---- Synchronous bus-write hook. */
extern uint16_t g_rdb_current_func;
void rdb_store8(uint16_t pc_hint, uint16_t addr, uint8_t val);

#define RDB_STORE8(pc_hint, addr, val) rdb_store8((uint16_t)(pc_hint), (uint16_t)(addr), (uint8_t)(val))

/* ---- Tier 1.5 ---- Per-call ring. Hook at every function prologue. */
void rdb_on_call(uint16_t func_pc);

/* ---- Tier 2 ---- Per-block hook. Generator emits one of these per
 * basic-block entry (function prologue + every label_XXXX:;). Registers
 * are read directly from g_cpu at the moment of entry. */
void rdb_on_block(uint16_t pc);

#define RDB_BLOCK_HOOK(pc) rdb_on_block((uint16_t)(pc))

/* ---- Tier 2.5 ---- Park primitives shared by block breakpoints and
 * WRAM watchpoints. Spins calling debug_server_poll() until unparked. */
extern volatile int g_rdb_paused;
void rdb_wait_if_parked(void);

/* ---- TCP ---- Dispatcher. Returns 1 if handled. */
int rdb_handle_cmd(const char *cmd, int id, const char *json);

/* Startup — idempotent. */
void rdb_init(void);

#ifdef __cplusplus
}
#endif

#else  /* NESRECOMP_REVERSE_DEBUG == 0 */

#define RDB_STORE8(pc_hint, addr, val) nes_write((uint16_t)(addr), (uint8_t)(val))
#define RDB_BLOCK_HOOK(pc)             ((void)0)
static inline void rdb_on_call(uint16_t pc_hint) { (void)pc_hint; }

#endif /* NESRECOMP_REVERSE_DEBUG */

#endif /* NESRECOMP_REVERSE_DEBUG_H */
