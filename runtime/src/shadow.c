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
 * once the game has drawn it with the static or instanced program. So our share grows as kinds of casters
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
    uint64_t model;      /* object+0x140 + g*0x240: the geometry's model record (instances) */
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
                if (out->n < MAX_CASTERS) out->c[out->n++] = (Caster){node, obj0, obj, geom, mdl, s};
                else out->overflow = 1;
            }
        }
    }
}


/* ---- the layer being drawn on this thread (the shadow layers all run on one render thread) */

enum { MAX_HIDDEN = 2048, PASS_DRAWS = 4096, RING = 64 };

/* The game's programs for static casters: projection x view x world, and a fragment stage
 * that does nothing (the same vertex program also comes with cut-out fragment programs).
 * Instanced casters: projection x view (vs cbuf 1 byte 128) x the copy's 3x4 transform (vs
 * cbuf 11, 48 bytes per copy, counted from the draw's first instance) x world (vs cbuf 2). */
#define STATIC_VS 0xb320bd9159e2fd0aull
#define INSTANCED_VS 0xa748428f2d024ad7ull
#define DEPTH_FS 0x6ead6a69737c52c2ull
/* Cut-out casters: the static or instanced program, plus texture coordinates (attribute 1) and
 * the material's alpha threshold (vs cbuf 3 byte 12) for a fragment stage that drops texels
 * below it (texture: fs slot 0). */
#define CUTOUT_STATIC_VS 0x4cbb8e9627462c17ull
#define CUTOUT_INSTANCED_VS 0x744205aaee24163eull
#define CUTOUT_FS 0xdceb89e5f80d79e1ull

static int is_instanced(uint64_t vs) { return vs == INSTANCED_VS || vs == CUTOUT_INSTANCED_VS; }
/* programs our pass mirrors */
static int mirrored(uint64_t vs, uint64_t fs) {
    return ((vs == STATIC_VS || vs == INSTANCED_VS) && fs == DEPTH_FS) ||
           ((vs == CUTOUT_STATIC_VS || vs == CUTOUT_INSTANCED_VS) && fs == CUTOUT_FS);
}

/* An instanced model's copies. The model renderer gets them from the model's callback
 * main+0x5f9420 (model+0xe0), which culls them against the view (box per copy, then an
 * occlusion test) and writes the kept ones into a new buffer each frame, bound as vs cbuf 11.
 * All copies: model+0x214 sets (0 counts as 1; each drawn as its own draw) of [model+0xd0]
 * (0x38 bytes each: first copy +0x30, count +0x32), their transforms at [model+0x168] (48
 * bytes each), a byte per copy at [model+0x170]. */
enum { MAX_SETS = 8 };
typedef struct {
    uint64_t transforms, flags;
    uint16_t first[MAX_SETS], in_set[MAX_SETS];
    uint32_t sets, count;  /* count: of all sets */
    int nudge;  /* model+0x218: the callback adds flag x 100 to each copy's first value */
} Copies;

/* 1: found; 0: not instanced; -1: a layout we don't read (split copies, too many sets). */
static int instances(uint64_t mdl, Copies *out) {
    if (!U64(mdl + 0x80)) return 0;
    uint64_t set = U64(mdl + 0xd0);
    out->sets = U8(mdl + 0x214) ? U8(mdl + 0x214) : 1;
    if (out->sets > MAX_SETS || U8(mdl + 0x216)) return -1;
    if (!set || !U64(mdl + 0x168) || !U64(mdl + 0x170)) return -1;
    out->transforms = U64(mdl + 0x168);
    out->flags = U64(mdl + 0x170);
    out->nudge = U8(mdl + 0x218) != 0;
    out->count = 0;
    for (uint32_t k = 0; k < out->sets; k++) {
        out->first[k] = U16(set + 0x38ull * k + 0x30);
        out->in_set[k] = U16(set + 0x38ull * k + 0x32);
        out->count += out->in_set[k];
    }
    return out->count ? 1 : -1;
}

/* Copy i (counted over all sets)'s transform as the callback hands it to the shader, as a
 * 4x4 (3 rows, then 0 0 0 1). */
static Mat4 instance_matrix(const Copies *c, uint32_t i) {
    uint32_t k = 0;
    while (k + 1 < c->sets && i >= c->in_set[k]) i -= c->in_set[k++];
    uint64_t at = (uint64_t)c->first[k] + i;
    Mat4 e = {{0}};
    memcpy(e.m, (const void *)(uintptr_t)(c->transforms + 48 * at), 48);
    if (c->nudge && e.m[0] < 100.0f) {
        float add = (float)U8(c->flags + at) * 100.0f;
        e.m[0] += e.m[0] >= 0 ? add : -add;
    }
    e.m[15] = 1;
    return e;
}

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
 * once all its submeshes are known to be drawn with STATIC_VS or INSTANCED_VS and DEPTH_FS. The model and
 * material are kept to notice an object's memory being reused for another one. */
enum { LEARN_SLOTS = 1 << 16 };
typedef struct {
    uint64_t texture;  /* 0: not a cut-out */
    float threshold;
    uint32_t uv_format, uv_offset;
} Cutout;
static struct { uint64_t obj, sub, model, mat, vs, fs; int ok; uint8_t cull, front; Cutout cut; } learned[LEARN_SLOTS];

static uint32_t learn_slot(uint64_t obj, uint64_t sub) {
    uint32_t h = (uint32_t)((obj * 0x9E3779B97F4A7C15ull ^ sub * 0xC2B2AE3D27D4EB4Full) >> 48);
    for (uint32_t i = 0; i < LEARN_SLOTS; i++, h = (h + 1) & (LEARN_SLOTS - 1))
        if (!learned[h].obj || (learned[h].obj == obj && learned[h].sub == sub)) return h;
    return UINT32_MAX;
}

/* Was the submesh last drawn by the game with programs our pass mirrors, and did our draw
 * match it (and the object not reused since)? Its slot in `learned`, or UINT32_MAX. */
static uint32_t learned_ours(uint64_t obj, uint64_t sub) {
    uint32_t h = learn_slot(obj, sub);
    if (h == UINT32_MAX || learned[h].obj != obj) return UINT32_MAX;
    if (learned[h].model != U64(obj + 0x140) || learned[h].mat != U64(obj + 0x120)) return UINT32_MAX;  /* reused */
    return mirrored(learned[h].vs, learned[h].fs) && learned[h].ok ? h : UINT32_MAX;
}

/* ok: our draw of it would equal the game's (same buffers, same matrix: compare_draw) */
static void learn(uint64_t obj, uint64_t sub, uint64_t vs, uint64_t fs, int ok, const uint8_t polygon[4],
                  const Cutout *cut) {
    uint32_t h = learn_slot(obj, sub);
    if (h == UINT32_MAX) return;
    learned[h].obj = obj, learned[h].sub = sub, learned[h].model = U64(obj + 0x140), learned[h].mat = U64(obj + 0x120);
    learned[h].vs = vs, learned[h].fs = fs, learned[h].ok = ok;
    learned[h].cull = polygon[0], learned[h].front = polygon[1];
    learned[h].cut = *cut;
}

/* The shadow map layer each cascade's draws went into, learned from the game's draws. */
static struct { uint64_t addr; uint32_t layer; int known; } cascade_target[3];

/* Statistics since the last report */
static long layers_seen, missing_total, extra_total, ours_total, drawn_total;
static long nodes_hidden, draws_native;
static long draws_since_toggle;  /* submeshes drawn by our pass since the last F9 */
static long compared, wrong_vb, wrong_ib, wrong_mvp;  /* the game's static and instanced draws vs ours */
static long compared_inst, wrong_inst;  /* of those, instanced; instance buffer or count different */
static long copies_culled;  /* copies the game drew that our culling drops */

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
        fprintf(stderr, "yw4: shadows:   static and instanced draws compared with ours %ld: wrong vertices %ld, indices %ld, "
                "matrix %ld; instanced %ld: wrong copies %ld, drawn copies we cull %ld\n",
                __atomic_exchange_n(&compared, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&wrong_vb, 0, __ATOMIC_RELAXED),
                __atomic_exchange_n(&wrong_ib, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&wrong_mvp, 0, __ATOMIC_RELAXED),
                __atomic_exchange_n(&compared_inst, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&wrong_inst, 0, __ATOMIC_RELAXED),
                __atomic_exchange_n(&copies_culled, 0, __ATOMIC_RELAXED));
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
static int compare_draw(const NvnCmdBuf *cb, uint64_t obj, uint64_t sub, uint64_t index_addr,
                        const ShadowDrawCall *call, int instanced);

/* A cut-out draw's texture, threshold and texture coordinates, as the game bound them; 0 if
 * its vertex layout isn't the one our pass reads: position (0x22) at 0 and texture
 * coordinates in attribute 1, both from one vertex buffer and stride. */
static int cutout_of(const NvnCmdBuf *cb, Cutout *cut) {
    uint32_t a0 = cb->trace_attribs[0], a1 = cb->trace_attribs[1];
    if (cb->trace_nattribs < 2 || (a0 & 0xFF) != 0x22 || a0 >> 8 & 0x1F || a0 >> 13) return 0;
    for (uint32_t i = 2; i < cb->trace_nattribs; i++)
        if (cb->trace_attribs[i] & 0xFF) return 0;
    uint32_t stream = a1 >> 8 & 0x1F;
    if (!(a1 & 0xFF) || stream >= 4 || cb->trace_vb[stream] != cb->trace_vb[0] ||
        (uint32_t)cb->trace_streams[stream] != (uint32_t)cb->trace_streams[0])
        return 0;
    if (!cb->trace_tex[1][0] || !cb->trace_ub[0][3]) return 0;
    cut->texture = cb->trace_tex[1][0];
    memcpy(&cut->threshold, (const void *)(uintptr_t)(cb->trace_ub[0][3] + 12), 4);
    cut->uv_format = a1 & 0xFF;
    cut->uv_offset = a1 >> 13;
    return 1;
}

void shadow_check_draw(const NvnCmdBuf *cb, uint64_t index_addr, const ShadowDrawCall *call) {
    if (!lay || !lay->active || !lay->pending) return;
    lay->pending = 0;
    uint64_t obj = lay->drawn[lay->ndrawn - 1][0], sub = lay->drawn[lay->ndrawn - 1][1];
    uint64_t vs = cb->program ? cb->program->stages[0].key : 0, fs = cb->program ? cb->program->stages[1].key : 0;
    Cutout cut = {0};
    int ok = mirrored(vs, fs) && (fs != CUTOUT_FS || cutout_of(cb, &cut)) &&
             compare_draw(cb, obj, sub, index_addr, call, is_instanced(vs));
    learn(obj, sub, vs, fs, ok, cb->polygon, &cut);
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
    d->texture = 0, d->threshold = 0, d->uv_format = d->uv_offset = 0;
    memcpy(d->mvp, mvp.m, sizeof d->mvp);
    return d->stride && (d->index_count || d->vertex_count);
}

/* Is the submesh ours (the game drew it with programs we mirror), and how do we draw it?
 * Appends its draws to d (at most `room`: an instanced one is a draw per copy) and returns
 * how many; -1: the game must draw it. */
static int ours(const Caster *c, const Mat4 *vp, ShadowDraw *d, int room) {
    uint32_t h = learned_ours(c->object, c->submesh);
    ShadowDraw one;
    if (h == UINT32_MAX || !describe(c, vp, &one)) return -1;
    one.cull = learned[h].cull, one.front = learned[h].front;
    one.texture = learned[h].cut.texture, one.threshold = learned[h].cut.threshold;
    one.uv_format = learned[h].cut.uv_format, one.uv_offset = learned[h].cut.uv_offset;
    if (one.texture && one.uv_offset + 4 > one.stride) return -1;
    if (!is_instanced(learned[h].vs)) {
        if (room < 1) return -1;
        d[0] = one;
        return 1;
    }
    Copies cp;
    if (instances(c->model, &cp) != 1 || cp.count > (uint32_t)room) return -1;
    /* each copy culled by the object's box, which is one copy's (no copy is placed inside
     * it; check mode logs that) */
    Mat4 world = load(c->own);
    int n = 0;
    for (uint32_t i = 0; i < cp.count; i++) {
        Mat4 e = instance_matrix(&cp, i), ew = mul(&e, &world), mvp = mul(vp, &ew);
        if (outside(c->object, &mvp)) continue;
        d[n] = one;
        memcpy(d[n].mvp, mvp.m, sizeof d[n].mvp);
        n++;
    }
    return n;
}

/* Is each of the n copies the game drew (48 bytes each at `game`) one of ours? */
static int copies_known(const Copies *cp, const float *game, uint32_t n) {
    for (uint32_t j = 0; j < n; j++) {
        int found = 0;
        for (uint32_t i = 0; i < cp->count && !found; i++) {
            Mat4 e = instance_matrix(cp, i);
            found = !memcmp(e.m, game + 12 * j, 48);
        }
        if (!found) return 0;
    }
    return 1;
}

static float max_diff(const float *a, const float *b, int n, float *scale) {
    float err = 0;
    *scale = 0;
    for (int i = 0; i < n; i++) {
        float e = a[i] - b[i], s = b[i] < 0 ? -b[i] : b[i];
        if ((e < 0 ? -e : e) > err) err = e < 0 ? -e : e;
        if (s > *scale) *scale = s;
    }
    return err;
}

/* Check mode, an instanced object: is its box (local space) one copy's, or does it hold all
 * the copies' positions (the group's)? Decides how to cull copies. Logged for a few objects. */
static void log_instance_box(const Caster *c, const Copies *cp) {
    static int shown;
    if (shown >= 8) return;
    float lo[3], hi[3];
    memcpy(lo, (const void *)(uintptr_t)(c->object + 0x80), 12);
    memcpy(hi, (const void *)(uintptr_t)(c->object + 0x90), 12);
    uint32_t inside = 0, nudged = 0;
    for (uint32_t i = 0; i < cp->count; i++) {
        Mat4 e = instance_matrix(cp, i);
        float t[3] = {e.m[3], e.m[7], e.m[11]};
        inside += t[0] >= lo[0] && t[0] <= hi[0] && t[1] >= lo[1] && t[1] <= hi[1] && t[2] >= lo[2] && t[2] <= hi[2];
        nudged += cp->nudge && (e.m[0] >= 100.0f || e.m[0] <= -100.0f);
    }
    Mat4 e0 = instance_matrix(cp, 0);
    shown++;
    fprintf(stderr, "yw4: shadows: instanced object %#" PRIx64 ": %u copies (%u nudged), %u placed inside its box "
            "(%.2f %.2f %.2f .. %.2f %.2f %.2f); copy 0 at %.2f %.2f %.2f\n", c->object, cp->count, nudged, inside,
            lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], e0.m[3], e0.m[7], e0.m[11]);
}

/* The game drew (obj, sub) with a program our pass mirrors; would our draw be the same? The
 * vertex and index addresses must be equal, and the matrices: static, ours = vs cbuf 2 byte
 * 128; instanced, the world = cbuf 2 byte 0, projection x view = cbuf 1 byte 128, the copies
 * = cbuf 11 and the draw's instance count, counted from vertex 0. */
static int compare_draw(const NvnCmdBuf *cb, uint64_t obj, uint64_t sub, uint64_t index_addr,
                        const ShadowDrawCall *call, int instanced) {
    const Caster *c = NULL;
    for (int i = 0; i < lay->ours.n && !c; i++)
        if (lay->ours.c[i].object == obj && lay->ours.c[i].submesh == sub) c = &lay->ours.c[i];
    ShadowDraw d;
    uint64_t ub = cb->trace_ub[0][2];
    if (!c || !describe(c, &lay->vp, &d) || !ub) return 0;
    const float *game = (const float *)(uintptr_t)(ub + 128);
    float err, scale;
    int bad_inst = 0;
    if (!instanced) {
        err = max_diff(d.mvp, game, 16, &scale);
    } else {
        uint64_t vp_ub = cb->trace_ub[0][1], game_copies = cb->trace_ub[0][11];
        Copies cp;
        float s2;
        err = max_diff((const float *)(uintptr_t)c->own, (const float *)(uintptr_t)ub, 16, &scale);
        if (vp_ub) {
            float e2 = max_diff(lay->vp.m, (const float *)(uintptr_t)(vp_ub + 128), 16, &s2);
            if (e2 / (s2 > 1 ? s2 : 1) > err / (scale > 1 ? scale : 1)) err = e2, scale = s2;
        } else {
            err = 1e30f;
        }
        /* the game drew the copies its callback kept: each must be one of ours */
        bad_inst = instances(c->model, &cp) != 1 || !game_copies || call->base_vertex != 0 ||
                   call->instances > cp.count || !copies_known(&cp, (const float *)(uintptr_t)game_copies, call->instances);
        __atomic_add_fetch(&compared_inst, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&wrong_inst, bad_inst, __ATOMIC_RELAXED);
        static int shown_inst;
        if (mode == MODE_CHECK && bad_inst && shown_inst++ < 5)
            fprintf(stderr, "yw4: shadows: check: instanced object %#" PRIx64 ": %u copies of ours (found: %d) vs the "
                    "game's %u at %#" PRIx64 " (base vertex %d; model sets %u +0x216 %u +0x218 %u)\n", obj,
                    instances(c->model, &cp) == 1 ? cp.count : 0, instances(c->model, &cp), call->instances,
                    game_copies, call->base_vertex, U8(c->model + 0x214), U8(c->model + 0x216), U8(c->model + 0x218));
        if (!bad_inst) {
            Mat4 world = load(c->own);
            for (uint32_t j = 0; j < call->instances; j++) {
                Mat4 e, ew, mvp;
                memcpy(e.m, (const float *)(uintptr_t)game_copies + 12 * j, 48);
                e.m[12] = e.m[13] = e.m[14] = 0, e.m[15] = 1;
                ew = mul(&e, &world), mvp = mul(&lay->vp, &ew);
                if (outside(c->object, &mvp)) __atomic_add_fetch(&copies_culled, 1, __ATOMIC_RELAXED);
            }
        }
        if (mode == MODE_CHECK && !bad_inst) log_instance_box(c, &cp);
    }
    int bad_vb = d.vertices != cb->trace_vb0, bad_ib = d.index_count && d.indices != index_addr;
    int bad_mvp = err > 1e-4f * (scale > 1 ? scale : 1);
    __atomic_add_fetch(&compared, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wrong_vb, bad_vb, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wrong_ib, bad_ib, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wrong_mvp, bad_mvp, __ATOMIC_RELAXED);
    if (bad_inst) return 0;
    static int shown;
    if (mode == MODE_CHECK && !instanced && (bad_vb || bad_ib || bad_mvp) && shown++ < 5) {
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
        for (; j < k->ours.n && k->ours.c[j].node == node; j++) {
            int n = all ? ours(&k->ours.c[j], &k->vp, &p->d[p->n], PASS_DRAWS - (int)p->n) : -1;
            if (n < 0) all = 0;
            else p->n += (uint32_t)n;
        }
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
                                                                : "our pass draws the static and instanced casters");
}

void shadow_toggle(void) {
    init_mode();
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE) == MODE_GAME ? MODE_NATIVE : MODE_GAME;
    /* a layer already begun keeps what it hid and queued: its cascade-end hook finishes it */
    __atomic_store_n(&shadow_checking, m != MODE_GAME, __ATOMIC_RELEASE);
    __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: shadows: %s (F9; %ld submeshes drawn by our pass since the last switch)\n",
            m == MODE_NATIVE ? "ours for the static and instanced casters" : "the game's",
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
