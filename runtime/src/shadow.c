/* Our shadow-caster pass (decision 0024, docs/frame.md "Shadow maps").
 *
 * The game draws each shadow cascade's casters with its model renderer main+0xa45890
 * (device, draw list, layer item), a layer item of kind 1. A hook runs before that function
 * (recomp/overrides.toml): returning 1 means we drew the layer ourselves, 0 lets the game's
 * code run as before. YW4_SHADOWS=native turns the pass on; anything it doesn't handle yet
 * makes it decline that layer.
 *
 * YW4_SHADOWS=check: gather each shadow layer's submeshes like the model renderer does, let
 * the game draw it, and compare with the submeshes the game actually drew (a second hook, on
 * the submesh draw main+0xa75e90). Missing ones are bugs in the gather; extra ones are casters
 * the game culled (we don't cull yet: harmless, the depth image is the same).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"
#include "yw4rt.h"

extern uint64_t yw4_frames_presented;  /* dispatch.c */

enum { LAYER_SHADOW = 1, MAX_CASTERS = 4096 };
enum { MODE_GAME, MODE_CHECK, MODE_NATIVE };

static int mode = -1;

#define U8(a) (*(const uint8_t *)(uintptr_t)(a))
#define S8(a) (*(const int8_t *)(uintptr_t)(a))
#define U16(a) (*(const uint16_t *)(uintptr_t)(a))
#define U64(a) (*(const uint64_t *)(uintptr_t)(a))

typedef struct {
    uint64_t object;     /* the draw object (after its +0x108 redirection) */
    uint64_t geometry;   /* *(object+0x128)[g] */
    uint64_t submesh;    /* the 0x50-byte submesh record */
} Caster;

typedef struct {
    Caster c[MAX_CASTERS];
    int n, overflow;
} Gathered;

/* The layer item's mask of sort keys, as the model renderer builds it: the item's mask at
 * +0x10 (plus bit 63), limited to keys [lo, hi) from +0xa / +0xc. */
static uint64_t layer_mask(uint64_t item) {
    uint16_t lo = U16(item + 0xa), hi = U16(item + 0xc);
    uint64_t below = lo < 0x3f ? ~0ull << lo : 1ull << 63;
    uint64_t above = hi < 0x40 ? ~(~0ull << hi) : ~0ull;
    return (U64(item + 0x10) | 1ull << 63) & below & above;
}

typedef struct { float m[16]; } Mat4;  /* row-major: out[i] = dot(row i, v), as the game's shaders use them */

static Mat4 mul(const Mat4 *a, const Mat4 *b) {
    Mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a->m[i * 4 + k] * b->m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

static Mat4 load(uint64_t addr) {
    Mat4 r;
    memcpy(r.m, (const void *)(uintptr_t)addr, sizeof r.m);
    return r;
}

/* The object's box (object+0x80 min, +0x90 max, local space) through `mvp` (projection ×
 * view × world) is entirely outside one clip plane: it can't reach this cascade. A box
 * with min > max is never culled. */
static int outside(uint64_t obj, const Mat4 *mvp) {
    float lo[3], hi[3];
    memcpy(lo, (const void *)(uintptr_t)(obj + 0x80), 12);
    memcpy(hi, (const void *)(uintptr_t)(obj + 0x90), 12);
    if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) return 0;
    /* planes every corner is outside of: x<-w x>w y<-w y>w. Not near and far: casters between
     * the light and the cascade still throw shadows into it */
    unsigned all = 0xF;
    for (int c = 0; c < 8; c++) {
        float v[4] = {c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2], 1}, p[4];
        for (int i = 0; i < 4; i++)
            p[i] = mvp->m[i * 4] * v[0] + mvp->m[i * 4 + 1] * v[1] + mvp->m[i * 4 + 2] * v[2] + mvp->m[i * 4 + 3];
        unsigned out = (p[0] < -p[3]) | (p[0] > p[3]) << 1 | (p[1] < -p[3]) << 2 | (p[1] > p[3]) << 3;
        all &= out;
        if (!all) return 0;
    }
    return 1;
}

/* The model renderer's selection (main+0xa45890, first phase), for one layer: which
 * (object, geometry, submesh) it draws. `vp` (the cascade's projection × view; NULL: none)
 * adds our own culling of whole objects by their box, in place of the renderer's. */
static void gather(uint64_t list, uint64_t item, const Mat4 *vp, Gathered *out) {
    uint8_t kind = U8(item + 8);
    uint16_t lo = U16(item + 0xa), hi = U16(item + 0xc);
    uint64_t mask = layer_mask(item);
    uint8_t skip_bits = kind == 1 ? 8 : 4;  /* submesh +0x3f: not drawn in this kind of layer */
    out->n = out->overflow = 0;
    for (uint64_t node = U64(list + 0x20); node; node = U64(node + 0x30)) {
        uint64_t obj0 = U64(node), obj = U64(obj0 + 0x108) ? U64(obj0 + 0x108) : obj0;
        if (kind == LAYER_SHADOW && !U8(obj + 0x1d1)) continue;  /* casts no shadow */
        if ((1u << (kind & 31)) & U8(node + 0x20)) continue;    /* node hidden from this kind */
        uint64_t mat = U64(obj + 0x120);
        if (mat) {
            if (U8(U64(mat + 0x18) + 0x23) ? kind != 0 : kind != 0 && U16(obj + 0x1b0)) continue;
        }
        int8_t key = S8(obj0 + 0x101), sort = S8(obj0 + 0x103);
        uint8_t ngeo = U8(obj + 0x1c8);
        /* Not instanced objects (an instance array at model+0x80, cbuf 11 of the instanced
         * shaders): their world matrix is the identity, each copy is placed by its own
         * transform. Culling them per copy is for later. */
        if (vp && !U64(U64(obj + 0x140) + 0x80)) {  /* world: the node's own object; box: the redirected one */
            Mat4 world = load(obj0), mvp = mul(vp, &world);
            if (outside(obj, &mvp)) continue;
        }
        uint64_t models = U64(obj + 0x140), geos = U64(obj + 0x128);
        for (uint8_t g = 0; g < ngeo; g++) {
            uint64_t geom = U64(geos + 8ull * g), mdl = models + 0x240ull * g;
            uint16_t nsub = U16(mdl + 0x200);
            if (!geom || !nsub || !U64(mdl + 0x100)) continue;
            /* loaded (stands in for the renderer's virtual "ready" call, vtable +0x18) */
            uint64_t res = U64(geom + 0x38);
            if (!U64(geom + 0x20) || !res || !U64(res) || !(U8(U64(res) + 0x42) & 1)) continue;
            uint64_t m = U64(mdl + 0x1f0);
            int shift = key < 0 ? -key : key;
            uint64_t keys = key < 0 ? m >> (shift & 63) : m << (shift & 63);
            if (!((sort < 0 ? keys : 1ull << (sort & 63)) & mask)) continue;
            uint64_t hdr = U64(geom + 0x18), base = hdr + 4ull * U16(hdr + 0xa);
            uint64_t subs = base + 4ull * U16(hdr + 0x60), ranges = base + 4ull * U16(hdr + 0x64);
            uint64_t inst = g == 0 ? U64(node + 0x10) : 0;
            for (uint16_t i = 0; i < nsub; i++) {
                uint64_t s = subs + 0x50ull * i;
                int k = sort < 0 ? (int8_t)(S8(ranges + 16ull * U8(s + 0x43) + 8) + key) : sort;
                if (k >= hi) break;
                if (k < lo || (k <= 0x3f && !((1ull << k) & mask))) continue;
                if (skip_bits & U8(s + 0x3f)) continue;
                if (inst && ((1u << (kind & 31)) & U8(inst + 0xb0 + 0xb8ull * i))) continue;
                if (!((1u << (U8(obj + 0x1cb) & 31)) & U8(s + 0x34))) continue;
                if (out->n < MAX_CASTERS) out->c[out->n++] = (Caster){obj, geom, s};
                else out->overflow = 1;
            }
        }
    }
}

/* ---- check mode: what the game drew in the layer, per thread */

typedef struct {
    int active;
    Mat4 vp;  /* the layer's cascade */
    Gathered ours;
    uint64_t drawn[MAX_CASTERS][2];  /* (object, submesh) */
    int ndrawn;
} Check;

static _Thread_local Check *chk;
static long layers_checked, missing_total, extra_total, ours_total, drawn_total;

static int same(const Caster *c, const uint64_t d[2]) { return c->object == d[0] && c->submesh == d[1]; }

static void finish_check(Check *k) {
    if (!k->active) return;
    k->active = 0;
    long missing = 0, extra = 0;
    for (int j = 0; j < k->ndrawn; j++) {
        int found = 0;
        for (int i = 0; i < k->ours.n && !found; i++) found = same(&k->ours.c[i], k->drawn[j]);
        if (!found && missing++ < 3) {
            uint64_t o = k->drawn[j][0];
            fprintf(stderr, "yw4: shadows: check: the game drew object %#" PRIx64 " submesh %#" PRIx64
                    ", the gather missed it\n", o, k->drawn[j][1]);
            const float *b = (const float *)(uintptr_t)(o + 0x80), *w = (const float *)(uintptr_t)o;
            fprintf(stderr, "yw4: shadows:   box %g %g %g .. %g %g %g; world row 0 %g %g %g %g, row 3 %g %g %g %g\n",
                    b[0], b[1], b[2], b[4], b[5], b[6], w[0], w[1], w[2], w[3], w[12], w[13], w[14], w[15]);
            Mat4 world = load(o), mvp = mul(&k->vp, &world);
            for (int c = 0; c < 8; c += 7) {
                float v[3] = {c & 1 ? b[4] : b[0], c & 2 ? b[5] : b[1], c & 4 ? b[6] : b[2]}, p[4];
                for (int i = 0; i < 4; i++)
                    p[i] = mvp.m[i * 4] * v[0] + mvp.m[i * 4 + 1] * v[1] + mvp.m[i * 4 + 2] * v[2] + mvp.m[i * 4 + 3];
                fprintf(stderr, "yw4: shadows:   corner %d clip %g %g %g %g\n", c, p[0], p[1], p[2], p[3]);
            }
        }
    }
    for (int i = 0; i < k->ours.n; i++) {
        int found = 0;
        for (int j = 0; j < k->ndrawn && !found; j++) found = same(&k->ours.c[i], k->drawn[j]);
        extra += !found;
    }
    __atomic_add_fetch(&layers_checked, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&missing_total, missing, __ATOMIC_RELAXED);
    __atomic_add_fetch(&extra_total, extra, __ATOMIC_RELAXED);
    __atomic_add_fetch(&ours_total, k->ours.n, __ATOMIC_RELAXED);
    __atomic_add_fetch(&drawn_total, k->ndrawn, __ATOMIC_RELAXED);
}

static void report(void) {
    static uint64_t reported;
    uint64_t f = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    if (f < reported + 600) return;
    reported = f;
    long l = __atomic_exchange_n(&layers_checked, 0, __ATOMIC_RELAXED);
    long o = __atomic_exchange_n(&ours_total, 0, __ATOMIC_RELAXED), d = __atomic_exchange_n(&drawn_total, 0, __ATOMIC_RELAXED);
    long m = __atomic_exchange_n(&missing_total, 0, __ATOMIC_RELAXED), e = __atomic_exchange_n(&extra_total, 0, __ATOMIC_RELAXED);
    fprintf(stderr, "yw4: shadows: frame %" PRIu64 ": %ld shadow layers; gathered %ld submeshes, the game drew %ld; "
            "missed %ld, extra (culled by the game) %ld\n", f, l, o, d, m, e);
}

/* ---- hooks */

int yw4_hook_model_renderer(CPUState *ctx) {
    if (mode == MODE_GAME) return 0;
    if (mode < 0) {
        const char *s = getenv("YW4_SHADOWS");
        mode = s && !strcmp(s, "native") ? MODE_NATIVE : s && !strcmp(s, "check") ? MODE_CHECK : MODE_GAME;
        if (mode != MODE_GAME) fprintf(stderr, "yw4: shadows: %s\n", mode == MODE_CHECK ? "checking the gather"
                                                                         : "native pass on (in progress: declines all)");
        if (mode == MODE_GAME) return 0;
    }
    if (chk) finish_check(chk);  /* the previous layer on this thread has been drawn */
    uint64_t list = ctx->x[1], item = ctx->x[2];
    if (!item || U8(item + 8) != LAYER_SHADOW) return 0;
    if (mode == MODE_CHECK) {
        if (!chk && !(chk = calloc(1, sizeof *chk))) return 0;
        /* the cascade's matrices, put in the render state by the game's job main+0x7ee510 */
        Mat4 proj = load(ctx->x[0] + 0x280), view = load(ctx->x[0] + 0x2c0);
        chk->vp = mul(&proj, &view);
        gather(list, item, &chk->vp, &chk->ours);
        chk->ndrawn = 0;
        chk->active = 1;
        report();
    }
    return 0;
}

/* main+0xa75e90 (device, geometry, submesh): the model renderer's submesh draw. Only noted. */
int yw4_hook_submesh_draw(CPUState *ctx) {
    if (!chk || !chk->active || chk->ndrawn >= MAX_CASTERS) return 0;
    static uint64_t base;
    if (!base)
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, "main")) base = *yw4_code[i].base;
    uint64_t ret = ctx->x[30] - base;
    if (ret != 0xa4bf94 && ret != 0xa4c2d0) return 0;  /* from the model renderer */
    /* the object being drawn is in the model renderer's stack slot sp+0x390 */
    chk->drawn[chk->ndrawn][0] = U64(ctx->sp + 0x390);
    chk->drawn[chk->ndrawn][1] = ctx->x[2];
    chk->ndrawn++;
    return 0;
}
