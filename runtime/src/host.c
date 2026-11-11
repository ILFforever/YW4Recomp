/* Host threads, locks, condition variables and a monotonic clock (host.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"

#ifdef _WIN32
#include <windows.h>
#endif

void yw4_exit(int code, const char *file, int line) {
    const char *base = strrchr(file, '/');
    const char *b2 = strrchr(file, '\\');
    if (b2 && (!base || b2 > base)) base = b2;
    base = base ? base + 1 : file;
#ifdef _WIN32
    MEMORYSTATUSEX m = {.dwLength = sizeof m};
    if (GlobalMemoryStatusEx(&m))
        fprintf(stderr, "yw4: exit %d from %s:%d (memory: %llu MiB of %llu free, commit %llu MiB of %llu free)\n", code,
                base, line, (unsigned long long)(m.ullAvailPhys >> 20), (unsigned long long)(m.ullTotalPhys >> 20),
                (unsigned long long)(m.ullAvailPageFile >> 20), (unsigned long long)(m.ullTotalPageFile >> 20));
    else
#endif
        fprintf(stderr, "yw4: exit %d from %s:%d\n", code, base, line);
    exit(code);
}

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#include <timeapi.h>
#include <psapi.h>

void host_lock_init(HostLock *l) { InitializeSRWLock((PSRWLOCK)l); }
void host_lock(HostLock *l) { AcquireSRWLockExclusive((PSRWLOCK)l); }
int host_trylock(HostLock *l) { return TryAcquireSRWLockExclusive((PSRWLOCK)l) != 0; }
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

int64_t host_process_age_ns(void) {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    GetSystemTimePreciseAsFileTime(&now);
    return (int64_t)((((uint64_t)now.dwHighDateTime << 32 | now.dwLowDateTime) -
                      ((uint64_t)created.dwHighDateTime << 32 | created.dwLowDateTime)) * 100);  /* 100 ns units */
}

void *host_thread_self(void) {
    HANDLE h = NULL;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                    THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_INFORMATION, FALSE, 0);
    return h;
}

void host_thread_set_priority(void *thread, int guest_priority) {
    static int on = -1;  /* YW4_THREAD_PRIO=0: every thread stays at normal priority (tests, A/B) */
    if (on < 0) on = !(getenv("YW4_THREAD_PRIO") && getenv("YW4_THREAD_PRIO")[0] == '0');
    if (!on) return;
    HANDLE h = thread ? (HANDLE)thread : GetCurrentThread();
    int p = THREAD_PRIORITY_NORMAL;
    if (guest_priority < 38) p = THREAD_PRIORITY_HIGHEST;
    else if (guest_priority <= 40) p = THREAD_PRIORITY_ABOVE_NORMAL;
    else if (guest_priority >= 58) p = THREAD_PRIORITY_LOWEST;
    else if (guest_priority >= 50) p = THREAD_PRIORITY_BELOW_NORMAL;
    SetThreadPriority(h, p);
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
    void *p = VirtualAlloc((void *)(uintptr_t)want, size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    if (!p) {
        DWORD err = GetLastError();
        fprintf(stderr,
                "yw4: host_reserve_watched: VirtualAlloc at %#" PRIx64 " (%" PRIu64 " bytes) failed: error %lu\n",
                want, (uint64_t)size, (unsigned long)err);
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void *)(uintptr_t)want, &mbi, sizeof mbi)) {
            char mod[MAX_PATH] = "";
            if (mbi.Type == MEM_IMAGE && mbi.AllocationBase) {
                GetModuleFileNameA((HMODULE)mbi.AllocationBase, mod, sizeof mod);
            }
            fprintf(stderr,
                    "yw4:   collision at %#" PRIx64 ": base=%#" PRIx64 " size=%#" PRIx64 " state=%#x type=%#x%s%s\n",
                    want, (uint64_t)(uintptr_t)mbi.BaseAddress, (uint64_t)mbi.RegionSize,
                    (unsigned)mbi.State, (unsigned)mbi.Type, mod[0] ? " module=" : "", mod[0] ? mod : "");
        }
    }
    return p;
}

void host_respawn(int argc, char **argv) {
    (void)argc;
    (void)argv;
    SetEnvironmentVariableA("YW4_ASLR_RETRY", "1");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi = {0};
    char *cmd = _strdup(GetCommandLineA());
    if (cmd && CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        free(cmd);
        yw4_exit((int)code, __FILE__, __LINE__);
    }
    free(cmd);
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

/* Read watchpoint (YW4_TRACE_AI, ai_trace.c): the page is made inaccessible; an access by the game reports it when it
 * touches [addr, addr + size) (any access in the page faults; only those are reported), then the page is opened for one
 * single-stepped instruction and closed again. After `left` reports it stays open. */
static uint64_t rw_addr, rw_size, rw_page;
static long rw_left;
static void (*rw_report)(uint64_t pc, uint64_t addr);
static _Thread_local int rw_stepping;

static LONG CALLBACK rw_handler(EXCEPTION_POINTERS *e) {
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    DWORD old;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && rw_page) {
        uint64_t a = (uint64_t)r->ExceptionInformation[1];
        if (a < rw_page || a >= rw_page + 4096) return EXCEPTION_CONTINUE_SEARCH;
        if (a < rw_addr + rw_size && a + 16 > rw_addr && __atomic_fetch_sub(&rw_left, 1, __ATOMIC_RELAXED) > 0)
            rw_report((uint64_t)(uintptr_t)r->ExceptionAddress, a);
        VirtualProtect((void *)(uintptr_t)rw_page, 4096, PAGE_READWRITE, &old);
        if (__atomic_load_n(&rw_left, __ATOMIC_RELAXED) <= 0) {  /* done: the page stays open */
            rw_page = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        e->ContextRecord->EFlags |= 0x100;
        rw_stepping = 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && rw_stepping) {
        rw_stepping = 0;
        if (rw_page) VirtualProtect((void *)(uintptr_t)rw_page, 4096, PAGE_NOACCESS, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int host_read_watch(uint64_t addr, uint64_t size, long reports, void (*report)(uint64_t pc, uint64_t addr)) {
    static int added;
    DWORD old;
    rw_addr = addr, rw_size = size, rw_left = reports, rw_report = report, rw_page = addr & ~(uint64_t)4095;
    if (!added && !AddVectoredExceptionHandler(1, rw_handler)) return -1;
    added = 1;
    return VirtualProtect((void *)(uintptr_t)rw_page, 4096, PAGE_NOACCESS, &old) ? 0 : -1;
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
#include <sys/uio.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>

void host_lock_init(HostLock *l) { pthread_mutex_init(l, NULL); }
void host_lock(HostLock *l) { pthread_mutex_lock(l); }
int host_trylock(HostLock *l) { return pthread_mutex_trylock(l) == 0; }
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
    /* glibc takes the thread's static thread-local storage out of its stack, and refuses a stack too small for it
     * (EINVAL): the vsync thread's 64 KiB was refused on Linux (2026-11-12). Stack pages are only used when
     * touched, so a 1 MiB floor costs nothing. */
    if (stack_size < ((size_t)1 << 20)) stack_size = (size_t)1 << 20;
    pthread_attr_setstacksize(&a, stack_size);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &a, thread_entry, s);
    pthread_attr_destroy(&a);
    if (r) fprintf(stderr, "yw4: host: pthread_create (stack %zu): %s\n", stack_size, strerror(r));
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

int64_t host_process_age_ns(void) {
    static int64_t first;
    int64_t now = host_now_ns();
    if (!first) first = now;
    return now - first;
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

void host_thread_set_priority(void *thread, int guest_priority) {
    (void)thread;
    (void)guest_priority;
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
        fprintf(stderr, "yw4: host_reserve_watched: mmap placed at %p instead of %#" PRIx64 "\n", p, want);
        return NULL;
    }
    if (p == MAP_FAILED) {
        fprintf(stderr, "yw4: host_reserve_watched: mmap at %#" PRIx64 " (%" PRIu64 " bytes) failed: %s\n",
                want, (uint64_t)size, strerror(errno));
        return NULL;
    }
    return p;
}

void host_respawn(int argc, char **argv) {
    (void)argc;
    setenv("YW4_ASLR_RETRY", "1", 1);
    execv("/proc/self/exe", argv);
}

long host_written_pages(uint64_t addr, uint64_t size, void (*fn)(uint64_t page, void *arg), void *arg) {
    (void)addr, (void)size, (void)fn, (void)arg;
    return -1;
}

int host_write_watch(uint64_t addr, uint64_t size, void (*report)(uint64_t pc, uint64_t addr)) {
    (void)addr, (void)size, (void)report;
    return -1;  /* to do: mprotect + SIGSEGV + single step */
}

int host_read_watch(uint64_t addr, uint64_t size, long reports, void (*report)(uint64_t pc, uint64_t addr)) {
    (void)addr, (void)size, (void)reports, (void)report;
    return -1;  /* Windows only for now (YW4_TRACE_AI) */
}

int host_readable(uint64_t addr, uint64_t size) {
    if (!size) return 1;
    if (addr + size < addr) return 0;
    long page = sysconf(_SC_PAGESIZE);
    uint64_t start_page = addr & ~(uint64_t)(page - 1);
    uint64_t end_page = (addr + size - 1) & ~(uint64_t)(page - 1);
    pid_t pid = getpid();
    char dummy;
    struct iovec local = {.iov_base = &dummy, .iov_len = 1};
    for (uint64_t p = start_page; p <= end_page; p += (uint64_t)page) {
        struct iovec remote = {.iov_base = (void *)(uintptr_t)p, .iov_len = 1};
        if (process_vm_readv(pid, &local, 1, &remote, 1, 0) != 1) return 0;
    }
    return 1;
}
#endif
