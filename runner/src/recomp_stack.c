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
#include <string.h>

const char *g_recomp_stack[RECOMP_STACK_DEPTH];
int         g_recomp_stack_top = 0;
const char *g_last_recomp_func = "(none)";

/* ---------------------------------------------------------------------------
 * Buffer-count watchpoint (env-gated: NESRECOMP_BUFWATCH="addr" or "addr:lo:hi")
 * Yoshi's Cookie keeps a VRAM update-list on the 6502 stack page; its first
 * entry's COUNT byte gets clobbered before func_F4C0 flushes it. This watches
 * one byte of g_ram continuously (sampled at every function boundary, so it
 * catches direct PHA writes that bypass nes_write) and logs every change with
 * the CPU stack pointer, recomp depth, and the two enclosing functions — so we
 * can attribute the clobber to an exact function/S without arm-and-capture. */
static void buf_watch(char ctx) {
    static int s_on = -1; static uint16_t s_addr = 0x102;
    static unsigned long long s_lo = 0, s_hi = ~0ULL;
    static int s_prev = -1; static int s_lines = 0;
    if (s_on < 0) {
        const char *e = getenv("NESRECOMP_BUFWATCH");
        if (e && *e) {
            s_on = 1; s_addr = (uint16_t)strtoul(e, NULL, 16);
            const char *c1 = strchr(e, ':');
            if (c1) { s_lo = strtoull(c1 + 1, NULL, 10);
                const char *c2 = strchr(c1 + 1, ':');
                if (c2) s_hi = strtoull(c2 + 1, NULL, 10); }
        } else s_on = 0;
    }
    if (!s_on) return;
    if (g_frame_count < s_lo || g_frame_count > s_hi) return;
    int cur = g_ram[s_addr & 0x7FF];
    if (cur != s_prev) {
        if (s_lines < 400) {
            const char *f0 = (g_recomp_stack_top > 0) ? g_recomp_stack[g_recomp_stack_top-1] : "(none)";
            const char *f1 = (g_recomp_stack_top > 1) ? g_recomp_stack[g_recomp_stack_top-2] : "(none)";
            fprintf(stderr, "[bufw] %c F=%llu $%03X %02X->%02X S=%02X depth=%d  fn=%s <- %s  hdr[100..107]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                ctx, (unsigned long long)g_frame_count, s_addr,
                s_prev < 0 ? 0xFF : s_prev, cur, g_cpu.S, g_recomp_stack_top, f0, f1,
                g_ram[0x100],g_ram[0x101],g_ram[0x102],g_ram[0x103],
                g_ram[0x104],g_ram[0x105],g_ram[0x106],g_ram[0x107]);
            fflush(stderr);
            s_lines++;
        }
        s_prev = cur;
    }
}

BailTraceEntry g_bail_trace[BAIL_TRACE_SIZE];
int g_bail_trace_count = 0;
int g_bail_trace_idx = 0;

/* ---------------------------------------------------------------------------
 * Per-function net-S tracker (env-gated: NESRECOMP_SDELTA="lo:hi").
 * Records S at each function entry (parallel to the shadow stack) and, at the
 * matching exit, logs any function whose net stack effect (entryS - exitS) is
 * nonzero — the recompiled function left the 6502 stack pointer imbalanced.
 * A function with a consistent nonzero net is a stack-idiom the emitter models
 * wrong (a PLA/PHA skipped by a branch-as-call, a synthetic return, etc.). */
static uint8_t s_entry_S[RECOMP_STACK_DEPTH];
static void sdelta_exit(void) {
    static int s_on = -1; static unsigned long long s_lo=0, s_hi=~0ULL; static int s_lines=0;
    if (s_on < 0) {
        const char *e = getenv("NESRECOMP_SDELTA");
        if (e && *e) { s_on = 1; s_lo = strtoull(e, NULL, 10);
            const char *c = strchr(e, ':'); if (c) s_hi = strtoull(c+1, NULL, 10); }
        else s_on = 0;
    }
    if (!s_on) return;
    if (g_frame_count < s_lo || g_frame_count > s_hi) return;
    int d = g_recomp_stack_top - 1;
    if (d < 0 || d >= RECOMP_STACK_DEPTH) return;
    int8_t net = (int8_t)(s_entry_S[d] - g_cpu.S);   /* >0 = net push (S down), <0 = net pull (S up) */
    if (net != 0 && s_lines < 300) {
        const char *fn = g_recomp_stack[d];
        fprintf(stderr, "[sdelta] F=%llu depth=%d fn=%s entryS=%02X exitS=%02X net=%d\n",
            (unsigned long long)g_frame_count, g_recomp_stack_top, fn ? fn : "?",
            s_entry_S[d], g_cpu.S, net);
        fflush(stderr); s_lines++;
    }
}

void recomp_stack_push(const char *name)
{
    if (g_recomp_stack_top < RECOMP_STACK_DEPTH) {
        s_entry_S[g_recomp_stack_top] = g_cpu.S;
        g_recomp_stack[g_recomp_stack_top++] = name;
    }
    g_last_recomp_func = name;
    buf_watch('P');

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
    buf_watch('p');
    sdelta_exit();
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
