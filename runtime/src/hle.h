/* Shared by the native implementations of sdk imports (HLE, decision 0011).
 *
 * An implementation is a GuestFn: it reads its arguments from x0-x7 / v0-v7 (then the guest
 * stack), as the AArch64 calling convention passes them, and writes its result to x0 or v0.
 * Guest pointers are host pointers (decision 0005), so a guest char* can be used directly.
 */
#ifndef HLE_H
#define HLE_H

#include <stddef.h>
#include <stdint.h>

#include "yw4rt.h"

#define GPTR(v) ((void *)(uintptr_t)(v))
#define GADDR(p) ((uint64_t)(uintptr_t)(p))

/* nn::Result: 0 is success. */
#define RESULT_SUCCESS 0u

/* Stops the program: an import was used in a way the runtime doesn't support yet, or the
 * game reported a fatal error. Prints the message and the guest address of the call. */
_Noreturn void yw4_hle_stop(CPUState *ctx, const char *fmt, ...);

/* Variadic arguments of a guest call (AAPCS64: integer arguments in x[next_gr..7] then the
 * stack, floating-point ones in v[next_vr..7] then the stack, 8 bytes per stack slot). */
typedef struct {
    CPUState *ctx;
    unsigned next_gr, next_vr;
    uint64_t stack;  /* guest address of the next stack slot */
} GuestVarargs;

/* ... starts after `fixed_gr` integer and `fixed_vr` floating-point named arguments. */
GuestVarargs yw4_varargs(CPUState *ctx, unsigned fixed_gr, unsigned fixed_vr);
uint64_t yw4_vararg_int(GuestVarargs *va);
double yw4_vararg_double(GuestVarargs *va);

/* printf-style formatting of a guest format string with guest varargs, into a host buffer.
 * Returns the length the full output would have (like snprintf). */
int yw4_guest_format(char *out, size_t size, const char *fmt, GuestVarargs *va);

/* The heap behind nn::mem::StandardAllocator (runtime/src/heap.c): a TLSF allocator over a
 * guest memory region. Plain C, no CPUState, so it's tested on its own. Not thread-safe yet:
 * there is one guest thread so far (decision 0010). */
typedef struct Yw4Heap Yw4Heap;
Yw4Heap *yw4_heap_create(void *region, size_t size);  /* NULL if the region is too small */
void yw4_heap_destroy(Yw4Heap *h);
void *yw4_heap_alloc(Yw4Heap *h, size_t size, size_t align);  /* align: power of two */
void *yw4_heap_realloc(Yw4Heap *h, void *p, size_t size);  /* NULL: out of memory, p unchanged */
/* 0 on success, -1 if p is not an allocated block of this heap (double free, bad pointer). */
int yw4_heap_free(Yw4Heap *h, void *p);
size_t yw4_heap_total_free(const Yw4Heap *h);  /* bytes in free blocks, headers excluded */
size_t yw4_heap_largest_free(const Yw4Heap *h);  /* largest single allocation possible */
int yw4_heap_is_allocated(const Yw4Heap *h, const void *p);  /* p is a live block's payload */

/* Implementations (hle_*.c), listed by mangled name in hle.c. */
void hle_os_QueryMemoryInfo(CPUState *ctx);
void hle_os_SetMemoryHeapSize(CPUState *ctx);
void hle_os_AllocateMemoryBlock(CPUState *ctx);
void hle_diag_LogImpl(CPUState *ctx);
void hle_diag_OnAssertionFailure(CPUState *ctx);
void hle_diag_AbortImpl(CPUState *ctx);
void hle_diag_AbortFormatImpl(CPUState *ctx);
void hle_mem_StandardAllocator_ctor(CPUState *ctx);
void hle_mem_StandardAllocator_Initialize(CPUState *ctx);
void hle_mem_StandardAllocator_Finalize(CPUState *ctx);
void hle_mem_StandardAllocator_Allocate(CPUState *ctx);
void hle_mem_StandardAllocator_AllocateAligned(CPUState *ctx);
void hle_mem_StandardAllocator_Free(CPUState *ctx);
void hle_mem_StandardAllocator_Reallocate(CPUState *ctx);
void hle_mem_StandardAllocator_GetTotalFreeSize(CPUState *ctx);
void hle_mem_StandardAllocator_GetAllocatableSize(CPUState *ctx);

#endif
