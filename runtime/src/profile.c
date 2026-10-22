/* Sampling profiler for the presenting thread (YW4_PROFILE=1, Windows only).
 *
 * Every millisecond a host thread stops the thread that presents frames (the game's main
 * thread), reads where it is, and lets it go again. At exit the counts go to
 * work/perf/profile_<time>.txt as offsets into yw4.exe (or "outside", for waits in system
 * DLLs); `python -m tools.yw4.profile` names them with the executable's symbol table. Only
 * where the thread is, not who called it: time spent in a function itself. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvn.h"

#ifdef _WIN32
#include <windows.h>

#define PROFILE_SLOTS (1 << 20)

static HANDLE target;
static uint64_t *keys, *counts;  /* open addressing on the exe offset; key 0 = empty */
static uint64_t outside, total;
static uintptr_t exe_base, exe_end;
static volatile LONG stop;
static char out_path[128];

static void write_out(void);

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

static DWORD WINAPI sampler(void *arg) {
    (void)arg;
    timeBeginPeriod(1);
    while (!stop) {
        Sleep(1);
        if (SuspendThread(target) == (DWORD)-1) continue;
        CONTEXT c = {.ContextFlags = CONTEXT_FULL};
        if (GetThreadContext(target, &c)) {
            total++;
            if (c.Rip >= exe_base && c.Rip < exe_end) {
                add(c.Rip - exe_base);
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
                        add((c.Rip - exe_base) | 1ull << 63);
                        break;
                    }
                }
            }
        }
        ResumeThread(target);
        if (total && total % 10000 == 0) write_out();  /* a killed run still leaves its numbers */
    }
    return 0;
}

static void write_out(void) {
    FILE *f = fopen(out_path, "w");
    if (!f) return;
    fprintf(f, "# samples %" PRIu64 " outside %" PRIu64 "\n", total, outside);
    for (size_t i = 0; i < PROFILE_SLOTS; i++)
        if (keys[i]) fprintf(f, "%" PRIx64 " %" PRIu64 "\n", keys[i], counts[i]);
    fclose(f);
}

static void finish(void) {
    if (!keys) return;
    stop = 1;
    Sleep(5);
    write_out();
    fprintf(stderr, "yw4: profile (%" PRIu64 " samples) in %s\n", total, out_path);
}

void yw4_profile_start_here(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    const char *on = getenv("YW4_PROFILE");
    if (!on || *on != '1') return;
    HMODULE exe = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    exe_base = (uintptr_t)exe, exe_end = exe_base + nt->OptionalHeader.SizeOfImage;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &now);
    char when[32];
    strftime(when, sizeof when, "%Y%m%d_%H%M%S", &tmv);
    snprintf(out_path, sizeof out_path, "work/perf/profile_%s.txt", when);
    keys = calloc(PROFILE_SLOTS, sizeof *keys), counts = calloc(PROFILE_SLOTS, sizeof *counts);
    if (!keys || !counts) return;
    atexit(finish);
    CreateThread(NULL, 1 << 16, sampler, NULL, 0, NULL);
    fprintf(stderr, "yw4: profiling the presenting thread\n");
}
#else
void yw4_profile_start_here(void) {}
#endif
