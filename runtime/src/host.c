/* Host threads, locks, condition variables and a monotonic clock (host.h). */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "host.h"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#include <timeapi.h>

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

static void (*crash_report)(const char *, uint64_t, uint64_t);

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e) {
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    const char *what = r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? "access violation"
                     : r->ExceptionCode == EXCEPTION_STACK_OVERFLOW ? "host stack overflow"
                     : r->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ? "illegal instruction"
                     : "exception";
    fprintf(stderr, "yw4: crash in yw4.exe+%#llx\n",
            (unsigned long long)((uintptr_t)r->ExceptionAddress - (uintptr_t)GetModuleHandleW(NULL)));
    crash_report(what, (uint64_t)(uintptr_t)r->ExceptionAddress,
                 r->NumberParameters >= 2 ? (uint64_t)r->ExceptionInformation[1] : 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

void host_install_crash_handler(void (*report)(const char *, uint64_t, uint64_t)) {
    crash_report = report;
    SetUnhandledExceptionFilter(crash_filter);
}

/* Windows' default timer ticks every 15.6 ms: Sleep and timed condition-variable waits
 * round up to it, so a 16.7 ms vsync sleep really lasted ~31 ms. timeBeginPeriod(1) makes
 * it 1 ms for the whole process (and its waits). */
void host_timer_init(void) { timeBeginPeriod(1); }

/* A high-resolution waitable timer per thread (Windows 10 1803+: sub-millisecond), else Sleep. */
void host_sleep_ns(int64_t ns) {
    static _Thread_local HANDLE timer;
    static _Thread_local int tried;
    if (ns <= 0) return;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due;
    due.QuadPart = -(ns + 99) / 100;  /* relative, in 100 ns units */
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    else Sleep((DWORD)((ns + 999999) / 1000000));
}

int64_t host_now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (int64_t)((unsigned __int128)now.QuadPart * 1000000000u / (uint64_t)freq.QuadPart);
}

#else
#include <sched.h>
#include <signal.h>

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

static void (*crash_report)(const char *, uint64_t, uint64_t);

static void crash_signal(int sig, siginfo_t *info, void *uc) {
    (void)uc;
    crash_report(sig == SIGSEGV ? "segmentation fault" : sig == SIGBUS ? "bus error" : "illegal instruction", 0,
                 (uint64_t)(uintptr_t)info->si_addr);
}

void host_install_crash_handler(void (*report)(const char *, uint64_t, uint64_t)) {
    crash_report = report;
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

void host_timer_init(void) {}

void host_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    while (nanosleep(&ts, &ts)) {}  /* interrupted: sleep the rest */
}

int64_t host_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
#endif
