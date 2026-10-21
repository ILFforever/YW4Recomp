/* Host threads, locks, condition variables and a monotonic clock (host.h). */
#include <stdlib.h>
#include <time.h>

#include "host.h"

#ifdef _WIN32
#include <process.h>
#include <windows.h>

void host_lock_init(HostLock *l) { InitializeSRWLock((PSRWLOCK)l); }
void host_lock(HostLock *l) { AcquireSRWLockExclusive((PSRWLOCK)l); }
void host_unlock(HostLock *l) { ReleaseSRWLockExclusive((PSRWLOCK)l); }

void host_cond_init(HostCond *c) { InitializeConditionVariable((PCONDITION_VARIABLE)c); }
void host_cond_signal(HostCond *c) { WakeConditionVariable((PCONDITION_VARIABLE)c); }

void host_cond_wait(HostCond *c, HostLock *l, int64_t timeout_ns) {
    /* Millisecond resolution, rounded up so a wait is never shorter than asked. */
    DWORD ms = INFINITE;
    if (timeout_ns >= 0) ms = timeout_ns >= 0xFFFFFFFEll * 1000000 ? 0xFFFFFFFE : (DWORD)((timeout_ns + 999999) / 1000000);
    SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)l, ms, 0);
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} Start;

static unsigned __stdcall thread_entry(void *p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

int host_thread_start(void (*fn)(void *), void *arg, size_t stack_size) {
    Start *s = malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn, s->arg = arg;
    uintptr_t h = _beginthreadex(NULL, (unsigned)stack_size, thread_entry, s, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) {
        free(s);
        return -1;
    }
    CloseHandle((HANDLE)h);  /* detached: nobody joins guest threads */
    return 0;
}

void host_yield(void) { SwitchToThread(); }

int64_t host_now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (int64_t)((unsigned __int128)now.QuadPart * 1000000000u / (uint64_t)freq.QuadPart);
}

#else
#include <sched.h>

void host_lock_init(HostLock *l) { pthread_mutex_init(l, NULL); }
void host_lock(HostLock *l) { pthread_mutex_lock(l); }
void host_unlock(HostLock *l) { pthread_mutex_unlock(l); }

void host_cond_init(HostCond *c) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);  /* timeouts don't jump with the wall clock */
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}
void host_cond_signal(HostCond *c) { pthread_cond_signal(c); }

void host_cond_wait(HostCond *c, HostLock *l, int64_t timeout_ns) {
    if (timeout_ns < 0) {
        pthread_cond_wait(c, l);
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t ns = ts.tv_nsec + timeout_ns % 1000000000;
    ts.tv_sec += (time_t)(timeout_ns / 1000000000 + ns / 1000000000);
    ts.tv_nsec = (long)(ns % 1000000000);
    pthread_cond_timedwait(c, l, &ts);
}

typedef struct {
    void (*fn)(void *);
    void *arg;
} Start;

static void *thread_entry(void *p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int host_thread_start(void (*fn)(void *), void *arg, size_t stack_size) {
    Start *s = malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn, s->arg = arg;
    pthread_attr_t a;
    pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_size);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &a, thread_entry, s);
    pthread_attr_destroy(&a);
    if (r) free(s);
    return r ? -1 : 0;
}

void host_yield(void) { sched_yield(); }

int64_t host_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
#endif
