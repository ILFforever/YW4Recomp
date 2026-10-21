/* Calls from main into sdk imports (decision 0010).
 *
 * Every import is a stub address; calling it lands in yw4_import_call with the import's
 * index. Imports with a native implementation in hle_table run it (arguments in x0-x7 and
 * v0-v7, result in x0/v0, as in the AArch64 calling convention); every other import stops
 * the program with its name and the guest address it was called from, so the next thing
 * to implement is always obvious.
 *
 * YW4_TRACE=1 prints every import call.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "yw4load.h"

typedef struct {
    const char *name;
    GuestFn fn;
} HleEntry;

/* Native implementations, by mangled sdk name (decision 0011). */
static const HleEntry hle_table[] = {
    /* nn::os memory (hle_os.c) */
    {"_ZN2nn2os15QueryMemoryInfoEPNS0_10MemoryInfoE", hle_os_QueryMemoryInfo},
    {"_ZN2nn2os17SetMemoryHeapSizeEm", hle_os_SetMemoryHeapSize},
    {"_ZN2nn2os19AllocateMemoryBlockEPmm", hle_os_AllocateMemoryBlock},
    /* nn::diag (hle_diag.c) */
    {"_ZN2nn4diag6detail7LogImplERKNS0_11LogMetaDataEPKcz", hle_diag_LogImpl},
    {"_ZN2nn4diag6detail18OnAssertionFailureENS0_13AssertionTypeEPKcS4_S4_i", hle_diag_OnAssertionFailure},
    {"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i", hle_diag_AbortImpl},
    {"nndetailDiagAbortFormatImpl", hle_diag_AbortFormatImpl},
    /* musl start-up, C++ ABI (hle_libc.c) */
    {"__nnmusl_init_dso", hle_libc_nnmusl_init_dso},
    {"__nnmusl_fini_dso", hle_libc_nnmusl_fini_dso},
    {"__cxa_guard_acquire", hle_libc_cxa_guard_acquire},
    {"__cxa_guard_release", hle_libc_cxa_guard_release},
    /* C library: memory, strings, ctype, integers (hle_string.c) */
    {"abs", hle_libc_abs},
    {"atoi", hle_libc_atoi},
    {"__errno_location", hle_libc_errno_location},
    {"isalnum", hle_libc_isalnum},
    {"iscntrl", hle_libc_iscntrl},
    {"isdigit", hle_libc_isdigit},
    {"islower", hle_libc_islower},
    {"ispunct", hle_libc_ispunct},
    {"isspace", hle_libc_isspace},
    {"isxdigit", hle_libc_isxdigit},
    {"memchr", hle_libc_memchr},
    {"memcmp", hle_libc_memcmp},
    {"memcpy", hle_libc_memcpy},
    {"memmove", hle_libc_memmove},
    {"memset", hle_libc_memset},
    {"memset_s", hle_libc_memset_s},
    {"strcasecmp", hle_libc_strcasecmp},
    {"strcat", hle_libc_strcat},
    {"strchr", hle_libc_strchr},
    {"strcmp", hle_libc_strcmp},
    {"strcoll", hle_libc_strcoll},
    {"strcpy", hle_libc_strcpy},
    {"strlen", hle_libc_strlen},
    {"strncat", hle_libc_strncat},
    {"strncmp", hle_libc_strncmp},
    {"strncpy", hle_libc_strncpy},
    {"strnlen", hle_libc_strnlen},
    {"strpbrk", hle_libc_strpbrk},
    {"strrchr", hle_libc_strrchr},
    {"strspn", hle_libc_strspn},
    {"strstr", hle_libc_strstr},
    {"strtok", hle_libc_strtok},
    {"strtol", hle_libc_strtol},
    {"strtoul", hle_libc_strtoul},
    {"strtoull", hle_libc_strtoull},
    {"tolower", hle_libc_tolower},
    {"toupper", hle_libc_toupper},
    /* nn::mem::StandardAllocator (hle_mem.c) */
    {"_ZN2nn3mem17StandardAllocatorC1Ev", hle_mem_StandardAllocator_ctor},
    {"_ZN2nn3mem17StandardAllocatorC2Ev", hle_mem_StandardAllocator_ctor},
    {"_ZN2nn3mem17StandardAllocator10InitializeEPvm", hle_mem_StandardAllocator_Initialize},
    {"_ZN2nn3mem17StandardAllocator10InitializeEPvmb", hle_mem_StandardAllocator_Initialize},
    {"_ZN2nn3mem17StandardAllocator8FinalizeEv", hle_mem_StandardAllocator_Finalize},
    {"_ZN2nn3mem17StandardAllocator8AllocateEm", hle_mem_StandardAllocator_Allocate},
    {"_ZN2nn3mem17StandardAllocator8AllocateEmm", hle_mem_StandardAllocator_AllocateAligned},
    {"_ZN2nn3mem17StandardAllocator4FreeEPv", hle_mem_StandardAllocator_Free},
    {"_ZN2nn3mem17StandardAllocator10ReallocateEPvm", hle_mem_StandardAllocator_Reallocate},
    {"_ZNK2nn3mem17StandardAllocator16GetTotalFreeSizeEv", hle_mem_StandardAllocator_GetTotalFreeSize},
    {"_ZNK2nn3mem17StandardAllocator18GetAllocatableSizeEv", hle_mem_StandardAllocator_GetAllocatableSize},
    {NULL, NULL},
};

static uint64_t caller_of(const CPUState *ctx) {
    return ctx->x[30] - 4 - yw4_image_base;  /* the BL that called the stub */
}

_Noreturn void yw4_hle_stop(CPUState *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "yw4: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n     called from main+%#" PRIx64 "\n", caller_of(ctx));
    exit(3);
}

static GuestFn find_impl(const char *name) {
    for (const HleEntry *e = hle_table; e->name; e++)
        if (strcmp(e->name, name) == 0) return e->fn;
    return NULL;
}

void yw4_import_call(CPUState *ctx, uint32_t index) {
    static int trace = -1;
    if (trace < 0) {
        const char *t = getenv("YW4_TRACE");
        trace = t && *t && *t != '0';
    }
    const char *name = yw4_imports[index].name;
    uint64_t caller = caller_of(ctx);
    if (trace)
        fprintf(stderr, "yw4: import %s from %#" PRIx64 "\n", name, caller);
    GuestFn fn = find_impl(name);
    if (fn) {
        fn(ctx);
        return;
    }
    fprintf(stderr, "yw4: unimplemented import #%u %s\n"
                    "     called from main+%#" PRIx64 " with x0=%#" PRIx64 " x1=%#" PRIx64 " x2=%#" PRIx64 "\n",
            index, name, caller, ctx->x[0], ctx->x[1], ctx->x[2]);
    exit(3);
}
