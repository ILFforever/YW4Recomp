/* Indirect calls and traps for recompiled code (decisions 0007, 0014). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
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

/* ---- probes: log calls to chosen guest functions, no rebuild needed.
 * YW4_PROBE=main+0x9f6170,sdk+0x1234,...: each recompiled function starts with
 * `if (yw4_probe_any) yw4_probe(ctx, entry)`, so a call to a listed function prints its
 * first four arguments and its caller (YW4_PROBE_LIMIT per function, default 20;
 * YW4_PROBE_BT=1 adds the call stack). Addresses must be function entries. */

enum { MAX_PROBES = 64 };
int yw4_probe_any;
static uint64_t probes[MAX_PROBES];
static long probe_hits[MAX_PROBES];
static int nprobes;
static long probe_limit = 20;
static int probe_bt;

void yw4_probe_init(void) {
    const char *s = getenv("YW4_PROBE");
    if (!s || !*s) return;
    if (getenv("YW4_PROBE_LIMIT")) probe_limit = atol(getenv("YW4_PROBE_LIMIT"));
    probe_bt = getenv("YW4_PROBE_BT") && *getenv("YW4_PROBE_BT") != '0';
    while (*s && nprobes < MAX_PROBES) {
        char mod[16] = "";
        size_t n = strcspn(s, "+,");
        if (n < sizeof mod && s[n] == '+') {
            memcpy(mod, s, n);
            s += n + 1;
        }
        char *e;
        uint64_t off = strtoull(s, &e, 16), base = 0;
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, mod)) base = *yw4_code[i].base;
        if (mod[0] && !base) fprintf(stderr, "yw4: probe: no module %s\n", mod);
        else probes[nprobes++] = base + off;
        s = *e == ',' ? e + 1 : e;
        if (e == s && *s) break;
    }
    fprintf(stderr, "yw4: probe: %d function(s)\n", nprobes);
    yw4_probe_any = nprobes > 0;
}

/* YW4_PROBE_UNIQUE=1: print a call only when its call stack (12 deep) is new: the
 * different paths that reach a function, however often it runs. Implies YW4_PROBE_BT. */
static int probe_unique = -1;

static int stack_is_new(const CPUState *ctx, uint64_t addr) {
    enum { SEEN = 8192 };
    static uint64_t seen[SEEN];
    static int nseen;
    uint64_t st[12], h = addr * 0x9E3779B97F4A7C15ull;
    int k = yw4_call_stack(ctx, st, 12);
    for (int j = 0; j < k; j++) h = (h ^ st[j]) * 0x100000001B3ull;
    for (int j = 0; j < nseen; j++)
        if (seen[j] == h) return 0;
    if (nseen < SEEN) seen[nseen++] = h;
    return 1;
}

/* YW4_WALK_LAYERS=1 (with main+0x9f6170 in YW4_PROBE): when the render thread draws a
 * layer (x2 = layer item; its draw list is *(*(item + 0x20) + 0x20), nodes linked at +0x30,
 * node[0] = draw object), print per layer every 300 calls: nodes, and how many the
 * renderer skips (layer kind 1 and object byte +0x1d1 == 0, or the node's mask byte +0x20
 * has the kind's bit). From the decompiled walker main+0xa45890. */
static void walk_layer(const CPUState *ctx) {
    static long calls;
    if (calls++ % 300 >= 8) return;  /* a few layers, now and then */
    uint64_t item = ctx->x[2];
    if (!item) return;
    uint8_t kind = *(const uint8_t *)(uintptr_t)(item + 8);
    uint64_t list = *(const uint64_t *)(uintptr_t)(item + 0x20);
    if (!list) return;
    uint64_t node = *(const uint64_t *)(uintptr_t)(list + 0x20);
    int n = 0, skipped = 0;
    for (; node && n < 100000; node = *(const uint64_t *)(uintptr_t)(node + 0x30), n++) {
        uint64_t obj = *(const uint64_t *)(uintptr_t)node;
        uint64_t o = *(const uint64_t *)(uintptr_t)(obj + 0x108) ? *(const uint64_t *)(uintptr_t)(obj + 0x108) : obj;
        if ((kind == 1 && !*(const uint8_t *)(uintptr_t)(o + 0x1d1)) ||
            ((1u << (kind & 31)) & *(const uint8_t *)(uintptr_t)(node + 0x20)))
            skipped++;
    }
    fprintf(stderr, "yw4: layer %#" PRIx64 " kind %u: %d draw objects, %d skipped by the renderer\n", item, kind, n,
            skipped);
}

/* YW4_WATCH=<hex address>:<size>: a data watch. Every function entry (probes on) compares
 * the watched bytes with their last value; a change is printed with the function being
 * entered and its call stack: the writer is that function's caller, or ran just before.
 * Slow (a check on every call), for short runs. Heap addresses repeat from run to run. */
static uint64_t watch_addr, watch_last;
static unsigned watch_size;

static void watch_check(const CPUState *ctx, uint64_t addr) {
    uint64_t v = 0;
    memcpy(&v, (const void *)(uintptr_t)watch_addr, watch_size);
    uint64_t old = __atomic_exchange_n(&watch_last, v, __ATOMIC_RELAXED);
    if (old == v) return;
    char a[48], b[48];
    fprintf(stderr, "yw4: watch %#" PRIx64 ": %#" PRIx64 " -> %#" PRIx64 " at entry of %s (from %s)\n", watch_addr, old, v,
            yw4_describe(addr, a), yw4_describe(ctx->x[30] - 4, b));
    uint64_t st[10];
    int k = yw4_call_stack(ctx, st, 10);
    for (int j = 1; j < k; j++) fprintf(stderr, "       %s\n", yw4_describe(st[j] - 4, a));
}

/* YW4_WATCHW=<hex address>:<size>: a real write watchpoint (host page protection,
 * Windows): every write reports the writing guest thread's call stack. Armed once the
 * address is mapped (checked at function entries). */
static uint64_t watchw_addr, watchw_size;

void yw4_watch_init(void) {
    const char *w = getenv("YW4_WATCHW");
    if (w && *w) {
        char *e;
        watchw_addr = strtoull(w, &e, 16);
        watchw_size = *e == ':' ? strtoull(e + 1, NULL, 10) : 8;
        yw4_probe_any = 1;
    }
    const char *s = getenv("YW4_WATCH");
    if (!s || !*s) return;
    char *e;
    watch_addr = strtoull(s, &e, 16);
    watch_size = *e == ':' ? (unsigned)strtoul(e + 1, NULL, 10) : 8;
    if (watch_size > 8) watch_size = 8;
    yw4_probe_any = 1;
    fprintf(stderr, "yw4: watch %#" PRIx64 " (%u bytes)\n", watch_addr, watch_size);
}

void yw4_probe(CPUState *ctx, uint64_t addr) {
    if (watchw_size) {
        static unsigned tries;
        if (tries++ % 4096 == 0 && host_readable(watchw_addr, watchw_size)) {
            yw4_write_watch_arm(watchw_addr, watchw_size);
            watchw_size = 0;
        }
    }
    /* the watched memory is only readable once the game has mapped it */
    if (watch_size) {
        static int readable;
        static unsigned tries;
        if (!readable && tries++ % 4096 == 0) readable = host_readable(watch_addr, watch_size);
        if (readable) watch_check(ctx, addr);
    }
    static int walk = -1;
    if (walk < 0) walk = getenv("YW4_WALK_LAYERS") && *getenv("YW4_WALK_LAYERS") != '0';
    if (walk && probe_hits[0] >= 0) {
        char a[48];
        if (!strcmp(yw4_describe(addr, a), "main+0x9f6170")) walk_layer(ctx);
    }
    if (probe_unique < 0) {
        probe_unique = getenv("YW4_PROBE_UNIQUE") && *getenv("YW4_PROBE_UNIQUE") != '0';
        if (probe_unique) probe_bt = 1;
    }
    for (int i = 0; i < nprobes; i++) {
        if (probes[i] != addr) continue;
        long n = __atomic_add_fetch(&probe_hits[i], 1, __ATOMIC_RELAXED);
        if (probe_unique ? !stack_is_new(ctx, addr) : n > probe_limit) return;
        char a[48], b[48];
        fprintf(stderr, "yw4: probe %s #%ld from %s: x0=%#" PRIx64 " x1=%#" PRIx64 " x2=%#" PRIx64 " x3=%#" PRIx64 "\n",
                yw4_describe(addr, a), n, yw4_describe(ctx->x[30] - 4, b), ctx->x[0], ctx->x[1], ctx->x[2], ctx->x[3]);
        /* YW4_PROBE_MEM=x0+0xf4:1,x0+0xa0:8,...: also print memory at register + offset
         * (size 1, 2, 4 or 8 bytes), to see the fields a function tests */
        static const char *mem = (const char *)1;
        if (mem == (const char *)1) mem = getenv("YW4_PROBE_MEM");
        /* pointer chains: x0+8*+0x78:2 = 2 bytes at (the pointer at x0+8) + 0x78 */
        for (const char *m = mem; m && *m;) {
            char *e;
            const char *start = m;
            uint64_t p = 0, v = 0;
            if (*m == '@') {  /* @main+0x56b82c8:1: a module-relative address */
                char mod[16] = "";
                size_t n = strcspn(m + 1, "+");
                if (n < sizeof mod) memcpy(mod, m + 1, n);
                for (size_t i = 0; i < yw4_ncode; i++)
                    if (!strcmp(yw4_code[i].name, mod)) p = *yw4_code[i].base;
                e = (char *)m + 1 + n;
            } else if (*m == 'x') {
                unsigned reg = (unsigned)strtoul(m + 1, &e, 10);
                p = reg < 31 ? ctx->x[reg] : 0;
            } else {
                break;
            }
            int ok = 1;
            for (;;) {
                if (*e == '+') p += strtoull(e + 1, &e, 16);
                if (*e != '*') break;
                e++;
                if (p < 0x7000000000ull || p >= 0x7800000000ull) ok = 0;
                p = ok ? *(const uint64_t *)(uintptr_t)p : 0;
            }
            unsigned size = *e == ':' ? (unsigned)strtoul(e + 1, &e, 10) : 8;
            if (ok && p >= 0x7000000000ull && p < 0x7800000000ull) memcpy(&v, (const void *)(uintptr_t)p, size <= 8 ? size : 8);
            fprintf(stderr, "       [%.*s] = %#" PRIx64 "%s\n", (int)(e - start), start, v, ok ? "" : " (bad pointer)");
            m = *e == ',' ? e + 1 : e;
        }
        if (probe_bt) {
            uint64_t st[12];
            int k = yw4_call_stack(ctx, st, 12);
            for (int j = 1; j < k; j++) fprintf(stderr, "       %s\n", yw4_describe(st[j] - 4, a));
        }
        return;
    }
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
