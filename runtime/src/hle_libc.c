/* The sdk's C library (musl, "nnmusl") as main sees it (decision 0012).
 *
 * __nnmusl_init_dso / __nnmusl_fini_dso are called by main's DT_INIT / DT_FINI with the
 * module's base and section bounds. In the sdk they register main as a module and give it a
 * TLS block in the calling thread. main never reads the thread pointer (no MRS at all,
 * decision 0008), so it has no use for that block: registering it is all bookkeeping the
 * native runtime doesn't need.
 */
#include <inttypes.h>

#include "hle.h"

/* What main's DT_INIT does with the result (main+0x30): 1 = "not now", constructors are
 * skipped and run on the next DT_INIT call; anything else = run the constructors now. The
 * sdk returns only 1 or 2. */
#define INIT_DSO_RUN_CONSTRUCTORS 2

/* int __nnmusl_init_dso(module base, ...section bounds) */
void hle_libc_nnmusl_init_dso(CPUState *ctx) {
    if (ctx->x[0] != yw4_image_base)
        yw4_hle_stop(ctx, "__nnmusl_init_dso for a module at %#" PRIx64 ": only main is loaded", ctx->x[0]);
    ctx->x[0] = INIT_DSO_RUN_CONSTRUCTORS;
}

/* __nnmusl_fini_dso(module base, ...): main's .fini_array is empty, nothing to undo. */
void hle_libc_nnmusl_fini_dso(CPUState *ctx) { (void)ctx; }

/* Function-local statics (Itanium C++ ABI 3.3.2). The guard is 64-bit; the game's inline
 * fast path tests bit 0 of its first byte with an acquire load (ldarb + tbz #0), so that
 * byte = 1 means "initialized". The second byte marks an initialization in progress.
 * One guest thread so far: when threads come, acquire must wait for another thread's
 * initialization instead of treating it as recursion. */

/* int __cxa_guard_acquire(uint64_t *guard): 1 = caller must initialize, 0 = already done */
void hle_libc_cxa_guard_acquire(CPUState *ctx) {
    uint8_t *g = GPTR(ctx->x[0]);
    if (__atomic_load_n(&g[0], __ATOMIC_ACQUIRE) & 1) {
        ctx->x[0] = 0;
        return;
    }
    if (g[1]) yw4_hle_stop(ctx, "static local at %#" PRIx64 " initialized recursively", ctx->x[0]);
    g[1] = 1;
    ctx->x[0] = 1;
}

/* void __cxa_guard_release(uint64_t *guard) */
void hle_libc_cxa_guard_release(CPUState *ctx) {
    uint8_t *g = GPTR(ctx->x[0]);
    g[1] = 0;
    __atomic_store_n(&g[0], 1, __ATOMIC_RELEASE);
}
