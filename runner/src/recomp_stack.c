/*
 * recomp_stack.c — Recompiled function call-stack tracking + bail trace.
 *
 * SHARED runner implementation (one source of truth; promoted from the three
 * divergent per-game copies). Defines the shadow call stack used by the TCP
 * debug server and the bail-event ring buffer. Both the recursion diagnostic
 * and the bail-trace disk log are opt-in (env-gated) so neither can stall a
 * normal run — previously the recursion dump fired on every depth-50 crossing
 * (~10k stderr lines/event), freezing games whose recompiled control flow
 * recurses deeply (e.g. Gumshoe's GxROM zapper dispatch).
 */
#include "recomp_stack.h"
#include "nes_runtime.h"   /* g_cpu, g_frame_count */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

const char *g_recomp_stack[RECOMP_STACK_DEPTH];
int         g_recomp_stack_top = 0;
const char *g_last_recomp_func = "(none)";

BailTraceEntry g_bail_trace[BAIL_TRACE_SIZE];
int g_bail_trace_count = 0;
int g_bail_trace_idx = 0;

void recomp_stack_push(const char *name)
{
    if (g_recomp_stack_top < RECOMP_STACK_DEPTH)
        g_recomp_stack[g_recomp_stack_top++] = name;
    g_last_recomp_func = name;

    /* Recursion diagnostic — opt-in (NESRECOMP_RECURSION_DEBUG=1) and FIRE-ONCE.
     * Dumps the shadow stack the first time depth crosses 50. Off by default so
     * deeply-recursing recompiled control flow can't spam stderr and stall. */
    if (g_recomp_stack_top == 50) {
        static int s_enabled = -1, s_dumped = 0;
        if (s_enabled < 0) {
            const char *e = getenv("NESRECOMP_RECURSION_DEBUG");
            s_enabled = (e && *e) ? 1 : 0;
        }
        if (s_enabled && !s_dumped) {
            fprintf(stderr, "\n[RECURSION] Stack depth hit 50 at frame %llu (dumping once):\n",
                    (unsigned long long)g_frame_count);
            for (int i = g_recomp_stack_top - 1; i >= 0; i--)
                fprintf(stderr, "  [%d] %s\n", i, g_recomp_stack[i] ? g_recomp_stack[i] : "?");
            fflush(stderr);
            s_dumped = 1;
        }
    }
}

void recomp_stack_pop(void)
{
    if (g_recomp_stack_top > 0)
        g_recomp_stack_top--;
    g_last_recomp_func = (g_recomp_stack_top > 0)
                        ? g_recomp_stack[g_recomp_stack_top - 1]
                        : "(none)";
}

void bail_trace(uint16_t caller_pc, uint8_t expected_sp)
{
    BailTraceEntry *e = &g_bail_trace[g_bail_trace_idx];
    e->frame            = g_frame_count;
    e->caller_pc        = caller_pc;
    e->expected_sp      = expected_sp;
    e->actual_sp        = g_cpu.S;
    e->recomp_stack_top = g_recomp_stack_top;
    e->recomp_stack_0   = (g_recomp_stack_top > 0) ? g_recomp_stack[g_recomp_stack_top - 1] : "(none)";
    e->recomp_stack_1   = (g_recomp_stack_top > 1) ? g_recomp_stack[g_recomp_stack_top - 2] : "(none)";
    g_bail_trace_idx    = (g_bail_trace_idx + 1) % BAIL_TRACE_SIZE;
    if (g_bail_trace_count < BAIL_TRACE_SIZE) g_bail_trace_count++;

    /* Per-bail disk logging is opt-in (NESRECOMP_BAIL_TRACE=1) — the in-memory
     * ring above (TCP-queryable) is always maintained; the fopen/fflush-per-bail
     * log would otherwise stall games that bail frequently. */
    static int s_log_enabled = -1;
    if (s_log_enabled < 0) {
        const char *en = getenv("NESRECOMP_BAIL_TRACE");
        s_log_enabled = (en && *en) ? 1 : 0;
    }
    if (s_log_enabled) {
        static FILE *s_bail_log = NULL;
        static int   s_opened = 0;
        if (!s_opened) {
            s_bail_log = fopen("bail_trace.log", "w");
            s_opened = 1;
            if (s_bail_log) {
                fprintf(s_bail_log, "frame,caller_pc,exp_sp,act_sp,stk_top,fn0,fn1\n");
                fflush(s_bail_log);
            }
        }
        if (s_bail_log) {
            fprintf(s_bail_log, "%llu,$%04X,$%02X,$%02X,%d,%s,%s\n",
                    (unsigned long long)e->frame, e->caller_pc,
                    e->expected_sp, e->actual_sp, e->recomp_stack_top,
                    e->recomp_stack_0 ? e->recomp_stack_0 : "?",
                    e->recomp_stack_1 ? e->recomp_stack_1 : "?");
            fflush(s_bail_log);
        }
    }
}
