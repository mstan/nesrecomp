/* Compatibility shim for generated sources produced before the reverse-debug
 * build was retired. New generated code does not include this header. */
#ifndef NESRECOMP_REVERSE_DEBUG_H
#define NESRECOMP_REVERSE_DEBUG_H

#include <stdint.h>

void nes_write(uint16_t addr, uint8_t val);

/* Force legacy generated instrumentation guards off, even when an old game
 * project still passes -DNESRECOMP_REVERSE_DEBUG=1. */
#ifdef NESRECOMP_REVERSE_DEBUG
#undef NESRECOMP_REVERSE_DEBUG
#endif
#define NESRECOMP_REVERSE_DEBUG 0

#define RDB_STORE8(pc_hint, addr, val) nes_write((uint16_t)(addr), (uint8_t)(val))
#define RDB_BLOCK_HOOK(pc)             ((void)0)
static inline void rdb_on_call(uint16_t pc_hint) { (void)pc_hint; }

#endif /* NESRECOMP_REVERSE_DEBUG_H */
