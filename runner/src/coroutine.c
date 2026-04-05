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

#define COROUTINE_STACK_SIZE (1024 * 1024)  /* 1 MB per coroutine */

static LPVOID s_scheduler_fiber = NULL;
static LPVOID s_coroutine_fibers[COROUTINE_MAX_CHANNELS] = {0};
static int    s_active = 0;
static int    s_current_channel = -1;

/* When the scheduler dispatches a START, it sets this to the entry point.
 * The fiber proc reads it and calls call_by_address. */
static uint16_t s_start_addr = 0;

/* Forward declaration — provided by generated dispatch code */
extern int call_by_address(uint16_t addr);

/* Fiber entry point for a new coroutine */
static void CALLBACK coroutine_fiber_proc(LPVOID param)
{
    int ch = (int)(intptr_t)param;
    (void)ch;
    /* Run the coroutine entry point. When the coroutine yields,
     * SwitchToFiber returns here — but actually it doesn't, because
     * yield switches to the scheduler fiber. The coroutine code
     * continues from the yield point when resumed. */
    call_by_address(s_start_addr);
    /* Coroutine returned without yielding — switch back to scheduler */
    SwitchToFiber(s_scheduler_fiber);
}

int coroutine_scheduler_setjmp(void)
{
    if (!s_active) {
        s_active = 1;
        /* Convert main thread to fiber (required for SwitchToFiber) */
        if (!s_scheduler_fiber) {
            s_scheduler_fiber = ConvertThreadToFiber(NULL);
        }
    }
    /* Return 0 — the scheduler loop uses this as a no-op marker.
     * (Unlike the setjmp version, Fibers don't need a setjmp point;
     *  yield just does SwitchToFiber(scheduler) directly.) */
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
    SwitchToFiber(s_scheduler_fiber);
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
        /* No fiber for this channel — the coroutine was set up by game code
         * writing directly to the channel table, not via the scheduler's START
         * path. Create a fiber now using the saved address from the channel
         * table as the entry point ($82/$83 hold the resume info). */
        {
            int ch_offset = channel * 4;
            uint8_t lo = g_ram[0x82 + ch_offset];
            uint8_t hi = g_ram[0x83 + ch_offset];
            uint16_t addr = ((uint16_t)hi << 8) | lo;
            /* Treat as a START with the saved address */
            coroutine_start(channel, addr);
        }
        return;
    }
    s_current_channel = channel;
    SwitchToFiber(s_coroutine_fibers[channel]);
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
    s_start_addr = addr;
    s_current_channel = channel;
    s_coroutine_fibers[channel] = CreateFiber(
        COROUTINE_STACK_SIZE, coroutine_fiber_proc, (LPVOID)(intptr_t)channel);
    SwitchToFiber(s_coroutine_fibers[channel]);
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

#else
/* Non-Windows stub — TODO: implement with ucontext */
#error "Coroutine support requires Windows Fibers (or ucontext on POSIX)"
#endif
