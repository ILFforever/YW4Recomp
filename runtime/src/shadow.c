/* Our shadow-caster pass (decision 0024, docs/frame.md "Shadow maps").
 *
 * The game draws each shadow cascade's casters with its model renderer main+0xa45890
 * (device, draw list, layer item), a layer item of kind 1. Hooks (recomp/overrides.toml) run
 * before it, before its submesh draw main+0xa75e90 and before the job that ends the cascade,
 * main+0x7ed350.
 *
 * YW4_SHADOWS=native, hybrid for now: we gather the layer's casters like the model renderer
 * does; the nodes whose every submesh we can draw are hidden from the game's layer (the
 * node's own "not in this kind of layer" bit) and drawn by our pass, which is queued right
 * after the game's draws of the layer (in the cascade-end hook), into the layer the game
 * bound and cleared. What we can draw is learned from the game's own draws: a submesh is ours
 * once the game has drawn it with the static program. So our share grows as kinds of casters
 * are added, and the game always draws the rest.
 *
 * YW4_SHADOWS=check: gather only, and compare with the submeshes the game drew. Missing ones
 * are bugs in the gather; extra ones are casters the game culled and we don't (instanced).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"
#include "shadow.h"
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
    uint64_t node;       /* the draw-list node */
    uint64_t own;        /* the node's own object: its world matrix (the model renderer reads it there) */
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

/* 4x4 inverse by cofactors, in double; 0 if singular. */
static int invert(const Mat4 *a, Mat4 *out) {
    double m[16], inv[16];
    for (int i = 0; i < 16; i++) m[i] = a->m[i];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0) return 0;
    for (int i = 0; i < 16; i++) out->m[i] = (float)(inv[i] / det);
    return 1;
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
                if (out->n < MAX_CASTERS) out->c[out->n++] = (Caster){node, obj0, obj, geom, s};
                else out->overflow = 1;
            }
        }
    }
}


/* ---- the layer being drawn on this thread (the shadow layers all run on one render thread) */

enum { MAX_HIDDEN = 2048, PASS_DRAWS = 2048, RING = 64 };

/* The game's programs for static casters: projection x view x world, and a fragment stage
 * that does nothing (the same vertex program also comes with cut-out fragment programs). */
#define STATIC_VS 0xb320bd9159e2fd0aull
#define DEPTH_FS 0x6ead6a69737c52c2ull

typedef struct {
    int active;
    uint64_t device;
    int cascade;           /* 0..2, from the view object the game's job main+0x7ee9a0 set */
    Mat4 vp;               /* the cascade's projection x view */
    Gathered ours;
    /* what the game drew (check mode: compared; native mode: learned) */
    uint64_t drawn[MAX_CASTERS][2];  /* (object, submesh) */
    int ndrawn;
    int pending;           /* a submesh draw was entered; the next recorded draw is its */
    /* native mode */
    uint64_t hidden[MAX_HIDDEN];  /* nodes hidden from the game's layer (their kind-1 bit set by us) */
    int nhidden;
    ShadowPass *pass;
} Layer;

static _Thread_local Layer *lay;

int shadow_checking;  /* nvn_cmdbuf.c calls shadow_check_draw while this is set */

/* What the game drew each (object, submesh) with, learned from its own draws: a node is ours
 * once all its submeshes are known to be drawn with STATIC_VS and DEPTH_FS. The model and
 * material are kept to notice an object's memory being reused for another one. */
enum { LEARN_SLOTS = 1 << 16 };
static struct { uint64_t obj, sub, model, mat, vs, fs; int ok; uint8_t cull, front; } learned[LEARN_SLOTS];

static uint32_t learn_slot(uint64_t obj, uint64_t sub) {
    uint32_t h = (uint32_t)((obj * 0x9E3779B97F4A7C15ull ^ sub * 0xC2B2AE3D27D4EB4Full) >> 48);
    for (uint32_t i = 0; i < LEARN_SLOTS; i++, h = (h + 1) & (LEARN_SLOTS - 1))
        if (!learned[h].obj || (learned[h].obj == obj && learned[h].sub == sub)) return h;
    return UINT32_MAX;
}

/* Was the submesh last drawn by the game with the static programs (and the object not reused)? */
static int learned_static(uint64_t obj, uint64_t sub) {
    uint32_t h = learn_slot(obj, sub);
    if (h == UINT32_MAX || learned[h].obj != obj) return 0;
    if (learned[h].model != U64(obj + 0x140) || learned[h].mat != U64(obj + 0x120)) return 0;  /* reused */
    return learned[h].vs == STATIC_VS && learned[h].fs == DEPTH_FS && learned[h].ok;
}

/* ok: our draw of it would equal the game's (same buffers, same matrix: compare_draw) */
static void learn(uint64_t obj, uint64_t sub, uint64_t vs, uint64_t fs, int ok, const uint8_t polygon[4]) {
    uint32_t h = learn_slot(obj, sub);
    if (h == UINT32_MAX) return;
    learned[h].obj = obj, learned[h].sub = sub, learned[h].model = U64(obj + 0x140), learned[h].mat = U64(obj + 0x120);
    learned[h].vs = vs, learned[h].fs = fs, learned[h].ok = ok;
    learned[h].cull = polygon[0], learned[h].front = polygon[1];
}

/* The shadow map layer each cascade's draws went into, learned from the game's draws. */
static struct { uint64_t addr; uint32_t layer; int known; } cascade_target[3];

/* Statistics since the last report */
static long layers_seen, missing_total, extra_total, ours_total, drawn_total;
static long nodes_hidden, draws_native;
static long draws_since_toggle;  /* submeshes drawn by our pass since the last F9 */
static long compared, wrong_vb, wrong_ib, wrong_mvp;  /* check mode: the game's static draws vs ours */

static int same(const Caster *c, const uint64_t d[2]) { return c->object == d[0] && c->submesh == d[1]; }

static void compare(Layer *k) {
    long missing = 0, extra = 0;
    for (int j = 0; j < k->ndrawn; j++) {
        int found = 0;
        for (int i = 0; i < k->ours.n && !found; i++) found = same(&k->ours.c[i], k->drawn[j]);
        if (!found && missing++ < 3)
            fprintf(stderr, "yw4: shadows: check: the game drew object %#" PRIx64 " submesh %#" PRIx64
                    ", the gather missed it\n", k->drawn[j][0], k->drawn[j][1]);
    }
    for (int i = 0; i < k->ours.n; i++) {
        int found = 0;
        for (int j = 0; j < k->ndrawn && !found; j++) found = same(&k->ours.c[i], k->drawn[j]);
        extra += !found;
    }
    __atomic_add_fetch(&missing_total, missing, __ATOMIC_RELAXED);
    __atomic_add_fetch(&extra_total, extra, __ATOMIC_RELAXED);
}

static void report(void) {
    static uint64_t reported;
    uint64_t f = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    if (f < reported + 600) return;
    reported = f;
    long l = __atomic_exchange_n(&layers_seen, 0, __ATOMIC_RELAXED);
    long o = __atomic_exchange_n(&ours_total, 0, __ATOMIC_RELAXED), d = __atomic_exchange_n(&drawn_total, 0, __ATOMIC_RELAXED);
    if (mode == MODE_CHECK) {
        long m = __atomic_exchange_n(&missing_total, 0, __ATOMIC_RELAXED);
        long e = __atomic_exchange_n(&extra_total, 0, __ATOMIC_RELAXED);
        fprintf(stderr, "yw4: shadows: frame %" PRIu64 ": %ld shadow layers; gathered %ld submeshes, the game drew %ld; "
                "missed %ld, extra (culled by the game) %ld\n", f, l, o, d, m, e);
        fprintf(stderr, "yw4: shadows:   static draws compared with ours %ld: wrong vertices %ld, indices %ld, matrix %ld\n",
                __atomic_exchange_n(&compared, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&wrong_vb, 0, __ATOMIC_RELAXED),
                __atomic_exchange_n(&wrong_ib, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&wrong_mvp, 0, __ATOMIC_RELAXED));
    } else {
        long ps, ds;
        gpu_shadow_counts(&ps, &ds);
        fprintf(stderr, "yw4: shadows: frame %" PRIu64 ": %ld shadow layers; %ld nodes and %ld submeshes drawn by us, "
                "%ld submeshes by the game; skipped: %ld passes (other target bound), %ld draws (memory)\n", f, l,
                __atomic_exchange_n(&nodes_hidden, 0, __ATOMIC_RELAXED),
                __atomic_exchange_n(&draws_native, 0, __ATOMIC_RELAXED), d, ps, ds);
    }
}

/* nvn_cmdbuf.c: a draw was recorded; if it is the submesh draw noted last on this thread, it
 * is the game's draw of that submesh: learn its program and the cascade's target. */
static int compare_draw(const NvnCmdBuf *cb, uint64_t obj, uint64_t sub, uint64_t index_addr);

void shadow_check_draw(const NvnCmdBuf *cb, uint64_t index_addr, const ShadowDrawCall *call) {
    (void)call;  /* instancing: used once instanced casters are learned (next step) */
    if (!lay || !lay->active || !lay->pending) return;
    lay->pending = 0;
    uint64_t obj = lay->drawn[lay->ndrawn - 1][0], sub = lay->drawn[lay->ndrawn - 1][1];
    uint64_t vs = cb->program ? cb->program->stages[0].key : 0, fs = cb->program ? cb->program->stages[1].key : 0;
    learn(obj, sub, vs, fs, vs == STATIC_VS && fs == DEPTH_FS && compare_draw(cb, obj, sub, index_addr), cb->polygon);
    if (lay->cascade >= 0) {
        cascade_target[lay->cascade].addr = cb->depth_addr;
        cascade_target[lay->cascade].layer = cb->depth_layer;
        cascade_target[lay->cascade].known = 1;
    }
}


/* ---- native passes: a ring of pass buffers, reused after RING passes (about 20 frames:
 * long after the GPU has run them) */

static ShadowPass *new_pass(void) {
    static ShadowPass *ring[RING];
    static unsigned next;
    ShadowPass **p = &ring[next++ % RING];
    if (!*p && !(*p = malloc(sizeof **p + PASS_DRAWS * sizeof(ShadowDraw)))) return NULL;
    (*p)->n = 0;
    return *p;
}

/* Index type of a submesh (+0x3f bit 1, through the table at main+0x12c22b0: 16 or 32 bit). */
static uint32_t index_type(uint64_t sub) { return U8(sub + 0x3f) & 2 ? 2 : 1; }

/* How our pass would draw this submesh (0: it can't): a triangle list, from the geometry's
 * buffers, with projection x view x the node's own object's world matrix. */
static int describe(const Caster *c, const Mat4 *vp, ShadowDraw *d) {
    if (U8(c->submesh + 0x3d) != 1) return 0;  /* primitive table index 1: triangle list */
    uint64_t bufs = U64(c->geometry + 0x20);
    if (!bufs) return 0;
    Mat4 world = load(c->own), mvp = mul(vp, &world);
    d->vertices = U64(bufs) + *(const uint32_t *)(uintptr_t)c->submesh;
    d->indices = U64(bufs + 0x68) + *(const uint32_t *)(uintptr_t)(c->submesh + 4);
    d->vertex_count = *(const uint32_t *)(uintptr_t)(c->submesh + 8);
    d->index_count = *(const uint32_t *)(uintptr_t)(c->submesh + 0xc);
    d->stride = U8(c->submesh + 0x3e);
    d->index_type = index_type(c->submesh);
    memcpy(d->mvp, mvp.m, sizeof d->mvp);
    return d->stride && (d->index_count || d->vertex_count);
}

/* Is the submesh ours (the game drew it with the static programs), and how do we draw it? */
static int ours(const Caster *c, const Mat4 *vp, ShadowDraw *d) {
    if (!learned_static(c->object, c->submesh) || !describe(c, vp, d)) return 0;
    uint32_t h = learn_slot(c->object, c->submesh);
    d->cull = learned[h].cull, d->front = learned[h].front;
    return 1;
}

/* Check mode: the game drew (obj, sub) with the static programs; would our pass draw the same?
 * The vertex and index addresses must be equal, and our matrix the one in vs cbuf 2 at byte 128. */
static int compare_draw(const NvnCmdBuf *cb, uint64_t obj, uint64_t sub, uint64_t index_addr) {
    const Caster *c = NULL;
    for (int i = 0; i < lay->ours.n && !c; i++)
        if (lay->ours.c[i].object == obj && lay->ours.c[i].submesh == sub) c = &lay->ours.c[i];
    ShadowDraw d;
    uint64_t ub = cb->trace_ub[0][2];
    if (!c || !describe(c, &lay->vp, &d) || !ub) return 0;
    const float *game = (const float *)(uintptr_t)(ub + 128);
    float err = 0, scale = 0;
    for (int i = 0; i < 16; i++) {
        float e = d.mvp[i] - game[i], a = game[i] < 0 ? -game[i] : game[i];
        if ((e < 0 ? -e : e) > err) err = e < 0 ? -e : e;
        if (a > scale) scale = a;
    }
    int bad_vb = d.vertices != cb->trace_vb0, bad_ib = d.index_count && d.indices != index_addr;
    int bad_mvp = err > 1e-4f * (scale > 1 ? scale : 1);
    __atomic_add_fetch(&compared, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wrong_vb, bad_vb, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wrong_ib, bad_ib, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wrong_mvp, bad_mvp, __ATOMIC_RELAXED);
    static int shown;
    if (mode == MODE_CHECK && (bad_vb || bad_ib || bad_mvp) && shown++ < 5) {
        Mat4 w2 = load(c->object), m2 = mul(&lay->vp, &w2);
        float err2 = 0;
        for (int i = 0; i < 16; i++) {
            float e = m2.m[i] - game[i];
            if ((e < 0 ? -e : e) > err2) err2 = e < 0 ? -e : e;
        }
        fprintf(stderr, "yw4: shadows: check: object %#" PRIx64 " (own %#" PRIx64 ") submesh %#" PRIx64 ": vertices %#"
                PRIx64 " vs the game's %#" PRIx64 ", indices %#" PRIx64 " vs %#" PRIx64 ", matrix off by %g (with the "
                "redirected object's world: %g)\n", obj, c->own, sub, d.vertices, cb->trace_vb0, d.indices, index_addr,
                err, err2);
        /* the world matrix the game used: inverse(projection x view) x its matrix */
        Mat4 g, inv;
        memcpy(g.m, game, sizeof g.m);
        if (invert(&lay->vp, &inv)) {
            Mat4 x = mul(&inv, &g);
            const float *w = (const float *)(uintptr_t)c->own;
            for (int r = 0; r < 4; r++)
                fprintf(stderr, "yw4: shadows:   world used row %d: %9.4f %9.4f %9.4f %9.4f | object+0: %9.4f %9.4f %9.4f "
                        "%9.4f | +0x40: %9.4f %9.4f %9.4f %9.4f\n", r, x.m[r * 4], x.m[r * 4 + 1], x.m[r * 4 + 2],
                        x.m[r * 4 + 3], w[r * 4], w[r * 4 + 1], w[r * 4 + 2], w[r * 4 + 3], w[16 + r * 4],
                        w[16 + r * 4 + 1], w[16 + r * 4 + 2], w[16 + r * 4 + 3]);
            fprintf(stderr, "yw4: shadows:   submesh +0x38 %u +0x42 %u +0x43 %u\n", U8(sub + 0x38), U8(sub + 0x42),
                    U8(sub + 0x43));
        }
    }
    return !bad_vb && !bad_ib && !bad_mvp;
}

/* Take over the nodes whose every gathered submesh we can draw: hide them from the game's
 * layer (their node's kind-1 bit) and add their draws to the pass. Each node goes back to the
 * game one frame in 32 (spread over the nodes), so what we learned from its draws is checked
 * again: a part of a model that starts moving (its node transform, which we don't apply yet)
 * then fails the check and stays with the game. */
static void take_over(Layer *k) {
    k->nhidden = 0;
    k->pass = NULL;
    if (k->cascade < 0 || !cascade_target[k->cascade].known) return;  /* the game hasn't shown us the target yet */
    ShadowPass *p = new_pass();
    if (!p) return;
    p->depth_addr = cascade_target[k->cascade].addr;
    p->depth_layer = cascade_target[k->cascade].layer;
    uint64_t frame = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    for (int i = 0; i < k->ours.n;) {
        uint64_t node = k->ours.c[i].node;
        int j = i, all = ((frame + (node >> 4)) & 31) != 0;
        uint32_t start = p->n;
        for (; j < k->ours.n && k->ours.c[j].node == node; j++)
            if (all && (p->n == PASS_DRAWS || !ours(&k->ours.c[j], &k->vp, &p->d[p->n++]))) all = 0;
        if (!all || k->nhidden == MAX_HIDDEN || (U8(node + 0x20) & 2)) {
            p->n = start;  /* the game draws this node */
        } else {
            *(uint8_t *)(uintptr_t)(node + 0x20) |= 2;
            k->hidden[k->nhidden++] = node;
        }
        i = j;
    }
    k->pass = p;
    __atomic_add_fetch(&nodes_hidden, k->nhidden, __ATOMIC_RELAXED);
    __atomic_add_fetch(&draws_native, p->n, __ATOMIC_RELAXED);
    __atomic_add_fetch(&draws_since_toggle, p->n, __ATOMIC_RELAXED);
}

/* The layer has been drawn by the game: give the nodes back, queue our pass after it. */
static void end_layer(Layer *k, const CPUState *ctx) {
    if (!k->active) return;
    k->active = 0;
    __atomic_add_fetch(&layers_seen, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&ours_total, k->ours.n, __ATOMIC_RELAXED);
    __atomic_add_fetch(&drawn_total, k->ndrawn, __ATOMIC_RELAXED);
    if (mode == MODE_CHECK) compare(k);
    for (int i = 0; i < k->nhidden; i++) *(uint8_t *)(uintptr_t)(k->hidden[i] + 0x20) &= (uint8_t)~2;
    k->nhidden = 0;
    if (k->pass && k->pass->n && ctx) {
        ShadowPass *p = k->pass;
        nvn_record_native(ctx, U64(k->device + 0x10d0) + 0xd8, gpu_shadow_pass, &p, sizeof p);
    }
    k->pass = NULL;
}

/* ---- the mode: YW4_SHADOWS at start, F9 (window.c, or a replay's "k <tick> F9") switches
 * between the game's shadows and ours while the game runs */

static void init_mode(void) {
    const char *s = getenv("YW4_SHADOWS");
    int m = s && !strcmp(s, "native") ? MODE_NATIVE : s && !strcmp(s, "check") ? MODE_CHECK : MODE_GAME;
    int unset = -1;
    if (!__atomic_compare_exchange_n(&mode, &unset, m, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&shadow_checking, m != MODE_GAME, __ATOMIC_RELEASE);
    if (m != MODE_GAME) fprintf(stderr, "yw4: shadows: %s\n", m == MODE_CHECK ? "checking the gather"
                                                                : "our pass draws the static casters");
}

void shadow_toggle(void) {
    init_mode();
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE) == MODE_GAME ? MODE_NATIVE : MODE_GAME;
    /* a layer already begun keeps what it hid and queued: its cascade-end hook finishes it */
    __atomic_store_n(&shadow_checking, m != MODE_GAME, __ATOMIC_RELEASE);
    __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: shadows: %s (F9; %ld submeshes drawn by our pass since the last switch)\n",
            m == MODE_NATIVE ? "ours for the static casters" : "the game's",
            __atomic_exchange_n(&draws_since_toggle, 0, __ATOMIC_RELAXED));
}

/* ---- hooks */

int yw4_hook_model_renderer(CPUState *ctx) {
    if (__atomic_load_n(&mode, __ATOMIC_ACQUIRE) < 0) init_mode();
    if (__atomic_load_n(&mode, __ATOMIC_ACQUIRE) == MODE_GAME) return 0;
    if (lay && lay->active) end_layer(lay, NULL);  /* not ended by its job (shouldn't happen): no pass */
    uint64_t list = ctx->x[1], item = ctx->x[2];
    if (!item || U8(item + 8) != LAYER_SHADOW) return 0;
    if (!lay && !(lay = calloc(1, sizeof *lay))) return 0;
    Layer *k = lay;
    k->device = ctx->x[0];
    uint64_t view = U64(k->device + 0x2780);
    k->cascade = view && U8(view) >= 1 && U8(view) <= 3 ? U8(view) - 1 : -1;
    /* the cascade's matrices, put in the render state by the game's job main+0x7ee510 */
    Mat4 proj = load(k->device + 0x280), viewm = load(k->device + 0x2c0);
    k->vp = mul(&proj, &viewm);
    gather(list, item, &k->vp, &k->ours);
    k->ndrawn = 0;
    k->pending = 0;
    if (mode == MODE_NATIVE) take_over(k);
    k->active = 1;
    report();
    return 0;
}

/* main+0xa75e90 (device, geometry, submesh): the model renderer's submesh draw. Only noted. */
int yw4_hook_submesh_draw(CPUState *ctx) {
    if (!lay || !lay->active || lay->ndrawn >= MAX_CASTERS) return 0;
    static uint64_t base;
    if (!base)
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, "main")) base = *yw4_code[i].base;
    uint64_t ret = ctx->x[30] - base;
    if (ret != 0xa4bf94 && ret != 0xa4c2d0) return 0;  /* from the model renderer */
    /* the object being drawn is in the model renderer's stack slot sp+0x390 */
    lay->drawn[lay->ndrawn][0] = U64(ctx->sp + 0x390);
    lay->drawn[lay->ndrawn][1] = ctx->x[2];
    lay->ndrawn++;
    lay->pending = 1;
    return 0;
}

/* main+0x7ed350 (device, ...): the job that ends a cascade, right after its layer. */
int yw4_hook_cascade_end(CPUState *ctx) {
    if (lay && lay->active) end_layer(lay, ctx);
    return 0;
}
