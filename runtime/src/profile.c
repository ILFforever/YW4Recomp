/* Sampling profiler (Windows only).
 *
 * YW4_PROFILE=1 samples the thread that presents frames (the game's main thread);
 * YW4_PROFILE=submit samples every thread that submits GPU command lists instead.
 * YW4_PROFILE_FROM=<frame> starts at that frame, to skip start-up.
 * YW4_PROFILE_THREAD=<n> samples only guest thread n (the performance report's "thread n").
 * YW4_PROFILE=hitch samples the presenting thread but keeps only the samples of frames that took longer than
 * YW4_PROFILE_HITCH_MS (default 100): what the game did in a freeze, not on average (2026-11-11, the Demon Hand).
 * Hitch mode samples every guest thread (the presenting one may only be waiting for another): each key carries the
 * guest thread id in bits 48-62.
 *
 * Every millisecond a host thread stops each sampled thread, reads where it is, and lets it
 * go again. The counts go to work/perf/profile_<time>.txt (rewritten every 10 s and at exit)
 * as offsets into yw4.exe; a sample in a system DLL is unwound to its first yw4.exe caller
 * and marked with bit 63. `python -m tools.yw4.profile` names them with the executable's
 * symbol table. Only where a thread is, not who called it: time spent in a function itself. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernel.h"
#include "nvn.h"

#ifdef _WIN32
#include <windows.h>

#define PROFILE_SLOTS (1 << 20)
#define MAX_TARGETS 48

static HANDLE targets[MAX_TARGETS];
static uint64_t target_tag[MAX_TARGETS];  /* hitch mode: the guest thread id << 48 */
static uint64_t cur_tag;
static volatile LONG ntargets;
static uint64_t *keys, *counts;  /* open addressing on the exe offset; key 0 = empty */
static uint64_t outside, total;
static uintptr_t exe_base, exe_end;
static volatile LONG stop;
static char out_path[128];
static int mode = -1;  /* 0 off, 1 presenting thread, 2 submitting threads, 3 presenting thread, hitches only */
extern uint64_t yw4_frames_presented;  /* dispatch.c */
static uint64_t pending[1 << 14];      /* hitch mode: this frame's samples, kept if the frame turns out long */
static int npending;
static uint64_t hitch_ms = 100;
static long readable_before;
static long long readable_ns_before;
static uint64_t from_frame;
static SRWLOCK lock = SRWLOCK_INIT;  /* the counts, between the sampler and write_out */

static void add(uint64_t key) {
    for (uint64_t i = (key * 0x9E3779B97F4A7C15ull) >> 44;; i = (i + 1) & (PROFILE_SLOTS - 1)) {
        if (keys[i] == key) {
            counts[i]++;
            return;
        }
        if (!keys[i]) {
            keys[i] = key, counts[i] = 1;
            return;
        }
    }
}

static void write_out(void) {
    AcquireSRWLockExclusive(&lock);
    FILE *f = fopen(out_path, "w");
    if (f) {
        fprintf(f, "# samples %" PRIu64 " outside %" PRIu64 "\n", total, outside);
        for (size_t i = 0; i < PROFILE_SLOTS; i++)
            if (keys[i]) fprintf(f, "%" PRIx64 " %" PRIu64 "\n", keys[i], counts[i]);
        fclose(f);
    }
    ReleaseSRWLockExclusive(&lock);
}

static void record(uint64_t key) {
    if (mode != 3) add(key);
    else if (npending < (int)(sizeof pending / sizeof *pending)) pending[npending++] = key | cur_tag;
}

/* Hitch mode: at each new frame, the last frame's samples go to the counts if it was long, else away. */
static void frame_check(void) {
    static uint64_t last_frame, last_ms;
    uint64_t f = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED), now = GetTickCount64();
    if (f == last_frame) return;
    if (last_ms && now - last_ms >= hitch_ms && npending && last_frame >= from_frame) {
        for (int i = 0; i < npending; i++) add(pending[i]);
        fprintf(stderr, "yw4: profile: frame %" PRIu64 " took %" PRIu64 " ms: its %d samples kept\n", last_frame,
                now - last_ms, npending);
        /* host.c counts host_readable's calls and callers: a long frame's system calls, named (2026-11-12) */
        extern volatile long host_readable_calls, host_readable_caller_n[8];
        extern volatile uintptr_t host_readable_callers[8];
        fprintf(stderr, "yw4: profile:   host_readable calls in it: %ld; callers so far:", host_readable_calls - readable_before);
        for (int i = 0; i < 8; i++)
            if (host_readable_callers[i])
                fprintf(stderr, " exe+%#llx x%ld", (unsigned long long)(host_readable_callers[i] - exe_base),
                        host_readable_caller_n[i]);
        extern volatile long long host_readable_ns, host_readable_max_ns;
        fprintf(stderr, "\nyw4: profile:   host_readable time in it: %lld ms, longest call %lld ms\n",
                (host_readable_ns - readable_ns_before) / 1000000, host_readable_max_ns / 1000000);
        host_readable_max_ns = 0;
    }
    {
        extern volatile long host_readable_calls;
        extern volatile long long host_readable_ns;
        readable_before = host_readable_calls, readable_ns_before = host_readable_ns;
    }
    npending = 0, last_frame = f, last_ms = now;
}

static void sample(HANDLE t) {
    if (SuspendThread(t) == (DWORD)-1) return;
    CONTEXT c = {.ContextFlags = CONTEXT_FULL};
    if (GetThreadContext(t, &c)) {
        total++;
        if (c.Rip >= exe_base && c.Rip < exe_end) {
            record(c.Rip - exe_base);
        } else {
            /* In a system DLL: unwind to the first yw4.exe frame, the caller (bit 63 marks it). */
            outside++;
            for (int depth = 0; depth < 64 && c.Rip; depth++) {
                DWORD64 base;
                RUNTIME_FUNCTION *fn = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                if (fn) {
                    void *data;
                    DWORD64 frame;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &data, &frame, NULL);
                } else {  /* a leaf: the return address is on top of the stack */
                    c.Rip = *(DWORD64 *)c.Rsp;
                    c.Rsp += 8;
                }
                if (c.Rip >= exe_base && c.Rip < exe_end) {
                    record((c.Rip - exe_base) | 1ull << 63);
                    break;
                }
            }
        }
    }
    ResumeThread(t);
}

static DWORD WINAPI sampler(void *arg) {
    (void)arg;
    timeBeginPeriod(1);
    uint64_t next_write = 10000;
    while (!stop) {
        Sleep(1);
        AcquireSRWLockExclusive(&lock);
        for (LONG i = 0; i < ntargets; i++) cur_tag = target_tag[i], sample(targets[i]);
        if (mode == 3) frame_check();
        ReleaseSRWLockExclusive(&lock);
        if (total >= next_write) write_out(), next_write = total + 10000;  /* a killed run still leaves its numbers */
    }
    return 0;
}

static void finish(void) {
    stop = 1;
    Sleep(5);
    write_out();
    fprintf(stderr, "yw4: profile (%" PRIu64 " samples) in %s\n", total, out_path);
}

static void setup(void) {
    const char *on = getenv("YW4_PROFILE");
    mode = !on || !*on || *on == '0' ? 0 : !strcmp(on, "submit") ? 2 : !strcmp(on, "hitch") ? 3 : 1;
    if (getenv("YW4_PROFILE_HITCH_MS")) hitch_ms = strtoull(getenv("YW4_PROFILE_HITCH_MS"), NULL, 10);
    const char *from = getenv("YW4_PROFILE_FROM");
    if (from) from_frame = strtoull(from, NULL, 10);
    if (!mode) return;
    HMODULE exe = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    exe_base = (uintptr_t)exe, exe_end = exe_base + nt->OptionalHeader.SizeOfImage;
    keys = calloc(PROFILE_SLOTS, sizeof *keys), counts = calloc(PROFILE_SLOTS, sizeof *counts);
    if (!keys || !counts) {
        mode = 0;
        return;
    }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &now);
    char when[32];
    strftime(when, sizeof when, "%Y%m%d_%H%M%S", &tmv);
    snprintf(out_path, sizeof out_path, "work/perf/profile_%s.txt", when);
    atexit(finish);
    CreateThread(NULL, 1 << 16, sampler, NULL, 0, NULL);
    fprintf(stderr, "yw4: profiling the %s thread(s) from frame %" PRIu64 "\n", mode == 2 ? "submitting" : mode == 3 ? "presenting (hitches only)" : "presenting",
            from_frame);
}

/* Adds the calling thread to the sampled ones (once) if it plays the role being profiled. */
void yw4_profile_thread(int role, uint64_t frame) {
    static _Thread_local int added;
    if (added || mode == 0) return;
    if (mode < 0) {
        static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
        BOOL pending;
        InitOnceBeginInitialize(&once, 0, &pending, NULL);
        if (pending) setup(), InitOnceComplete(&once, 0, NULL);
        if (!mode) return;
    }
    if (mode == 3 ? !(role & (YW4_ROLE_PRESENT | YW4_ROLE_SUBMIT | YW4_ROLE_ANY))
                  : (role == YW4_ROLE_PRESENT) != (mode == 1) || role == YW4_ROLE_ANY || frame < from_frame)
        return;
    const char *only = getenv("YW4_PROFILE_THREAD");
    if (only && *only && strtoull(only, NULL, 10) != yw4_thread_id()) return;
    added = 1;
    HANDLE h;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    AcquireSRWLockExclusive(&lock);
    if (ntargets < MAX_TARGETS) target_tag[ntargets] = (uint64_t)(yw4_thread_id() & 0x7fff) << 48, targets[ntargets++] = h;
    ReleaseSRWLockExclusive(&lock);
}
#else
void yw4_profile_thread(int role, uint64_t frame) { (void)role, (void)frame; }
#endif
