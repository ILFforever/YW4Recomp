/* The few Microsoft C/C++ runtime pieces NVIDIA's static NGX library (DLSS, decision 0040) expects. It is built
 * with MSVC; we build with llvm-mingw, which has neither MSVC's runtime nor its C++ library. What the library
 * reaches for, found by linking a test against it: operator new/delete (MSVC names), the floating-point marker,
 * the /GS stack cookie and its check, the two GS exception filters named in its unwind tables, two MSVC STL
 * error functions, and thread-safe local statics. It throws no C++ exceptions (no __CxxFrameHandler). Empty
 * stand-ins for libmsvcprt.a and libOLDNAMES.a (recomp/build.py) satisfy its default-library requests. */
#if defined(YW4_DLSS) && defined(_WIN32)  /* NVIDIA's Linux library is built with GCC and needs none of this */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

void *yw4_msvc_new(size_t n) __asm__("??2@YAPEAX_K@Z");
void *yw4_msvc_new(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) abort();
    return p;
}
void yw4_msvc_delete(void *p) __asm__("??3@YAXPEAX@Z");
void yw4_msvc_delete(void *p) { free(p); }

int _fltused = 0x9875;

/* /GS: the cookie the library's functions copy onto their stack and check on return */
uintptr_t __security_cookie = 0x2B992DDFA232ull;
void __security_check_cookie(uintptr_t c) {
    if (c != __security_cookie) {
        fputs("yw4: dlss: stack cookie mismatch in NVIDIA's library\n", stderr);
        abort();
    }
}

/* only reached while an exception unwinds through the library's frames: let it pass on */
EXCEPTION_DISPOSITION __GSHandlerCheck(void *rec, void *frame, void *ctx, void *disp) {
    (void)rec, (void)frame, (void)ctx, (void)disp;
    return ExceptionContinueSearch;
}
EXCEPTION_DISPOSITION __GSHandlerCheck_EH(void *rec, void *frame, void *ctx, void *disp) {
    (void)rec, (void)frame, (void)ctx, (void)disp;
    return ExceptionContinueSearch;
}

static void xlength(const char *m) {
    fprintf(stderr, "yw4: dlss: length error in NVIDIA's library: %s\n", m);
    abort();
}
static void xbadalloc(void) {
    fputs("yw4: dlss: out of memory in NVIDIA's library\n", stderr);
    abort();
}
void (*yw4_imp_xlength)(const char *) __asm__("__imp_?_Xlength_error@std@@YAXPEBD@Z") = xlength;
void (*yw4_imp_xbadalloc)(void) __asm__("__imp_?_Xbad_alloc@std@@YAXXZ") = xbadalloc;

/* Thread-safe local statics, as MSVC's runtime does them. The compiled code calls the header when the static's guard
 * is newer than this thread's epoch, then runs the initialiser itself if the guard is -1. So the header: waits while
 * another thread initialises it (-1), claims it (0 -> -1) and returns WITHOUT the lock, or sees it done. The lock is
 * never held while an initialiser runs: one static's initialiser may need another (nested), which deadlocked when
 * the header kept the lock; and a second thread used to get -1 back and run the initialiser again (code audit,
 * 2026-11-24). */
static SRWLOCK init_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE init_done = CONDITION_VARIABLE_INIT;
static int init_epoch = INT32_MIN;
__thread int _Init_thread_epoch = INT32_MIN;
void _Init_thread_header(int *once) {
    AcquireSRWLockExclusive(&init_lock);
    while (*once == -1) SleepConditionVariableSRW(&init_done, &init_lock, INFINITE, 0);
    if (*once == 0) *once = -1;               /* ours: the caller runs the initialiser, then the footer */
    else _Init_thread_epoch = init_epoch;     /* done: this thread skips the header for it from now on */
    ReleaseSRWLockExclusive(&init_lock);
}
void _Init_thread_footer(int *once) {
    AcquireSRWLockExclusive(&init_lock);
    *once = ++init_epoch;
    _Init_thread_epoch = init_epoch;
    ReleaseSRWLockExclusive(&init_lock);
    WakeAllConditionVariable(&init_done);
}
void _Init_thread_abort(int *once) {  /* the initialiser threw: back to "not yet", and wake the waiters to retry */
    AcquireSRWLockExclusive(&init_lock);
    *once = 0;
    ReleaseSRWLockExclusive(&init_lock);
    WakeAllConditionVariable(&init_done);
}
#endif
