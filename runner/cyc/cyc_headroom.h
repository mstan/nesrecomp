/* Host-only measurement totals. They never enter snapshots or netplay identity. */
/* Instructions are real fetch attempts; native service work is reported
 * separately and never invents instruction or PC observations. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#ifndef NESRECOMP_HEADROOM_COUNTERS
#define NESRECOMP_HEADROOM_COUNTERS 1
#endif
enum { HR_S_NONE, HR_S_RAM_R, HR_S_RAM_W, HR_S_ROM_FOLD, HR_S_FETCH_ROM, HR_S_CODE_RAM,
       HR_S_IO_R, HR_S_IO_W, HR_S_WATCH_W, HR_S_FETCH_INTERP, HR_S_N };
#if NESRECOMP_HEADROOM_COUNTERS
#define HR_COUNT(...) do { __VA_ARGS__; } while (0)
/* Older generated path markers remain source-compatible. Fetch totals now
 * belong to physical bus publication, so no CFG-edge work is necessary. */
#define HR_PATH_LOCAL
#define HR_PATH_START(n) ((void)0)
#define HR_PATH_ADD(n) ((void)0)
extern uint64_t hr_ins;
extern uint64_t hr_frames;
extern int hr_pc_active;
void hr_fetch_pc(uint16_t pc);
void hr_int_enter(uint16_t vector);
void hr_int_leave(void);
void hr_start(void);
void hr_clock_cut(void);
void hr_clock_resume(void);
void hr_dump(void);
void hr_print(const char *format, ...);
size_t hr_report(char *out, size_t size);
static inline void hr_fetch(uint16_t pc) {
    if (hr_pc_active) hr_fetch_pc(pc);
}
#else
#define HR_COUNT(...) ((void)0)
#define HR_PATH_LOCAL
#define HR_PATH_START(n) ((void)0)
#define HR_PATH_ADD(n) ((void)0)
#define hr_fetch(pc) ((void)0)
#define hr_int_enter(vector) ((void)0)
#define hr_int_leave() ((void)0)
#define hr_start() ((void)0)
#define hr_clock_cut() ((void)0)
#define hr_clock_resume() ((void)0)
#define hr_dump() ((void)0)
#endif
