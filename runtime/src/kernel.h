/* The Horizon kernel as the recompiled sdk sees it (decision 0014): private to the runtime. */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

#include "yw4rt.h"

#include <stdio.h>

/* Performance log: what a guest thread was seen doing, and each thread's CPU use. */
enum { YW4_ROLE_PRESENT = 1, YW4_ROLE_SUBMIT = 2 };
void yw4_thread_role(int role);
/* The guest thread id of the calling thread (the performance report's "thread N"); 0 if none. */
uint64_t yw4_thread_id(void);
void yw4_thread_cpu_report(FILE *f, int64_t interval_ns);
/* YW4_WAITPROF=1: where the main thread blocked over the last `frames` frames (then cleared). */
void yw4_wait_report(FILE *f, int frames);

/* Horizon memory types and permissions, as svcQueryMemory reports them. */
enum {
    MEM_UNMAPPED = 0x00, MEM_CODE_STATIC = 0x03, MEM_CODE_MUTABLE = 0x04, MEM_HEAP = 0x05,
    MEM_SHARED = 0x06, MEM_ALIAS = 0x07, MEM_STACK = 0x0B, MEM_THREAD_LOCAL = 0x0C,
};
enum { PERM_NONE = 0, PERM_R = 1, PERM_W = 2, PERM_X = 4, PERM_RW = 3, PERM_RX = 5 };

/* Kernel objects behind handles. Each kind embeds KObject first. */
typedef enum { KOBJ_THREAD = 1, KOBJ_SESSION, KOBJ_EVENT, KOBJ_SHARED_MEMORY, KOBJ_TRANSFER_MEMORY } KType;
typedef struct {
    KType type;
} KObject;

uint32_t yw4_handle_new(KObject *o);
KObject *yw4_handle_get(uint32_t h);   /* NULL if not a valid handle */
void yw4_handle_close(uint32_t h);     /* forgets the handle; the object's owner frees it */

/* Events: signalled by the kernel or a service, waited on by the game (WaitSynchronization).
 * Like everything here, call only while the kernel lock is held: from inside an SVC or a
 * service command (decision 0015). */
typedef struct {
    KObject obj;
    int signaled;
    int host_signalled;  /* a host thread signals it (vsync...): waiting on it is never a deadlock */
} KEvent;
KEvent *yw4_event_new(uint32_t *handle);  /* a new unsignalled event and a handle to it */
void yw4_event_signal(KEvent *e);
void yw4_event_signal_from_host(KEvent *e);  /* the same from a host thread: takes the kernel lock */
void yw4_event_clear(KEvent *e);

/* Shared memory a service gives the game: the game maps it at an address of its choice
 * (svcMapSharedMemory); on_map then fills it. */
typedef struct KSharedMemory KSharedMemory;
struct KSharedMemory {
    KObject obj;
    uint64_t size;
    uint64_t guest_addr;  /* 0 until mapped */
    void (*on_map)(KSharedMemory *m);
};
KSharedMemory *yw4_shared_memory_new(uint64_t size, void (*on_map)(KSharedMemory *), uint32_t *handle);

/* Transfer memory: a block of the game's own memory it lends to a service
 * (svcCreateTransferMemory, then passed in an IPC message). The service uses it in place. */
typedef struct {
    KObject obj;
    uint64_t addr, size;
    uint32_t perm;  /* what the game itself may still do with it meanwhile */
} KTransferMemory;
KTransferMemory *yw4_transfer_memory_get(uint32_t handle);  /* NULL if not one */

/* The display's vsync (vi.c): a 60 Hz host thread, started on first use. */
void yw4_vsync_start(void);
/* Controller input into hid's shared memory (svc_hid.c); the vsync thread calls it. */
void yw4_hid_update(uint64_t tick);
uint64_t yw4_vsync_count(void);
void yw4_wait_vsync(uint64_t n);  /* until vsync number n has happened */

/* For native code that runs inside a guest call rather than an SVC (overrides) and needs
 * kernel objects: the lock every SVC holds. */
void yw4_kernel_lock(void);
void yw4_kernel_unlock(void);

/* Stops the program after printing the current guest thread and its backtrace: for "not
 * implemented yet" stops reached from guest code (any guest thread, lock held or not). */
_Noreturn void yw4_stop(void);
/* Up to n return addresses of ctx's guest call stack (frame-pointer chain); how many. */
int yw4_call_stack(const CPUState *ctx, uint64_t *out, int n);
/* YW4_WATCHW: report every write to [addr, addr + size) with the writer's call stack. */
int yw4_write_watch_arm(uint64_t addr, uint64_t size);

/* Sets up the memory map (modules, main stack, thread-local regions) and the main thread,
 * which runs on the calling host thread with `ctx` (its TPIDRRO_EL0 is set here); returns
 * the main thread's handle. Call after yw4_load_modules. */
uint32_t yw4_kernel_init(CPUState *ctx, uint64_t stack, uint64_t stack_size);

#endif
