/* Our shadow mask, the engine side (docs/frame.md "Lighting stage").
 *
 * The engine's shadow mask is main+0x7f3e90(device, renderer): one half-size draw with the program
 * 98c09b/06d03ad7 that works out, per pixel, how much of the sun reaches the surface from the cascaded
 * shadow maps. Our readable version of its fragment program (runtime/shaders/shadow_mask.frag) is used
 * in place of the game's in that draw (gpu_draw.c); everything else (the draw, its inputs, the target)
 * stays the engine's.
 *
 * YW4_SHADOW_MASK=native: ours; YW4_SHADOW_MASK=game: the game's program. Default: ours; the settings panel
 * (F1, DEVELOPER OPTIONS, SHADOW MASK) switches while playing.
 * YW4_AB_AT=<GPU frames> with YW4_AB_PASS=shadowmask: in those frames the engine's pass runs twice from
 * the same images, ours and the game's, and the targets are compared texel by texel (gpu_ab.c): a hook
 * (recomp/overrides.toml) records the range's markers around the engine's function.
 *
 * Not bit-exact (2026-10-30, late-game save, 4K 21:9): the same operations in the same order, yet 2-4 of
 * 2.8 million texels per frame differ by one bit of the 11-bit result with soft shadows off (~130 with them
 * on: a hair's change tips the blocker search). Splicing the game's translated code into ours section by
 * section only moved the differences, with the same compiler and options: the driver optimises the two
 * equivalent programs a little differently. The owner accepted it (not visible).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gbuffer.h"
#include "gpu.h"
#include "yw4rt.h"

enum { MODE_GAME, MODE_NATIVE };
static int mode = -1;
static _Thread_local int in_original;  /* the hook's own call of the engine's function */

#define U64(a) (*(const uint64_t *)(uintptr_t)(a))

static int mask_mode(void) {
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    if (m >= 0) return m;
    const char *s = getenv("YW4_SHADOW_MASK");
    m = s && !strcmp(s, "game") ? MODE_GAME : MODE_NATIVE;
    int unset = -1;
    if (__atomic_compare_exchange_n(&mode, &unset, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && m != MODE_GAME)
        fprintf(stderr, "yw4: shadow mask: ours\n");
    return __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
}

int shadow_mask_native(void) { return mask_mode() == MODE_NATIVE; }

void shadow_mask_toggle(void) {
    int m = shadow_mask_native() ? MODE_GAME : MODE_NATIVE;
    __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: shadow mask: %s (settings panel)\n", m == MODE_NATIVE ? "ours" : "the game's");
}

int yw4_hook_shadow_mask(CPUState *ctx) {
    if (in_original || !gpu_ab_wanted("shadowmask")) return 0;
    static GuestFn original;
    if (!original) original = yw4_lookup(yw4_image_base + 0x7f3e90);
    if (!original) return 0;
    uint64_t cmd = U64(ctx->x[0] + 0x10d0) + 0xd8;  /* the device's command buffer */
    nvn_record_native(ctx, cmd, gpu_ab_native, "bshadowmask", 11);
    in_original = 1;
    original(ctx);
    in_original = 0;
    nvn_record_native(ctx, cmd, gpu_ab_native, "eshadowmask", 11);
    return 1;
}
