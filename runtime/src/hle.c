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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yw4load.h"

typedef struct {
    const char *name;
    GuestFn fn;
} HleEntry;

/* Native implementations, by mangled sdk name. Empty: the skeleton only finds out which
 * imports the game needs first. */
static const HleEntry hle_table[] = {
    {NULL, NULL},
};

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
    uint64_t caller = ctx->x[30] - 4 - yw4_image_base;  /* the BL that called the stub */
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
