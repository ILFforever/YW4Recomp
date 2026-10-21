/* yw4: load main and the sdk, then start the game the way the Switch does (decision 0014).
 *
 *   yw4 <exefs work directory: main.bin, sdk.bin> <romfs folder>
 *
 * rtld is not needed: the loader relocates both modules. Then the main guest thread calls
 * the sdk's nn::init::Start(thread handle, argument region, init, fini), as rtld would. It
 * calls back to run the sdk's DT_INIT right after nnosInitialize, and main's DT_INIT
 * after nninitStartup (when the game has set up its heap), then calls nnMain. The two
 * callbacks are host functions (yw4_host_function), as in LAS Recomp's start-up.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernel.h"
#include "romfs.h"
#include "yw4load.h"

#define TLR_SIZE 0x200              /* Horizon's thread-local region per thread */

static void init_module(CPUState *ctx, size_t i) {
    const Yw4Module *m = &yw4_modules[i];
    fprintf(stderr, "yw4: running %s DT_INIT (%s+%#" PRIx64 ")\n", m->name, m->name, m->dt_init);
    yw4_resolve(*m->base + m->dt_init)(ctx);
    fprintf(stderr, "yw4: %s DT_INIT returned\n", m->name);
}
static void init_sdk(CPUState *ctx) { init_module(ctx, YW4_SDK); }
static void init_main(CPUState *ctx) { init_module(ctx, YW4_MAIN); }

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: yw4 <exefs work directory with main.bin and sdk.bin> <romfs folder>\n");
        return 1;
    }
    if (yw4_load_modules(argv[1]) || romfs_open(argv[2])) return 1;

    CPUState *ctx = calloc(1, sizeof *ctx);
    uint8_t *stack = yw4_reserve(YW4_STACK_BASE, yw4_main_stack_size);
    uint8_t *tlr = yw4_reserve(YW4_TLR_BASE, 0x1000);
    if (!ctx || !stack || !tlr) {
        fprintf(stderr, "yw4: out of memory\n");
        return 1;
    }
    ctx->sp = ((uint64_t)(uintptr_t)(stack + yw4_main_stack_size)) & ~15ull;
    ctx->tpidrro_el0 = (uint64_t)(uintptr_t)tlr;
    (void)TLR_SIZE;

    ctx->x[0] = yw4_kernel_init((uint64_t)(uintptr_t)stack, yw4_main_stack_size, (uint64_t)(uintptr_t)tlr);
    ctx->x[1] = 0;  /* argument region: none */
    ctx->x[2] = yw4_host_function(init_sdk);
    ctx->x[3] = yw4_host_function(init_main);
    ctx->x[30] = 0;  /* returning from the entry is a normal C return; x30 is never jumped to */
    fprintf(stderr, "yw4: main thread: stack %#" PRIx64 " bytes, priority %u; running nn::init::Start (sdk+%#" PRIx64 ")\n",
            yw4_main_stack_size, yw4_main_priority, yw4_entry_init_start);
    yw4_resolve(yw4_sdk_base + yw4_entry_init_start)(ctx);
    fprintf(stderr, "yw4: nn::init::Start returned, x0=%#" PRIx64 "\n", ctx->x[0]);
    return 0;
}
