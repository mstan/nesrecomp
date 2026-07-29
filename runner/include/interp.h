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

/* Resume an explicit stack continuation without treating initial stack lifts
 * as a return boundary. */
int nes_interp_resume(uint16_t addr);
/* Clear interpreter-only native bookkeeping after a non-local save-state
 * resume discarded the old C stack. */
void nes_interp_reset_context(void);

/*
 * Native collaboration policy while an interpreter island owns execution.
 *
 * ISLAND is the correctness-first default: nested JSR/JMP targets remain in
 * the interpreter until the island reaches an architectural exit. SAFE allows
 * balanced JSR calls to bounce into native code but keeps tail transfers in
 * the island. LEGACY allows both calls and tails to bounce.
 */
typedef enum {
    NES_INTERP_HANDOFF_ISLAND = 0,
    NES_INTERP_HANDOFF_SAFE   = 1,
    NES_INTERP_HANDOFF_LEGACY = 2,
} NesInterpHandoffMode;

void nes_interp_set_native_handoff_mode(NesInterpHandoffMode mode);
NesInterpHandoffMode nes_interp_get_native_handoff_mode(void);

/*
 * Explicit architectural outcome of the most recent interpreter run.
 *
 * The public dispatch ABI remains a handled/not-handled integer for generated
 * code compatibility. This result records why execution left the island and
 * the guest PC/stack state at that boundary, so callers and diagnostics no
 * longer have to infer every outcome from g_rts_target and S alone.
 */
typedef enum {
    NES_INTERP_EXIT_DECLINED      = 0,
    NES_INTERP_EXIT_RETURN        = 1,
    NES_INTERP_EXIT_RTI           = 2,
    NES_INTERP_EXIT_NATIVE_ESCAPE = 3,
    NES_INTERP_EXIT_STACK_ESCAPE  = 4,
    NES_INTERP_EXIT_BRK           = 5,
} NesInterpExitKind;

typedef struct {
    NesInterpExitKind kind;
    uint16_t entry_pc;
    uint16_t next_pc;
    uint8_t entry_s;
    uint8_t exit_s;
} NesInterpExit;

void nes_interp_get_last_exit(NesInterpExit *out);

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
    uint64_t native_handoffs_suppressed; /* handoffs kept inside interp island */
    uint64_t declines;          /* interp could not safely handle a dispatch */
    uint64_t policy_traps;      /* declines converted to debug pause/fault */
    uint32_t instrs_this_frame; /* interpreted instructions in the current frame */
    uint32_t max_instrs_run;    /* largest single interp_run instruction count */
} NesInterpStats;

void nes_interp_get_stats(NesInterpStats *out);

/* Called once per rendered frame (from the runner) to roll the per-frame
 * instruction counter. Safe to call even if the interpreter never fired. */
void nes_interp_frame_boundary(void);
