/*
 * recomp_stack.h — Recompiled function call-stack tracking + bail trace.
 *
 * Maintains a shadow call stack of up to RECOMP_STACK_DEPTH function names.
 * Generated code emits recomp_stack_push/pop at function entry/exit and
 * bail_trace() at stack-mismatch bail sites — all under #ifdef
 * RECOMP_STACK_TRACKING. Queryable via the TCP debug server ("call_stack").
 *
 * SHARED runner file (one source of truth). Previously copied per-game with
 * three divergent variants (minimal / +recursion-dump / +bail-trace); this is
 * the superset, so every game links regardless of its generation vintage.
 */
#pragma once
#include <stdint.h>

/* 512 (was 16 in the minimal variant) so the shadow stack can still capture a
 * meaningful trace for games whose recompiled control flow recurses deeply. */
#define RECOMP_STACK_DEPTH 512

extern const char *g_recomp_stack[RECOMP_STACK_DEPTH];
extern int         g_recomp_stack_top;
extern const char *g_last_recomp_func;

void recomp_stack_push(const char *name);
void recomp_stack_pop(void);

/* Bail trace: records recompiler "bail" events (a recompiled function returning
 * early because the 6502 stack pointer did not match on return — the recompiler's
 * stack-mismatch safety net). The in-memory ring is always maintained (cheap,
 * TCP-queryable); the per-bail bail_trace.log disk write is opt-in via
 * NESRECOMP_BAIL_TRACE=1 (avoids the per-event fflush stall class). */
void bail_trace(uint16_t caller_pc, uint8_t expected_sp);

#define BAIL_TRACE_SIZE 32
typedef struct {
    uint64_t    frame;
    uint16_t    caller_pc;
    uint8_t     expected_sp;
    uint8_t     actual_sp;
    int         recomp_stack_top;
    const char *recomp_stack_0; /* top of recomp stack at bail */
    const char *recomp_stack_1; /* second entry */
} BailTraceEntry;

extern BailTraceEntry g_bail_trace[BAIL_TRACE_SIZE];
extern int g_bail_trace_count;
extern int g_bail_trace_idx;
