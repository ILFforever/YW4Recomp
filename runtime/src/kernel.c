/* The Horizon kernel as the recompiled sdk sees it (decisions 0014, 0015).
 *
 * The sdk talks to the kernel only through SVC instructions: arguments in x0-x7, result
 * code in w0, outputs in x1.. (Switchbrew "SVC"). Each one is implemented when the game
 * first reaches it; an unimplemented SVC stops with its number, as unimplemented imports
 * used to. LAS Recomp's kernel (github.com/peiikai/LASRecomp, runtime/kernel.c, MIT) was the
 * reference for argument layouts and result codes; Atmosphère's kernel (mesosphere) for
 * what the synchronisation calls do, step by step.
 *
 * Threads (decision 0015): every guest thread is a host thread with its own CPUState and
 * thread-local region. One lock (klock) guards all kernel and service state: every SVC
 * runs holding it, and a thread that blocks waits on its own host condition variable,
 * which releases the lock meanwhile. Guest code itself runs without the lock, truly in
 * parallel; priorities and core masks are recorded but the host schedules.
 *
 * YW4_TRACE_SVC=1 prints every call.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "host.h"
#include "ipc.h"
#include "kernel.h"
#include "yw4load.h"

/* Result codes (module 1, the kernel) */
#define R_OK 0u
#define R_INVALID_SIZE 0xCA01u
#define R_INVALID_ADDRESS 0xCC01u
#define R_OUT_OF_MEMORY 0xD001u
#define R_OUT_OF_RESOURCE 0xD201u
#define R_INVALID_PRIORITY 0xE001u
#define R_INVALID_HANDLE 0xE401u
#define R_TIMEOUT 0xEA01u
#define R_CANCELLED 0xEC01u
#define R_OUT_OF_RANGE 0xEE01u
#define R_INVALID_ENUM 0xF001u
#define R_INVALID_STATE 0xFA01u

#define WAITERS_BIT 0x40000000u  /* in a mutex word: other threads are waiting */

#define PSEUDO_CURRENT_THREAD 0xFFFF8000u
#define PSEUDO_CURRENT_PROCESS 0xFFFF8001u

#define PROGRAM_ID 0x010086C00AF7C000ull  /* Yo-kai Watch 4++ (CLAUDE.md) */
#define TOTAL_MEMORY 0xCD500000ull        /* application pool of a 4 GiB Switch */

static HostLock klock;

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
    if (size % 0x200000 || size > YW4_HEAP_SIZE) return R_INVALID_SIZE;
    if (size > heap_size && yw4_commit(YW4_HEAP_BASE + heap_size, size - heap_size)) return R_OUT_OF_MEMORY;
    if (size < heap_size) yw4_decommit(YW4_HEAP_BASE + size, heap_size - size);
    heap_size = size;
    region_remove(YW4_HEAP_BASE);
    if (size) region_add(YW4_HEAP_BASE, size, MEM_HEAP, PERM_RW);
    return R_OK;
}

/* ---- threads */

typedef enum {
    WAIT_NONE,
    WAIT_SYNC,     /* WaitSynchronization: for one of wait_objs to be signalled */
    WAIT_LOCK,     /* ArbitrateLock: for the mutex at addr to be handed over */
    WAIT_CONDVAR,  /* WaitProcessWideKeyAtomic: for a signal on the key at addr */
    WAIT_ADDRESS,  /* WaitForAddress: for SignalToAddress on addr */
} WaitKind;

#define MAX_WAIT_OBJECTS 0x40  /* Horizon's limit for WaitSynchronization */

typedef struct KThread KThread;
struct KThread {
    KObject obj;
    uint64_t id;
    int32_t priority, core;
    CPUState *ctx;
    uint64_t entry, tlr;
    int slot;            /* index of its thread-local region */
    int started, exited, handle_closed;
    HostCond cond;
    /* The current wait (klock held for all of these). */
    WaitKind wait;
    int woken;
    int forever;         /* the wait has no timeout: not counted in nrunnable meanwhile */
    uint32_t result;     /* what the wait ends with */
    int32_t index;       /* WAIT_SYNC: which object */
    uint64_t seq;        /* order of arrival, for FIFO among equal priorities */
    uint64_t addr;       /* WAIT_LOCK: mutex; WAIT_CONDVAR: key; WAIT_ADDRESS: address */
    uint64_t mutex;      /* WAIT_CONDVAR: the mutex to take back when signalled */
    uint32_t tag;        /* WAIT_LOCK/WAIT_CONDVAR: the value that marks this thread as owner */
    int cancel_pending;  /* CancelSynchronization while not in WAIT_SYNC */
    KObject *objs[MAX_WAIT_OBJECTS];
    int32_t nobjs;
    KThread *next;       /* all threads */
};

/* Thread-local regions: one 4 KiB page per thread slot (Horizon packs 0x200-byte regions
 * eight to a page; a page each is simpler and the guest can't tell). */
#define MAX_THREADS 256
#define TLR_STRIDE 0x1000ull
static KThread *slots[MAX_THREADS];

#define GUEST_THREAD_HOST_STACK (16u << 20)  /* host stack per guest thread: reserved, not committed */

static KThread *threads;            /* every thread not yet freed */
static KThread main_thread;
static uint64_t next_thread_id = 1, next_seq;
static int nrunnable;               /* started, not exited, not in a wait without timeout */
static _Thread_local KThread *cur;  /* the guest thread this host thread runs */

/* ---- handles */

#define MAX_HANDLES 4096
static KObject *handles[MAX_HANDLES];

uint32_t yw4_handle_new(KObject *o) {
    for (uint32_t i = 1; i < MAX_HANDLES; i++)
        if (!handles[i]) {
            handles[i] = o;
            return 0x100 + i;
        }
    fprintf(stderr, "yw4: kernel: out of handles\n");
    exit(3);
}

KObject *yw4_handle_get(uint32_t h) {
    if (h == PSEUDO_CURRENT_THREAD) return &cur->obj;
    return h - 0x100 < MAX_HANDLES ? handles[h - 0x100] : NULL;
}

void yw4_handle_close(uint32_t h) {
    if (h - 0x100 < MAX_HANDLES) handles[h - 0x100] = NULL;
}

static KThread *thread_get(uint32_t h) {
    KObject *o = yw4_handle_get(h);
    return o && o->type == KOBJ_THREAD ? (KThread *)o : NULL;
}

static uint32_t *u32p(uint64_t a) { return GPTR(a); }

/* The guest call stack, from the frame-pointer chain (x29 -> [previous x29, return address]). */
static void backtrace(const CPUState *ctx) {
    char where[48];
    uint64_t fp = ctx->x[29];
    fprintf(stderr, "     guest backtrace:\n");
    for (int i = 0; i < 16 && fp >= YW4_GUEST_LO && fp < YW4_GUEST_HI && !(fp & 7); i++) {
        uint64_t ret = *(const uint64_t *)GPTR(fp + 8);
        if (!ret) break;
        fprintf(stderr, "       %s\n", yw4_describe(ret - 4, where));
        fp = *(const uint64_t *)GPTR(fp);
    }
}

void yw4_kernel_lock(void) { host_lock(&klock); }
void yw4_kernel_unlock(void) { host_unlock(&klock); }

_Noreturn void yw4_stop(void) {
    fprintf(stderr, "     on thread %" PRIu64 "\n", cur->id);
    backtrace(cur->ctx);
    exit(3);
}

/* ---- waiting and waking (klock held) */

static int is_signaled(const KObject *o) {
    if (o->type == KOBJ_EVENT) return ((const KEvent *)o)->signaled;
    return ((const KThread *)o)->exited;  /* KOBJ_THREAD: WaitSynchronization only accepts these two */
}

/* Ends t's wait with `result`; it runs again once it gets klock. */
static void end_wait(KThread *t, uint32_t result) {
    if (t->forever) {  /* runnable from now on, even before it gets klock back */
        t->forever = 0;
        nrunnable++;
    }
    t->wait = WAIT_NONE;
    t->result = result;
    t->woken = 1;
    host_cond_signal(&t->cond);
}

/* t would be served before u: higher priority (lower number), then first come. */
static int before(const KThread *t, const KThread *u) {
    return t->priority != u->priority ? t->priority < u->priority : t->seq < u->seq;
}

/* The best thread waiting as `kind` on `addr`, and how many wait so (NULL, 0 if none). */
static KThread *first_waiter(WaitKind kind, uint64_t addr, int *count) {
    KThread *best = NULL;
    int n = 0;
    for (KThread *t = threads; t; t = t->next)
        if (t->wait == kind && t->addr == addr) {
            n++;
            if (!best || before(t, best)) best = t;
        }
    if (count) *count = n;
    return best;
}

static const char *const wait_names[] = {"running", "WaitSynchronization", "ArbitrateLock (mutex)",
                                         "WaitProcessWideKeyAtomic (condition variable)", "WaitForAddress"};

/* Every guest thread waits for something only another guest thread could do: nothing will
 * ever run again. (Waits on host-signalled events, like vsync, don't count: a host thread
 * ends them.) So this is a real deadlock, or a missing kernel/service feature: stop and show
 * where everyone is. */
static _Noreturn void all_blocked(void) {
    fprintf(stderr, "yw4: kernel: every guest thread waits forever:\n");
    for (KThread *t = threads; t; t = t->next) {
        if (!t->started || t->exited) continue;
        fprintf(stderr, "   thread %" PRIu64 " (priority %d): %s", t->id, t->priority, wait_names[t->wait]);
        if (t->wait != WAIT_NONE && t->wait != WAIT_SYNC) fprintf(stderr, " at %#" PRIx64, t->addr);
        fprintf(stderr, "\n");
        backtrace(t->ctx);
    }
    exit(3);
}

/* Blocks the current thread in the wait it has set up, for at most timeout_ns (< 0: no
 * limit). Returns the result its waker gave, or R_TIMEOUT. host_can_wake: a host thread may
 * end this wait (a host-signalled event), so it doesn't count as blocked forever. */
static uint32_t wait_current_ex(int64_t timeout_ns, int host_can_wake) {
    KThread *t = cur;
    t->woken = 0;
    t->seq = next_seq++;
    int forever = timeout_ns < 0;
    int64_t deadline = forever ? 0 : host_now_ns() + timeout_ns;
    t->forever = forever && !host_can_wake;
    if (t->forever && --nrunnable == 0) all_blocked();
    while (!t->woken) {
        int64_t left = forever ? -1 : deadline - host_now_ns();
        if (!forever && left <= 0) break;
        host_cond_wait(&t->cond, &klock, left);
    }
    if (!t->woken) {  /* timed out: leaving whatever list it was on is just this */
        t->wait = WAIT_NONE;
        t->result = R_TIMEOUT;
    }
    return t->result;
}

static uint32_t wait_current(int64_t timeout_ns) { return wait_current_ex(timeout_ns, 0); }

/* An object became signalled: wake everyone waiting on it. */
static void wake_sync_waiters(const KObject *o) {
    for (KThread *t = threads; t; t = t->next)
        if (t->wait == WAIT_SYNC)
            for (int32_t i = 0; i < t->nobjs; i++)
                if (t->objs[i] == o) {
                    t->index = i;
                    end_wait(t, R_OK);
                    break;
                }
}

static uint32_t wait_synchronization(uint64_t handles_addr, int32_t n, int64_t timeout, int32_t *index) {
    KThread *t = cur;
    if (n < 0 || n > MAX_WAIT_OBJECTS) return R_OUT_OF_RANGE;
    const uint32_t *hs = GPTR(handles_addr);
    for (int32_t i = 0; i < n; i++) {
        KObject *o = yw4_handle_get(hs[i]);
        if (!o) return R_INVALID_HANDLE;
        if (o->type != KOBJ_EVENT && o->type != KOBJ_THREAD) {
            fprintf(stderr, "yw4: kernel: WaitSynchronization on a handle of kind %d: not implemented\n", o->type);
            exit(3);
        }
        t->objs[i] = o;
    }
    if (t->cancel_pending) {
        t->cancel_pending = 0;
        return R_CANCELLED;
    }
    for (int32_t i = 0; i < n; i++)
        if (is_signaled(t->objs[i])) {
            *index = i;
            return R_OK;
        }
    if (timeout == 0) return R_TIMEOUT;
    int host_can_wake = 0;
    for (int32_t i = 0; i < n; i++)
        if (t->objs[i]->type == KOBJ_EVENT && ((KEvent *)t->objs[i])->host_signalled) host_can_wake = 1;
    t->nobjs = n;
    t->wait = WAIT_SYNC;
    uint32_t r = wait_current_ex(timeout, host_can_wake);
    *index = r == R_OK ? t->index : -1;
    return r;
}

/* ---- the thread's life */

static uint64_t tlr_address(int slot) { return YW4_TLR_BASE + (uint64_t)slot * TLR_STRIDE; }

static int tlr_alloc(KThread *t) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (!slots[i]) {
            if (yw4_commit(tlr_address(i), TLR_STRIDE)) return -1;
            memset(GPTR(tlr_address(i)), 0, TLR_STRIDE);  /* a reused slot starts clean too */
            slots[i] = t;
            t->slot = i;
            t->tlr = tlr_address(i);
            return 0;
        }
    return -1;
}

/* Frees a thread once it has exited and its handle is closed: nothing can refer to it. */
static void thread_free_if_done(KThread *t) {
    if (!t->exited || !t->handle_closed || t == &main_thread) return;
    for (KThread **p = &threads; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    slots[t->slot] = NULL;
    free(t->ctx);
    free(t);
}

static void thread_exit(KThread *t) {
    t->exited = 1;
    nrunnable--;
    wake_sync_waiters(&t->obj);  /* a thread's handle is signalled when it ends */
    if (nrunnable == 0) {
        for (KThread *u = threads; u; u = u->next)
            if (u->started && !u->exited) all_blocked();  /* the others all wait forever */
    }
    thread_free_if_done(t);
}

/* ExitThread leaves the guest code through a jump back to the thread's host entry. */
static _Thread_local void **exit_jump;

static void thread_main(void *p) {
    KThread *t = p;
    void *jump[5];  /* __builtin_setjmp's buffer: frame pointer, resume address, stack pointer */
    cur = t;
    exit_jump = jump;
    if (__builtin_setjmp(jump) == 0) {
        yw4_resolve(t->entry)(t->ctx);
        host_lock(&klock);  /* the entry function returned: the thread ends as if by ExitThread */
        thread_exit(t);
        host_unlock(&klock);
    }
    /* Here after ExitThread too, which already did thread_exit; t may be freed by now. */
}

static uint32_t create_thread(uint64_t entry, uint64_t arg, uint64_t stack_top, int32_t priority, int32_t core,
                              uint32_t *handle) {
    if (priority < 0 || priority > 63) return R_INVALID_PRIORITY;
    if (core == -2) core = main_thread.core;  /* the process's ideal core */
    if (core < 0 || core > 2) {
        fprintf(stderr, "yw4: kernel: CreateThread on core %d: not implemented\n", core);
        exit(3);
    }
    KThread *t = calloc(1, sizeof *t);
    CPUState *ctx = calloc(1, sizeof *ctx);
    if (!t || !ctx || tlr_alloc(t)) {
        free(t), free(ctx);
        return R_OUT_OF_RESOURCE;
    }
    t->obj.type = KOBJ_THREAD;
    t->id = next_thread_id++;
    t->priority = priority, t->core = core;
    t->entry = entry;
    t->ctx = ctx;
    host_cond_init(&t->cond);
    ctx->x[0] = arg;
    ctx->sp = stack_top & ~15ull;
    ctx->tpidrro_el0 = t->tlr;
    t->next = threads;
    threads = t;
    *handle = yw4_handle_new(&t->obj);
    return R_OK;
}

static uint32_t start_thread(uint32_t handle) {
    KThread *t = thread_get(handle);
    if (!t) return R_INVALID_HANDLE;
    if (t->started) return R_INVALID_STATE;
    t->started = 1;
    nrunnable++;
    if (host_thread_start(thread_main, t, GUEST_THREAD_HOST_STACK)) {
        fprintf(stderr, "yw4: kernel: cannot start a host thread\n");
        exit(3);
    }
    return R_OK;
}

uint32_t yw4_kernel_init(CPUState *ctx, uint64_t stack, uint64_t stack_size) {
    host_lock_init(&klock);
    for (size_t i = 0; i < yw4_nmodules; i++) {  /* as Horizon maps an NSO: three regions */
        const Yw4Module *m = &yw4_modules[i];
        uint64_t b = *m->base;
        region_add(b, m->ro_offset, MEM_CODE_STATIC, PERM_RX);                                  /* .text */
        region_add(b + m->ro_offset, m->data_offset - m->ro_offset, MEM_CODE_STATIC, PERM_R);    /* .rodata */
        region_add(b + m->data_offset, m->mem_size - m->data_offset, MEM_CODE_MUTABLE, PERM_RW); /* .data, .bss */
    }
    region_add(stack, stack_size, MEM_STACK, PERM_RW);
    /* The heap's and the thread-local regions' ranges first, then everything else that is
     * free: later mappings are commits. */
    if (!yw4_reserve_range(YW4_HEAP_BASE, YW4_HEAP_SIZE) || !yw4_reserve_range(YW4_TLR_BASE, MAX_THREADS * TLR_STRIDE)) {
        fprintf(stderr, "yw4: kernel: cannot reserve the heap and thread-local regions\n");
        exit(3);
    }
    region_add(YW4_TLR_BASE, MAX_THREADS * TLR_STRIDE, MEM_THREAD_LOCAL, PERM_RW);
    yw4_reserve_free(YW4_GUEST_LO, YW4_GUEST_HI);

    KThread *t = &main_thread;
    t->obj.type = KOBJ_THREAD;
    t->id = next_thread_id++;
    t->priority = (int32_t)yw4_main_priority;
    t->core = 0;  /* the NPDM's default core */
    t->ctx = ctx;
    t->started = 1;
    host_cond_init(&t->cond);
    if (tlr_alloc(t)) {
        fprintf(stderr, "yw4: kernel: cannot make the main thread's thread-local region\n");
        exit(3);
    }
    ctx->tpidrro_el0 = t->tlr;
    threads = t;
    nrunnable = 1;
    cur = t;
    return yw4_handle_new(&t->obj);
}

/* ---- events */

KEvent *yw4_event_new(uint32_t *handle) {
    KEvent *e = calloc(1, sizeof *e);
    if (!e) exit(3);
    e->obj.type = KOBJ_EVENT;
    *handle = yw4_handle_new(&e->obj);
    return e;
}

void yw4_event_signal_from_host(KEvent *e) {
    host_lock(&klock);
    yw4_event_signal(e);
    host_unlock(&klock);
}

void yw4_event_signal(KEvent *e) {
    e->signaled = 1;
    wake_sync_waiters(&e->obj);
}
void yw4_event_clear(KEvent *e) { e->signaled = 0; }

static KEvent *event_get(uint32_t h) {
    KObject *o = yw4_handle_get(h);
    return o && o->type == KOBJ_EVENT ? (KEvent *)o : NULL;
}

/* ---- shared memory */

KSharedMemory *yw4_shared_memory_new(uint64_t size, void (*on_map)(KSharedMemory *), uint32_t *handle) {
    KSharedMemory *m = calloc(1, sizeof *m);
    if (!m) exit(3);
    m->obj.type = KOBJ_SHARED_MEMORY;
    m->size = size;
    m->on_map = on_map;
    *handle = yw4_handle_new(&m->obj);
    return m;
}

static uint32_t map_shared_memory(uint32_t handle, uint64_t addr, uint64_t size) {
    KObject *o = yw4_handle_get(handle);
    if (!o || o->type != KOBJ_SHARED_MEMORY) return R_INVALID_HANDLE;
    KSharedMemory *m = (KSharedMemory *)o;
    if (size != m->size || addr & 0xFFF) return R_INVALID_SIZE;
    if (yw4_commit(addr, size)) {  /* the game chose a free range (reserved at start-up): put memory there */
        fprintf(stderr, "yw4: kernel: cannot map shared memory at %#" PRIx64 "\n", addr);
        exit(3);
    }
    region_add(addr, size, MEM_SHARED, PERM_RW);
    m->guest_addr = addr;
    if (m->on_map) m->on_map(m);
    return R_OK;
}

/* ---- transfer memory */

static uint32_t create_transfer_memory(uint64_t addr, uint64_t size, uint32_t perm, uint32_t *handle) {
    if ((addr | size) & 0xFFF || !size) return R_INVALID_SIZE;
    if (perm != PERM_NONE && perm != PERM_R && perm != PERM_RW) return R_INVALID_ENUM;  /* InvalidNewMemoryPermission */
    KTransferMemory *m = calloc(1, sizeof *m);
    if (!m) exit(3);
    m->obj.type = KOBJ_TRANSFER_MEMORY;
    m->addr = addr, m->size = size, m->perm = perm;
    *handle = yw4_handle_new(&m->obj);
    return R_OK;
}

KTransferMemory *yw4_transfer_memory_get(uint32_t handle) {
    KObject *o = yw4_handle_get(handle);
    return o && o->type == KOBJ_TRANSFER_MEMORY ? (KTransferMemory *)o : NULL;
}

/* ---- mutexes and condition variables (mesosphere KConditionVariable)
 *
 * The sdk's mutex is a word in guest memory: 0 = free, else the owner's tag (its thread
 * handle), with WAITERS_BIT once someone waits. Taking and releasing a free mutex is pure
 * guest code (exclusive load/store, decision 0009); the kernel is only called when there is
 * contention. A condition variable is a key word: 1 while someone may be waiting. Guest
 * code writes these words concurrently, so the kernel changes them atomically too. */

/* ArbitrateUnlock and the first half of WaitProcessWideKeyAtomic: hand the mutex to its
 * best waiter (WAITERS_BIT kept if others still wait), or free it. */
static void release_lock(uint64_t mutex) {
    int n;
    KThread *next = first_waiter(WAIT_LOCK, mutex, &n);
    __atomic_store_n(u32p(mutex), next ? next->tag | (n > 1 ? WAITERS_BIT : 0) : 0, __ATOMIC_SEQ_CST);
    if (next) end_wait(next, R_OK);
}

static uint32_t arbitrate_lock(uint32_t owner, uint64_t mutex, uint32_t tag) {
    if (mutex & 3) return R_INVALID_ADDRESS;
    if (!thread_get(owner)) return R_INVALID_HANDLE;
    /* Wait only if the word still says "owner, with waiters"; otherwise it changed in the
     * meantime and the guest tries again. */
    if (__atomic_load_n(u32p(mutex), __ATOMIC_SEQ_CST) != (owner | WAITERS_BIT)) return R_OK;
    cur->wait = WAIT_LOCK;
    cur->addr = mutex;
    cur->tag = tag;
    return wait_current(-1);
}

static uint32_t wait_condvar(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout) {
    if (mutex & 3) return R_INVALID_ADDRESS;
    release_lock(mutex);
    __atomic_store_n(u32p(key), 1, __ATOMIC_SEQ_CST);  /* "someone waits" */
    if (timeout == 0) return R_TIMEOUT;
    cur->wait = WAIT_CONDVAR;
    cur->addr = key;
    cur->mutex = mutex;
    cur->tag = tag;
    /* On a timeout the mutex is NOT held: the sdk takes it again itself (as libnx does). */
    return wait_current(timeout);
}

/* A signalled condition-variable waiter takes its mutex back: at once if it is free, else
 * it becomes a waiter of the mutex (and the owner's unlock will call the kernel). */
static void condvar_wake(KThread *t) {
    uint32_t *word = u32p(t->mutex);
    uint32_t v = __atomic_load_n(word, __ATOMIC_SEQ_CST);
    for (;;) {
        uint32_t want = v ? v | WAITERS_BIT : t->tag;
        if (__atomic_compare_exchange_n(word, &v, want, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
    }
    if (!v) {
        end_wait(t, R_OK);
    } else if (!thread_get(v & ~WAITERS_BIT)) {
        end_wait(t, R_INVALID_STATE);  /* the word names no thread */
    } else {
        t->wait = WAIT_LOCK;  /* keeps waiting, same deadline; joins the mutex's queue now */
        t->addr = t->mutex;
        t->seq = next_seq++;
    }
}

static void signal_condvar(uint64_t key, int32_t count) {
    for (int32_t n = 0; count <= 0 || n < count; n++) {
        KThread *t = first_waiter(WAIT_CONDVAR, key, NULL);
        if (!t) break;
        condvar_wake(t);
    }
    if (!first_waiter(WAIT_CONDVAR, key, NULL)) __atomic_store_n(u32p(key), 0, __ATOMIC_SEQ_CST);
}

/* ---- address arbiter (mesosphere KAddressArbiter): wait while a word has some value */

static uint32_t wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout) {
    if (addr & 3) return R_INVALID_ADDRESS;
    int32_t *word = GPTR(addr);
    switch (type) {
    case 0:  /* WaitIfLessThan */
        if (__atomic_load_n(word, __ATOMIC_SEQ_CST) >= value) return R_INVALID_STATE;
        break;
    case 1: {  /* DecrementAndWaitIfLessThan */
        int32_t v = __atomic_load_n(word, __ATOMIC_SEQ_CST);
        do
            if (v >= value) return R_INVALID_STATE;
        while (!__atomic_compare_exchange_n(word, &v, v - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        break;
    }
    case 2:  /* WaitIfEqual */
        if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != value) return R_INVALID_STATE;
        break;
    default:
        return R_INVALID_ENUM;
    }
    if (timeout == 0) return R_TIMEOUT;
    cur->wait = WAIT_ADDRESS;
    cur->addr = addr;
    return wait_current(timeout);
}

static uint32_t signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    if (addr & 3) return R_INVALID_ADDRESS;
    int32_t *word = GPTR(addr);
    switch (type) {
    case 0:  /* Signal */
        break;
    case 1: {  /* SignalAndIncrementIfEqual */
        int32_t v = value;
        if (!__atomic_compare_exchange_n(word, &v, value + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return R_INVALID_STATE;
        break;
    }
    case 2:  /* SignalAndModifyByWaitingCountIfEqual: its value rule isn't verified yet */
        fprintf(stderr, "yw4: kernel: SignalToAddress type 2 (modify by waiting count): not implemented\n");
        exit(3);
    default:
        return R_INVALID_ENUM;
    }
    for (int32_t n = 0; count <= 0 || n < count; n++) {
        KThread *t = first_waiter(WAIT_ADDRESS, addr, NULL);
        if (!t) break;
        end_wait(t, R_OK);
    }
    return R_OK;
}

/* ---- the rest */

static void sleep_thread(int64_t ns) {
    host_unlock(&klock);  /* nothing to wake it: sleep without holding the kernel */
    if (ns <= 0) host_yield();  /* 0, -1, -2: the yield variants */
    else yw4_host_sleep_ns(ns);
    host_lock(&klock);
}

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
    case 0x04:  /* MapMemory(dst, src, size): dst aliases src, src is inaccessible meanwhile */
    case 0x05: {  /* UnmapMemory(dst, src, size) */
        uint64_t dst = x[0], src = x[1], size = x[2];
        if ((dst | src | size) & 0xFFF || !size) { x[0] = R_INVALID_SIZE; return; }
        if (imm == 0x04) {
            /* Copying is equivalent to aliasing: nothing may touch src until UnmapMemory. */
            if (yw4_commit(dst, size)) {
                fprintf(stderr, "yw4: kernel: MapMemory: cannot map %#" PRIx64 " bytes at %#" PRIx64 "\n", size, dst);
                exit(3);
            }
            memcpy(GPTR(dst), GPTR(src), size);
            region_add(dst, size, dst - YW4_STACK_BASE < YW4_STACK_SIZE ? MEM_STACK : MEM_ALIAS, PERM_RW);
        } else {
            memcpy(GPTR(src), GPTR(dst), size);
            yw4_decommit(dst, size);
            region_remove(dst);
        }
        x[0] = R_OK;
        return;
    }
    case 0x06:  /* QueryMemory(MemoryInfo *out, PageInfo *_, addr) -> page info */
        query_memory(x[2], x[0]);
        x[0] = R_OK;
        x[1] = 0;
        return;
    case 0x08: {  /* CreateThread(_, entry, argument, stack top, priority, core) -> handle */
        uint32_t h = 0;
        x[0] = create_thread(x[1], x[2], x[3], (int32_t)x[4], (int32_t)x[5], &h);
        x[1] = h;
        return;
    }
    case 0x09:  /* StartThread(handle) */
        x[0] = start_thread((uint32_t)x[0]);
        return;
    case 0x0A:  /* ExitThread: does not return */
        if (cur == &main_thread) {
            fprintf(stderr, "yw4: kernel: the main thread called ExitThread: not implemented\n");
            exit(3);
        }
        thread_exit(cur);
        host_unlock(&klock);
        __builtin_longjmp(exit_jump, 1);
    case 0x0B:  /* SleepThread(ns): 0 / -1 / -2 are yields */
        sleep_thread((int64_t)x[0]);
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
    case 0x10:  /* GetCurrentProcessorNumber: the ideal core, as threads don't migrate here */
        x[0] = (uint64_t)cur->core;
        return;
    case 0x11: case 0x12: case 0x17: {  /* SignalEvent / ClearEvent / ResetSignal(handle) */
        KEvent *e = event_get((uint32_t)x[0]);
        if (!e) x[0] = R_INVALID_HANDLE;
        else if (imm == 0x11) yw4_event_signal(e), x[0] = R_OK;
        else if (imm == 0x17 && !e->signaled) x[0] = R_INVALID_STATE;
        else yw4_event_clear(e), x[0] = R_OK;
        return;
    }
    case 0x13:  /* MapSharedMemory(handle, address, size, permission) */
        x[0] = map_shared_memory((uint32_t)x[0], x[1], x[2]);
        return;
    case 0x15: {  /* CreateTransferMemory(_, address, size, permission) -> handle */
        uint32_t h = 0;
        x[0] = create_transfer_memory(x[1], x[2], (uint32_t)x[3], &h);
        x[1] = h;
        return;
    }
    case 0x16: {  /* CloseHandle(handle) */
        uint32_t h = (uint32_t)x[0];
        KThread *t = thread_get(h);
        if (!ipc_close_handle(h)) yw4_handle_close(h);
        if (t && h != PSEUDO_CURRENT_THREAD) {
            t->handle_closed = 1;
            thread_free_if_done(t);
        }
        x[0] = R_OK;
        return;
    }
    case 0x18: {  /* WaitSynchronization(_, handles, count, timeout ns) -> index */
        int32_t index = -1;
        x[0] = wait_synchronization(x[1], (int32_t)x[2], (int64_t)x[3], &index);
        x[1] = (uint64_t)(uint32_t)index;
        return;
    }
    case 0x19: {  /* CancelSynchronization(thread handle) */
        KThread *t = thread_get((uint32_t)x[0]);
        if (!t) { x[0] = R_INVALID_HANDLE; return; }
        if (t->wait == WAIT_SYNC) end_wait(t, R_CANCELLED);
        else t->cancel_pending = 1;
        x[0] = R_OK;
        return;
    }
    case 0x1A:  /* ArbitrateLock(owner handle, mutex, requester tag) */
        x[0] = arbitrate_lock((uint32_t)x[0], x[1], (uint32_t)x[2]);
        return;
    case 0x1B:  /* ArbitrateUnlock(mutex) */
        if (x[0] & 3) { x[0] = R_INVALID_ADDRESS; return; }
        release_lock(x[0]);
        x[0] = R_OK;
        return;
    case 0x1C:  /* WaitProcessWideKeyAtomic(mutex, key, tag, timeout ns) */
        x[0] = wait_condvar(x[0], x[1], (uint32_t)x[2], (int64_t)x[3]);
        return;
    case 0x1D:  /* SignalProcessWideKey(key, count; <= 0: all) */
        signal_condvar(x[0], (int32_t)x[1]);
        x[0] = R_OK;
        return;
    case 0x1E:  /* GetSystemTick */
        x[0] = yw4_cntpct();
        return;
    case 0x1F: {  /* ConnectToNamedPort(_, name) -> session */
        uint32_t h = 0;
        x[0] = ipc_connect_named_port((const char *)GPTR(x[1]), &h);
        x[1] = h;
        return;
    }
    case 0x21:  /* SendSyncRequest(session): the message is in this thread's TLR */
        x[0] = ipc_send_request((uint32_t)x[0], ctx->tpidrro_el0);
        return;
    case 0x22:  /* SendSyncRequestWithUserBuffer(buffer, size, session) */
        x[0] = ipc_send_request((uint32_t)x[2], x[0]);
        return;
    case 0x25: {  /* GetThreadId(_, handle) -> id */
        KThread *t = thread_get((uint32_t)x[1]);
        x[0] = t ? R_OK : R_INVALID_HANDLE;
        x[1] = t ? t->id : 0;
        return;
    }
    case 0x26: {  /* Break(reason, info, size): the program stops itself */
        char where[48];
        fprintf(stderr, "yw4: guest svcBreak(reason %#" PRIx64 ") at %s", x[0], yw4_describe(x[30], where));
        if (x[1] && x[2] == 4)
            fprintf(stderr, ", result %#x (module %u, description %u)", *u32p(x[1]), *u32p(x[1]) & 0x1FF, *u32p(x[1]) >> 9);
        fprintf(stderr, " on thread %" PRIu64 "\n", cur->id);
        backtrace(ctx);
        exit(3);
    }
    case 0x27:  /* OutputDebugString(text, size) */
        fprintf(stderr, "[game debug] %.*s\n", (int)x[1], (const char *)GPTR(x[0]));
        x[0] = R_OK;
        return;
    case 0x29: {  /* GetInfo(_, id0, handle, id1) -> value */
        uint64_t v = 0;
        x[0] = get_info((uint32_t)x[1], (uint32_t)x[2], x[3], &v);
        x[1] = v;
        return;
    }
    case 0x34:  /* WaitForAddress(address, type, value, timeout ns) */
        x[0] = wait_for_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int64_t)x[3]);
        return;
    case 0x35:  /* SignalToAddress(address, type, value, count; <= 0: all) */
        x[0] = signal_to_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int32_t)x[3]);
        return;
    case 0x45: {  /* CreateEvent -> writable, readable (one object behind both handles) */
        uint32_t w;
        KEvent *e = yw4_event_new(&w);
        x[0] = R_OK;
        x[1] = w;
        x[2] = yw4_handle_new(&e->obj);
        return;
    }
    default: {
        char where[48];
        fprintf(stderr, "yw4: unimplemented SVC %#x at %s on thread %" PRIu64 " (x0=%#" PRIx64 " x1=%#" PRIx64
                        " x2=%#" PRIx64 " x3=%#" PRIx64 ")\n", imm, yw4_describe(x[30], where), cur->id, x[0], x[1], x[2], x[3]);
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
    uint64_t in[4] = {ctx->x[0], ctx->x[1], ctx->x[2], ctx->x[3]};
    host_lock(&klock);
    svc(ctx, imm);
    if (trace)
        fprintf(stderr, "yw4: [t%" PRIu64 "] svc %#04x(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ") -> %#" PRIx64
                        " %#" PRIx64 "\n", cur->id, imm, in[0], in[1], in[2], in[3], ctx->x[0], ctx->x[1]);
    host_unlock(&klock);
}

/* CNTPCT_EL0 counts at 19.2 MHz from boot on the Switch: here, from the first call. */
uint64_t yw4_cntpct(void) {
    static int64_t start;
    int64_t now = host_now_ns();
    int64_t s = __atomic_load_n(&start, __ATOMIC_RELAXED);
    if (!s) {
        int64_t zero = 0;
        __atomic_compare_exchange_n(&start, &zero, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        s = __atomic_load_n(&start, __ATOMIC_RELAXED);
    }
    return (uint64_t)((unsigned __int128)(now - s) * 192u / 10000u);
}

void yw4_msr_fpcr(CPUState *ctx, uint64_t value) {
    if (value != 0) {  /* rounding modes, flush-to-zero, default NaN: not modelled (decision 0008) */
        fprintf(stderr, "yw4: guest sets FPCR to %#" PRIx64 "; only 0 is supported\n", value);
        exit(3);
    }
    ctx->fpcr = 0;
}
