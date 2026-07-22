/*
 * interp.h — 6502 interpreter fallback tier.
 *
 * When call_by_address has no recompiled function for a guest PC (a dynamic
 * dispatch the static finder missed, or code executed from RAM/SRAM), the
 * generated dispatcher routes the miss here instead of silently terminating
 * the control-flow path. The interpreter runs the missed code against the
 * shared CPU/memory state and hands control back to recompiled code at the
 * proper boundary, so the game keeps running and EVERY miss in a session is
 * surfaced in one run (see docs/PHASE1_INTERP_FALLBACK_PLAN.md).
 *
 * Semantics mirror recompiler/src/code_generator.c exactly (same flag macros,
 * addressing, bus helpers) and the decode table is shared
 * (recompiler/src/cpu6502_decoder.c), so interpreted and recompiled execution
 * agree by construction.
 *
 * Precondition: the boundary contract requires the 6502 RAM stack to mirror
 * the C call stack, i.e. the game must be built with push_all_jsr. When that
 * flag is off the interpreter self-disables and the miss falls back to the
 * legacy policy (LOG_RETURN / FATAL / TRAP).
 */
#pragma once
#include <stdint.h>

/* Entry point invoked from the generated call_by_address miss paths.
 * Returns 1 if the miss was handled (interpreted) and the game should
 * continue; 0 if not handled (interpreter disabled / unsupported), in which
 * case the configured dispatch-miss policy has been applied and the caller
 * behaves as the legacy `return 0`. */
int nes_interp_dispatch(uint16_t addr);

/* Execute an intentional RAM/SRAM vector entry, such as an IRQ vector below
 * $8000. This is not a static-discovery miss and does not log dispatch_misses. */
int nes_interp_interrupt(uint16_t addr);

/* Execute from a known guest PC without recording it as a dispatch miss.
 * Used for explicit continuation/resume paths where the caller already has
 * a PC from the guest stack rather than a missing generated entry. */
int nes_interp_resume(uint16_t addr);

/* Runtime control. enabled defaults on when push_all_jsr is set, unless the
 * env var NESRECOMP_INTERP_FALLBACK=off overrides. */
void nes_interp_set_enabled(int enabled);
int  nes_interp_is_enabled(void);

/* ---- Stats (measurement hook for the JIT decision + diagnostics) ---- */
typedef struct {
    uint64_t instrs_total;      /* interpreted instructions since process start */
    uint64_t runs;              /* top-level interp_run invocations */
    uint64_t watchdog_trips;    /* runs that hit the per-run instruction cap */
    uint64_t native_handoffs;   /* JSR/JMP handed off to recompiled code */
    uint32_t instrs_this_frame; /* interpreted instructions in the current frame */
    uint32_t max_instrs_run;    /* largest single interp_run instruction count */
} NesInterpStats;

void nes_interp_get_stats(NesInterpStats *out);

/* Called once per rendered frame (from the runner) to roll the per-frame
 * instruction counter. Safe to call even if the interpreter never fired. */
void nes_interp_frame_boundary(void);
