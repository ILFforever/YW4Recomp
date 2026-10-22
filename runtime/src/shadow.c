/* Our shadow-caster pass (decision 0024, docs/frame.md "Shadow maps").
 *
 * The game draws each shadow cascade's casters with its model renderer main+0xa45890
 * (device, draw list, layer item), a layer item of kind 1. A hook runs before that function
 * (recomp/overrides.toml): returning 1 means we drew the layer ourselves, 0 lets the game's
 * code run as before. YW4_SHADOWS=native turns the pass on; anything it doesn't handle yet
 * makes it decline that layer.
 *
 * Step 1 (this file so far): count the shadow layers and decline every one.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"
#include "yw4rt.h"

extern uint64_t yw4_frames_presented;  /* dispatch.c */

enum { LAYER_SHADOW = 1 };

static int enabled = -1;

static long layers, nodes;  /* shadow layers and their draw-list nodes, since the last report */

/* Walk the layer's draw list the way the model renderer starts its walk (nodes from
 * [list + 0x20], linked at +0x30): for now only counted. */
static void count_layer(uint64_t list) {
    uint64_t n = *(const uint64_t *)(uintptr_t)(list + 0x20);
    long k = 0;
    for (; n && k < 1000000; n = *(const uint64_t *)(uintptr_t)(n + 0x30)) k++;
    __atomic_add_fetch(&layers, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&nodes, k, __ATOMIC_RELAXED);
}

int yw4_hook_model_renderer(CPUState *ctx) {
    uint64_t list = ctx->x[1], item = ctx->x[2];
    if (!item || *(const uint8_t *)(uintptr_t)(item + 8) != LAYER_SHADOW) return 0;
    if (enabled < 0) {
        const char *s = getenv("YW4_SHADOWS");
        enabled = s && !strcmp(s, "native");
        if (enabled) fprintf(stderr, "yw4: shadows: native pass on (step 1: counting only)\n");
    }
    if (!enabled) return 0;
    count_layer(list);
    static uint64_t reported;
    uint64_t f = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    if (f >= reported + 600) {
        reported = f;
        long l = __atomic_exchange_n(&layers, 0, __ATOMIC_RELAXED), k = __atomic_exchange_n(&nodes, 0, __ATOMIC_RELAXED);
        fprintf(stderr, "yw4: shadows: frame %" PRIu64 ": %ld shadow layers, %ld draw-list nodes (all declined)\n", f, l,
                k);
    }
    return 0;
}
