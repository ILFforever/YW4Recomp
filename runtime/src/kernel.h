/* The Horizon kernel as the recompiled sdk sees it (decision 0014): private to the runtime. */
#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

#include "yw4rt.h"

/* Horizon memory types and permissions, as svcQueryMemory reports them. */
enum {
    MEM_UNMAPPED = 0x00, MEM_CODE_STATIC = 0x03, MEM_CODE_MUTABLE = 0x04, MEM_HEAP = 0x05,
    MEM_ALIAS = 0x07, MEM_STACK = 0x0B, MEM_THREAD_LOCAL = 0x0C,
};
enum { PERM_NONE = 0, PERM_R = 1, PERM_W = 2, PERM_X = 4, PERM_RW = 3, PERM_RX = 5 };

/* Sets up the memory map (modules, main stack, its thread-local region) and the main
 * thread; returns the main thread's handle. Call after yw4_load_modules. */
uint32_t yw4_kernel_init(uint64_t stack, uint64_t stack_size, uint64_t tlr);

#endif
