/* Indirect calls and traps for recompiled code (decision 0007). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "yw4rt.h"

GuestFn (*yw4_lookup_hook)(uint64_t addr);

GuestFn yw4_lookup(uint64_t addr) {
    if (yw4_lookup_hook) {
        GuestFn fn = yw4_lookup_hook(addr);
        if (fn) return fn;
    }
    uint64_t off = addr - yw4_image_base;
    size_t lo = 0, hi = yw4_nfuncs;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (yw4_funcs[mid].offset < off) lo = mid + 1;
        else hi = mid;
    }
    return lo < yw4_nfuncs && yw4_funcs[lo].offset == off ? yw4_funcs[lo].fn : NULL;
}

GuestFn yw4_resolve(uint64_t target) {
    GuestFn fn = yw4_lookup(target);
    if (!fn) {
        fprintf(stderr, "yw4: indirect call to %#" PRIx64 " (image offset %#" PRIx64
                "): no translated function there\n", target, target - yw4_image_base);
        abort();
    }
    return fn;
}

_Noreturn void yw4_unimplemented(CPUState *ctx, uint64_t pc, uint32_t enc) {
    (void)ctx;
    fprintf(stderr, "yw4: untranslated instruction %08" PRIx32 " at image offset %#" PRIx64 "\n",
            enc, pc - yw4_image_base);
    abort();
}

_Noreturn void yw4_guest_trap(CPUState *ctx, uint64_t pc, uint32_t enc) {
    (void)ctx;
    fprintf(stderr, "yw4: guest trap %08" PRIx32 " at image offset %#" PRIx64 "\n",
            enc, pc - yw4_image_base);
    abort();
}
