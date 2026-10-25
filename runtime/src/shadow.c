/* Our shadow-caster pass (decision 0024, docs/frame.md "Shadow maps").
 *
 * The game draws each shadow cascade's casters with its model renderer main+0xa45890
 * (device, draw list, layer item), a layer item of kind 1. Hooks (recomp/overrides.toml) run
 * before it, before its submesh draw main+0xa75e90 and before the job that ends the cascade,
 * main+0x7ed350.
 *
 * YW4_SHADOWS=native: the game still chooses what goes into a cascade (its walk of the draw
 * list, its culling, its sort), but when it is about to draw a submesh our pass can draw (the
 * submesh-draw hook), we skip the game's draw, with its state flush, NVN calls and their
 * translation, and add ours to a pass that is queued right after the game's draws of the
 * layer (in the cascade-end hook), into the layer the game bound and cleared. What we can
 * draw is learned from the game's own draws of that submesh; each object goes back to the
 * game one frame in 32 to check it again. So our shadows are always the game's selection, and
 * the game draws whatever we can't.
 *
 * YW4_SHADOWS=check: gather only, and compare with the submeshes the game drew. Missing ones
 * are bugs in the gather; extra ones are casters the game culled and we don't (instanced).
 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gbuffer.h"
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
    /* planes every corner is outside of: x<-w x>w y<-w y>w, behind the projection's eye (w<=0:
     * the cascades' projections are perspective-like, and there the side tests flip), beyond
     * the far plane (z>w). Not the near plane: casters between the light and the cascade still
     * throw shadows into it */
    unsigned all = 0x3F;
    for (int c = 0; c < 8; c++) {
        float v[4] = {c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2], 1}, p[4];
        for (int i = 0; i < 4; i++)
            p[i] = mvp->m[i * 4] * v[0] + mvp->m[i * 4 + 1] * v[1] + mvp->m[i * 4 + 2] * v[2] + mvp->m[i * 4 + 3];
        unsigned out = (p[0] < -p[3]) | (p[0] > p[3]) << 1 | (p[1] < -p[3]) << 2 | (p[1] > p[3]) << 3 |
                       (p[3] <= 0) << 4 | (p[2] > p[3]) << 5;
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

enum { PASS_DRAWS = 4096, RING = 64 };

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
/* Skinned casters: up to 8 bones per vertex from a per-submesh palette (vs cbuf 4), then as
 * the static program. Usually with the fade fragment program, which drops pixels by a screen
 * pattern while the object fades out (fs cbuf 3 byte 28 below 1); while it is 1 nothing is
 * dropped and our depth-only draw is the same. */
#define SKINNED_VS 0x71faac9f597b428dull
/* The same with up to 4 bones per vertex: float weights (attribute 1) and bone indices (attribute 2)
 * in the formats the game's vertex layout gives, no 1/65535 scale (shadow_skinned4.vert). */
#define SKINNED4_VS 0x26b7074dd30b03b3ull
static int is_skinned(uint64_t vs) { return vs == SKINNED_VS || vs == SKINNED4_VS; }
#define FADE_FS 0xb7cd6ed8cd28a184ull
/* Fading instanced casters: the instanced programs for copies the callback nudged (flag x 100
 * added to the first value). The vertex program decodes each copy: first value sign x 100 x
 * fract(|v| / 100), fade floor(|v| / 100) / 255; the fragment program drops the fragment where
 * fade x fs cbuf 3 byte 28 is below the 16x16 dither texture (fs slot 18) at the pixel mod 16.
 * The cut-out one first drops texels as the cut-out programs do (dceb89's test in d5f536). */
#define FADE_INSTANCED_VS 0x9904c21a86e1e9f8ull
#define FADE_CUTOUT_INSTANCED_VS 0xf8951f14aed20733ull
#define FADE_CUTOUT_FS 0xd5f536a14604f183ull

/* Wind-swayed casters (plants): instanced or not, always with the cut-out fragment program; the
 * vertices move with the wind in world space before projection x view (shadow_wind.vert has the
 * math): vs cbuf 6 rows 36, 47 (wind direction, strength, time; wind map placement), cbuf 7 rows
 * 0, 3 (the material's), vs textures 11 (noise) and 12 (wind map); attribute 1 the vertex's wind
 * weights, attribute 2 its texture coordinates. */
#define WIND_INSTANCED_VS 0xfa9033d5819c7ebbull
#define WIND_VS 0x61ebacf08dbc2b7dull

static int is_instanced(uint64_t vs) {
    return vs == INSTANCED_VS || vs == CUTOUT_INSTANCED_VS || vs == FADE_INSTANCED_VS || vs == FADE_CUTOUT_INSTANCED_VS ||
           vs == WIND_INSTANCED_VS;
}
static int is_wind(uint64_t vs) { return vs == WIND_INSTANCED_VS || vs == WIND_VS; }
static int is_fading(uint64_t vs) { return vs == FADE_INSTANCED_VS || vs == FADE_CUTOUT_INSTANCED_VS; }
/* programs our pass mirrors */
static int mirrored(uint64_t vs, uint64_t fs) {
    return ((vs == STATIC_VS || vs == INSTANCED_VS) && fs == DEPTH_FS) ||
           ((vs == CUTOUT_STATIC_VS || vs == CUTOUT_INSTANCED_VS) && fs == CUTOUT_FS) ||
           (is_skinned(vs) && (fs == DEPTH_FS || fs == FADE_FS)) || (vs == FADE_INSTANCED_VS && fs == FADE_FS) ||
           (vs == FADE_CUTOUT_INSTANCED_VS && fs == FADE_CUTOUT_FS) || (is_wind(vs) && fs == CUTOUT_FS);
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
    int game_drew;         /* the game has drawn in this layer: its first draw binds and clears the layer */
    const float *pending_copies;  /* check: the copies pending when the game's draw began */
    uint32_t pending_count;
    int pending_copies_ok;
    ShadowPass *pass;
    /* the renderer's world for the submesh being drawn ([sp+0x30]+0x40 at the submesh draw, as
     * for the G-buffer): the object's own world is wrong for posed parts (the bamboo forest's
     * stalks: one object, a world per part) */
    Mat4 world;
    int have_world;
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
    uint64_t dither;   /* fading instanced casters: the dither texture (fs slot 18) */
    uint32_t wind_format, wind_offset;  /* wind-swayed casters: attribute 1 */
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
static long draws_replaced, draws_native;
static long draws_since_toggle;  /* submeshes drawn by our pass since the last F9 */
static long compared, wrong_vb, wrong_ib, wrong_mvp;  /* the game's static and instanced draws vs ours */
static long compared_inst, wrong_inst, pending_checked, pending_wrong;  /* of those, instanced; instance buffer or count different */
static long copies_culled;  /* copies the game drew that our culling drops */
/* the game's draws our pass can't repeat, by programs and why (reject: not mirrored, then which
 * test of shadow_check_draw / compare_draw turned it down) */
enum { R_PROGRAM, R_CASTER, R_DESCRIBE, R_COPIES, R_VB, R_IB, R_MATRIX, R_LAYOUT, R_FADE, R_SKINNED, R_PALETTE, R_N };
static const char *const reject_name[R_N] = {"other programs", "not gathered", "not described", "copies",
                                             "vertex buffer", "index buffer", "matrix", "vertex layout or texture",
                                             "fade", "skinned vertex layout", "bone palette"};
static _Thread_local int reject;
enum { OTHERS = 24 };
static struct { uint64_t vs, fs; long n[R_N]; } others[OTHERS];

static void note_other(uint64_t vs, uint64_t fs, int why) {
    int i = 0;
    while (i < OTHERS && __atomic_load_n(&others[i].vs, __ATOMIC_RELAXED) &&
           !(others[i].vs == vs && others[i].fs == fs))
        i++;
    if (i == OTHERS) return;
    if (!others[i].vs) others[i].fs = fs, __atomic_store_n(&others[i].vs, vs | !vs, __ATOMIC_RELAXED);
    __atomic_add_fetch(&others[i].n[why], 1, __ATOMIC_RELAXED);
}

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
        fprintf(stderr, "yw4: shadows:   instanced draws: the copies pending at the draw differ from the game's in %ld of %ld\n",
                __atomic_exchange_n(&pending_wrong, 0, __ATOMIC_RELAXED),
                __atomic_exchange_n(&pending_checked, 0, __ATOMIC_RELAXED));
        fprintf(stderr, "yw4: shadows:   static and instanced draws compared with ours %ld: wrong vertices %ld, indices %ld, "
                "matrix %ld; instanced %ld: wrong copies %ld, drawn copies we cull %ld\n",
                __atomic_exchange_n(&compared, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&wrong_vb, 0, __ATOMIC_RELAXED),
                __atomic_exchange_n(&wrong_ib, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&wrong_mvp, 0, __ATOMIC_RELAXED),
                __atomic_exchange_n(&compared_inst, 0, __ATOMIC_RELAXED), __atomic_exchange_n(&wrong_inst, 0, __ATOMIC_RELAXED),
                __atomic_exchange_n(&copies_culled, 0, __ATOMIC_RELAXED));
    } else {
        long ps, ds;
        gpu_shadow_counts(&ps, &ds);
        fprintf(stderr, "yw4: shadows: frame %" PRIu64 ": %ld shadow layers; %ld of the game's submesh draws replaced by "
                "%ld of ours, %ld left to the game; skipped: %ld passes (other target bound), %ld draws (memory)\n", f, l,
                __atomic_exchange_n(&draws_replaced, 0, __ATOMIC_RELAXED),
                __atomic_exchange_n(&draws_native, 0, __ATOMIC_RELAXED), d, ps, ds);
    }
    for (int i = 0; i < OTHERS && others[i].vs; i++) {
        fprintf(stderr, "yw4: shadows:   the game's: vs %016" PRIx64 " fs %016" PRIx64 ":", others[i].vs, others[i].fs);
        for (int r = 0; r < R_N; r++)
            if (others[i].n[r]) fprintf(stderr, " %s %ld,", reject_name[r], others[i].n[r]);
        fprintf(stderr, "\n");
    }
    memset(others, 0, sizeof others);
}

/* nvn_cmdbuf.c: a draw was recorded; if it is the submesh draw noted last on this thread, it
 * is the game's draw of that submesh: learn its program and the cascade's target. */
static int compare_draw(const NvnCmdBuf *cb, uint64_t obj, uint64_t sub, uint64_t index_addr,
                        const ShadowDrawCall *call, int instanced);

/* A skinned submesh's bone palette (3x4 per bone), as the model renderer finds it: the
 * submesh's 0x140-byte record at [model+0xc8] (the size at +0x98) holds at +0x78 the buffer
 * object the engine uploads into vs cbuf 4 right before the draw; its data at [B+0x40] +
 * [B+0x58]. 0 if missing. */
static int palette_of(const Caster *c, uint64_t *data, uint32_t *size) {
    uint64_t recs = U64(c->model + 0xc8), hdr = U64(c->geometry + 0x18);
    if (!recs || !hdr) return 0;
    uint64_t subs = hdr + 4ull * U16(hdr + 0xa) + 4ull * U16(hdr + 0x60);
    if (c->submesh < subs || (c->submesh - subs) % 0x50 || (c->submesh - subs) / 0x50 >= U16(c->model + 0x200)) return 0;
    uint64_t rec = recs + 0x140 * ((c->submesh - subs) / 0x50), b = U64(rec + 0x78);
    if (!b || !U64(b + 0x40)) return 0;
    *data = U64(b + 0x40) + *(const uint32_t *)(uintptr_t)(b + 0x58);
    *size = *(const uint32_t *)(uintptr_t)(rec + 0x98);
    return *size && *size % 48 == 0 && *size <= 3 * 256 * 16;
}

static const Caster *caster_of(uint64_t obj, uint64_t sub) {
    for (int i = 0; i < lay->ours.n; i++)
        if (lay->ours.c[i].object == obj && lay->ours.c[i].submesh == sub) return &lay->ours.c[i];
    return NULL;
}

/* Is our palette the one the game drew with? The game's animation writes the next pose into the
 * same buffer while the render thread draws, so the copy the game made a moment ago can be a
 * little different from what we read now: close (mean difference per value under 0.01) is
 * enough. Another submesh's or another model's palette is far from it. */
static int palette_close(const float *ours, const float *game, uint32_t size) {
    double sum = 0;
    uint32_t n = size / 4;
    for (uint32_t i = 0; i < n; i++) sum += fabs((double)ours[i] - game[i]);
    return n && sum / n < 0.01;
}

/* A skinned draw our pass can repeat: the vertex layout our pipeline reads (position at 0,
 * weights 0x2f at 28, bone indices 0x2c at 44, one vertex buffer and stride), our palette
 * the game's cbuf 4 (palette_close), and with the fade program, the fade at 1. */
static int skinned_ok(const NvnCmdBuf *cb, uint64_t obj, uint64_t sub, uint64_t vs, uint64_t fs, Cutout *cut) {
    static const uint32_t want[3] = {0x22, 0x2f | 28u << 13, 0x2c | 44u << 13};
    if (cb->trace_nattribs < 3) return 0;
    for (uint32_t i = 0; i < cb->trace_nattribs; i++) {
        uint32_t a = cb->trace_attribs[i], stream = a >> 8 & 0x1F;
        /* the 4-bone program: position at 0, weights and bone indices in any format and place */
        if (vs == SKINNED4_VS ? (i >= 3 ? (a & 0xFF) != 0 : i == 0 ? (a & ~(0x1Fu << 8)) != want[0] : !(a & 0xFF))
                              : (i >= 3 ? (a & 0xFF) != 0 : (a & ~(0x1Fu << 8)) != want[i]))
            return 0;
        if (i < 3 && (stream >= 4 || cb->trace_vb[stream] != cb->trace_vb[0] ||
                      (uint32_t)cb->trace_streams[stream] != (uint32_t)cb->trace_streams[0]))
            return 0;
    }
    const Caster *c = caster_of(obj, sub);
    uint64_t data;
    uint32_t size;
    reject = R_PALETTE;
    if (!c || !palette_of(c, &data, &size) || !cb->trace_ub[0][4] ||
        !palette_close((const float *)(uintptr_t)data, (const float *)(uintptr_t)cb->trace_ub[0][4], size))
        return 0;
    if (fs == FADE_FS) {
        float fade;
        reject = R_FADE;
        if (!cb->trace_ub[1][3]) return 0;
        memcpy(&fade, (const void *)(uintptr_t)(cb->trace_ub[1][3] + 28), 4);
        if (fade != 1.0f) return 0;
    }
    if (vs == SKINNED4_VS) {  /* the weights' and bone indices' formats and offsets, for our draw */
        uint32_t a1 = cb->trace_attribs[1], a2 = cb->trace_attribs[2];
        cut->uv_format = a1 & 0xFF, cut->uv_offset = a1 >> 13;
        cut->wind_format = a2 & 0xFF, cut->wind_offset = a2 >> 13;
    }
    return 1;
}

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

static uint64_t wind_tex[2];  /* the wind draws' vs textures 11 and 12, learned from them */

/* A wind-swayed draw our pass can repeat: position (0x22) at 0, wind weights in attribute 1 and
 * texture coordinates in attribute 2, all from one vertex buffer and stride; the leaf texture
 * and threshold as for cut-out casters; both wind textures bound (one texture each everywhere,
 * it seems: the latest handles, and a word if another texture turns up). */
static int wind_of(const NvnCmdBuf *cb, Cutout *cut) {
    uint32_t a0 = cb->trace_attribs[0], a1 = cb->trace_attribs[1], a2 = cb->trace_attribs[2];
    if (cb->trace_nattribs < 3 || (a0 & 0xFF) != 0x22 || a0 >> 8 & 0x1F || a0 >> 13) return 0;
    for (uint32_t i = 3; i < cb->trace_nattribs; i++)
        if (cb->trace_attribs[i] & 0xFF) return 0;
    for (int i = 1; i < 3; i++) {
        uint32_t a = cb->trace_attribs[i], stream = a >> 8 & 0x1F;
        if (!(a & 0xFF) || stream >= 4 || cb->trace_vb[stream] != cb->trace_vb[0] ||
            (uint32_t)cb->trace_streams[stream] != (uint32_t)cb->trace_streams[0])
            return 0;
    }
    if (!cb->trace_tex[1][0] || !cb->trace_ub[0][3] || !cb->trace_tex[0][11] || !cb->trace_tex[0][12]) return 0;
    cut->texture = cb->trace_tex[1][0];
    memcpy(&cut->threshold, (const void *)(uintptr_t)(cb->trace_ub[0][3] + 12), 4);
    cut->uv_format = a2 & 0xFF, cut->uv_offset = a2 >> 13;
    cut->wind_format = a1 & 0xFF, cut->wind_offset = a1 >> 13;
    for (int t = 0; t < 2; t++) {
        uint64_t was = __atomic_exchange_n(&wind_tex[t], cb->trace_tex[0][11 + t], __ATOMIC_RELAXED);
        static int shown;
        if (was && (was & 0xFFFFF) != (cb->trace_tex[0][11 + t] & 0xFFFFF) && shown++ < 5)
            fprintf(stderr, "yw4: shadows: another wind texture %d: %#" PRIx64 " (was %#" PRIx64 ")\n", 11 + t,
                    cb->trace_tex[0][11 + t], was);
    }
    return 1;
}

static uint64_t fade_dither;  /* the fading draws' dither texture, learned from them */

/* A fading instanced draw our pass can repeat: its dither texture bound (fs slot 18), the
 * material's own fade (fs cbuf 3 byte 28) at 1, so that only the copies' fades remain, and for
 * the cut-out one the cut-out layout and data. */
static int fading_of(const NvnCmdBuf *cb, uint64_t fs, Cutout *cut) {
    float fade;
    if (!cb->trace_tex[1][18] || !cb->trace_ub[1][3]) return 0;
    memcpy(&fade, (const void *)(uintptr_t)(cb->trace_ub[1][3] + 28), 4);
    if (fade != 1.0f || (fs == FADE_CUTOUT_FS && !cutout_of(cb, cut))) return 0;
    cut->dither = cb->trace_tex[1][18];
    /* one dither texture for every fading draw, it seems (handle: texture id | sampler << 20; the
     * sampler changes, and texelFetch doesn't use it): the latest handle; say so if another
     * texture turns up */
    uint64_t was = __atomic_exchange_n(&fade_dither, cut->dither, __ATOMIC_RELAXED);
    static int shown;
    if (was && (was & 0xFFFFF) != (cut->dither & 0xFFFFF) && shown++ < 5)
        fprintf(stderr, "yw4: shadows: another dither texture %#" PRIx64 " (was %#" PRIx64 ")\n", cut->dither, was);
    return 1;
}

void shadow_check_draw(const NvnCmdBuf *cb, uint64_t index_addr, const ShadowDrawCall *call) {
    if (!lay || !lay->active || !lay->pending) return;
    lay->pending = 0;
    uint64_t obj = lay->drawn[lay->ndrawn - 1][0], sub = lay->drawn[lay->ndrawn - 1][1];
    uint64_t vs = cb->program ? cb->program->stages[0].key : 0, fs = cb->program ? cb->program->stages[1].key : 0;
    if (is_instanced(vs) && lay->pending_copies_ok) {  /* the copies ours() takes, as the game then drew */
        __atomic_add_fetch(&pending_checked, 1, __ATOMIC_RELAXED);
        if (lay->pending_count != call->instances || !cb->trace_ub[0][11] ||
            memcmp(lay->pending_copies, (const void *)(uintptr_t)cb->trace_ub[0][11], 48ull * lay->pending_count))
            __atomic_add_fetch(&pending_wrong, 1, __ATOMIC_RELAXED);
    }
    Cutout cut = {0};
    reject = R_PROGRAM;
    int ok = mirrored(vs, fs) &&
             (reject = R_LAYOUT, fs != CUTOUT_FS || (is_wind(vs) ? wind_of(cb, &cut) : cutout_of(cb, &cut))) &&
             (reject = R_FADE, !is_fading(vs) || fading_of(cb, fs, &cut)) &&
             (reject = R_SKINNED, !is_skinned(vs) || skinned_ok(cb, obj, sub, vs, fs, &cut)) &&
             compare_draw(cb, obj, sub, index_addr, call, is_instanced(vs) ? 1 : vs == WIND_VS ? 2 : 0);
    learn(obj, sub, vs, fs, ok, cb->polygon, &cut);
    if (!ok) note_other(vs, fs, reject);
    /* the pass draws where the game's draws in this layer went: a cascade's layers can have
     * different targets (cutscenes: two layers per cascade, two shadow maps) */
    if (lay->pass) {
        lay->pass->depth_addr = cb->depth_addr;
        lay->pass->depth_layer = cb->depth_layer;
    }
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
/* The caster's world: the renderer's for the submesh being drawn, else its object's. */
static Mat4 world_of(const Caster *c) { return lay && lay->have_world ? lay->world : load(c->own); }

static int describe(const Caster *c, const Mat4 *vp, ShadowDraw *d) {
    if (U8(c->submesh + 0x3d) != 1) return 0;  /* primitive table index 1: triangle list */
    uint64_t bufs = U64(c->geometry + 0x20);
    if (!bufs) return 0;
    Mat4 world = world_of(c), mvp = mul(vp, &world);
    d->vertices = U64(bufs) + *(const uint32_t *)(uintptr_t)c->submesh;
    /* the geometry's index buffer object (+0x28), as the submesh draw finds it: usually the second
     * object of the vertex buffer's record (+0x68 of it), not always */
    uint64_t ib = U64(c->geometry + 0x28);
    d->indices = ib ? U64(ib) + *(const uint32_t *)(uintptr_t)(c->submesh + 4) : 0;
    d->vertex_count = *(const uint32_t *)(uintptr_t)(c->submesh + 8);
    d->index_count = *(const uint32_t *)(uintptr_t)(c->submesh + 0xc);
    d->stride = U8(c->submesh + 0x3e);
    d->index_type = index_type(c->submesh);
    d->texture = 0, d->threshold = 0, d->uv_format = d->uv_offset = 0;
    d->palette_offset = d->palette_size = 0;
    d->dither = 0, d->first_copy = d->copies = 0, d->skin4 = 0;
    d->wind_tex[0] = d->wind_tex[1] = 0, d->wind_format = d->wind_offset = d->wind_header = 0;
    memcpy(d->mvp, mvp.m, sizeof d->mvp);
    return d->stride && (d->index_count ? d->indices != 0 : d->vertex_count != 0);
}

/* The copies the game's instance callback kept for the draw it is about to make: the model
 * renderer has put the callback's buffer object in vertex constant slot 11 and the count at
 * device+0x45cc before it calls the submesh draw. 0 if there is none. */
static const void *bound_constants(uint64_t device, int slot);
static int pending_copies(uint64_t device, const float **data, uint32_t *count) {
    if (!(*data = bound_constants(device, 11))) return 0;
    *count = *(const uint32_t *)(uintptr_t)(device + 0x45cc);
    return *count != 0;
}

/* The CPU data of the buffer object bound to vertex constant slot `slot` in the engine state
 * (device+13000, 0x18 bytes per slot; data at [B+0x40] + [B+0x58]); NULL if none. */
static const void *bound_constants(uint64_t device, int slot) {
    uint64_t b = U64(device + 13000 + 0x18ull * slot);
    if (!b || !U64(b + 0x40)) return NULL;
    return (const void *)(uintptr_t)(U64(b + 0x40) + *(const uint32_t *)(uintptr_t)(b + 0x58));
}

/* Is the submesh ours (the game drew it with programs we mirror), and how do we draw it?
 * Appends its draw to d (at most `room`; an instanced one with its copies in the copies ring) and returns
 * how many; -1: the game must draw it. */
static int env_on(const char *name) {
    const char *s = getenv(name);
    return s && atoi(s);
}

/* The game's draws left to it, for narrowing down a fault by elimination:
 * YW4_SHADOWS_NO_SKINNED=1 (characters), YW4_SHADOWS_NO_CUTOUT=1 (cut-out casters),
 * YW4_SHADOWS_NO_INSTANCED=1 (repeated props), YW4_SHADOWS_NO_WIND=1 (plants). */
static int leave_to_game(uint64_t vs, uint64_t fs) {
    static int no_skinned = -1, no_cutout = -1, no_instanced = -1, no_wind = -1;
    if (no_skinned < 0) no_skinned = env_on("YW4_SHADOWS_NO_SKINNED");
    if (no_cutout < 0) no_cutout = env_on("YW4_SHADOWS_NO_CUTOUT");
    if (no_instanced < 0) no_instanced = env_on("YW4_SHADOWS_NO_INSTANCED");
    if (no_wind < 0) no_wind = env_on("YW4_SHADOWS_NO_WIND");
    return (no_skinned && is_skinned(vs)) || (no_cutout && (fs == CUTOUT_FS || fs == FADE_CUTOUT_FS)) ||
           (no_instanced && is_instanced(vs)) || (no_wind && is_wind(vs));
}

static int copies_known(const Copies *cp, const float *game, uint32_t n);

/* A wind-swayed submesh (one learned as ours, `one` described): one draw, with a header in the
 * copies ring holding the wind constants as bound now (vs cbuf 6 rows 36, 47, cbuf 7 rows 0, 3:
 * equal to the game's draw in all of 50,000 checked), then per copy its copy x world rows (or the
 * world's alone without copies), as shadow_wind.vert reads them; projection x view in mvp. */
static int wind_draw(const Caster *c, const Mat4 *vp, uint64_t device, uint64_t vs, const ShadowDraw *one,
                     ShadowDraw *d, int room) {
    const float *c6 = bound_constants(device, 6), *c7 = bound_constants(device, 7);
    uint64_t t11 = __atomic_load_n(&wind_tex[0], __ATOMIC_RELAXED), t12 = __atomic_load_n(&wind_tex[1], __ATOMIC_RELAXED);
    if (room < 1 || !c6 || !c7 || !t11 || !t12 || !one->texture) return -1;
    const float *copies = NULL;
    uint32_t count = 1, first;
    if (vs == WIND_INSTANCED_VS) {  /* the copies the game would draw, as for the other instanced casters */
        Copies cp;
        if (!pending_copies(device, &copies, &count) || instances(c->model, &cp) != 1 || count > cp.count ||
            !copies_known(&cp, copies, 1) || !copies_known(&cp, copies + 12 * (count - 1), 1))
            return -1;
        for (uint32_t i = 0; i < count; i++)  /* no wind program decodes a nudged copy */
            if (copies[12 * i] >= 100.0f || copies[12 * i] <= -100.0f) return -1;
    }
    ShadowCopy *out = gpu_shadow_copies(count + 1, &first);
    if (!out) return -1;
    memcpy(out[0].mvp, c6 + 4 * 36, 16), memcpy(out[0].mvp + 4, c6 + 4 * 47, 16);
    memcpy(out[0].mvp + 8, c7, 16), memcpy(out[0].mvp + 12, c7 + 4 * 3, 16);
    Mat4 world = world_of(c);
    for (uint32_t i = 0; i < count; i++) {
        Mat4 m = world;
        if (copies) {
            Mat4 e = {{0}};
            memcpy(e.m, copies + 12 * i, 48);
            e.m[15] = 1;
            m = mul(&e, &world);
        }
        memcpy(out[1 + i].mvp, m.m, sizeof m.m);
        out[1 + i].fade = 1;
    }
    d[0] = *one;
    memcpy(d[0].mvp, vp->m, sizeof d[0].mvp);
    d[0].first_copy = first + 1, d[0].copies = count, d[0].wind_header = first;
    d[0].wind_tex[0] = t11, d[0].wind_tex[1] = t12;
    return 1;
}

static int ours(const Caster *c, const Mat4 *vp, uint64_t device, ShadowDraw *d, int room) {
    uint32_t h = learned_ours(c->object, c->submesh);
    ShadowDraw one;
    if (h == UINT32_MAX || leave_to_game(learned[h].vs, learned[h].fs) || !describe(c, vp, &one)) return -1;
    one.cull = learned[h].cull, one.front = learned[h].front;
    one.texture = learned[h].cut.texture, one.threshold = learned[h].cut.threshold;
    one.uv_format = learned[h].cut.uv_format, one.uv_offset = learned[h].cut.uv_offset;
    if (one.texture && one.uv_offset + 4 > one.stride) return -1;
    if (is_wind(learned[h].vs)) {
        one.wind_format = learned[h].cut.wind_format, one.wind_offset = learned[h].cut.wind_offset;
        if (one.wind_offset + 4 > one.stride) return -1;
        return wind_draw(c, vp, device, learned[h].vs, &one, d, room);
    }
    if (learned[h].vs == SKINNED4_VS) {  /* weights and bone indices as the game's layout had them */
        one.skin4 = 1;
        one.wind_format = learned[h].cut.wind_format, one.wind_offset = learned[h].cut.wind_offset;
        if (one.uv_offset + 4 > one.stride || one.wind_offset + 4 > one.stride) return -1;
    }
    if (is_skinned(learned[h].vs)) {
        /* the palette the game's draw would use, copied for ours: the snapshot the model renderer
         * bound to vs cbuf 4 (equal to the game's draw in every check; the submesh's own buffer
         * that palette_of finds is being written with the next pose and matched about half the
         * time, so the shadow's pose lagged and jumped) */
        uint64_t data;
        const void *pose = bound_constants(device, 4);
        if (room < 1 || !pose || !palette_of(c, &data, &one.palette_size) ||
            !gpu_shadow_palette(pose, one.palette_size, &one.palette_offset))
            return -1;
    }
    if (!is_instanced(learned[h].vs)) {
        if (room < 1) return -1;
        d[0] = one;
        return 1;
    }
    /* exactly the copies the game would draw: its callback culled them (side planes, near and
     * far, occlusion); copies crossing the cascade projection's eye plane would come out as
     * long streaks. They must be the model's own, or the game draws it: checked for the first and
     * the last (every copy was, in 100,000+ checked draws; all of them cost up to 64 x 64
     * compares a draw). One draw for all of them, as the game's. */
    const float *copies;
    uint32_t count, first;
    Copies cp;
    if (room < 1 || !pending_copies(device, &copies, &count) || instances(c->model, &cp) != 1 || count > cp.count ||
        !copies_known(&cp, copies, 1) || !copies_known(&cp, copies + 12 * (count - 1), 1))
        return -1;
    /* a copy the callback nudged (flag x 100 added to its first value: fading) makes the game
     * draw with the fading program, never with the plain instanced ones (drawn raw it stretches
     * across the layer). Learning sees the program change only one frame in 32, so every draw
     * decides as the game does: any nudged copy, and all copies are decoded as the fading vertex
     * program does, in the same float operations, and drawn with the dither. */
    int fading = 0;
    for (uint32_t i = 0; i < count; i++) fading |= copies[12 * i] >= 100.0f || copies[12 * i] <= -100.0f;
    uint64_t dither = __atomic_load_n(&fade_dither, __ATOMIC_RELAXED);
    if (fading && !dither) return -1;  /* no fading draw learned yet */
    ShadowCopy *out = gpu_shadow_copies(count, &first);
    if (!out) return -1;
    Mat4 world = world_of(c);
    for (uint32_t i = 0; i < count; i++) {
        Mat4 e = {{0}};
        memcpy(e.m, copies + 12 * i, 48);
        e.m[15] = 1;
        float fade = 1;
        if (fading) {
            float v = e.m[0] * 0.00999999978f, a = fabsf(v);
            float sign = (float)((e.m[0] < 0.0f ? -1 : 0) - (e.m[0] > 0.0f ? -1 : 0));
            e.m[0] = (a - floorf(a)) * sign * 100.0f;
            fade = floorf(a) * 0.00392156886f;
        }
        Mat4 ew = mul(&e, &world), mvp = mul(vp, &ew);
        memcpy(out[i].mvp, mvp.m, sizeof out[i].mvp);
        out[i].fade = fade;
    }
    d[0] = one;
    d[0].first_copy = first, d[0].copies = count, d[0].dither = fading ? dither : 0;
    return 1;
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
 * 128; instanced (1), the world = cbuf 2 byte 0, projection x view = cbuf 1 byte 128, the copies
 * = cbuf 11 and the draw's instance count, counted from vertex 0; 2: as instanced, no copies. */
static int compare_draw(const NvnCmdBuf *cb, uint64_t obj, uint64_t sub, uint64_t index_addr,
                        const ShadowDrawCall *call, int instanced) {
    const Caster *c = NULL;
    for (int i = 0; i < lay->ours.n && !c; i++)
        if (lay->ours.c[i].object == obj && lay->ours.c[i].submesh == sub) c = &lay->ours.c[i];
    ShadowDraw d;
    uint64_t ub = cb->trace_ub[0][2];
    if (!c) return reject = R_CASTER, 0;
    if (!describe(c, &lay->vp, &d) || !ub) return reject = R_DESCRIBE, 0;
    const float *game = (const float *)(uintptr_t)(ub + 128);
    float err, scale;
    int bad_inst = 0;
    if (!instanced) {
        err = max_diff(d.mvp, game, 16, &scale);
    } else {
        uint64_t vp_ub = cb->trace_ub[0][1], game_copies = cb->trace_ub[0][11];
        Copies cp;
        float s2;
        Mat4 world = world_of(c);
        err = max_diff(world.m, (const float *)(uintptr_t)ub, 16, &scale);
        if (vp_ub) {
            float e2 = max_diff(lay->vp.m, (const float *)(uintptr_t)(vp_ub + 128), 16, &s2);
            if (e2 / (s2 > 1 ? s2 : 1) > err / (scale > 1 ? scale : 1)) err = e2, scale = s2;
        } else {
            err = 1e30f;
        }
        if (instanced == 1) {  /* 2: world and projection x view only (WIND_VS) */
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
                Mat4 world = world_of(c);
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
    }
    int bad_vb = d.vertices != cb->trace_vb0, bad_ib = d.index_count && d.indices != index_addr;
    int bad_mvp = err > 1e-4f * (scale > 1 ? scale : 1);
    __atomic_add_fetch(&compared, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wrong_vb, bad_vb, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wrong_ib, bad_ib, __ATOMIC_RELAXED);
    __atomic_add_fetch(&wrong_mvp, bad_mvp, __ATOMIC_RELAXED);
    reject = bad_inst ? R_COPIES : bad_vb ? R_VB : bad_ib ? R_IB : R_MATRIX;
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

/* Native mode: the pass our replaced draws go into, for the cascade's layer. */
static void begin_pass(Layer *k) {
    k->pass = NULL;
    k->game_drew = 0;
    if (k->cascade < 0 || !cascade_target[k->cascade].known) return;  /* the game hasn't shown us the target yet */
    ShadowPass *p = new_pass();
    if (!p) return;
    p->depth_addr = cascade_target[k->cascade].addr;
    p->depth_layer = cascade_target[k->cascade].layer;
    k->pass = p;
}

/* The layer has been drawn by the game: queue our pass after it. */
static void end_layer(Layer *k, const CPUState *ctx) {
    if (!k->active) return;
    k->active = 0;
    __atomic_add_fetch(&layers_seen, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&ours_total, k->ours.n, __ATOMIC_RELAXED);
    __atomic_add_fetch(&drawn_total, k->ndrawn, __ATOMIC_RELAXED);
    if (mode == MODE_CHECK) compare(k);
    if (k->pass && k->pass->n && ctx) {
        ShadowPass *p = k->pass;
        __atomic_add_fetch(&draws_native, p->n, __ATOMIC_RELAXED);
        __atomic_add_fetch(&draws_since_toggle, p->n, __ATOMIC_RELAXED);
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
                                                                : "our pass draws the casters it mirrors");
}

const char *shadow_mode_name(void) {
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    return m == MODE_NATIVE ? "CUSTOM" : m == MODE_CHECK ? "CHECK" : "ORIGINAL";
}

void shadow_toggle(void) {
    init_mode();
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE) == MODE_GAME ? MODE_NATIVE : MODE_GAME;
    /* a layer already begun keeps what it hid and queued: its cascade-end hook finishes it */
    __atomic_store_n(&shadow_checking, m != MODE_GAME, __ATOMIC_RELEASE);
    __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: shadows: %s (F9; %ld submeshes drawn by our pass since the last switch)\n",
            m == MODE_NATIVE ? "ours" : "the game's",
            __atomic_exchange_n(&draws_since_toggle, 0, __ATOMIC_RELAXED));
}

/* ---- YW4_TRACE_LAYERS=<frame>: every layer the model renderer draws while presented frames
 * <frame> to <frame>+2 are current, one line each when the next layer starts on that thread: its
 * kind (+8), key range (+0xa, +0xc), mask (+0x10) and how many submeshes it drew. Maps layer kinds
 * to the GPU passes (the main view, docs/frame.md). */
extern uint64_t yw4_frames_presented;  /* dispatch.c */
static _Thread_local uint64_t trace_item;
static _Thread_local long trace_draws;

static int trace_layers_on(void) {
    static int from = -2;
    if (from == -2) {
        const char *e = getenv("YW4_TRACE_LAYERS");
        from = e && *e ? atoi(e) : -1;
    }
    uint64_t f = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    return from >= 0 && f >= (uint64_t)from && f < (uint64_t)from + 3;
}

static void trace_layer_start(const CPUState *ctx) {
    if (trace_item)
        fprintf(stderr, "yw4: layer t%04x item %#" PRIx64 " kind %u keys %u..%u mask %#x: %ld submeshes\n",
                (unsigned)((uintptr_t)ctx >> 4 & 0xFFFF), trace_item, U8(trace_item + 8), U16(trace_item + 0xa),
                U16(trace_item + 0xc), U8(trace_item + 0x10), trace_draws);
    trace_item = trace_layers_on() ? ctx->x[2] : 0;
    trace_draws = 0;
}

/* ---- hooks */

int yw4_hook_model_renderer(CPUState *ctx) {
    trace_layer_start(ctx);
    gbuffer_layer_start(ctx);  /* the G-buffer pass (decision 0026) shares these hooks */
    if (__atomic_load_n(&mode, __ATOMIC_ACQUIRE) < 0) init_mode();
    /* A cascade's job list can draw more than one shadow layer (cutscenes: two per cascade) before
     * the cascade-end job: the next layer of the same cascade ends the previous one, which queues its
     * pass here, still inside the cascade's target. Anything else: not ended by its job, no pass.
     * Before the mode check: after F9 to the game's shadows the next layer's draws must not be
     * taken for the open one's. */
    if (lay && lay->active) {
        uint64_t v = U64(ctx->x[0] + 0x2780);
        int c = v && U8(v) >= 1 && U8(v) <= 3 ? U8(v) - 1 : -1;
        end_layer(lay, lay->device == ctx->x[0] && c >= 0 && c == lay->cascade ? ctx : NULL);
    }
    if (__atomic_load_n(&mode, __ATOMIC_ACQUIRE) == MODE_GAME) return 0;
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
    if (mode == MODE_NATIVE) begin_pass(k);
    k->active = 1;
    report();
    return 0;
}

/* main+0xa75e90 (device, geometry, submesh): the model renderer's submesh draw. Noted (the
 * game's draw that follows is learned from); in native mode replaced by ours when we can. */
int yw4_hook_submesh_draw(CPUState *ctx) {
    trace_draws++;
    gbuffer_submesh(ctx);
    if (!lay || !lay->active || lay->ndrawn >= MAX_CASTERS) return 0;
    static uint64_t base;
    if (!base)
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, "main")) base = *yw4_code[i].base;
    uint64_t ret = ctx->x[30] - base;
    if (ret != 0xa4bf94 && ret != 0xa4c2d0) return 0;  /* from the model renderer */
    /* the object being drawn is in the model renderer's stack slot sp+0x390 */
    uint64_t obj = U64(ctx->sp + 0x390), sub = ctx->x[2];
    Layer *k = lay;
    memset(&k->world, 0, sizeof k->world);
    k->have_world = gbuffer_hook_world(ctx, k->world.m);
    k->world.m[15] = 1;
    ShadowPass *p = k->pass;
    uint64_t frame = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    if (mode == MODE_NATIVE && p && k->game_drew && ((frame + (obj >> 4)) & 31) != 0) {
        const Caster *c = caster_of(obj, sub);
        int n = c ? ours(c, &k->vp, k->device, &p->d[p->n], PASS_DRAWS - (int)p->n) : -1;
        if (n >= 0) {
            p->n += (uint32_t)n;
            __atomic_add_fetch(&draws_replaced, 1, __ATOMIC_RELAXED);
            /* YW4_SHADOWS_BOTH=1: the game draws too (ours on top), to tell our draws' errors
             * from the effects of skipping the game's draw */
            static int both = -1;
            if (both < 0) both = env_on("YW4_SHADOWS_BOTH");
            if (!both) return 1;  /* the game's draw is skipped */
        }
    }
    k->pending_copies_ok = pending_copies(k->device, &k->pending_copies, &k->pending_count);
    k->drawn[k->ndrawn][0] = obj;
    k->drawn[k->ndrawn][1] = sub;
    k->ndrawn++;
    k->pending = 1;
    k->game_drew = 1;
    return 0;
}

/* main+0x7ed350 (device, ...): the job that ends a cascade, right after its layer. */
int yw4_hook_cascade_end(CPUState *ctx) {
    if (lay && lay->active) end_layer(lay, ctx);
    return 0;
}
