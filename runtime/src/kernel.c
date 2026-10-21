/* The Horizon kernel as the recompiled sdk sees it (decision 0014).
 *
 * The sdk talks to the kernel only through SVC instructions: arguments in x0-x7, result
 * code in w0, outputs in x1.. (Switchbrew "SVC"). Each one is implemented when the game
 * first reaches it; an unimplemented SVC stops with its number, as unimplemented imports
 * used to. LAS Recomp's kernel (github.com/peiikai/LASRecomp, runtime/kernel.c, MIT) was the
 * reference for argument layouts and result codes.
 *
 * Single guest thread so far. YW4_TRACE_SVC=1 prints every call.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <time.h>

#include "hle.h"
#include "kernel.h"
#include "yw4load.h"

/* Result codes (module 1, the kernel) */
#define R_OK 0u
#define R_INVALID_SIZE 0xCA01u
#define R_INVALID_ADDRESS 0xCC01u
#define R_OUT_OF_MEMORY 0xD001u
#define R_INVALID_HANDLE 0xE401u
#define R_INVALID_ENUM 0xF001u
#define R_TIMEOUT 0xEA01u

#define WAITERS_BIT 0x40000000u  /* in a mutex word: other threads are waiting */

#define PSEUDO_CURRENT_THREAD 0xFFFF8000u
#define PSEUDO_CURRENT_PROCESS 0xFFFF8001u

#define PROGRAM_ID 0x010086C00AF7C000ull  /* Yo-kai Watch 4++ (CLAUDE.md) */
#define TOTAL_MEMORY 0xCD500000ull        /* application pool of a 4 GiB Switch */

/* ---- memory map, for svcQueryMemory */

typedef struct {
    uint64_t addr, size;
    uint32_t type, perm;
} Region;

#define MAX_REGIONS 256
static Region regions[MAX_REGIONS];
static int nregions;

static void region_add(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm) {
    if (nregions == MAX_REGIONS) {
        fprintf(stderr, "yw4: kernel: too many memory regions\n");
        exit(3);
    }
    int i = nregions++;
    while (i > 0 && regions[i - 1].addr > addr) {  /* kept sorted by address */
        regions[i] = regions[i - 1];
        i--;
    }
    regions[i] = (Region){addr, size, type, perm};
}

static void region_remove(uint64_t addr) {
    for (int i = 0; i < nregions; i++)
        if (regions[i].addr == addr) {
            memmove(&regions[i], &regions[i + 1], (size_t)(nregions - i - 1) * sizeof *regions);
            nregions--;
            return;
        }
}

/* svcQueryMemory's MemoryInfo: the region containing addr, or the unmapped gap around it. */
static void query_memory(uint64_t addr, uint64_t out) {
    struct {
        uint64_t addr, size;
        uint32_t type, attr, perm, ipc_refcount, device_refcount, pad;
    } info = {0};
    uint64_t gap_lo = 0, gap_hi = 1ull << 39;
    for (int i = 0; i < nregions; i++) {
        const Region *r = &regions[i];
        if (addr - r->addr < r->size) {
            info.addr = r->addr, info.size = r->size, info.type = r->type, info.perm = r->perm;
            memcpy(GPTR(out), &info, sizeof info);
            return;
        }
        if (r->addr + r->size <= addr) gap_lo = r->addr + r->size;
        else if (r->addr < gap_hi) gap_hi = r->addr;
    }
    info.addr = gap_lo, info.size = gap_hi - gap_lo;  /* MEM_UNMAPPED, no permissions */
    memcpy(GPTR(out), &info, sizeof info);
}

/* ---- heap */

static uint64_t heap_size;

static uint32_t set_heap_size(uint64_t size) {
    static int reserved;
    if (size % 0x200000 || size > YW4_HEAP_SIZE) return R_INVALID_SIZE;
    if (!reserved) {
        if (!yw4_reserve_range(YW4_HEAP_BASE, YW4_HEAP_SIZE)) {
            fprintf(stderr, "yw4: kernel: cannot reserve the heap region at %#llx\n", (unsigned long long)YW4_HEAP_BASE);
            exit(3);
        }
        reserved = 1;
    }
    if (size > heap_size && yw4_commit(YW4_HEAP_BASE + heap_size, size - heap_size)) return R_OUT_OF_MEMORY;
    if (size < heap_size) yw4_decommit(YW4_HEAP_BASE + size, heap_size - size);
    heap_size = size;
    region_remove(YW4_HEAP_BASE);
    if (size) region_add(YW4_HEAP_BASE, size, MEM_HEAP, PERM_RW);
    return R_OK;
}

/* ---- handles */

typedef enum { KOBJ_THREAD = 1 } KType;
typedef struct {
    KType type;
} KObject;

typedef struct {
    KObject obj;
    uint64_t id;
    int32_t priority, core;
} KThread;

#define MAX_HANDLES 4096
static KObject *handles[MAX_HANDLES];
static KThread main_thread = {{KOBJ_THREAD}, 1, 0, 0};

static uint32_t handle_new(KObject *o) {
    for (uint32_t i = 1; i < MAX_HANDLES; i++)
        if (!handles[i]) {
            handles[i] = o;
            return 0x100 + i;
        }
    fprintf(stderr, "yw4: kernel: out of handles\n");
    exit(3);
}

static KObject *handle_get(uint32_t h) {
    if (h == PSEUDO_CURRENT_THREAD) return &main_thread.obj;  /* one guest thread so far */
    return h - 0x100 < MAX_HANDLES ? handles[h - 0x100] : NULL;
}

static KThread *thread_get(uint32_t h) {
    KObject *o = handle_get(h);
    return o && o->type == KOBJ_THREAD ? (KThread *)o : NULL;
}

uint32_t yw4_kernel_init(uint64_t stack, uint64_t stack_size, uint64_t tlr) {
    for (size_t i = 0; i < yw4_nmodules; i++)
        region_add(*yw4_modules[i].base, yw4_modules[i].mem_size, MEM_CODE_STATIC, PERM_RX);
    region_add(stack, stack_size, MEM_STACK, PERM_RW);
    region_add(tlr, 0x1000, MEM_THREAD_LOCAL, PERM_RW);
    main_thread.priority = (int32_t)yw4_main_priority;
    main_thread.core = 0;  /* the NPDM's default core */
    return handle_new(&main_thread.obj);
}

/* ---- synchronisation, single guest thread
 *
 * The sdk's mutexes and condition variables live in guest memory and only call the kernel
 * when another thread is involved: ArbitrateLock when the word shows an owner with waiters,
 * ArbitrateUnlock when it shows waiters, the condition-variable key when someone waits.
 * With one thread there is never anyone to wake; a wait with no timeout would never end,
 * so it stops instead. Real blocking comes with threads. */

static uint32_t *u32p(uint64_t a) { return GPTR(a); }

static void sleep_ns(int64_t ns) {
    if (ns <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)((ns + 999999) / 1000000));  /* millisecond resolution; precise waits come with threads */
#else
    struct timespec ts = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    nanosleep(&ts, NULL);
#endif
}

static _Noreturn void deadlock(const char *what, uint64_t addr) {
    fprintf(stderr, "yw4: kernel: %s at %#" PRIx64 " would wait forever: only one guest thread exists\n", what, addr);
    exit(3);
}

/* ---- the system calls */

static uint64_t entropy(uint64_t which) {
    /* Fixed for now, so runs are reproducible; real entropy can come later. */
    return 0x9E3779B97F4A7C15ull * (which + 1) ^ 0x5D5E2B1C3A4F6E7Dull;
}

static uint32_t get_info(uint32_t id0, uint32_t handle, uint64_t id1, uint64_t *out) {
    (void)handle;
    switch (id0) {
    case 0: *out = 0xF; return R_OK;                         /* CoreMask */
    case 1: *out = 0xFFFFFFFFFFFFFFFFull; return R_OK;       /* PriorityMask */
    case 2: *out = YW4_ALIAS_BASE; return R_OK;
    case 3: *out = YW4_ALIAS_SIZE; return R_OK;
    case 4: *out = YW4_HEAP_BASE; return R_OK;
    case 5: *out = YW4_HEAP_SIZE; return R_OK;
    case 6: case 21: *out = TOTAL_MEMORY; return R_OK;      /* Total(NonSystem)MemorySize */
    case 7: case 22: {                                      /* Used(NonSystem)MemorySize */
        uint64_t used = heap_size;
        for (size_t i = 0; i < yw4_nmodules; i++) used += yw4_modules[i].mem_size;
        *out = used + yw4_main_stack_size;
        return R_OK;
    }
    case 8: *out = 0; return R_OK;                           /* DebuggerAttached */
    case 9: *out = 0; return R_OK;                           /* ResourceLimit: none */
    case 11: *out = entropy(id1); return R_OK;               /* RandomEntropy, subtype 0..3 */
    case 12: *out = YW4_GUEST_LO; return R_OK;               /* AslrRegion */
    case 13: *out = YW4_GUEST_HI - YW4_GUEST_LO; return R_OK;
    case 14: *out = YW4_STACK_BASE; return R_OK;             /* StackRegion */
    case 15: *out = YW4_STACK_SIZE; return R_OK;
    case 16: case 17: *out = 0; return R_OK;                 /* SystemResourceSize total/used */
    case 18: *out = PROGRAM_ID; return R_OK;
    case 20: *out = 0; return R_OK;                          /* UserExceptionContextAddress */
    default:
        fprintf(stderr, "yw4: kernel: GetInfo(%u, %#x, %#" PRIx64 ") not implemented\n", id0, handle, id1);
        exit(3);
    }
}

static void svc(CPUState *ctx, uint32_t imm) {
    uint64_t *x = ctx->x;
    switch (imm) {
    case 0x01: {  /* SetHeapSize(_, size) -> address */
        uint32_t r = set_heap_size(x[1]);
        x[0] = r;
        x[1] = r == R_OK ? YW4_HEAP_BASE : 0;
        return;
    }
    case 0x06:  /* QueryMemory(MemoryInfo *out, PageInfo *_, addr) -> page info */
        query_memory(x[2], x[0]);
        x[0] = R_OK;
        x[1] = 0;
        return;
    case 0x0B:  /* SleepThread(ns): 0 / -1 / -2 are yields */
        sleep_ns((int64_t)x[0]);
        return;
    case 0x0C: {  /* GetThreadPriority(_, handle) -> priority */
        KThread *t = thread_get((uint32_t)x[1]);
        x[0] = t ? R_OK : R_INVALID_HANDLE;
        x[1] = t ? (uint64_t)t->priority : 0;
        return;
    }
    case 0x0D: {  /* SetThreadPriority(handle, priority) */
        KThread *t = thread_get((uint32_t)x[0]);
        if (t) t->priority = (int32_t)x[1];
        x[0] = t ? R_OK : R_INVALID_HANDLE;
        return;
    }
    case 0x0E: {  /* GetThreadCoreMask(_, _, handle) -> ideal core, affinity mask */
        KThread *t = thread_get((uint32_t)x[2]);
        x[0] = t ? R_OK : R_INVALID_HANDLE;
        x[1] = t ? (uint64_t)(uint32_t)t->core : 0;
        x[2] = 0x7;  /* cores 0-2: an application's */
        return;
    }
    case 0x0F: {  /* SetThreadCoreMask(handle, ideal core, mask): recorded, not enforced */
        KThread *t = thread_get((uint32_t)x[0]);
        if (t && (int32_t)x[1] >= 0) t->core = (int32_t)x[1];
        x[0] = t ? R_OK : R_INVALID_HANDLE;
        return;
    }
    case 0x10:  /* GetCurrentProcessorNumber */
        x[0] = (uint64_t)main_thread.core;
        return;
    case 0x1A:  /* ArbitrateLock(owner handle, mutex, requester): wait unless already released */
        if (__atomic_load_n(u32p(x[1]), __ATOMIC_SEQ_CST) == ((uint32_t)x[0] | WAITERS_BIT)) deadlock("ArbitrateLock", x[1]);
        x[0] = R_OK;
        return;
    case 0x1B:  /* ArbitrateUnlock(mutex): no waiter to hand it to */
        __atomic_store_n(u32p(x[0]), 0, __ATOMIC_SEQ_CST);
        x[0] = R_OK;
        return;
    case 0x1C: {  /* WaitProcessWideKeyAtomic(mutex, key, tag, timeout ns) */
        uint64_t mutex = x[0], key = x[1];
        uint32_t tag = (uint32_t)x[2];
        int64_t timeout = (int64_t)x[3];
        if (timeout < 0) deadlock("WaitProcessWideKeyAtomic", key);
        __atomic_store_n(u32p(mutex), 0, __ATOMIC_SEQ_CST);  /* release the mutex */
        __atomic_store_n(u32p(key), 1, __ATOMIC_SEQ_CST);    /* "someone waits" */
        sleep_ns(timeout);
        uint32_t expected = 0;                                /* take the mutex back */
        if (!__atomic_compare_exchange_n(u32p(mutex), &expected, tag, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            deadlock("WaitProcessWideKeyAtomic (re-lock)", mutex);
        x[0] = R_TIMEOUT;
        return;
    }
    case 0x1D:  /* SignalProcessWideKey(key, count): nobody waits, so the key goes back to 0 */
        __atomic_store_n(u32p(x[0]), 0, __ATOMIC_SEQ_CST);
        x[0] = R_OK;
        return;
    case 0x1E:  /* GetSystemTick */
        x[0] = yw4_cntpct();
        return;
    case 0x25: {  /* GetThreadId(_, handle) -> id */
        KThread *t = thread_get((uint32_t)x[1]);
        x[0] = t ? R_OK : R_INVALID_HANDLE;
        x[1] = t ? t->id : 0;
        return;
    }
    case 0x29: {  /* GetInfo(_, id0, handle, id1) -> value */
        uint64_t v = 0;
        x[0] = get_info((uint32_t)x[1], (uint32_t)x[2], x[3], &v);
        x[1] = v;
        return;
    }
    default: {
        char where[48];
        fprintf(stderr, "yw4: unimplemented SVC %#x at %s (x0=%#" PRIx64 " x1=%#" PRIx64 " x2=%#" PRIx64
                        " x3=%#" PRIx64 ")\n", imm, yw4_describe(x[30], where), x[0], x[1], x[2], x[3]);
        exit(3);
    }
    }
}

void yw4_svc(CPUState *ctx, uint32_t imm) {
    static int trace = -1;
    if (trace < 0) {
        const char *t = getenv("YW4_TRACE_SVC");
        trace = t && *t && *t != '0';
    }
    if (!trace) {
        svc(ctx, imm);
        return;
    }
    uint64_t in[4] = {ctx->x[0], ctx->x[1], ctx->x[2], ctx->x[3]};
    svc(ctx, imm);
    fprintf(stderr, "yw4: svc %#04x(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ") -> %#" PRIx64 " %#" PRIx64 "\n",
            imm, in[0], in[1], in[2], in[3], ctx->x[0], ctx->x[1]);
}

/* CNTPCT_EL0 counts at 19.2 MHz on the Switch. */
uint64_t yw4_cntpct(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);  /* C11; a monotonic clock comes with threads */
    unsigned __int128 ns = (unsigned __int128)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
    return (uint64_t)(ns * 192u / 10000u);
}

void yw4_msr_fpcr(CPUState *ctx, uint64_t value) {
    if (value != 0) {  /* rounding modes, flush-to-zero, default NaN: not modelled (decision 0008) */
        fprintf(stderr, "yw4: guest sets FPCR to %#" PRIx64 "; only 0 is supported\n", value);
        exit(3);
    }
    ctx->fpcr = 0;
}
