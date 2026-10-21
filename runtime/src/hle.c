/* Native implementations of sdk functions, by mangled name (decisions 0010-0013).
 *
 * Since decision 0014 the sdk is recompiled and runs itself, so main no longer calls these
 * through import stubs. They take arguments in x0-x7 / v0-v7 and return in x0/v0, as in the
 * AArch64 calling convention, so any of them can become an override of the recompiled
 * function later (yw4_hle_find).
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
    /* libm (hle_math.c) */
    {"acos", hle_math_acos},
    {"acosf", hle_math_acosf},
    {"asin", hle_math_asin},
    {"asinf", hle_math_asinf},
    {"atan", hle_math_atan},
    {"atan2", hle_math_atan2},
    {"atan2f", hle_math_atan2f},
    {"atanf", hle_math_atanf},
    {"ceil", hle_math_ceil},
    {"ceilf", hle_math_ceilf},
    {"cos", hle_math_cos},
    {"cosf", hle_math_cosf},
    {"cosh", hle_math_cosh},
    {"exp", hle_math_exp},
    {"exp2f", hle_math_exp2f},
    {"expf", hle_math_expf},
    {"fabs", hle_math_fabs},
    {"fabsf", hle_math_fabsf},
    {"floor", hle_math_floor},
    {"floorf", hle_math_floorf},
    {"fmod", hle_math_fmod},
    {"fmodf", hle_math_fmodf},
    {"frexp", hle_math_frexp},
    {"ldexp", hle_math_ldexp},
    {"log", hle_math_log},
    {"log10", hle_math_log10},
    {"log10f", hle_math_log10f},
    {"logf", hle_math_logf},
    {"modf", hle_math_modf},
    {"modff", hle_math_modff},
    {"pow", hle_math_pow},
    {"powf", hle_math_powf},
    {"sin", hle_math_sin},
    {"sinf", hle_math_sinf},
    {"sinh", hle_math_sinh},
    {"sqrt", hle_math_sqrt},
    {"sqrtf", hle_math_sqrtf},
    {"tan", hle_math_tan},
    {"tanf", hle_math_tanf},
    {"tanh", hle_math_tanh},
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

_Noreturn void yw4_hle_stop(CPUState *ctx, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "yw4: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    char where[48];
    fprintf(stderr, "\n     called from %s\n", yw4_describe(ctx->x[30] - 4, where));
    exit(3);
}

/* The native implementation of an sdk function, by mangled name, or NULL. Since decision
 * 0014 the sdk itself is recompiled, so nothing reaches these through import stubs any
 * more. They stay until the recompiled sdk passes the same checks (sdkref --check); this
 * lookup is also where symbol overrides (NVN) will come from. */
GuestFn yw4_hle_find(const char *name) {
    for (const HleEntry *e = hle_table; e->name; e++)
        if (strcmp(e->name, name) == 0) return e->fn;
    return NULL;
}
