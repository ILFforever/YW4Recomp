/* yw4: load main and run it the way the Switch's sdk start-up does (decision 0010):
 * nninitStartup (the game's memory set-up hook), then DT_INIT (static constructors), then
 * nnMain.
 *
 *   yw4 <path to work/exefs/main.bin>
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "yw4load.h"

#define GUEST_STACK_SIZE (8u << 20)  /* main thread stack; enough for now, the NPDM value comes later */

static void run(CPUState *ctx, const char *what, uint64_t offset) {
    fprintf(stderr, "yw4: running %s (main+%#" PRIx64 ")\n", what, offset);
    ctx->x[30] = 0;  /* returning from the entry is a normal C return; x30 is never jumped to */
    yw4_resolve(yw4_image_base + offset)(ctx);
    fprintf(stderr, "yw4: %s returned, x0=%#" PRIx64 "\n", what, ctx->x[0]);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: yw4 <path to main.bin from work/exefs>\n");
        return 1;
    }
    if (yw4_load_image(argv[1])) return 1;

    CPUState *ctx = calloc(1, sizeof *ctx);
    uint8_t *stack = yw4_reserve(YW4_STACK_BASE, GUEST_STACK_SIZE);
    if (!ctx || !stack) {
        fprintf(stderr, "yw4: out of memory\n");
        return 1;
    }
    ctx->sp = ((uint64_t)(uintptr_t)(stack + GUEST_STACK_SIZE)) & ~15ull;

    run(ctx, "nninitStartup", yw4_entry_startup);
    run(ctx, "DT_INIT (static constructors)", yw4_entry_init);
    run(ctx, "nnMain", yw4_entry_main);
    return 0;
}
