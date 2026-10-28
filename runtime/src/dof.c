/* Our distance blur, the engine side (decision 0038, docs/frame.md "Distance blur").
 *
 * The engine's distance blur is main+0x8007d0(device, renderer): the scene shrunk into the half-size image,
 * blur rounds between that and its pair (main+0x7f3620), and a composite onto the scene by depth. A hook
 * (recomp/overrides.toml) runs before it and gathers what those draws use from the renderer object: its image
 * records 0 (scene), 9 and 10 (the half-size pair) and 42 (the G-buffer's depth), the number of rounds, the blur
 * radius and spread, and the focus settings, computed into the same constants as the engine computes them (the
 * Gaussian weights with the sdk's expf, which is musl's: runtime/src/hle_math.c). Our pass (gpu_post.c) is
 * recorded into the device's command buffer in their place.
 *
 * Not ours, so the engine's function runs: the variant with renderer +0x2c70 set (a depth-aware shrink with a
 * lookup texture, a different composite), and a depth record without its descriptor.
 *
 * YW4_DOF_PASS=native: ours, and the hook skips the engine's function. check: the engine draws as usual and ours
 * draws the same into copies first, compared after the game's composite (gpu_post.c). game: the engine's.
 * Default: ours; the settings panel (F1, DISTANCE BLUR PASS) switches while playing.
 * As with bloom (bloom.c), the engine's record of what is bound is left as it was; what it changes that is not
 * a binding is done here (skipped_effects).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "post.h"
#include "yw4rt.h"

float musl_expf(float);  /* the sdk's expf (hle_math.c) */

enum { MODE_GAME, MODE_CHECK, MODE_NATIVE };
static int mode = -1;
static int blur_mode_v = -1;  /* the shared blur rounds' (YW4_BLUR_PASS) */
static _Thread_local int in_original;  /* the blur hook's check: the game's own run of f_7f3620 */
static uint64_t variant_seen;  /* calls left to the game: the +0x2c70 variant */

#define U8(a) (*(const uint8_t *)(uintptr_t)(a))
#define U16(a) (*(const uint16_t *)(uintptr_t)(a))
#define U32(a) (*(const uint32_t *)(uintptr_t)(a))
#define U64(a) (*(const uint64_t *)(uintptr_t)(a))
#define F32(a) (*(const float *)(uintptr_t)(a))
#define U16W(a) (*(uint16_t *)(uintptr_t)(a))
#define U32W(a) (*(uint32_t *)(uintptr_t)(a))

static int dof_mode(void) {
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    if (m >= 0) return m;
    const char *s = getenv("YW4_DOF_PASS");
    m = s && !strcmp(s, "game") ? MODE_GAME : s && !strcmp(s, "check") ? MODE_CHECK : MODE_NATIVE;
    int unset = -1;
    if (__atomic_compare_exchange_n(&mode, &unset, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && m != MODE_GAME)
        fprintf(stderr, "yw4: distance blur: %s\n", m == MODE_CHECK ? "ours checked against the game's" : "ours");
    return __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
}

int dof_native(void) { return dof_mode() == MODE_NATIVE; }

void dof_toggle(void) {
    int m = dof_native() ? MODE_GAME : MODE_NATIVE;
    __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: distance blur: %s (settings panel)\n", m == MODE_NATIVE ? "ours" : "the game's");
}

/* The renderer's image record i: its guest NVNtexture (record +0x10 is a view, its first word + 0x18), or 0. */
static uint64_t record_texture(uint64_t renderer, unsigned i) {
    uint64_t rec = renderer + 0x2e28 + 0x28ull * i, view = U64(rec + 0x10);
    return view && U64(view) ? U64(view) + 0x18 : 0;
}

/* f_7f3620's weights for radius r: exp(-k^2 / 2r^2) for k = 0..7, over twice their sum, the centre counted twice
 * in the shader. In its order and precision (float, the sum left to right). */
static void gauss_weights(float r, float w[8]) {
    float r2 = r * r;
    float e0 = musl_expf(-0.0f / r2), e1 = musl_expf(-0.5f / r2), e2 = musl_expf(-2.0f / r2), e3 = musl_expf(-4.5f / r2);
    float e4 = musl_expf(-8.0f / r2), e5 = musl_expf(-12.5f / r2), e6 = musl_expf(-18.0f / r2);
    float e7 = musl_expf(-24.5f / r2);
    float sum = e0 + 0.0f + e1 + e2 + e3 + e4 + e5 + e6 + e7;
    sum = sum + sum;
    w[0] = e0 / sum, w[1] = e1 / sum, w[2] = e2 / sum, w[3] = e3 / sum;
    w[4] = e4 / sum, w[5] = e5 / sum, w[6] = e6 / sum, w[7] = e7 / sum;
}

/* What the engine's distance blur changes besides its bindings, as it does it: the device's flag +0x45da bit 0,
 * the "render target read as a texture" bits in +0x45ac for every image it binds (0x12 when the record's +0x22
 * or +0x23 is set, else 0x10), and its draw statistics (+0x4654 draws, +0x4658 count / 3). */
static void skipped_effects(uint64_t device, uint64_t renderer, uint32_t draws) {
    static const unsigned records[] = {0, 9, 10, 42};
    U16W(device + 0x45da) |= 1;
    for (unsigned i = 0; i < sizeof records / sizeof *records; i++) {
        uint64_t rec = renderer + 0x2e28 + 0x28ull * records[i];
        U32W(device + 0x45ac) |= U8(rec + 0x22) || U8(rec + 0x23) ? 0x12u : 0x10u;
    }
    U32W(device + 0x4654) += draws;
    U32W(device + 0x4658) += draws * (4 / 3);
}

/* hook on main+0x8007d0 (device, renderer), before its body */
int yw4_hook_dof(CPUState *ctx) {
    int m = dof_mode();
    if (m == MODE_GAME) return 0;
    uint64_t device = ctx->x[0], renderer = ctx->x[1];
    uint32_t rounds = U32(renderer + 0x868);
    if (!rounds) return 0;  /* the engine draws nothing either */
    if (U8(renderer + 0x2c70)) {
        if (!variant_seen++) fprintf(stderr, "yw4: distance blur: the depth-aware variant: the game's\n");
        return 0;
    }
    uint64_t desc = U64(renderer + 0x34c0);  /* the depth record's descriptor: its size at u16 +4, +6 */
    static const unsigned records[DOF_IMAGES] = {0, 9, 10, 42};
    DofPass p;
    memset(&p, 0, sizeof p);
    for (int i = 0; i < DOF_IMAGES; i++) {
        uint64_t g = record_texture(renderer, records[i]);
        const NvnTexture *t = g ? HOST(NvnTexture, g) : NULL;
        if (!t) {
            static int logged;
            if (!logged++) fprintf(stderr, "yw4: distance blur: image record %u has no texture: the game's\n", records[i]);
            return 0;
        }
        memcpy(&p.tex[i], t, sizeof p.tex[i]);
    }
    p.rounds = rounds > 32 ? 32 : rounds;
    {   /* the blur's settings when they change (battles use more rounds than towns), the first 32 times */
        static uint32_t last_rounds, changes;
        static float last_radius, last_spread;
        float radius = F32(renderer + 0x864), spread = F32(renderer + 0x860);
        if ((rounds != last_rounds || radius != last_radius || spread != last_spread) && changes++ < 32)
            fprintf(stderr, "yw4: distance blur: %u rounds, radius %g, spread %g\n", rounds, radius, spread);
        last_rounds = rounds, last_radius = radius, last_spread = spread;
    }
    gauss_weights(F32(renderer + 0x864), p.weights);
    p.spread = F32(renderer + 0x860);
    float n = F32(renderer + 0x86c);
    p.texel[0] = n / (float)U16(desc + 4), p.texel[1] = n / (float)U16(desc + 6);
    /* the composite's fragment constants, as the engine computes them */
    float near = F32(renderer + 0x5c0), far = F32(renderer + 0x5c4);
    float f0 = F32(renderer + 0x850), f1 = F32(renderer + 0x854), f2 = F32(renderer + 0x858), f3 = F32(renderer + 0x85c);
    float range = f1 - f0;
    float *c = p.focus;
    c[0] = near * far, c[1] = near - far, c[2] = near - f0, c[3] = range != 0.0f ? 1.0f / range : 0.0f;
    c[4] = f2, c[5] = 1.0f - f3, c[6] = f2 != 0.0f ? 1.0f / f2 : 0.0f, c[7] = f3 != 0.0f ? 1.0f / f3 : 0.0f;
    p.far = far;
    p.check = m == MODE_CHECK;
    nvn_record_native(ctx, U64(device + 0x10d0) + 0xd8, gpu_dof_pass, &p, sizeof p);
    if (m != MODE_NATIVE) return 0;
    skipped_effects(device, renderer, 2 + 2 * p.rounds);
    return 1;
}

/* ---- the engine's shared blur rounds: main+0x7f3620(radius s0, device x0, spread pair x1, rounds w2, target id w3,
 * down attachment w4, across attachment w5, record a x6, record b x7). Per round it blurs record a across into
 * the target's attachment w5 (record b) and record b down into attachment w4 (record a), with the weights of
 * gauss_weights and the spread over the target's size. Its callers: the distance blur above (whose own pass draws
 * these rounds when it is ours), the effects pass main+0x7f1bf0 (battles: up to 12 rounds of records 12 and 11)
 * and main+0x804570, main+0x80a0a0. YW4_BLUR_PASS=native: ours; check: ours into copies, then the game's, compared
 * (gpu_post.c); default: the game's. */

static int blur_mode(void) {
    int m = __atomic_load_n(&blur_mode_v, __ATOMIC_ACQUIRE);
    if (m >= 0) return m;
    const char *s = getenv("YW4_BLUR_PASS");
    m = s && !strcmp(s, "native") ? MODE_NATIVE : s && !strcmp(s, "check") ? MODE_CHECK : MODE_GAME;
    int unset = -1;
    if (__atomic_compare_exchange_n(&blur_mode_v, &unset, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && m != MODE_GAME)
        fprintf(stderr, "yw4: blur rounds: %s\n", m == MODE_CHECK ? "ours checked against the game's" : "ours");
    return __atomic_load_n(&blur_mode_v, __ATOMIC_ACQUIRE);
}

int blur_native(void) { return blur_mode() == MODE_NATIVE; }

static int fast_v = -1;

int blur_fast(void) {
    int f = __atomic_load_n(&fast_v, __ATOMIC_ACQUIRE);
    if (f < 0) {
        f = getenv("YW4_BLUR_FAST") && atoi(getenv("YW4_BLUR_FAST")) ? 1 : 0;
        __atomic_store_n(&fast_v, f, __ATOMIC_RELEASE);
    }
    return f;
}

void blur_fast_toggle(void) {
    int f = !blur_fast();
    __atomic_store_n(&fast_v, f, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: blur rounds: %s (settings panel)\n", f ? "fast" : "the game's");
}

/* A record pointer's texture (the record's +0x10 view, its first word + 0x18), copied; 0 if none. */
static int record_copy(uint64_t rec, NvnTexture *out) {
    uint64_t view = rec ? U64(rec + 0x10) : 0, g = view && U64(view) ? U64(view) + 0x18 : 0;
    const NvnTexture *t = g ? HOST(NvnTexture, g) : NULL;
    if (!t) return 0;
    memcpy(out, t, sizeof *out);
    return 1;
}

int yw4_hook_blur(CPUState *ctx) {
    if (in_original) return 0;
    int m = blur_mode();
    if (blur_fast()) m = MODE_NATIVE;  /* the fast rounds are ours (and not checked: they differ) */
    if (m == MODE_GAME) return 0;
    uint64_t device = ctx->x[0], spread = ctx->x[1], ra = ctx->x[6], rb = ctx->x[7];
    uint32_t rounds = (uint32_t)ctx->x[2];
    BlurPass p;
    memset(&p, 0, sizeof p);
    if (!rounds || !record_copy(ra, &p.a) || !record_copy(rb, &p.b)) return 0;
    p.rounds = rounds > 31 ? 32 : rounds;
    gauss_weights(ctx->v[0].f[0], p.weights);
    /* over the target's size: the engine's is its render target's (u16 +0x3a, +0x3c), the records' images' */
    p.step[0] = F32(spread) / (float)p.b.width, p.step[1] = F32(spread + 4) / (float)p.b.height;
    {   /* what is blurred and how much, when it changes, the first 32 times */
        static uint32_t last_rounds, changes;
        static uint64_t last_a;
        if ((rounds != last_rounds || ra != last_a) && changes++ < 32)
            fprintf(stderr, "yw4: blur rounds: %u rounds of %ux%u (from main+%#llx), radius %g\n", rounds, p.a.width,
                    p.a.height, (unsigned long long)(ctx->x[30] - yw4_image_base), ctx->v[0].f[0]);
        last_rounds = rounds, last_a = ra;
    }
    uint64_t cmd = U64(device + 0x10d0) + 0xd8;
    p.check = m == MODE_CHECK;
    nvn_record_native(ctx, cmd, gpu_blur_pass, &p, sizeof p);
    if (m == MODE_CHECK) {  /* the game's rounds now, then the comparison */
        static GuestFn original;
        if (!original) original = yw4_lookup(yw4_image_base + 0x7f3620);
        if (!original) return 0;
        in_original = 1;
        original(ctx);
        in_original = 0;
        nvn_record_native(ctx, cmd, gpu_blur_check_end, &p, sizeof p);
        return 1;
    }
    U16W(device + 0x45da) |= 1;  /* what f_7f3900 changes besides bindings, per pass (see skipped_effects) */
    for (int i = 0; i < 2; i++) {
        uint64_t rec = i ? rb : ra;
        U32W(device + 0x45ac) |= U8(rec + 0x22) || U8(rec + 0x23) ? 0x12u : 0x10u;
    }
    U32W(device + 0x4654) += 2 * p.rounds;
    U32W(device + 0x4658) += 2 * p.rounds * (4 / 3);
    return 1;
}
