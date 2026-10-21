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

/* Native implementation by mangled name, or NULL (hle.c). */
GuestFn yw4_hle_find(const char *name);

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
void hle_libc_nnmusl_init_dso(CPUState *ctx);
void hle_libc_nnmusl_fini_dso(CPUState *ctx);
void hle_libc_cxa_guard_acquire(CPUState *ctx);
void hle_libc_cxa_guard_release(CPUState *ctx);
/* hle_string.c: memory, strings, ctype, integer parsing */
void hle_libc_abs(CPUState *ctx);
void hle_libc_atoi(CPUState *ctx);
void hle_libc_errno_location(CPUState *ctx);
void hle_libc_isalnum(CPUState *ctx);
void hle_libc_iscntrl(CPUState *ctx);
void hle_libc_isdigit(CPUState *ctx);
void hle_libc_islower(CPUState *ctx);
void hle_libc_ispunct(CPUState *ctx);
void hle_libc_isspace(CPUState *ctx);
void hle_libc_isxdigit(CPUState *ctx);
void hle_libc_memchr(CPUState *ctx);
void hle_libc_memcmp(CPUState *ctx);
void hle_libc_memcpy(CPUState *ctx);
void hle_libc_memmove(CPUState *ctx);
void hle_libc_memset(CPUState *ctx);
void hle_libc_memset_s(CPUState *ctx);
void hle_libc_strcasecmp(CPUState *ctx);
void hle_libc_strcat(CPUState *ctx);
void hle_libc_strchr(CPUState *ctx);
void hle_libc_strcmp(CPUState *ctx);
void hle_libc_strcoll(CPUState *ctx);
void hle_libc_strcpy(CPUState *ctx);
void hle_libc_strlen(CPUState *ctx);
void hle_libc_strncat(CPUState *ctx);
void hle_libc_strncmp(CPUState *ctx);
void hle_libc_strncpy(CPUState *ctx);
void hle_libc_strnlen(CPUState *ctx);
void hle_libc_strpbrk(CPUState *ctx);
void hle_libc_strrchr(CPUState *ctx);
void hle_libc_strspn(CPUState *ctx);
void hle_libc_strstr(CPUState *ctx);
void hle_libc_strtok(CPUState *ctx);
void hle_libc_strtol(CPUState *ctx);
void hle_libc_strtoul(CPUState *ctx);
void hle_libc_strtoull(CPUState *ctx);
void hle_libc_tolower(CPUState *ctx);
void hle_libc_toupper(CPUState *ctx);
/* hle_math.c: libm (musl 1.1.19 + exact instructions) */
void hle_math_acos(CPUState *ctx);
void hle_math_acosf(CPUState *ctx);
void hle_math_asin(CPUState *ctx);
void hle_math_asinf(CPUState *ctx);
void hle_math_atan(CPUState *ctx);
void hle_math_atan2(CPUState *ctx);
void hle_math_atan2f(CPUState *ctx);
void hle_math_atanf(CPUState *ctx);
void hle_math_ceil(CPUState *ctx);
void hle_math_ceilf(CPUState *ctx);
void hle_math_cos(CPUState *ctx);
void hle_math_cosf(CPUState *ctx);
void hle_math_cosh(CPUState *ctx);
void hle_math_exp(CPUState *ctx);
void hle_math_exp2f(CPUState *ctx);
void hle_math_expf(CPUState *ctx);
void hle_math_fabs(CPUState *ctx);
void hle_math_fabsf(CPUState *ctx);
void hle_math_floor(CPUState *ctx);
void hle_math_floorf(CPUState *ctx);
void hle_math_fmod(CPUState *ctx);
void hle_math_fmodf(CPUState *ctx);
void hle_math_frexp(CPUState *ctx);
void hle_math_ldexp(CPUState *ctx);
void hle_math_log(CPUState *ctx);
void hle_math_log10(CPUState *ctx);
void hle_math_log10f(CPUState *ctx);
void hle_math_logf(CPUState *ctx);
void hle_math_modf(CPUState *ctx);
void hle_math_modff(CPUState *ctx);
void hle_math_pow(CPUState *ctx);
void hle_math_powf(CPUState *ctx);
void hle_math_sin(CPUState *ctx);
void hle_math_sinf(CPUState *ctx);
void hle_math_sinh(CPUState *ctx);
void hle_math_sqrt(CPUState *ctx);
void hle_math_sqrtf(CPUState *ctx);
void hle_math_tan(CPUState *ctx);
void hle_math_tanf(CPUState *ctx);
void hle_math_tanh(CPUState *ctx);
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
