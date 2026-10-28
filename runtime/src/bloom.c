/* Our bloom pass, the engine side (decision 0037, docs/frame.md "Post-processing").
 *
 * The engine's bloom is main+0x802e40(device, renderer): a bright pass, three levels blurred across and
 * down, and a composite onto the scene. A hook (recomp/overrides.toml) runs before it and gathers what
 * those 8 draws use, straight from the renderer object: its images (0x28-byte records from +0x2e28;
 * record +0x10 is a texture view whose first word + 0x18 is the guest NVNtexture) and its settings. Our
 * pass (gpu_bloom.c) is recorded into the device's command buffer in their place.
 *
 * YW4_BLOOM=native: ours, and the hook skips the engine's function. YW4_BLOOM=check: the engine draws as
 * usual, and ours draws the same into copies first; after the game's composite the two are compared
 * texel by texel (gpu_bloom.c). YW4_BLOOM=game: the engine's. Default: ours; the settings panel (F1, BLOOM)
 * switches while playing.
 *
 * When the engine's function is skipped, the engine's record of what is bound (texture slots, constant
 * buffers, the program) stays as it was, and so do the game's commands, because none of the engine's bloom
 * commands are sent either: the two agree. Setting that record as the function would would make the engine
 * skip bindings the GPU never got. What it changes that is not a binding is done here (skipped_effects).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bloom.h"
#include "yw4rt.h"

enum { MODE_GAME, MODE_CHECK, MODE_NATIVE };
static int mode = -1;

#define U8(a) (*(const uint8_t *)(uintptr_t)(a))
#define U16W(a) (*(uint16_t *)(uintptr_t)(a))
#define U32W(a) (*(uint32_t *)(uintptr_t)(a))
#define U64(a) (*(const uint64_t *)(uintptr_t)(a))
#define F32(a) (*(const float *)(uintptr_t)(a))

static int bloom_mode(void) {
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    if (m >= 0) return m;
    const char *s = getenv("YW4_BLOOM");
    m = s && !strcmp(s, "game") ? MODE_GAME : s && !strcmp(s, "check") ? MODE_CHECK : MODE_NATIVE;
    int unset = -1;
    if (__atomic_compare_exchange_n(&mode, &unset, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && m != MODE_GAME)
        fprintf(stderr, "yw4: bloom: %s\n", m == MODE_CHECK ? "ours checked against the game's" : "ours");
    return __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
}

int bloom_native(void) { return bloom_mode() == MODE_NATIVE; }

void bloom_toggle(void) {
    int m = bloom_native() ? MODE_GAME : MODE_NATIVE;
    __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: bloom: %s (settings panel)\n", m == MODE_NATIVE ? "ours" : "the game's");
}

/* The guest NVNtexture of the renderer's image record i, or 0. */
static uint64_t record_texture(uint64_t renderer, unsigned i) {
    uint64_t rec = renderer + 0x2e28 + 0x28ull * i, view = U64(rec + 0x10);
    return view && U64(view) ? U64(view) + 0x18 : 0;
}

/* What the engine's bloom changes besides its bindings, as it does it: the device's flag +0x45da bit 0, the
 * "render target read as a texture" bits in +0x45ac for every image it binds (0x12 when the record's
 * +0x22 or +0x23 is set, else 0x10), and its draw statistics (+0x4654 draws, +0x4658 count / 3: 8 quads of
 * 4 vertices). */
static void skipped_effects(uint64_t device, uint64_t renderer, const unsigned *records, int n) {
    U16W(device + 0x45da) |= 1;
    for (int i = 0; i < n; i++) {
        uint64_t rec = renderer + 0x2e28 + 0x28ull * records[i];
        U32W(device + 0x45ac) |= U8(rec + 0x22) || U8(rec + 0x23) ? 0x12u : 0x10u;
    }
    U32W(device + 0x4654) += 8;
    U32W(device + 0x4658) += 8 * (4 / 3);
}

/* hook on main+0x802e40 (device, renderer), before its body */
int yw4_hook_bloom(CPUState *ctx) {
    int m = bloom_mode();
    if (m == MODE_GAME) return 0;
    uint64_t device = ctx->x[0], renderer = ctx->x[1];
    static const unsigned records[BLOOM_IMAGES] = {0, 11, 9, 20, 19, 22, 21, 24, 23};
    BloomPass p;
    memset(&p, 0, sizeof p);
    for (int i = 0; i < BLOOM_IMAGES; i++) {
        uint64_t g = record_texture(renderer, records[i]);
        if (!g) {
            static int logged;
            if (!logged++) fprintf(stderr, "yw4: bloom: image record %u has no texture: the game's bloom\n", records[i]);
            return 0;
        }
        const NvnTexture *t = HOST(NvnTexture, g);  /* the runtime's texture, named by the guest object */
        if (!t) return 0;
        memcpy(&p.tex[i], t, sizeof p.tex[i]);
    }
    /* bright pass: (+0x800, +0x80c) for the scene, (+0x7f0, +0x7fc) for the half image; with +0x388d set,
     * each threshold is at least 1 */
    memcpy(p.bright, (const void *)(uintptr_t)(renderer + 0x800), 16);
    memcpy(p.bright + 4, (const void *)(uintptr_t)(renderer + 0x7f0), 16);
    if (U8(renderer + 0x388d)) {
        if (p.bright[3] < 1.0f) p.bright[3] = 1.0f;
        if (p.bright[7] < 1.0f) p.bright[7] = 1.0f;
    }
    for (int l = 0; l < 3; l++) p.step[l] = F32(renderer + 0x7c0 + 4 * l);
    float *c = p.composite;
    c[0] = F32(renderer + 0x7d0), c[1] = F32(renderer + 0x7e0), c[2] = F32(renderer + 0x7d4), c[3] = F32(renderer + 0x7e4);
    c[4] = F32(renderer + 0x7d8), c[5] = F32(renderer + 0x7e8);
    p.check = m == MODE_CHECK;
    nvn_record_native(ctx, U64(device + 0x10d0) + 0xd8, gpu_bloom_pass, &p, sizeof p);
    if (m != MODE_NATIVE) return 0;
    skipped_effects(device, renderer, records, BLOOM_IMAGES);
    return 1;
}
