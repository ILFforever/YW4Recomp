/* The few Microsoft C/C++ runtime pieces NVIDIA's static NGX library (DLSS, decision 0040) expects. It is built
 * with MSVC; we build with llvm-mingw, which has neither MSVC's runtime nor its C++ library. What the library
 * reaches for, found by linking a test against it: operator new/delete (MSVC names), the floating-point marker,
 * the /GS stack cookie and its check, the two GS exception filters named in its unwind tables, two MSVC STL
 * error functions, and thread-safe local statics. It throws no C++ exceptions (no __CxxFrameHandler). Empty
 * stand-ins for libmsvcprt.a and libOLDNAMES.a (recomp/build.py) satisfy its default-library requests. */
#ifdef YW4_DLSS
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

/* Thread-safe local statics: the header claims a static for initialisation (0: not yet) and holds one lock
 * until the footer marks it done; a static already done (> 0) passes. Few, one-time: one lock for all. */
static SRWLOCK init_lock = SRWLOCK_INIT;
static int init_epoch = INT32_MIN;
__thread int _Init_thread_epoch = INT32_MIN;
void _Init_thread_header(int *once) {
    AcquireSRWLockExclusive(&init_lock);
    if (*once == 0) {
        *once = -1;
        return;
    }
    ReleaseSRWLockExclusive(&init_lock);
}
void _Init_thread_footer(int *once) {
    *once = ++init_epoch;
    _Init_thread_epoch = init_epoch;
    ReleaseSRWLockExclusive(&init_lock);
}
#endif
