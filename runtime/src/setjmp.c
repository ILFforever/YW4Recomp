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

#include "hle.h"
#include "kernel.h"
#include "yw4load.h"
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

/* Diagnostic: a longjmp in this game is Lua's luaD_throw. The protected call that catches
 * it keeps its lua_State in a callee-saved register (restored by the longjmp), and the
 * error message is the value on top of that state's stack. Lua 5.2, 64-bit: lua_State.top
 * at +0x10; a TValue is {value, int tt} (16 bytes); a string has tt 0x44 (short) or 0x54
 * (long) and its characters at +0x18. Prints the message if a register looks like that. */
static int guest_ok(uint64_t a) { return a >= YW4_GUEST_LO && a < YW4_GUEST_HI && !(a & 7); }

static void lua_error_message(const CPUState *ctx) {
    /* luaD_rawrunprotected keeps L in its frame, and L->errorJmp (+0x70) still points into
     * that frame when the throw arrives: look for such a word in the frame. */
    for (uint64_t at = ctx->sp; at < ctx->sp + 0x200; at += 8) {
        uint64_t L = *(const uint64_t *)GPTR(at);
        if (!guest_ok(L) || !guest_ok(L + 0x70)) continue;
        uint64_t ej = *(const uint64_t *)GPTR(L + 0x70);
        if (ej < ctx->sp || ej >= ctx->sp + 0x200) continue;
        /* lua_longjmp {previous; jmp_buf; int status}: status at +0x140 in this build.
         * 1 LUA_YIELD (a coroutine yielding from C: normal), 2 run, 3 syntax, 4 memory, 6 error handler */
        int status = *(const int32_t *)GPTR(ej + 0x140);
        fprintf(stderr, "yw4:   Lua throw status %d\n", status);
        if (status == 1) return;
        uint64_t top = *(const uint64_t *)GPTR(L + 0x10);
        if (!guest_ok(top) || !guest_ok(top - 16)) continue;
        uint64_t ts = *(const uint64_t *)GPTR(top - 16);
        uint32_t tt = *(const uint32_t *)GPTR(top - 8);
        if ((tt != 0x44 && tt != 0x54) || !guest_ok(ts)) continue;
        uint64_t len = *(const uint64_t *)GPTR(ts + 0x10);
        if (len > 400) len = 400;
        fprintf(stderr, "yw4:   Lua error (lua_State %#llx): %.*s\n", (unsigned long long)L, (int)len,
                (const char *)GPTR(ts + 0x18));
        return;
    }
    fprintf(stderr, "yw4:   (no Lua error message found)\n");
}

void yw4_setjmp_resume(uint64_t target) {
    if (!thread_ctx) return;
    for (int i = npoints - 1; i >= 0; i--)
        if (points[i].ret == target && points[i].sp == thread_ctx->sp) {
            /* YW4_TRACE_LONGJMP=1: report longjmps (in this game, Lua's luaD_throw: errors,
             * and coroutine yields from C) with the Lua status and error message */
            static int trace = -1;
            if (trace < 0) trace = getenv("YW4_TRACE_LONGJMP") && *getenv("YW4_TRACE_LONGJMP") != '0';
            static long count;
            char where[48];
            if (trace && (count++ < 20 || count % 1000 == 0)) {
                fprintf(stderr, "yw4: longjmp #%ld to %s\n", count, yw4_describe(target, where));
                lua_error_message(thread_ctx);
            }
            npoints = i + 1;  /* the frames below are being left */
            longjmp(*points[i].jb, 1);
        }
}
