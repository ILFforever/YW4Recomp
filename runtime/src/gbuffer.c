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

enum { LAYER_GBUFFER = 2 };
enum { MODE_GAME, MODE_CHECK, MODE_NATIVE };
static int mode = -1;
int gbuffer_checking;

/* The G-buffer's main vertex program (half the draws in the schoolyard). */
#define GBUFFER_VS 0xbc358b88a500029full

static void init_mode(void) {
    const char *e = getenv("YW4_GBUFFER");
    int m = e && !strcmp(e, "check") ? MODE_CHECK : e && !strcmp(e, "native") ? MODE_NATIVE : MODE_GAME;
    gbuffer_checking = m == MODE_CHECK;
    __atomic_store_n(&mode, m, __ATOMIC_RELEASE);
}

int gbuffer_native(void) {
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    if (m < 0) init_mode(), m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    return m == MODE_NATIVE;
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
static struct { uint64_t ret; int slot, at; long n; } sources[SOURCES];
static long unexplained[2];

/* Projection x view of the last frame's G-buffer layer, for last frame's matrix (rows 12-15). */
static Mat4 prev_vp, this_vp;
static uint64_t this_vp_frame = ~0ull;

/* What the checks found since the last summary. */
static struct {
    long layers, submeshes, draws, main_vs;
    long vb_ok, ib_ok, world_ok, wvp_ok, prev_a, prev_b, prev_c;
    float wvp_worst;
    int shown, slot_shown;
    long slot_ok, slot_is_object, slot_is_model, hook_world_ok, hook_wvp_ok, ib_bad[2];
} st;

void gbuffer_layer_start(const CPUState *ctx) {
    if (__atomic_load_n(&mode, __ATOMIC_ACQUIRE) < 0) init_mode();
    uint64_t item = ctx->x[2];
    cur.armed = 0;
    cur.active = mode != MODE_GAME && item && U8(item + 8) == LAYER_GBUFFER;
    if (!cur.active) return;
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
    static uint64_t last;
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

void gbuffer_check_draw(const NvnCmdBuf *cb, uint64_t index_addr) {
    if (!cur.armed) return;
    cur.armed = 0;
    st.draws++;
    const NvnProgram *p = cb->program;
    if (!p || p->stages[0].key != GBUFFER_VS || !cb->trace_ub[0][2]) {
        summary();
        return;
    }
    st.main_vs++;
    uint64_t bufs = U64(cur.geometry + 0x20);
    if (bufs && cb->trace_vb0 == U64(bufs) + U32(cur.submesh)) st.vb_ok++;
    if (bufs && index_addr == U64(bufs + 0x68) + U32(cur.submesh + 4)) st.ib_ok++;
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
    if (!bufs || index_addr != U64(bufs + 0x68) + U32(cur.submesh + 4)) st.ib_bad[cur.ret == 0xa4c2d0]++;
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
