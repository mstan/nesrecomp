/*
 * coroutine.c — Fiber-based coroutine support for NES recomp (runner-level)
 *
 * Provides coroutine yield/resume for NES games with cooperative schedulers.
 * Uses Windows Fibers (CreateFiber/SwitchToFiber) which give each coroutine
 * its own C stack — required because setjmp/longjmp can't jump to a stack
 * frame that has been unwound.
 *
 * On non-Windows platforms, this would use ucontext or a similar facility.
 */
#include "coroutine.h"
#include "nes_runtime.h"
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>

/* ---- Write-ahead log ----
 * Each entry records: what the PREVIOUS action was + its outcome,
 * then what we're ABOUT to do next. If we crash, the last entry
 * tells us what killed us. */
static FILE *s_wal = NULL;
static const char *s_wal_path = NULL;
static int s_wal_prev_depth = 0;  /* vblank depth at last log */

static void wal_open(void) {
    if (s_wal) return;
    /* Try exe-relative path */
    s_wal_path = "coroutine_wal.log";
    s_wal = fopen(s_wal_path, "w");
    if (s_wal) {
        fprintf(s_wal, "=== Coroutine WAL started ===\n");
        fflush(s_wal);
    }
}

static void wal_log(const char *prev_result, const char *next_action,
                    int channel, uint16_t addr) {
    if (!s_wal) wal_open();
    if (!s_wal) return;
    extern int runtime_get_vblank_depth(void);
    int depth = runtime_get_vblank_depth();
    fprintf(s_wal,
        "f=%llu | prev: %s | SP=$%02X I=%d vdepth=%d | NEXT: %s ch=%d addr=$%04X\n",
        (unsigned long long)g_frame_count, prev_result,
        g_cpu.S, g_cpu.I, depth,
        next_action, channel, (int)addr);
    fflush(s_wal);
    s_wal_prev_depth = depth;
}

#define COROUTINE_STACK_SIZE (1024 * 1024)  /* 1 MB per coroutine */

static LPVOID s_scheduler_fiber = NULL;
static LPVOID s_coroutine_fibers[COROUTINE_MAX_CHANNELS] = {0};
static int    s_active = 0;
static int    s_current_channel = -1;

/* When the scheduler dispatches a START, it sets this to the entry point.
 * The fiber proc reads it and calls call_by_address. */
static uint16_t s_start_addr = 0;

/* Debug counters for coroutine diagnostics */
static int s_yield_count = 0;
static int s_resume_count = 0;
static int s_start_count = 0;
static uint8_t s_last_yield_sp = 0;
static uint8_t s_last_resume_sp = 0;

/* Scheduler event trace ring buffer */
static SchedTraceEntry s_sched_trace[SCHED_TRACE_SIZE];
static int s_sched_trace_count = 0;
static int s_sched_trace_idx = 0;

static void sched_trace_record(SchedEventType evt, int channel, uint16_t addr) {
    SchedTraceEntry *e = &s_sched_trace[s_sched_trace_idx];
    e->frame = g_frame_count;
    e->channel = channel;
    e->addr = addr;
    e->sp_before = g_cpu.S;
    e->event_type = (uint8_t)evt;
    s_sched_trace_idx = (s_sched_trace_idx + 1) % SCHED_TRACE_SIZE;
    if (s_sched_trace_count < SCHED_TRACE_SIZE) s_sched_trace_count++;
}

/* Forward declaration — provided by generated dispatch code */
extern int call_by_address(uint16_t addr);

/* Flag: fiber finished (returned from call_by_address without yielding).
 * Set by the fiber proc so the scheduler can clean up. */
static int s_fiber_finished_channel = -1;

/* Fiber entry point for a new coroutine */
static void CALLBACK coroutine_fiber_proc(LPVOID param)
{
    int ch = (int)(intptr_t)param;
    /* Run the coroutine entry point. When the coroutine yields,
     * SwitchToFiber returns here — but actually it doesn't, because
     * yield switches to the scheduler fiber. The coroutine code
     * continues from the yield point when resumed. */
    call_by_address(s_start_addr);
    /* Coroutine returned without yielding. Mark this channel as finished
     * so the scheduler can clean up the fiber (we can't DeleteFiber from
     * within the fiber itself). */
    wal_log("coroutine RETURNED (no yield)", "mark dead + switch to sched", ch, s_start_addr);
    s_fiber_finished_channel = ch;
    SwitchToFiber(s_scheduler_fiber);
    /* If we get here, the scheduler erroneously resumed a dead fiber.
     * Loop forever switching back — this is a bug but shouldn't crash. */
    for (;;) {
        wal_log("!!! ZOMBIE FIBER resumed", "switching back to sched", ch, 0xDEAD);
        s_fiber_finished_channel = ch;
        SwitchToFiber(s_scheduler_fiber);
    }
}

/* Flag: coroutine requested scheduler restart (JMP $FEAA from game code) */
static int s_restart_requested = 0;

int coroutine_scheduler_setjmp(void)
{
    if (!s_active) {
        s_active = 1;
        /* Convert main thread to fiber (required for SwitchToFiber) */
        if (!s_scheduler_fiber) {
            s_scheduler_fiber = ConvertThreadToFiber(NULL);
        }
    }
    /* If we're on a coroutine fiber (not the scheduler), the game code is
     * trying to re-enter the scheduler (e.g., JMP $FEAA after game mode
     * change). On real NES, LDX #$FF; TXS resets the hardware stack,
     * discarding all return addresses. In the recomp, we must yield back
     * to the real scheduler to unwind the C stack. */
    if (s_scheduler_fiber) {
        LPVOID current = GetCurrentFiber();
        if (current != s_scheduler_fiber) {
            /* We're on a coroutine fiber — signal scheduler restart */
            s_restart_requested = 1;
            sched_trace_record(SCHED_EVT_YIELD, s_current_channel, 0xFEAA);
            wal_log("SCHEDULER RE-ENTRY from fiber!", "RESTART->switch to sched fiber",
                    s_current_channel, 0xFEAA);
            SwitchToFiber(s_scheduler_fiber);
            /* If we get here, the scheduler resumed us after restart — shouldn't happen */
            wal_log("!!! ZOMBIE: resumed after restart", "BUG", s_current_channel, 0xFEAA);
            return 0;
        }
    }
    /* On scheduler fiber: if a restart was requested, clean up old fibers. */
    if (s_restart_requested) {
        wal_log("sched fiber: restart requested", "CLEANUP all fibers",
                s_current_channel, 0xFEAA);
        s_restart_requested = 0;
        for (int i = 0; i < COROUTINE_MAX_CHANNELS; i++) {
            if (s_coroutine_fibers[i]) {
                DeleteFiber(s_coroutine_fibers[i]);
                s_coroutine_fibers[i] = NULL;
            }
        }
        s_current_channel = -1;
        /* Reset vblank depth — the fiber switch may have interrupted
         * a vblank callback, leaving depth stuck > 0 */
        extern void runtime_reset_vblank_depth(void);
        runtime_reset_vblank_depth();
        wal_log("CLEANUP done, vblank depth reset", "sched: entering scan loop",
                -1, 0xFEAA);
    }
    return 0;
}

void coroutine_yield(void)
{
    if (!s_active || !s_scheduler_fiber) {
        fprintf(stderr, "[coroutine] yield: no active scheduler\n");
        return;
    }
    /* Switch back to the scheduler fiber. The scheduler's code continues
     * from after the SwitchToFiber that dispatched this coroutine. */
    s_yield_count++;
    s_last_yield_sp = g_cpu.S;
    sched_trace_record(SCHED_EVT_YIELD, s_current_channel, 0);
    wal_log("(in coroutine)", "YIELD->scheduler", s_current_channel, 0);
    SwitchToFiber(s_scheduler_fiber);
    wal_log("RESUMED from scheduler", "(coroutine continues)", s_current_channel, 0);
    /* When we get here, the scheduler has resumed us via coroutine_resume.
     * The generated code after coroutine_yield() does `return;` which
     * returns to the coroutine code that called func_FF21. */
}

void coroutine_resume(int channel)
{
    if (channel < 0 || channel >= COROUTINE_MAX_CHANNELS) {
        fprintf(stderr, "[coroutine] resume: bad channel %d\n", channel);
        return;
    }
    if (!s_coroutine_fibers[channel]) {
        /* No fiber for this channel. This happens when:
         * 1. Game code wrote the channel table directly (not via scheduler START)
         * 2. A previous START returned without yielding (fiber cleaned up)
         *
         * In RESUME mode, $82/$83 hold the saved SP, not an address.
         * We can't resume without a fiber — the channel is effectively dead.
         * Log it and return; the scheduler will skip this channel. */
        wal_log("RESUME: no fiber (dead channel)", "skip", channel, 0);
        return;
    }
    s_current_channel = channel;
    s_resume_count++;
    s_last_resume_sp = g_cpu.S;
    sched_trace_record(SCHED_EVT_RESUME, channel, 0);
    wal_log("sched: channel ready", "RESUME coroutine", channel, 0);
    s_fiber_finished_channel = -1;
    SwitchToFiber(s_coroutine_fibers[channel]);
    /* If the coroutine returned without yielding, clean up the dead fiber */
    if (s_fiber_finished_channel == channel) {
        wal_log("RESUMED coroutine FINISHED (cleanup)", "delete fiber", channel, 0);
        DeleteFiber(s_coroutine_fibers[channel]);
        s_coroutine_fibers[channel] = NULL;
        s_fiber_finished_channel = -1;
    }
    wal_log("coroutine yielded back", "sched: scan next", channel, 0);
    /* When we get here, the coroutine yielded back. Scheduler continues. */
}

void coroutine_start(int channel, uint16_t addr)
{
    if (channel < 0 || channel >= COROUTINE_MAX_CHANNELS) {
        fprintf(stderr, "[coroutine] start: bad channel %d\n", channel);
        return;
    }
    /* Clean up old fiber if any */
    if (s_coroutine_fibers[channel]) {
        DeleteFiber(s_coroutine_fibers[channel]);
        s_coroutine_fibers[channel] = NULL;
    }
    s_start_count++;
    s_start_addr = addr;
    s_current_channel = channel;
    sched_trace_record(SCHED_EVT_START, channel, addr);
    wal_log("sched: START dispatch", "CREATE fiber + switch", channel, addr);
    s_fiber_finished_channel = -1;
    s_coroutine_fibers[channel] = CreateFiber(
        COROUTINE_STACK_SIZE, coroutine_fiber_proc, (LPVOID)(intptr_t)channel);
    SwitchToFiber(s_coroutine_fibers[channel]);
    /* If the coroutine returned without yielding, clean up the dead fiber */
    if (s_fiber_finished_channel == channel) {
        wal_log("coroutine FINISHED (cleanup)", "delete fiber", channel, addr);
        DeleteFiber(s_coroutine_fibers[channel]);
        s_coroutine_fibers[channel] = NULL;
        s_fiber_finished_channel = -1;
    }
    wal_log("coroutine yielded/finished", "sched: scan next", channel, addr);
    /* When we get here, the coroutine yielded back (or finished). */
}

void coroutine_set_channel(int channel)
{
    s_current_channel = channel;
}

int coroutine_is_active(void)
{
    return s_active;
}

int coroutine_has_context(int channel)
{
    if (channel < 0 || channel >= COROUTINE_MAX_CHANNELS) return 0;
    return s_coroutine_fibers[channel] != NULL;
}

void coroutine_get_debug_counters(int *yields, int *resumes, int *starts,
                                   uint8_t *yield_sp, uint8_t *resume_sp)
{
    if (yields)    *yields    = s_yield_count;
    if (resumes)   *resumes   = s_resume_count;
    if (starts)    *starts    = s_start_count;
    if (yield_sp)  *yield_sp  = s_last_yield_sp;
    if (resume_sp) *resume_sp = s_last_resume_sp;
}

void coroutine_get_sched_trace(int *out_count, int *out_idx) {
    *out_count = s_sched_trace_count;
    *out_idx = s_sched_trace_idx;
}

const SchedTraceEntry *coroutine_get_sched_trace_buf(void) {
    return s_sched_trace;
}

int coroutine_get_current_channel(void) {
    return s_current_channel;
}

#else
/* Non-Windows stub — TODO: implement with ucontext */
#error "Coroutine support requires Windows Fibers (or ucontext on POSIX)"
#endif
