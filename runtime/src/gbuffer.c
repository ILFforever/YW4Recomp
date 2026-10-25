/* Our G-buffer pass (decision 0026, docs/frame.md "Main view: the passes").
 *
 * The game draws the G-buffer with its model renderer main+0xa45890 on a layer of kind 2 (render
 * thread B), the same renderer and submesh draw main+0xa75e90 the shadow pass hooks (shadow.c,
 * which calls in here from its hooks).
 *
 * Step 1, YW4_GBUFFER=check: nothing is drawn. Before each G-buffer submesh draw we work out what
 * our pass would use (the vertex and index buffers from the geometry and submesh records, the
 * world matrix from the object, projection x view from the render state) and compare it with the
 * draw the game then records: its vertex and index buffers and vertex constants 2 (the object's
 * world in rows 0-2, this frame's projection x view x world in rows 8-11, last frame's in rows
 * 12-15). A summary goes to the log every 300 frames.
 *
 * Step 2a, YW4_GBUFFER=programs: our readable programs (runtime/shaders/gbuffer_main.*) in the
 * game's own draws (gpu_draw.c swaps them in).
 *
 * Step 2b, YW4_GBUFFER=native: also our own draw. When the game records the DrawElements of a
 * G-buffer submesh drawn with the main programs, we record ours in its place (gbuffer_take_draw):
 * the vertex and index buffers from the submesh, vertex constants 2 computed here from the
 * renderer's world and the camera. Material constants, textures, vertex formats and the targets
 * stay what the game bound before its draw. A draw whose buffers are not the ones the game was
 * about to draw stays the game's.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gbuffer.h"
#include "host.h"
#include "mat4.h"

extern uint64_t yw4_frames_presented;  /* dispatch.c */

#define U8(a) (*(const uint8_t *)(uintptr_t)(a))
#define U32(a) (*(const uint32_t *)(uintptr_t)(a))
#define U64(a) (*(const uint64_t *)(uintptr_t)(a))

enum { LAYER_MATERIAL = 0, LAYER_GBUFFER = 2 };

/* The lit material pass (docs/frame.md "Main view: the passes"): the same objects again with full
 * materials, layers of kind 0 with sort keys 0..2 (render thread C). Our draws go there too, in the
 * same mode as the G-buffer's (YW4_MATERIAL=0: not); the programs are in the same table. */
static int material_layer(uint64_t item) {
    static int on = -1;
    if (on < 0) on = !getenv("YW4_MATERIAL") || atoi(getenv("YW4_MATERIAL"));
    return on && U8(item + 8) == LAYER_MATERIAL && *(const uint16_t *)(uintptr_t)(item + 0xa) == 0 &&
           *(const uint16_t *)(uintptr_t)(item + 0xc) == 2;
}
/* The main view's other model renderer layers (kind 0, other keys: the named stages, characters,
 * map, effects; docs/frame.md "Main-view stages"). Our draws there too (YW4_STAGES=0: not). */
static int stage_layer(uint64_t item) {
    static int on = -1;
    if (on < 0) on = !getenv("YW4_STAGES") || atoi(getenv("YW4_STAGES"));
    return on && U8(item + 8) == LAYER_MATERIAL && !material_layer(item);
}
static _Thread_local const char *pass_name = "gbuffer";  /* this thread's pass, for the summaries */
enum { MODE_GAME, MODE_CHECK, MODE_PROGRAMS, MODE_NATIVE };
static int mode = -1;
int gbuffer_checking;

/* The G-buffer's main vertex program (two thirds of the draws in the schoolyard). */
#define GBUFFER_VS 0xbc358b88a500029full

/* Where a G-buffer vertex program reads the object from vertex constants 2: up to four parts, each
 * at a row (4 floats a row). The renderer's world W is 3x4 (with 0 0 0 1 below as 4x4); the game
 * puts this frame's matrices where the programs read last frame's, so only the camera's motion
 * shows in the motion vectors. Programs without P x V x W multiply by the camera themselves (from
 * constants 1). Our draw writes these parts and nothing else. From the translated programs; check
 * mode compares them with the game's, bit for bit. */
/* W 3 rows, W 4 rows, view x W, P x view x W, and the skinned programs' last frame's view x W:
 * this frame's on most draws, but on some (0.5-30%, by area) one of a few fixed matrices that are
 * the same in every area (left in the constant memory?); copied from the game's bound constants, as
 * last frame's bones (constants 5) are. */
enum { PART_NONE, PART_W3, PART_W4, PART_VW, PART_WVP, PART_GAME };
typedef struct {
    uint64_t vs;
    struct { uint8_t what, row; } part[4];
} Layout;
static const Layout layouts[] = {
    {GBUFFER_VS, {{PART_W3, 0}, {PART_WVP, 8}, {PART_W4, 12}}},
    {0x07b7085fd39014daull, {{PART_W3, 0}, {PART_WVP, 8}, {PART_W4, 12}}},
    {0x44dd8154987b814bull, {{PART_W3, 0}, {PART_WVP, 8}, {PART_W4, 12}}},  /* skinned: bones in constants 4, last frame's in 5 */
    {0xb742aa7d4094cd28ull, {{PART_VW, 4}, {PART_WVP, 8}, {PART_GAME, 16}}},  /* skinned */
    {0x69ccf1e136bafdbbull, {{PART_W3, 0}, {PART_W4, 12}}},
    {0x9447ec08ee99eb00ull, {{PART_W3, 0}, {PART_W4, 12}}},
    {0x48ad99b0a01a2650ull, {{PART_W3, 0}, {PART_W4, 12}}},
    {0xb018461f506d0557ull, {{PART_W3, 0}, {PART_W4, 12}}},
    {0xe3bbbbb06699f239ull, {{PART_W3, 0}, {PART_W4, 12}}},
    {0x2d6853131f6a178full, {{PART_VW, 4}, {PART_WVP, 8}, {PART_GAME, 16}}},  /* skinned */
    {0x15c39ce2be80ffbdull, {{PART_VW, 4}, {PART_WVP, 8}, {PART_GAME, 16}}},
    {0xdfe0364f78f6b929ull, {{PART_W3, 0}, {PART_W4, 12}}},
    {0x045ee58bf4978aacull, {{PART_W3, 0}, {PART_W4, 12}}},
    /* the lit material pass's */
    {0x046b2a15a631058full, {{PART_W3, 0}, {PART_WVP, 8}}},
    {0xdcc6c449f889ccfeull, {{PART_W3, 0}, {PART_WVP, 8}}},
    {0x2a1b178ed00aa1f6ull, {{PART_W3, 0}, {PART_WVP, 8}}},
    {0x8871f59f38c71272ull, {{PART_W3, 0}, {PART_WVP, 8}}},
    {0x2e114dd75c870331ull, {{PART_W3, 0}, {PART_WVP, 8}}},  /* skinned */
    {0x58da2816a6c92e87ull, {{PART_W3, 0}}},
    {0x3e96b46f8899902dull, {{PART_W3, 0}}},
    {0xce3616ad2c7ba2c3ull, {{PART_W3, 0}}},
    {0x37b9e7176d0468f6ull, {{PART_W3, 0}}},
    {0xce03c241bb3aaf4cull, {{PART_W3, 0}}},
    {0xa4ec7992db13fd68ull, {{PART_W3, 0}}},
    {0xadc29b1b44829818ull, {{PART_W3, 0}}},
    {0x9b9f9ed2cc08aa0eull, {{PART_W3, 0}}},
    {0x358916c1961dd9b9ull, {{PART_W4, 0}, {PART_WVP, 8}}},  /* skinned */
    {0x2bed6627a501b602ull, {{PART_WVP, 8}}},
    {0x5bd1d944ba55c3a7ull, {{PART_WVP, 8}}},
    {0x9dd0c8c2b5a55909ull, {{PART_W4, 0}, {PART_W4, 12}}},
    {0x0216502064e7bbc3ull, {{PART_W3, 0}, {PART_WVP, 8}}},
    {0x41a8fd685b64dffdull, {{PART_W3, 0}}},
    {0xa56f491c7ffbe48eull, {{PART_W3, 0}}},
    {0xb813fb690e38a1e5ull, {{PART_W3, 0}}},
    {0x98c09b816c2787a2ull, {{PART_NONE, 0}}},  /* reads no object constants */
    {0x6488f2ab4b0873d5ull, {{PART_W3, 0}, {PART_W4, 12}}},  /* G-buffer */
    {0x99c29881d52eb8b7ull, {{PART_W3, 0}, {PART_W4, 12}}},  /* G-buffer */
    {0xdd7df0ff45403bfeull, {{PART_W3, 0}, {PART_VW, 4}, {PART_WVP, 8}}},  /* reads only row 6 of view x W */
    {0x1e14f9290339d1d5ull, {{PART_W3, 0}, {PART_VW, 4}, {PART_W4, 12}}},  /* G-buffer; row 6 only */
    {0x948d32df75118c5full, {{PART_W3, 0}, {PART_VW, 4}}},                 /* stages */
    /* the main view's stage layers' */
    {0xea32f254e28caa24ull, {{PART_WVP, 8}}},
    {0xf7b17fd7a6ebea03ull, {{PART_WVP, 8}}},
};
static int part_rows(int what) { return what == PART_W3 ? 3 : what ? 4 : 0; }
/* Are the layout's parts the same, bit for bit, in a and b? */
static int same_parts(const Layout *l, const float *a, const float *b) {
    for (int i = 0; i < 4; i++)
        if (memcmp(a + 4 * l->part[i].row, b + 4 * l->part[i].row, 16 * part_rows(l->part[i].what))) return 0;
    return 1;
}
enum { LAYOUTS = sizeof layouts / sizeof *layouts };
static int layout_of(const NvnProgram *p) {
    for (int i = 0; p && i < LAYOUTS; i++)
        if (layouts[i].vs == p->stages[0].key) return i;
    return -1;
}
/* YW4_GBUFFER_LAYOUTS=<hex mask of the table's entries>: which programs native mode draws (all by
 * default), to narrow a fault down */
static int layout_on(int l) {
    static long long mask = -2;
    if (mask == -2) mask = getenv("YW4_GBUFFER_LAYOUTS") ? strtoll(getenv("YW4_GBUFFER_LAYOUTS"), NULL, 16) : -1;
    return (int)(mask >> l & 1);
}

static void init_mode(void) {
    const char *e = getenv("YW4_GBUFFER");
    int m = !e                        ? MODE_GAME
            : !strcmp(e, "check")    ? MODE_CHECK
            : !strcmp(e, "programs") ? MODE_PROGRAMS
            : !strcmp(e, "native")   ? MODE_NATIVE
                                     : MODE_GAME;
    gbuffer_checking = m == MODE_CHECK;
    __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
}

int gbuffer_native(void) {
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    if (m < 0) init_mode(), m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    return m == MODE_NATIVE || m == MODE_PROGRAMS;
}

/* YW4_GBUFFER_ALTERNATE=<frame>: for 16 frames from that GPU frame, odd frames ours and even
 * frames the game's, to compare the two in one run (a frame dumped as F was drawn as F-1). */
int gbuffer_native_frame(uint64_t frame) {
    static long long alt = -2;
    if (alt == -2) alt = getenv("YW4_GBUFFER_ALTERNATE") ? atoll(getenv("YW4_GBUFFER_ALTERNATE")) : -1;
    if (alt >= 0 && frame >= (uint64_t)alt && frame < (uint64_t)alt + 16) return (int)(frame & 1);
    return gbuffer_native();
}

void gbuffer_toggle(void) {
    int m = gbuffer_native() ? MODE_GAME : MODE_NATIVE;
    gbuffer_checking = 0;
    __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gbuffer: %s (F6, presented frame %" PRIu64 ")\n", m == MODE_NATIVE ? "ours" : "the game's",
            __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED));
}

/* The layer being drawn on this thread, and the submesh about to be drawn. */
static _Thread_local struct {
    int active;  /* a G-buffer layer */
    uint64_t device;
    Mat4 vp;     /* projection x view, from the render state */
    int armed;   /* a submesh noted: the next draw recorded on this thread is its */
    uint64_t object, geometry, submesh;
    uint64_t sp, ret;  /* the model renderer's stack and which of its two call sites */
    float world[12];   /* the renderer's world for this draw ([sp+0x30]+0x40), read at the hook */
    int have_world;
} cur;

/* host_readable is a system call: remember the answer per 64 KiB block */
static int readable(uint64_t addr, uint64_t size) {
    enum { SLOTS = 1 << 12 };
    static uint64_t key[SLOTS];
    static uint8_t val[SLOTS];
    for (uint64_t b = addr >> 16; b <= (addr + size - 1) >> 16; b++) {
        uint32_t h = (uint32_t)(b * 0x9E3779B97F4A7C15ull >> 52);
        while (key[h] && key[h] != b) h = (h + 1) & (SLOTS - 1);
        if (!key[h]) val[h] = (uint8_t)(host_readable(b << 16, 1 << 16) ? 1 : 2), key[h] = b;
        if (val[h] != 1) return 0;
    }
    return 1;
}
static int guest_ptr(uint64_t p) { return p >= 0x7000000000ull && p < 0x7800000000ull && !(p & 7); }

/* Where the world matrix of a mismatching draw came from: per call site, a stack slot of the
 * model renderer holding a pointer whose +0 or +0x40 is the world the game put in constants 2. */
enum { SOURCES = 16 };
static _Thread_local struct { uint64_t ret; int slot, at; long n; } sources[SOURCES];
static _Thread_local long unexplained[2];

/* Projection x view of the last frame's G-buffer layer, for last frame's matrix (rows 12-15). */
static Mat4 prev_vp, this_vp;
static uint64_t this_vp_frame = ~0ull;

/* What the checks found since the last summary. */
static _Thread_local struct {
    long layers, submeshes, draws, main_vs;
    long vb_ok, ib_ok, world_ok, wvp_ok, prev_a, prev_b, prev_c;
    float wvp_worst;
    int shown, slot_shown;
    long slot_ok, slot_is_object, slot_is_model, hook_world_ok, hook_wvp_ok, ib_bad[2];
    int ib_shown;
} st;
/* per layout: draws, how many had all their parts of constants 2 bit-identical to ours, and our
 * index buffer */
static _Thread_local struct { long n, exact, ib; } lay_st[LAYOUTS];
/* game: the game's bound vertex constants 2, for the parts we copy (PART_GAME) */
static void object_constants(const Layout *l, const float *game, float out[GBUFFER_OBJECT_FLOATS]);

void gbuffer_layer_start(const CPUState *ctx) {
    if (__atomic_load_n(&mode, __ATOMIC_ACQUIRE) < 0) init_mode();
    uint64_t item = ctx->x[2];
    cur.armed = 0;
    /* the previous layer on this thread was a material layer: its colour target as it left it */
    static _Thread_local int after_material;
    if (after_material && getenv("YW4_CAPTURE_LAYER_AT"))
        nvn_record_native(ctx, U64(ctx->x[0] + 0x10d0) + 0xd8, gpu_capture_layer, "material", 8);
    after_material = item && material_layer(item) && mode != MODE_GAME;
    cur.active = mode != MODE_GAME && item && (U8(item + 8) == LAYER_GBUFFER || material_layer(item) || stage_layer(item));
    if (!cur.active) return;
    pass_name = U8(item + 8) == LAYER_GBUFFER ? "gbuffer" : material_layer(item) ? "material" : "stages";
    cur.device = ctx->x[0];
    /* the camera's projection and view in the render state (the shadow cascades' jobs put theirs
     * at the same offsets on their own thread) */
    Mat4 proj = mat4_load(cur.device + 0x280), view = mat4_load(cur.device + 0x2c0);
    cur.vp = mat4_mul(&proj, &view);
    uint64_t frame = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    if (frame != this_vp_frame) prev_vp = this_vp, this_vp = cur.vp, this_vp_frame = frame;
    st.layers++;
}

void gbuffer_submesh(const CPUState *ctx) {
    if (!cur.active) return;
    static uint64_t base;
    if (!base)
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, "main")) base = *yw4_code[i].base;
    uint64_t ret = ctx->x[30] - base;
    if (ret != 0xa4bf94 && ret != 0xa4c2d0) return;  /* from the model renderer */
    /* the object being drawn is in the model renderer's stack slot sp+0x390 */
    cur.object = U64(ctx->sp + 0x390), cur.geometry = ctx->x[1], cur.submesh = ctx->x[2];
    cur.sp = ctx->sp, cur.ret = ret;
    uint64_t rw = U64(ctx->sp + 0x30);
    cur.have_world = guest_ptr(rw) && readable(rw + 0x40, 48);
    if (cur.have_world) memcpy(cur.world, (const void *)(uintptr_t)(rw + 0x40), sizeof cur.world);
    cur.armed = 1;
    st.submeshes++;
}

static void summary(void) {
    static _Thread_local uint64_t last;
    uint64_t frame = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    if (frame < last + 300) return;
    last = frame;
    fprintf(stderr,
            "yw4: gbuffer: check: %ld layers, %ld submeshes, %ld draws, %ld with the main program: vertex buffer ok "
            "%ld, index buffer ok %ld, world ok %ld, projection x view x world ok %ld (worst %.2g); last frame's: "
            "world = object+0x40 %ld, world = object+0 %ld, constants 1 rows 32-35 = last frame's projection x "
            "view %ld\n",
            st.layers, st.submeshes, st.draws, st.main_vs, st.vb_ok, st.ib_ok, st.world_ok, st.wvp_ok, st.wvp_worst,
            st.prev_a, st.prev_b, st.prev_c);
    fprintf(stderr, "yw4: gbuffer:   world = [sp+0x30]+0x40: %ld of %ld (pointer is the object: %ld, its model record: %ld);"
            " read at the hook: %ld, projection x view x it: %ld; index buffer wrong: %ld (call site a4bf94), %ld (a4c2d0)\n",
            st.slot_ok, st.main_vs, st.slot_is_object, st.slot_is_model, st.hook_world_ok, st.hook_wvp_ok, st.ib_bad[0],
            st.ib_bad[1]);
    for (int l = 0; l < LAYOUTS; l++)
        if (lay_st[l].n)
            fprintf(stderr, "yw4: %s:   vs %016" PRIx64 ": %ld draws, constants 2 bit-identical %ld, index buffer"
                    " ours %ld\n", pass_name, layouts[l].vs, lay_st[l].n, lay_st[l].exact, lay_st[l].ib);
    memset(lay_st, 0, sizeof lay_st);
    for (int i = 0; i < SOURCES; i++)
        if (sources[i].n)
            fprintf(stderr, "yw4: gbuffer:   world from call site main+%#" PRIx64 ": [sp+%#x]+%#x, %ld draws\n",
                    sources[i].ret, sources[i].slot, sources[i].at, sources[i].n);
    if (unexplained[0] || unexplained[1])
        fprintf(stderr, "yw4: gbuffer:   world not found: %ld (call site a4bf94), %ld (a4c2d0)\n", unexplained[0],
                unexplained[1]);
    memset(&st, 0, sizeof st);
    memset(sources, 0, sizeof sources);
    unexplained[0] = unexplained[1] = 0;
}


/* The submesh's indices, as the submesh draw main+0xa75e90 finds them: the geometry's index buffer
 * object (+0x28; usually, not always, the second object in the vertex buffer's record, +0x68 of
 * it), at the submesh's offset (+4). 0 if there is none. */
static uint64_t indices_of(uint64_t geometry, uint64_t submesh) {
    uint64_t ib = U64(geometry + 0x28);
    return ib && U64(ib) ? U64(ib) + U32(submesh + 4) : 0;
}

/* An index buffer that is not ours: what the game drew instead. */
static void show_ib(uint64_t bufs, uint64_t vb0, uint64_t index_addr, uint32_t type, uint32_t count) {
    if (st.ib_shown >= 6) return;
    st.ib_shown++;
    uint64_t ours = indices_of(cur.geometry, cur.submesh);
    fprintf(stderr, "yw4: gbuffer: index buffer differs (call site %#" PRIx64 "): game %#" PRIx64 " type %u count %u,"
            " ours %#" PRIx64 " (game - ours %" PRId64 ") type %u count %u; vertices: game %#" PRIx64 " ours %#" PRIx64
            "; object %#" PRIx64 " geometry %#" PRIx64 "\n", cur.ret, index_addr, type, count, ours,
            (int64_t)(index_addr - ours), U8(cur.submesh + 0x3f) & 2 ? 2 : 1, U32(cur.submesh + 0xc),
            vb0, bufs ? U64(bufs) + U32(cur.submesh) : 0, cur.object, cur.geometry);
    fprintf(stderr, "yw4: gbuffer:   submesh:");
    for (int i = 0; i < 0x50; i += 4) fprintf(stderr, " %08x", U32(cur.submesh + i));
    fprintf(stderr, "\nyw4: gbuffer:   buffers:");
    for (int i = 0; bufs && i < 0x90; i += 8) fprintf(stderr, " %" PRIx64, U64(bufs + i));
    fprintf(stderr, "\n");
}

/* A mismatching draw: look for the world the game used in the model renderer's stack. */
static void find_world(const float *game_world) {
    for (int slot = 0; slot < 0x800; slot += 8) {
        uint64_t p = U64(cur.sp + slot);
        if (!guest_ptr(p) || !readable(p, 0x80)) continue;
        for (int at = 0; at <= 0x40; at += 0x40)
            if (mat4_diff(game_world, (const float *)(uintptr_t)(p + at), 12) < 1e-5f) {
                int i = 0;
                while (i < SOURCES && sources[i].n && !(sources[i].ret == cur.ret && sources[i].slot == slot &&
                                                         sources[i].at == at))
                    i++;
                if (i < SOURCES) sources[i].ret = cur.ret, sources[i].slot = slot, sources[i].at = at, sources[i].n++;
                return;
            }
    }
    unexplained[cur.ret == 0xa4c2d0]++;
}

void gbuffer_check_draw(const NvnCmdBuf *cb, uint64_t index_addr, uint32_t type, uint32_t count) {
    if (!cur.armed) return;
    cur.armed = 0;
    st.draws++;
    const NvnProgram *p = cb->program;
    int l = layout_of(p);
    if (l >= 0 && cb->trace_ub[0][2] && cur.have_world) {  /* our constants against the game's, bit for bit */
        const Layout *lay = &layouts[l];
        const float *game = (const float *)(uintptr_t)cb->trace_ub[0][2];
        float ours[GBUFFER_OBJECT_FLOATS];
        object_constants(lay, game, ours);
        lay_st[l].n++;
        lay_st[l].exact += same_parts(lay, game, ours);
        uint64_t bufs = U64(cur.geometry + 0x20);
        int ib_ok = U32(cur.submesh + 0xc) ? index_addr && index_addr == indices_of(cur.geometry, cur.submesh)
                                           : !index_addr;  /* no indices: drawn as the vertices */
        lay_st[l].ib += ib_ok;
        if (!ib_ok) show_ib(bufs, cb->trace_vb0, index_addr, type, count);
        static int shown[LAYOUTS];
        if (!same_parts(lay, game, ours) && shown[l] < 2) {
            shown[l]++;
            fprintf(stderr, "yw4: gbuffer: vs %016" PRIx64 " constants 2 rows 0-19 (game, then ours):\n", lay->vs);
            for (int r = 0; r < 20; r++)
                fprintf(stderr, "yw4: gbuffer:   %2d: %g %g %g %g | %g %g %g %g\n", r, game[4 * r], game[4 * r + 1],
                        game[4 * r + 2], game[4 * r + 3], ours[4 * r], ours[4 * r + 1], ours[4 * r + 2], ours[4 * r + 3]);
        }
    }
    if (!p || p->stages[0].key != GBUFFER_VS || !cb->trace_ub[0][2]) {
        summary();
        return;
    }
    st.main_vs++;
    uint64_t bufs = U64(cur.geometry + 0x20);
    if (bufs && cb->trace_vb0 == U64(bufs) + U32(cur.submesh)) st.vb_ok++;
    if (index_addr && index_addr == indices_of(cur.geometry, cur.submesh)) st.ib_ok++;
    const float *c2 = (const float *)(uintptr_t)cb->trace_ub[0][2];
    Mat4 world = mat4_load(cur.object), world40 = mat4_load(cur.object + 0x40);
    if (mat4_diff(c2, world.m, 12) < 1e-4f) st.world_ok++;
    else find_world(c2);
    if (cur.have_world && mat4_diff(c2, cur.world, 12) < 1e-5f) st.hook_world_ok++;
    if (cur.have_world) {  /* projection x view x the renderer's world (3x4, last row 0 0 0 1) */
        Mat4 w = {{0}};
        memcpy(w.m, cur.world, sizeof cur.world);
        w.m[15] = 1;
        Mat4 hw = mat4_mul(&cur.vp, &w);
        if (mat4_diff(c2 + 32, hw.m, 16) < 1e-3f) st.hook_wvp_ok++;
    }
    if (U32(cur.submesh + 0xc) && index_addr != indices_of(cur.geometry, cur.submesh)) {
        st.ib_bad[cur.ret == 0xa4c2d0]++;
        show_ib(bufs, cb->trace_vb0, index_addr, type, count);
    }
    /* the renderer's own world: [sp+0x30] + 0x40 (found for posed parts), for every draw */
    uint64_t rw = U64(cur.sp + 0x30);
    if (guest_ptr(rw) && readable(rw, 0x80) && mat4_diff(c2, (const float *)(uintptr_t)(rw + 0x40), 12) < 1e-5f) {
        st.slot_ok++;
        if (rw == cur.object) st.slot_is_object++;
        /* is it the geometry's model record (object+0x140 + 0x240 x geometry)? */
        uint64_t geos = U64(cur.object + 0x128), models = U64(cur.object + 0x140);
        for (unsigned g = 0; g < U8(cur.object + 0x1c8) && g < 16; g++)
            if (U64(geos + 8ull * g) == cur.geometry && rw == models + 0x240ull * g) st.slot_is_model++;
        static int dumped;
        if (dumped < 2) {  /* what the record is: its first words, next to the object's */
            dumped++;
            fprintf(stderr, "yw4: gbuffer: [sp+0x30] %#" PRIx64 " (object %#" PRIx64 ", object - record %#" PRIx64 "):",
                    rw, cur.object, cur.object - rw);
            for (int i = 0; i < 8; i++) fprintf(stderr, " %" PRIx64, U64(rw + 8ull * i));
            fprintf(stderr, " | object:");
            for (int i = 0; i < 8; i++) fprintf(stderr, " %" PRIx64, U64(cur.object + 8ull * i));
            fprintf(stderr, "\n");
        }
    } else if (st.slot_shown < 2) {
        st.slot_shown++;
        fprintf(stderr, "yw4: gbuffer: [sp+0x30] = %#" PRIx64 " (object %#" PRIx64 ") does not hold this draw's world\n",
                rw, cur.object);
    }
    Mat4 wvp = mat4_mul(&cur.vp, &world);
    float d = mat4_diff(c2 + 32, wvp.m, 16);
    if (d < 1e-3f) st.wvp_ok++;
    if (d > st.wvp_worst) st.wvp_worst = d;
    /* last frame's position: last frame's projection x view (vertex constants 1, rows 32-35) x
     * last frame's world (rows 12-15) */
    if (mat4_diff(c2 + 48, world40.m, 16) < 1e-4f) st.prev_a++;
    if (mat4_diff(c2 + 48, world.m, 16) < 1e-4f) st.prev_b++;
    if (cb->trace_ub[0][1] && mat4_diff((const float *)(uintptr_t)cb->trace_ub[0][1] + 128, prev_vp.m, 16) < 1e-3f)
        st.prev_c++;
    if (d >= 1e-3f && st.shown < 3) {  /* what the objects with another world are */
        st.shown++;
        uint64_t models = U64(cur.object + 0x140);
        fprintf(stderr, "yw4: gbuffer: world differs: object %#" PRIx64 " (+0x108 %#" PRIx64 ", instances %#" PRIx64
                ") cbuf 2 row 0: %g %g %g %g, object row 0: %g %g %g %g\n", cur.object, U64(cur.object + 0x108),
                models ? U64(models + 0x80) : 0, c2[0], c2[1], c2[2], c2[3], world.m[0], world.m[1], world.m[2],
                world.m[3]);
    }
    summary();
}

/* ---- step 2b: our draw in place of the game's */

static _Thread_local struct {
    long taken, left[6];  /* left to the game: program, no world, not a plain triangle list, vertex
                           * buffer or layout, index buffer, count or type */
    struct { uint64_t vs, fs; long n; } other[24];  /* the programs left to the game, by draws */
} nat;

static void note_other(const NvnProgram *p) {
    uint64_t vs = p ? p->stages[0].key : 0, fs = p ? p->stages[1].key : 0;
    int i = 0;
    while (i < 24 && nat.other[i].n && !(nat.other[i].vs == vs && nat.other[i].fs == fs)) i++;
    if (i < 24) nat.other[i].vs = vs, nat.other[i].fs = fs, nat.other[i].n++;
}

static void native_summary(void) {
    static _Thread_local uint64_t last;
    uint64_t frame = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    if (frame < last + 300) return;
    last = frame;
    fprintf(stderr, "yw4: %s: native: %ld draws ours; the game's: %ld other programs, %ld no world, %ld not a "
            "triangle list, %ld other vertex buffer or layout, %ld other index buffer, %ld other count or type\n",
            pass_name, nat.taken, nat.left[0], nat.left[1], nat.left[2], nat.left[3], nat.left[4], nat.left[5]);
    for (int i = 0; i < 24 && nat.other[i].n; i++)
        fprintf(stderr, "yw4: %s:   the game's: vs %016" PRIx64 " fs %016" PRIx64 ": %ld draws\n", pass_name,
                nat.other[i].vs, nat.other[i].fs, nat.other[i].n);
    memset(&nat, 0, sizeof nat);
}

/* Vertex constants 2 for the submesh noted last, as layout l reads them: the renderer's world,
 * projection x view x world, the world again as last frame's (the game puts this frame's there, so
 * only the camera's motion shows in the motion vectors). */
static void object_constants(const Layout *l, const float *game, float out[GBUFFER_OBJECT_FLOATS]) {
    Mat4 w = {{0}};
    memcpy(w.m, cur.world, sizeof cur.world);
    w.m[15] = 1;
    /* view x world, then projection x that: each summed in order without fused multiply-adds, the
     * game's order, bit-identical on every draw checked ((P x V) x W or fused sums are off by a last
     * bit on 2-60% of draws) */
    Mat4 proj = mat4_load(cur.device + 0x280), view = mat4_load(cur.device + 0x2c0);
    Mat4 vw = mat4_mul(&view, &w), wvp = mat4_mul(&proj, &vw);
    memset(out, 0, GBUFFER_OBJECT_FLOATS * sizeof *out);
    for (int i = 0; i < 4; i++) {
        int what = l->part[i].what;
        const float *m = what == PART_VW ? vw.m : what == PART_WVP ? wvp.m : what == PART_GAME ? game + 4 * l->part[i].row : w.m;
        memcpy(out + 4 * l->part[i].row, m, 16 * part_rows(what));
    }
}

int gbuffer_take_draw(CPUState *ctx, const NvnCmdBuf *cb, int indexed) {
    if (!cur.armed || __atomic_load_n(&mode, __ATOMIC_ACQUIRE) != MODE_NATIVE) return 0;
    cur.armed = 0;
    native_summary();
    /* DrawElements(cmd, primitive, type, count, address) or DrawArrays(cmd, primitive, first, count) */
    uint32_t prim = (uint32_t)ctx->x[1], type = indexed ? (uint32_t)ctx->x[2] : 0, count = (uint32_t)ctx->x[3];
    uint64_t index_addr = indexed ? ctx->x[4] : 0, first = indexed ? 0 : ctx->x[2];
    const NvnProgram *p = cb->program;
    /* any fragment program: our draw changes only what the vertex program reads */
    int l = layout_of(p);
    if (l < 0 || !layout_on(l)) return note_other(p), nat.left[0]++, 0;
    if (!cur.have_world) return nat.left[1]++, 0;
    if (U8(cur.submesh + 0x3d) != 1 || prim != 4) return nat.left[2]++, 0;  /* table index 1, NVN's triangles */
    uint64_t bufs = U64(cur.geometry + 0x20);
    uint32_t stride = U8(cur.submesh + 0x3e);
    GBufferDraw d;
    d.vertices = bufs ? U64(bufs) + U32(cur.submesh) : 0;
    d.vertex_count = U32(cur.submesh + 8);
    d.vertex_bytes = d.vertex_count * stride;
    /* a submesh without indices (+0xc 0) is drawn as its vertices in order */
    d.index_count = U32(cur.submesh + 0xc);
    d.indices = d.index_count ? indices_of(cur.geometry, cur.submesh) : 0;
    d.index_type = !d.index_count ? 0 : U8(cur.submesh + 0x3f) & 2 ? 2 : 1;
    d.primitive = 4;
    /* the attributes' streams (one each: 0-4 for this program) all bound to the submesh's vertices
     * with its stride, so our buffer can go in all of them */
    int one_stream = cb->trace_nattribs > 0;
    for (uint32_t i = 0; i < cb->trace_nattribs; i++) {
        uint32_t a = cb->trace_attribs[i], s = a >> 8 & 0x1F;
        if ((a & 0xFF) && (s >= 8 || cb->trace_vb[s] != cb->trace_vb0 || (uint32_t)cb->trace_streams[s] != stride))
            one_stream = 0;
    }
    if (!d.vertices || !stride || cb->trace_vb0 != d.vertices || !one_stream) {
        static int shown;
        if (shown++ < 3) {
            fprintf(stderr, "yw4: gbuffer: native: vertex layout not ours: buffer %#" PRIx64 " (ours %#" PRIx64 "), stride"
                    " %u, stream 0 %#" PRIx64 ", attributes:", cb->trace_vb0, d.vertices, stride, cb->trace_streams[0]);
            for (uint32_t i = 0; i < cb->trace_nattribs; i++) fprintf(stderr, " %#x", cb->trace_attribs[i]);
            fprintf(stderr, "\n");
        }
        return nat.left[3]++, 0;
    }
    if (index_addr != d.indices) return nat.left[4]++, 0;
    if (d.index_count ? count != d.index_count || type != d.index_type
                      : first || count != d.vertex_count)
        return nat.left[5]++, 0;
    memcpy(d.game, &ctx->x[1], sizeof d.game);
    d.game_indexed = indexed;
    if (!cb->trace_ub[0][2]) return nat.left[1]++, 0;
    object_constants(&layouts[l], (const float *)(uintptr_t)cb->trace_ub[0][2], d.object);
    nvn_record_native(ctx, ctx->x[0], gpu_gbuffer_draw, &d, sizeof d);
    nat.taken++;
    return 1;
}
