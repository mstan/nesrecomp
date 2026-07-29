#ifndef NESRECOMP_WINDOWS_PTHREAD_COMPAT_H
#define NESRECOMP_WINDOWS_PTHREAD_COMPAT_H

/* recomp-net 47b3d1 uses a mutex around libjuice callbacks. Its ICE source
 * currently names the small pthread mutex API directly; map that API to the
 * native Windows primitive until the portable wrapper lands upstream. */
#include <windows.h>

typedef CRITICAL_SECTION pthread_mutex_t;

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr) {
    (void)attr;
    InitializeCriticalSection(mutex);
    return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    DeleteCriticalSection(mutex);
    return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *mutex) {
    EnterCriticalSection(mutex);
    return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    LeaveCriticalSection(mutex);
    return 0;
}

#endif
