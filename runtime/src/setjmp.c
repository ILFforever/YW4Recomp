/* setjmp and longjmp in recompiled code (decision 0018).
 *
 * The guest's setjmp saves the callee-saved registers, sp and the return address; its
 * longjmp restores them and branches to that return address. Recompiled code has no
 * function there: the return address is the middle of the function that called setjmp.
 *
 * So the translator turns each call to setjmp into a host setjmp in the calling
 * function's own C frame, registered here under (return address, guest sp). The guest
 * longjmp's final indirect branch misses in yw4_resolve, which asks yw4_setjmp_resume; a
 * registered point is jumped to with longjmp, which lands in that frame right after the
 * setjmp call. The guest registers are already the restored ones.
 *
 * The C library's setjmp/longjmp, not GCC/Clang's __builtin_ ones: on Windows x64 Clang's
 * __builtin_setjmp saves the canonical frame address but code addresses locals through rbp,
 * which sits above it in frames bigger than 128 bytes (rbp = rsp + 0x80 in f_d2cde0), so
 * after __builtin_longjmp locals were read from the wrong place. The CRT's longjmp unwinds
 * properly (Windows: RtlUnwind over the unwind data Clang emits for every function).
 *
 * Points are per thread, ordered outermost first (decreasing guest sp: the stack grows
 * down). Registering at sp S drops points deeper than S, since those frames must have
 * returned. A longjmp to a frame that has returned is undefined in C too.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel.h"
#include "yw4rt.h"

#define MAX_POINTS 256

typedef struct {
    uint64_t ret, sp;
    jmp_buf *jb;
} Point;

static _Thread_local Point points[MAX_POINTS];
static _Thread_local int npoints;
static _Thread_local CPUState *thread_ctx;

void yw4_setjmp_register(CPUState *ctx, uint64_t ret, jmp_buf jb) {
    uint64_t sp = ctx->sp;
    thread_ctx = ctx;
    while (npoints && points[npoints - 1].sp < sp) npoints--;
    for (int i = npoints - 1; i >= 0 && points[i].sp == sp; i--)
        if (points[i].ret == ret) {  /* the same call site in the same frame, again */
            points[i].jb = (jmp_buf *)jb;
            npoints = i + 1;
            return;
        }
    if (npoints == MAX_POINTS) {
        fprintf(stderr, "yw4: more than %d nested setjmp points on one thread\n", MAX_POINTS);
        yw4_stop();
    }
    points[npoints++] = (Point){ret, sp, (jmp_buf *)jb};
}

void yw4_setjmp_resume(uint64_t target) {
    if (!thread_ctx) return;
    for (int i = npoints - 1; i >= 0; i--)
        if (points[i].ret == target && points[i].sp == thread_ctx->sp) {
            npoints = i + 1;  /* the frames below are being left */
            longjmp(*points[i].jb, 1);
        }
}
