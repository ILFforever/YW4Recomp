/* TLSF heap over a guest memory region: the native nn::mem::StandardAllocator (decision 0011).
 *
 * TLSF ("two-level segregated fit", Masmano et al. 2004): free blocks sit in lists indexed by
 * (first level = power of two of the size, second level = which of 16 slices of that power).
 * Two bitmaps say which lists are non-empty, so finding a block that fits is a couple of
 * count-trailing-zeros, O(1). Neighbouring free blocks are always merged, so fragmentation
 * stays low over a long game session.
 *
 * Every block starts with a 16-byte header inside the region:
 *   prev_phys  the block physically before it (NULL for the first)
 *   size       whole block size including the header, a multiple of 16; bit 0 = free
 * A free block also keeps its list links in the first 16 bytes of its payload, so the
 * smallest block is 32 bytes. A used 16-byte sentinel at the end of the region stops merges.
 * Payloads are 16-byte aligned, like malloc on AArch64.
 *
 * Only the lists and bitmaps live in host memory; everything the guest sees is in its region.
 */
#include <stdlib.h>
#include <string.h>

#include "hle.h"

typedef struct Block {
    struct Block *prev_phys;
    uint64_t size;
    struct Block *next_free, *prev_free;  /* only while free: overlaps the payload */
} Block;

#define HDR 16u
#define MIN_BLOCK 32u
#define FREE_BIT 1u
#define SL_LOG 4
#define SL_COUNT (1u << SL_LOG)
#define FL_SHIFT (SL_LOG + 4)       /* sizes below 256 bytes go linearly into first level 0 */
#define SMALL (1u << FL_SHIFT)
#define FL_COUNT (64 - FL_SHIFT + 1)

struct Yw4Heap {
    uint8_t *start, *end;           /* the managed region, 16-aligned */
    uint64_t fl_map;
    uint32_t sl_map[FL_COUNT];
    Block *lists[FL_COUNT][SL_COUNT];
    size_t free_bytes;              /* payload bytes of all free blocks */
};

static inline size_t bsize(const Block *b) { return b->size & ~(uint64_t)15; }
static inline int is_free(const Block *b) { return b->size & FREE_BIT; }
static inline Block *next_phys(const Block *b) { return (Block *)((uint8_t *)b + bsize(b)); }
static inline void *payload(Block *b) { return (uint8_t *)b + HDR; }
static inline int fls64(uint64_t v) { return 63 - __builtin_clzll(v); }

static void mapping(size_t size, unsigned *fl, unsigned *sl) {
    if (size < SMALL) {
        *fl = 0;
        *sl = (unsigned)(size / (SMALL / SL_COUNT));
    } else {
        int f = fls64(size);
        *sl = (unsigned)(size >> (f - SL_LOG)) ^ SL_COUNT;
        *fl = (unsigned)(f - FL_SHIFT + 1);
    }
}

static void insert(Yw4Heap *h, Block *b) {
    unsigned fl, sl;
    mapping(bsize(b), &fl, &sl);
    b->size |= FREE_BIT;
    b->prev_free = NULL;
    b->next_free = h->lists[fl][sl];
    if (b->next_free) b->next_free->prev_free = b;
    h->lists[fl][sl] = b;
    h->fl_map |= 1ull << fl;
    h->sl_map[fl] |= 1u << sl;
    h->free_bytes += bsize(b) - HDR;
}

static void unlink_free(Yw4Heap *h, Block *b) {
    unsigned fl, sl;
    mapping(bsize(b), &fl, &sl);
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else h->lists[fl][sl] = b->next_free;
    if (b->next_free) b->next_free->prev_free = b->prev_free;
    if (!h->lists[fl][sl]) {
        h->sl_map[fl] &= ~(1u << sl);
        if (!h->sl_map[fl]) h->fl_map &= ~(1ull << fl);
    }
    b->size &= ~(uint64_t)FREE_BIT;
    h->free_bytes -= bsize(b) - HDR;
}

/* A free block of at least `size` bytes, or NULL. The request is first rounded up to the
 * next list, where any block fits (O(1)); only if that finds nothing is the request's own
 * list searched, so that the largest free block can still be allocated whole. */
static Block *find_fit(Yw4Heap *h, size_t size) {
    unsigned fl, sl;
    size_t rounded = size >= SMALL ? size + (1ull << (fls64(size) - SL_LOG)) - 1 : size;
    mapping(rounded, &fl, &sl);
    if (fl < FL_COUNT) {
        uint32_t sl_bits = sl < SL_COUNT ? h->sl_map[fl] & (~0u << sl) : 0;
        uint64_t fl_bits = h->fl_map & (~0ull << (fl + 1));
        if (sl_bits) return h->lists[fl][__builtin_ctz(sl_bits)];
        if (fl_bits) {
            fl = (unsigned)__builtin_ctzll(fl_bits);
            return h->lists[fl][__builtin_ctz(h->sl_map[fl])];
        }
    }
    mapping(size, &fl, &sl);
    if (fl >= FL_COUNT) return NULL;
    for (Block *b = h->lists[fl][sl]; b; b = b->next_free)
        if (bsize(b) >= size) return b;
    return NULL;
}

/* Cuts a used block down to `size`, returning the rest to the free lists (merged with the
 * next block if that is free). */
static void trim(Yw4Heap *h, Block *b, size_t size) {
    if (bsize(b) - size < MIN_BLOCK) return;
    Block *rest = (Block *)((uint8_t *)b + size);
    rest->prev_phys = b;
    rest->size = bsize(b) - size;
    b->size = size;  /* used, so no flag bits */
    Block *n = next_phys(rest);
    if (is_free(n)) {
        unlink_free(h, n);
        rest->size += bsize(n);
        n = next_phys(rest);
    }
    n->prev_phys = rest;
    insert(h, rest);
}

static size_t block_size_for(size_t size) {
    if (size > SIZE_MAX / 2) return 0;
    size_t n = ((size + 15) & ~(size_t)15) + HDR;
    return n < MIN_BLOCK ? MIN_BLOCK : n;
}

Yw4Heap *yw4_heap_create(void *region, size_t size) {
    uint8_t *start = (uint8_t *)(((uintptr_t)region + 15) & ~(uintptr_t)15);
    uint8_t *end = (uint8_t *)(((uintptr_t)region + size) & ~(uintptr_t)15);
    if (end <= start || (size_t)(end - start) < MIN_BLOCK + HDR) return NULL;
    Yw4Heap *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->start = start;
    h->end = end;
    Block *first = (Block *)start;
    Block *sentinel = (Block *)(end - HDR);
    first->prev_phys = NULL;
    first->size = (uint64_t)((uint8_t *)sentinel - start);
    sentinel->prev_phys = first;
    sentinel->size = 0;  /* used, size 0: never merged, never walked past */
    insert(h, first);
    return h;
}

void yw4_heap_destroy(Yw4Heap *h) { free(h); }

void *yw4_heap_alloc(Yw4Heap *h, size_t size, size_t align) {
    if (align < 16) align = 16;
    if (align & (align - 1)) return NULL;
    size_t need = block_size_for(size ? size : 1);
    if (!need || align > SIZE_MAX / 4) return NULL;
    /* An aligned payload may need a gap in front, which must be big enough to stay a block. */
    size_t search = align > 16 ? need + align + MIN_BLOCK : need;
    Block *b = find_fit(h, search);
    if (!b) return NULL;
    unlink_free(h, b);
    if (align > 16) {
        uintptr_t p = ((uintptr_t)payload(b) + align - 1) & ~(uintptr_t)(align - 1);
        size_t gap = p - (uintptr_t)payload(b);
        if (gap && gap < MIN_BLOCK) gap += align;  /* align >= 32 here, so this is enough */
        if (gap) {
            Block *a = (Block *)((uint8_t *)b + gap);
            a->prev_phys = b;
            a->size = bsize(b) - gap;
            next_phys(a)->prev_phys = a;
            b->size = gap;
            insert(h, b);  /* the gap's previous neighbour is used, else it would have merged */
            b = a;
        }
    }
    trim(h, b, need);
    return payload(b);
}

/* The used block whose payload is p, or NULL if p isn't one. Walks nothing: checks the
 * header and that both neighbours point back at it, which catches double frees and most
 * stray pointers. */
static Block *checked_block(const Yw4Heap *h, const void *p) {
    if ((const uint8_t *)p < h->start + HDR || (const uint8_t *)p >= h->end - HDR || ((uintptr_t)p & 15))
        return NULL;
    Block *b = (Block *)((uintptr_t)p - HDR);
    if (is_free(b) || bsize(b) < MIN_BLOCK || (uint8_t *)next_phys(b) > h->end - HDR) return NULL;
    if (next_phys(b)->prev_phys != b) return NULL;
    if (b->prev_phys && next_phys(b->prev_phys) != b) return NULL;
    return b;
}

int yw4_heap_is_allocated(const Yw4Heap *h, const void *p) { return checked_block(h, p) != NULL; }

int yw4_heap_free(Yw4Heap *h, void *p) {
    if (!p) return 0;
    Block *b = checked_block(h, p);
    if (!b) return -1;
    Block *n = next_phys(b);
    if (is_free(n)) {
        unlink_free(h, n);
        b->size += bsize(n);
    }
    Block *prev = b->prev_phys;
    if (prev && is_free(prev)) {
        unlink_free(h, prev);
        prev->size += bsize(b);
        b = prev;
    }
    next_phys(b)->prev_phys = b;
    insert(h, b);
    return 0;
}

void *yw4_heap_realloc(Yw4Heap *h, void *p, size_t size) {
    if (!p) return yw4_heap_alloc(h, size, 16);
    Block *b = checked_block(h, p);
    if (!b) return NULL;
    size_t need = block_size_for(size ? size : 1);
    if (!need) return NULL;
    if (bsize(b) < need) {
        Block *n = next_phys(b);  /* grow in place into a free neighbour */
        if (is_free(n) && bsize(b) + bsize(n) >= need) {
            unlink_free(h, n);
            b->size += bsize(n);
            next_phys(b)->prev_phys = b;
        } else {
            void *q = yw4_heap_alloc(h, size, 16);
            if (!q) return NULL;
            memcpy(q, p, bsize(b) - HDR);
            yw4_heap_free(h, p);
            return q;
        }
    }
    trim(h, b, need);
    return p;
}

size_t yw4_heap_total_free(const Yw4Heap *h) { return h->free_bytes; }

size_t yw4_heap_largest_free(const Yw4Heap *h) {
    if (!h->fl_map) return 0;
    unsigned fl = (unsigned)fls64(h->fl_map);
    size_t best = 0;
    for (const Block *b = h->lists[fl][fls64(h->sl_map[fl])]; b; b = b->next_free)
        if (bsize(b) > best) best = bsize(b);
    return best - HDR;
}
