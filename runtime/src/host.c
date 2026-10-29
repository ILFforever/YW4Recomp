/* Host threads, locks, condition variables and a monotonic clock (host.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#include <timeapi.h>
#include <psapi.h>

void host_lock_init(HostLock *l) { InitializeSRWLock((PSRWLOCK)l); }
void host_lock(HostLock *l) { AcquireSRWLockExclusive((PSRWLOCK)l); }
void host_unlock(HostLock *l) { ReleaseSRWLockExclusive((PSRWLOCK)l); }

void host_cond_init(HostCond *c) { InitializeConditionVariable((PCONDITION_VARIABLE)c); }
void host_cond_signal(HostCond *c) { WakeConditionVariable((PCONDITION_VARIABLE)c); }
void host_cond_broadcast(HostCond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); }

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
    /* The timer can't wait less than ~0.5 ms, but the game polls its job counters with
     * nn::os::SleepThread(1 us) on the main thread: each poll overslept ~500x. Short sleeps
     * give the core away and check the clock instead (YW4_SHORT_SLEEP=0: the timer, as before). */
    static int short_sleep = -1;
    if (short_sleep < 0) {
        const char *e = getenv("YW4_SHORT_SLEEP");
        short_sleep = !(e && *e == '0');
    }
    if (short_sleep && ns < 200000) {
        int64_t end = host_now_ns() + ns;
        do SwitchToThread();
        while (host_now_ns() < end);
        return;
    }
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

void *host_thread_self(void) {
    HANDLE h = NULL;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_QUERY_LIMITED_INFORMATION,
                    FALSE, 0);
    return h;
}

int64_t host_thread_cpu_ns(void *thread) {
    FILETIME created, exited, kernel, user;
    if (!thread || !GetThreadTimes((HANDLE)thread, &created, &exited, &kernel, &user)) return 0;
    return (int64_t)((((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                      ((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime)) * 100);
}

void host_process_usage(int64_t *cpu_ns, uint64_t *rss_bytes) {
    FILETIME created, exited, kernel, user;
    PROCESS_MEMORY_COUNTERS pmc = {sizeof pmc};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    *cpu_ns = (int64_t)((((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                         ((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime)) * 100);  /* 100 ns units */
    *rss_bytes = GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.WorkingSetSize : 0;
}

int host_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (int)n : 1;
}

void *host_reserve_watched(uint64_t want, size_t size) {
    return VirtualAlloc((void *)(uintptr_t)want, size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
}

/* Write watchpoint: the watched page is made read-only; a write to it faults on the writing
 * thread, which reports it (if it touches [addr, addr + size)), then the page is opened for
 * one single-stepped instruction and protected again. Writes to the page by other threads
 * during that one step are missed. */
static uint64_t ww_addr, ww_size, ww_page;
static void (*ww_report)(uint64_t pc, uint64_t addr);
static _Thread_local int ww_stepping, ww_hit;

static LONG CALLBACK ww_handler(EXCEPTION_POINTERS *e) {
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    DWORD old;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1) {
        uint64_t a = (uint64_t)r->ExceptionInformation[1];
        if (a < ww_page || a >= ww_page + 4096) return EXCEPTION_CONTINUE_SEARCH;
        ww_hit = a < ww_addr + ww_size && a + 16 > ww_addr;
        if (ww_hit) ww_report((uint64_t)(uintptr_t)r->ExceptionAddress, a);
        VirtualProtect((void *)(uintptr_t)ww_page, 4096, PAGE_READWRITE, &old);
        e->ContextRecord->EFlags |= 0x100;  /* trap after this one instruction */
        ww_stepping = 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && ww_stepping) {
        ww_stepping = 0;
        if (ww_hit) {  /* the watched bytes after the write */
            uint64_t v = 0;
            memcpy(&v, (const void *)(uintptr_t)ww_addr, ww_size < 8 ? ww_size : 8);
            fprintf(stderr, "       -> watched value now %#llx\n", (unsigned long long)v);
        }
        VirtualProtect((void *)(uintptr_t)ww_page, 4096, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int host_write_watch(uint64_t addr, uint64_t size, void (*report)(uint64_t pc, uint64_t addr)) {
    DWORD old;
    ww_addr = addr, ww_size = size, ww_page = addr & ~(uint64_t)4095, ww_report = report;
    if (!AddVectoredExceptionHandler(1, ww_handler)) return -1;
    return VirtualProtect((void *)(uintptr_t)ww_page, 4096, PAGE_READONLY, &old) ? 0 : -1;
}

int host_readable(uint64_t addr, uint64_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) return 0;
    return addr + size <= (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

long host_written_pages(uint64_t addr, uint64_t size, void (*fn)(uint64_t page, void *arg), void *arg) {
    enum { BATCH = 4096 };
    static void *pages[BATCH];  /* callers hold the draw path's lock */
    long total = 0;
    for (;;) {
        ULONG_PTR n = BATCH;
        DWORD gran;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)addr, (SIZE_T)size, pages, &n, &gran)) return -1;
        for (ULONG_PTR i = 0; i < n; i++) fn((uint64_t)(uintptr_t)pages[i], arg);
        total += (long)n;
        if (n < BATCH) return total;  /* a full batch: there may be more */
    }
}

#else
#include <sys/mman.h>
#include <unistd.h>
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
void host_cond_broadcast(HostCond *c) { pthread_cond_broadcast(c); }

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

void *host_thread_self(void) {
    clockid_t *c = malloc(sizeof *c);
    if (c && pthread_getcpuclockid(pthread_self(), c)) free(c), c = NULL;
    return c;
}

int64_t host_thread_cpu_ns(void *thread) {
    struct timespec ts;
    if (!thread || clock_gettime(*(clockid_t *)thread, &ts)) return 0;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void host_process_usage(int64_t *cpu_ns, uint64_t *rss_bytes) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    *cpu_ns = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    unsigned long pages = 0, resident = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%lu %lu", &pages, &resident) != 2) resident = 0;
        fclose(f);
    }
    *rss_bytes = (uint64_t)resident * 4096;
}

int host_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* No write tracking on Linux yet (to do: soft-dirty bits or mprotect): plain reservation,
 * and every page counts as written. */
void *host_reserve_watched(uint64_t want, size_t size) {
    void *p = mmap((void *)(uintptr_t)want, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p != MAP_FAILED && p != (void *)(uintptr_t)want) {
        munmap(p, size);
        return NULL;
    }
    return p == MAP_FAILED ? NULL : p;
}

long host_written_pages(uint64_t addr, uint64_t size, void (*fn)(uint64_t page, void *arg), void *arg) {
    (void)addr, (void)size, (void)fn, (void)arg;
    return -1;
}

int host_write_watch(uint64_t addr, uint64_t size, void (*report)(uint64_t pc, uint64_t addr)) {
    (void)addr, (void)size, (void)report;
    return -1;  /* to do: mprotect + SIGSEGV + single step */
}

int host_readable(uint64_t addr, uint64_t size) {  /* msync on unmapped pages fails with ENOMEM */
    long page = sysconf(_SC_PAGESIZE);
    uint64_t a = addr & ~(uint64_t)(page - 1);
    return msync((void *)(uintptr_t)a, (size_t)(addr + size - a), MS_ASYNC) == 0;
}
#endif
