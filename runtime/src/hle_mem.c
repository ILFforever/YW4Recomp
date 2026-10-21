/* nn::mem::StandardAllocator on the native TLSF heap (heap.c, decision 0011).
 *
 * The guest object (wherever the game put it) holds only a pointer to the host-side heap
 * state, in its first 8 bytes; the sdk's own fields are never looked at by game code. The
 * memory handed out is inside the region the game gave to Initialize, as on the Switch.
 *
 * Anything unexpected (a bad pointer, use before Initialize) stops the program instead of
 * returning an error the game might ignore.
 */
#include <inttypes.h>

#include "hle.h"

#define DEFAULT_ALIGNMENT 16

static Yw4Heap **slot(CPUState *ctx) { return GPTR(ctx->x[0]); }

static Yw4Heap *heap_of(CPUState *ctx) {
    Yw4Heap *h = *slot(ctx);
    if (!h) yw4_hle_stop(ctx, "StandardAllocator %#" PRIx64 " used before Initialize", ctx->x[0]);
    return h;
}

/* StandardAllocator::StandardAllocator() (C1 and C2) */
void hle_mem_StandardAllocator_ctor(CPUState *ctx) { *slot(ctx) = NULL; }

/* Initialize(void *address, size_t size) and Initialize(void *address, size_t size,
 * bool enableThreadCache). The thread cache only changes speed, so it is ignored. */
void hle_mem_StandardAllocator_Initialize(CPUState *ctx) {
    if (*slot(ctx)) yw4_hle_stop(ctx, "StandardAllocator %#" PRIx64 " initialized twice", ctx->x[0]);
    Yw4Heap *h = yw4_heap_create(GPTR(ctx->x[1]), ctx->x[2]);
    if (!h) yw4_hle_stop(ctx, "StandardAllocator::Initialize(%#" PRIx64 ", %#" PRIx64 ") failed", ctx->x[1], ctx->x[2]);
    *slot(ctx) = h;
}

void hle_mem_StandardAllocator_Finalize(CPUState *ctx) {
    yw4_heap_destroy(heap_of(ctx));
    *slot(ctx) = NULL;
}

/* void *Allocate(size_t size): NULL when out of memory, as on the Switch. */
void hle_mem_StandardAllocator_Allocate(CPUState *ctx) {
    ctx->x[0] = GADDR(yw4_heap_alloc(heap_of(ctx), ctx->x[1], DEFAULT_ALIGNMENT));
}

/* void *Allocate(size_t size, size_t alignment) */
void hle_mem_StandardAllocator_AllocateAligned(CPUState *ctx) {
    uint64_t align = ctx->x[2];
    if (align & (align - 1)) yw4_hle_stop(ctx, "StandardAllocator::Allocate: alignment %#" PRIx64 " is not a power of two", align);
    ctx->x[0] = GADDR(yw4_heap_alloc(heap_of(ctx), ctx->x[1], align));
}

void hle_mem_StandardAllocator_Free(CPUState *ctx) {
    if (yw4_heap_free(heap_of(ctx), GPTR(ctx->x[1])))
        yw4_hle_stop(ctx, "StandardAllocator::Free(%#" PRIx64 "): not an allocated block", ctx->x[1]);
}

/* void *Reallocate(void *address, size_t newSize). A null address allocates. Size 0 stops:
 * whether the sdk frees or keeps the block then isn't verified. */
void hle_mem_StandardAllocator_Reallocate(CPUState *ctx) {
    Yw4Heap *h = heap_of(ctx);
    void *p = GPTR(ctx->x[1]);
    if (p && !yw4_heap_is_allocated(h, p))
        yw4_hle_stop(ctx, "StandardAllocator::Reallocate(%#" PRIx64 "): not an allocated block", ctx->x[1]);
    if (p && !ctx->x[2]) yw4_hle_stop(ctx, "StandardAllocator::Reallocate to size 0: semantics unverified");
    ctx->x[0] = GADDR(yw4_heap_realloc(h, p, ctx->x[2]));
}

void hle_mem_StandardAllocator_GetTotalFreeSize(CPUState *ctx) { ctx->x[0] = yw4_heap_total_free(heap_of(ctx)); }

void hle_mem_StandardAllocator_GetAllocatableSize(CPUState *ctx) { ctx->x[0] = yw4_heap_largest_free(heap_of(ctx)); }
