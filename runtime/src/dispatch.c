/* Indirect calls and traps for recompiled code (decisions 0007, 0014). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel.h"
#include "yw4rt.h"

GuestFn (*yw4_lookup_hook)(uint64_t addr);

static const Yw4CodeModule *module_of(uint64_t addr) {
    for (size_t i = 0; i < yw4_ncode; i++) {
        const Yw4CodeModule *m = &yw4_code[i];
        if (*m->base && addr - *m->base < m->size) return m;
    }
    return NULL;
}

const char *yw4_describe(uint64_t addr, char buf[48]) {
    const Yw4CodeModule *m = module_of(addr);
    if (m) snprintf(buf, 48, "%s+%#" PRIx64, m->name, addr - *m->base);
    else snprintf(buf, 48, "%#" PRIx64, addr);
    return buf;
}

GuestFn yw4_lookup(uint64_t addr) {
    if (yw4_lookup_hook) {
        GuestFn fn = yw4_lookup_hook(addr);
        if (fn) return fn;
    }
    const Yw4CodeModule *m = module_of(addr);
    if (!m) return NULL;
    uint64_t off = addr - *m->base;
    size_t lo = 0, hi = m->nfuncs;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (m->funcs[mid].offset < off) lo = mid + 1;
        else hi = mid;
    }
    return lo < m->nfuncs && m->funcs[lo].offset == off ? m->funcs[lo].fn : NULL;
}

GuestFn yw4_resolve(uint64_t target) {
    GuestFn fn = yw4_lookup(target);
    if (!fn) {
        yw4_setjmp_resume(target);  /* a longjmp back to a setjmp's return address? */
        char where[48];
        fprintf(stderr, "yw4: indirect call to %s: no translated function there\n", yw4_describe(target, where));
        yw4_stop();
    }
    return fn;
}

_Noreturn void yw4_unimplemented(CPUState *ctx, uint64_t pc, uint32_t enc) {
    (void)ctx;
    char where[48];
    fprintf(stderr, "yw4: untranslated instruction %08" PRIx32 " at %s\n", enc, yw4_describe(pc, where));
    abort();
}

_Noreturn void yw4_guest_trap(CPUState *ctx, uint64_t pc, uint32_t enc) {
    (void)ctx;
    char where[48];
    fprintf(stderr, "yw4: guest trap %08" PRIx32 " at %s\n", enc, yw4_describe(pc, where));
    abort();
}
