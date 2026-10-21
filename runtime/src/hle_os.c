/* nn::os memory: the heap the game carves its allocators from (decision 0011).
 *
 * On the Switch a game asks how much memory it has (QueryMemoryInfo), sets its heap to that
 * (SetMemoryHeapSize) and takes blocks from the heap in 2 MiB units (AllocateMemoryBlock).
 * YW4's nninitStartup takes 0xB9C00000 bytes (~2.9 GiB) for its malloc and a second block
 * for whatever is left. The game never frees a block (FreeMemoryBlock isn't imported), so
 * blocks are handed out bottom-up from one reserved range.
 */
#include <inttypes.h>

#include "hle.h"
#include "yw4load.h"

#define HEAP_BASE 0x7200000000ull  /* just above main (0x7100000000): inside the 39-bit space */
#define BLOCK_UNIT 0x200000ull     /* nn::os::MemoryBlockUnitSize, 2 MiB */

/* What a game sees on a retail 4 GiB Switch: the application pool of the 4 GiB memory
 * arrangement. Taken from the kernel's memory arrangement table, not measured on hardware. */
#define TOTAL_AVAILABLE 0xCD500000ull
#define MAIN_STACK_SIZE (8ull << 20)  /* = GUEST_STACK_SIZE in main.c */

static uint8_t *heap;
static uint64_t heap_size, heap_used;

/* nn::os::QueryMemoryInfo(nn::os::MemoryInfo*) */
void hle_os_QueryMemoryInfo(CPUState *ctx) {
    struct {
        uint64_t total_available, total_used, heap_size, heap_allocated, program_size, stack_size;
        int32_t thread_count;
    } *info = GPTR(ctx->x[0]);
    info->total_available = TOTAL_AVAILABLE;
    info->program_size = yw4_image_mem_size;
    info->stack_size = MAIN_STACK_SIZE;
    info->heap_size = heap_size;
    info->heap_allocated = heap_used;
    info->total_used = info->program_size + info->stack_size + heap_size;
    info->thread_count = 1;
}

/* nn::Result nn::os::SetMemoryHeapSize(size_t). Only the first call (from nninitStartup) is
 * supported: growing or shrinking a heap that blocks were taken from would need more care. */
void hle_os_SetMemoryHeapSize(CPUState *ctx) {
    uint64_t size = ctx->x[0];
    if (size % BLOCK_UNIT || (heap && size != heap_size))
        yw4_hle_stop(ctx, "SetMemoryHeapSize(%#" PRIx64 "): only one heap of whole 2 MiB units is supported "
                          "(current %#" PRIx64 ")", size, heap_size);
    if (!heap) {
        heap = yw4_reserve(HEAP_BASE, size);
        if (!heap) yw4_hle_stop(ctx, "cannot reserve a %#" PRIx64 "-byte heap", size);
        heap_size = size;
    }
    ctx->x[0] = RESULT_SUCCESS;
}

/* nn::Result nn::os::AllocateMemoryBlock(uintptr_t *address, size_t size) */
void hle_os_AllocateMemoryBlock(CPUState *ctx) {
    uint64_t *out = GPTR(ctx->x[0]), size = ctx->x[1];
    if (!size || size % BLOCK_UNIT || size > heap_size - heap_used)
        yw4_hle_stop(ctx, "AllocateMemoryBlock(%#" PRIx64 "): heap %#" PRIx64 ", %#" PRIx64 " used",
                     size, heap_size, heap_used);
    *out = GADDR(heap + heap_used);
    heap_used += size;
    ctx->x[0] = RESULT_SUCCESS;
}
