/* One deadline owner: sleep on the host timer, spin only the final50us.
 * SDL's renderer may already have waited for VSync; an expired deadline
 * returns immediately. No simulation state or process-wide timer settings. */
#pragma once
#include <SDL.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
typedef struct { HANDLE timer; } CycFrameWait;
static inline CycFrameWait cyc_frame_wait_create(void) {
    typedef HANDLE (WINAPI *CreateTimer)(LPSECURITY_ATTRIBUTES,LPCWSTR,DWORD,DWORD);
    CreateTimer create=(CreateTimer)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"CreateWaitableTimerExW");
    CycFrameWait w={create?create(NULL,NULL,0x2,TIMER_MODIFY_STATE|SYNCHRONIZE):NULL};
    if(!w.timer)w.timer=CreateWaitableTimerW(NULL,FALSE,NULL);
    return w;
}
static inline void cyc_frame_wait_destroy(CycFrameWait *w){if(w->timer)CloseHandle(w->timer);w->timer=NULL;}
#else
#include <time.h>
typedef struct { int unused; } CycFrameWait;
static inline CycFrameWait cyc_frame_wait_create(void){CycFrameWait w={0};return w;}
static inline void cyc_frame_wait_destroy(CycFrameWait *w){(void)w;}
#endif
static inline void cyc_frame_wait_until(CycFrameWait *w,Uint64 deadline,Uint64 frequency) {
    const Uint64 tail=frequency/20000; /* 50us, independent of SDL millisecond rounding */
    for(;;){
        Uint64 now=SDL_GetPerformanceCounter();if(now>=deadline)return;
        Uint64 left=deadline-now;if(left<=tail)continue;
#ifdef _WIN32
        if(w->timer){
            LARGE_INTEGER due;due.QuadPart=-(LONGLONG)((left-tail)*10000000.0/frequency);
            if(due.QuadPart&&SetWaitableTimer(w->timer,&due,0,NULL,NULL,FALSE)&&WaitForSingleObject(w->timer,INFINITE)==WAIT_OBJECT_0)continue;
        }
        Uint32 ms=(Uint32)(left*1000/frequency);if(ms)SDL_Delay(ms);
#else
        (void)w;Uint64 ns=(Uint64)((left-tail)*1000000000.0/frequency);
        struct timespec pause={(time_t)(ns/1000000000),(long)(ns%1000000000)};
        nanosleep(&pause,NULL);
#endif
    }
}
