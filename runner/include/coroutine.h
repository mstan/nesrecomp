/*
 * coroutine.h — setjmp/longjmp-based coroutine support for NES recomp
 *
 * Many NES games use a cooperative coroutine scheduler that saves/restores
 * the 6502 stack pointer to switch between tasks. The recompiled C code
 * can't use the 6502 stack for C control flow, so we use setjmp/longjmp.
 *
 * Flow:
 *   1. Scheduler calls coroutine_scheduler_setjmp() at loop top.
 *   2. Scheduler dispatches coroutine via call_by_address (START) or
 *      coroutine_resume() (RESUME — longjmps to saved coroutine context).
 *   3. Coroutine yields via coroutine_yield(), which:
 *      a. Saves the coroutine's C context (setjmp into per-channel slot)
 *      b. longjmps back to the scheduler's setjmp point
 *   4. On RESUME, scheduler calls coroutine_resume(channel) which
 *      longjmps to the saved coroutine context from step 3a.
 *
 * START path: just call_by_address — the first yield saves the context.
 * RESUME path: coroutine_resume longjmps to where the coroutine yielded.
 */
#pragma once
#include <stdint.h>
#include <setjmp.h>

#define COROUTINE_MAX_CHANNELS 4

/* Save the scheduler's C context. Returns 0 on first call,
 * non-zero when longjmp'd back from coroutine_yield(). */
int coroutine_scheduler_setjmp(void);

/* Yield from a coroutine back to the scheduler.
 * Saves the current C context for later resume, then longjmps
 * to the scheduler's setjmp point. */
void coroutine_yield(void);

/* Resume a previously-yielded coroutine.
 * channel: 0-3 (the coroutine channel to resume).
 * longjmps to the saved coroutine context. When the coroutine
 * yields again, control returns to the scheduler's setjmp. */
void coroutine_resume(int channel);

/* Set which channel is currently being dispatched.
 * Called by the generated scheduler code before dispatching. */
void coroutine_set_channel(int channel);

/* Start a new coroutine on the given channel at the given 6502 address.
 * Creates a new fiber, dispatches to the address via call_by_address.
 * Returns when the coroutine yields. */
void coroutine_start(int channel, uint16_t addr);

/* Returns 1 if the coroutine system is active. */
int coroutine_is_active(void);

/* Returns 1 if the given channel has a saved context (can be resumed). */
int coroutine_has_context(int channel);
