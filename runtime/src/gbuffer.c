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
#define U16(a) (*(const uint16_t *)(uintptr_t)(a))
#define U32(a) (*(const uint32_t *)(uintptr_t)(a))
#define U64(a) (*(const uint64_t *)(uintptr_t)(a))

enum { LAYER_MATERIAL = 0, LAYER_SHADOW = 1, LAYER_GBUFFER = 2 };

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
/* The shadow cascades' layers (kind 1): the draws the shadow pass (shadow.c) leaves to the game
 * are ours here, with the game's program and the cascade's camera (YW4_SHADOW_PROGRAMS=0: not). */
static int shadow_layer(uint64_t item) {
    static int on = -1;
    if (on < 0) on = !getenv("YW4_SHADOW_PROGRAMS") || atoi(getenv("YW4_SHADOW_PROGRAMS"));
    return on && U8(item + 8) == LAYER_SHADOW;
}
static _Thread_local const char *pass_name = "gbuffer";  /* this thread's pass, for the summaries */
enum { MODE_GAME, MODE_CHECK, MODE_PROGRAMS, MODE_NATIVE };
static int mode = -1;
int gbuffer_checking;

/* The G-buffer's main vertex program (two thirds of the draws in the schoolyard). */
#define GBUFFER_VS 0xbc358b88a500029full

/* The object's constants, vertex constants 2: every model program that reads them has the
 * engine's local-matrix block there (CBUSE_UB_LOCAL_MATRIX_IDX in the shader's header; 717 of the
 * game's 757 vertex programs, all with the same members): rows 0-3 mtxLW (world, 3x4 with 0 0 0 1
 * below), 4-7 mtxLV (view x world), 8-11 mtxLP (projection x view x world), 12-15 mtxLWOld and
 * 16-19 mtxLVOld (last frame's). recomp/shader.py writes in each program's .iface which block a
 * slot holds and which rows the program reads. Rows 0-11 are ours; last frame's are the object's
 * history, copied from the game's bound block (the game puts this frame's world there for most
 * objects, so only the camera's motion shows in the motion vectors; the skinned programs' last
 * view x world is sometimes one of a few fixed matrices). Check mode compares the rows each
 * program reads, bit for bit. */
enum { LOCAL_ROWS = 20, OUR_ROWS = 12 };

static const char *shader_dir(void) {
    const char *d = getenv("YW4_SHADER_DIR");
    return d && *d ? d : "work/shaders/glsl";
}

/* The rows of constants 2 a vertex program reads, from its .iface; 0 if they are not the local
 * matrix block, or not all at fixed rows of it. */
static uint64_t read_local_rows(uint64_t vs) {
    char path[1024], line[256];
    snprintf(path, sizeof path, "%s/%016" PRIx64 ".iface", shader_dir(), vs);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int local = 0;
    uint64_t rows = 0;
    while (fgets(line, sizeof line, f)) {
        char kind, word[128] = "";
        unsigned slot;
        if (sscanf(line, "%c %u %127s", &kind, &slot, word) < 2 || slot != 2) continue;
        if (kind == 'c') local = !strcmp(word, "CBUSE_UB_LOCAL_MATRIX_IDX");
        if (kind == 'r') rows = word[0] == '*' ? ~0ull : strtoull(word, NULL, 16);
    }
    fclose(f);
    return local && !(rows >> LOCAL_ROWS) ? rows : 0;
}

/* The same, remembered per program (the render threads ask before every model draw). */
static uint64_t local_rows(const NvnProgram *p) {
    enum { SLOTS = 1 << 11 };
    static struct { uint64_t vs, rows; } seen[SLOTS];  /* rows + 1 (0: not read yet) */
    if (!p) return 0;
    uint64_t vs = p->stages[0].key;
    uint32_t h = (uint32_t)(vs * 0x9E3779B97F4A7C15ull >> 53);
    for (uint32_t i = 0; i < SLOTS; i++, h = (h + 1) & (SLOTS - 1)) {
        uint64_t k = __atomic_load_n(&seen[h].vs, __ATOMIC_ACQUIRE);
        if (k == vs) {
            uint64_t r = __atomic_load_n(&seen[h].rows, __ATOMIC_ACQUIRE);
            if (r) return r - 1;
            break;  /* another thread is reading it: read it too */
        }
        if (!k) {
            uint64_t zero = 0;
            if (!__atomic_compare_exchange_n(&seen[h].vs, &zero, vs, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
                zero != vs)
                continue;
            uint64_t r = read_local_rows(vs);
            __atomic_store_n(&seen[h].rows, r + 1, __ATOMIC_RELEASE);
            return r;
        }
    }
    return read_local_rows(vs);
}

/* Are the rows in the mask the same, bit for bit, in a and b? */
static int same_rows(uint64_t rows, const float *a, const float *b) {
    for (int r = 0; r < LOCAL_ROWS; r++)
        if (rows >> r & 1 && memcmp(a + 4 * r, b + 4 * r, 16)) return 0;
    return 1;
}

/* YW4_GBUFFER_SKIP=<vertex program keys, hex, comma separated>: native mode leaves these to the
 * game, to narrow a fault down */
static int skipped(const NvnProgram *p) {
    enum { MAX = 32 };
    static uint64_t keys[MAX];
    static int n = -1;
    if (n < 0) {
        int k = 0;
        for (const char *e = getenv("YW4_GBUFFER_SKIP"); e && *e && k < MAX;) {
            char *end;
            keys[k++] = strtoull(e, &end, 16);
            e = *end == ',' ? end + 1 : "";
        }
        __atomic_store_n(&n, k, __ATOMIC_RELEASE);
    }
    for (int i = 0; i < __atomic_load_n(&n, __ATOMIC_ACQUIRE); i++)
        if (keys[i] == p->stages[0].key) return 1;
    return 0;
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
    if (gpu_ab_game) return 0;  /* the same-frame A/B's second run */
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
    uint64_t list;     /* the layer's draw list */
    uint64_t item;     /* the draw item (0x48 bytes) of this submesh, 0 if not found */
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
/* per program: draws, how many had the rows of constants 2 it reads bit-identical to ours, and our
 * index buffer */
enum { PROGRAM_STATS = 96 };
static _Thread_local struct { uint64_t vs; long n, exact, ib, item, mat_n, mat_ok[2]; } prog_st[PROGRAM_STATS];
/* game: the game's bound vertex constants 2, for last frame's rows */
static void object_constants(const float *game, float out[GBUFFER_OBJECT_FLOATS]);

/* Same-frame A/B (gpu_ab.c): the compared pass's layer, open in this command buffer until the next
 * layer starts there or the game ends the buffer's recording (thread B's G-buffer layer is the
 * last layer of its list). */
static struct {
    uint64_t cmd;
    char name[16];
} ab_open;

static void ab_close(const CPUState *ctx) {
    char m[17] = "e";
    memcpy(m + 1, ab_open.name, 16);
    nvn_record_native(ctx, ab_open.cmd, gpu_ab_native, m, 1 + (uint32_t)strlen(ab_open.name));
    ab_open.cmd = 0;
}

void gbuffer_end_recording(const CPUState *ctx, uint64_t cmd) {
    if (cmd && ab_open.cmd == cmd) ab_close(ctx);
}

void gbuffer_layer_start(const CPUState *ctx) {
    if (__atomic_load_n(&mode, __ATOMIC_ACQUIRE) < 0) init_mode();
    uint64_t item = ctx->x[2];
    cur.armed = 0;
    /* the previous layer on this thread was a material layer: its colour target as it left it */
    static _Thread_local int after_material;
    if (after_material && getenv("YW4_CAPTURE_LAYER_AT"))
        nvn_record_native(ctx, U64(ctx->x[0] + 0x10d0) + 0xd8, gpu_capture_layer, "material", 8);
    after_material = item && material_layer(item) && mode != MODE_GAME;
    /* same-frame A/B (gpu_ab.c): the compared pass's layer ends here, or starts */
    uint64_t cmd = U64(ctx->x[0] + 0x10d0) + 0xd8;  /* the command buffer this thread records into */
    if (ab_open.cmd == cmd) ab_close(ctx);
    const char *name = !item || mode == MODE_GAME ? NULL
                       : U8(item + 8) == LAYER_GBUFFER ? "gbuffer" : material_layer(item) ? "material"
                       : shadow_layer(item)          ? "shadow"
                                                     : NULL;
    if (name && gpu_ab_wanted(name) && !ab_open.cmd) {
        char m[17] = "b";
        strcpy(m + 1, name);
        nvn_record_native(ctx, cmd, gpu_ab_native, m, 1 + (uint32_t)strlen(name));
        strcpy(ab_open.name, name);
        ab_open.cmd = cmd;
    }
    cur.active = mode != MODE_GAME && item &&
                 (U8(item + 8) == LAYER_GBUFFER || material_layer(item) || stage_layer(item) || shadow_layer(item));
    if (!cur.active) return;
    pass_name = U8(item + 8) == LAYER_GBUFFER ? "gbuffer" : material_layer(item) ? "material"
                : shadow_layer(item)          ? "shadow"  : "stages";
    cur.device = ctx->x[0], cur.list = ctx->x[1];
    /* the camera's projection and view in the render state (the shadow cascades' jobs put theirs
     * at the same offsets on their own thread) */
    Mat4 proj = mat4_load(cur.device + 0x280), view = mat4_load(cur.device + 0x2c0);
    cur.vp = mat4_mul(&proj, &view);
    uint64_t frame = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    if (frame != this_vp_frame && !shadow_layer(item)) prev_vp = this_vp, this_vp = cur.vp, this_vp_frame = frame;
    st.layers++;
}

/* The model renderer's draw item (0x48 bytes, docs/frame.md "Materials") for the submesh noted:
 * +0 geometry, +8 model record, +0x10 submesh records, +0x18 material records, +0x28 texture
 * references, +0x30 material constants, +0x40 (u16) the submesh's index. Found in the renderer's
 * stack, as a slot pointing at it or as the item itself; where it was is remembered per call site. */
static int is_item(uint64_t p) {
    return guest_ptr(p) && readable(p, 0x48) && U64(p) == cur.geometry &&
           U64(p + 0x10) + 0x50ull * U16(p + 0x40) == cur.submesh;
}
static uint64_t find_item(uint64_t sp, int site) {
    static int where[2] = {-1, -1};  /* stack offset, +1 bit: 1 the slot holds a pointer, 0 the item is there */
    int w = __atomic_load_n(&where[site], __ATOMIC_RELAXED);
    if (w >= 0) {
        uint64_t p = w & 1 ? U64(sp + (w & ~1)) : sp + (uint64_t)w;
        if (is_item(p)) return p;
    }
    for (int off = 0; off < 0x2000; off += 8) {
        if (is_item(sp + off)) return __atomic_store_n(&where[site], off, __ATOMIC_RELAXED), sp + off;
        uint64_t p = U64(sp + off);
        if (is_item(p)) return __atomic_store_n(&where[site], off | 1, __ATOMIC_RELAXED), p;
    }
    static int logged;
    if (logged++ < 3) fprintf(stderr, "yw4: gbuffer: no draw item found for submesh %#" PRIx64 "\n", cur.submesh);
    return 0;
}

/* The material constants the renderer uploads for the item's submesh (0x80 bytes, zero after the
 * material's own): the material record (+0x18, 16 bytes each, by the submesh's byte +0x43) gives
 * the start (+2, in 16-byte units, in the item's +0x30) and count (+0xa); a model record with
 * +0x219 bit 0 and per-object overrides at +0x78 takes 0x80 bytes per material from there instead.
 * 0 if there are none (the game then binds the previous ones). */
static int material_constants(uint64_t item, uint8_t out[0x80]) {
    uint64_t model = U64(item + 8);
    unsigned m = U8(cur.submesh + 0x43);
    uint64_t rec = U64(item + 0x18) + 16ull * m;
    unsigned n = U8(rec + 0xa);
    if (!n || n > 8) return 0;
    uint64_t src = U8(model + 0x219) & 1 && U64(model + 0x78) ? U64(model + 0x78) + 0x80ull * m
                                                               : U64(item + 0x30) + 16ull * U16(rec + 2);
    memset(out, 0, 0x80);
    memcpy(out, (const void *)(uintptr_t)src, 16 * n);
    return 1;
}

void gbuffer_disarm(void) { cur.armed = 0; }

int gbuffer_hook_world(const CPUState *ctx, float out[12]) {
    uint64_t rw = U64(ctx->sp + 0x30);
    if (!guest_ptr(rw) || !readable(rw + 0x40, 48)) return 0;
    memcpy(out, (const void *)(uintptr_t)(rw + 0x40), 48);
    return 1;
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
    cur.have_world = gbuffer_hook_world(ctx, cur.world);
    cur.item = find_item(ctx->sp, ret == 0xa4c2d0);
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
    for (int i = 0; i < PROGRAM_STATS && prog_st[i].n; i++)
        fprintf(stderr, "yw4: %s:   vs %016" PRIx64 ": %ld draws, constants 2 bit-identical %ld, index buffer"
                " ours %ld; draw item found %ld, material constants %ld: ours bound in vs %ld, fs %ld\n",
                pass_name, prog_st[i].vs, prog_st[i].n, prog_st[i].exact, prog_st[i].ib, prog_st[i].item,
                prog_st[i].mat_n, prog_st[i].mat_ok[0], prog_st[i].mat_ok[1]);
    memset(prog_st, 0, sizeof prog_st);
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
    uint64_t rows = local_rows(p);
    if (rows && cb->trace_ub[0][2] && cur.have_world) {  /* our constants against the game's, bit for bit */
        const float *game = (const float *)(uintptr_t)cb->trace_ub[0][2];
        float ours[GBUFFER_OBJECT_FLOATS];
        object_constants(game, ours);
        int l = 0;
        while (l < PROGRAM_STATS && prog_st[l].n && prog_st[l].vs != p->stages[0].key) l++;
        if (l == PROGRAM_STATS) l = PROGRAM_STATS - 1;  /* full: the last one counts the rest */
        prog_st[l].vs = p->stages[0].key;
        prog_st[l].n++;
        prog_st[l].exact += same_rows(rows, game, ours);
        /* the material constants, against what the game bound at the draw list's slot (+0x7a) */
        uint8_t mat[0x80];
        unsigned slot = U16(cur.list + 0x7a);
        if (cur.item && slot < 16 && material_constants(cur.item, mat)) {
            prog_st[l].mat_n++;
            for (int stage = 0; stage < 2; stage++)
                prog_st[l].mat_ok[stage] += cb->trace_ub[stage][slot] &&
                                            !memcmp((const void *)(uintptr_t)cb->trace_ub[stage][slot], mat, 0x80);
        }
        prog_st[l].item += cur.item != 0;
        uint64_t bufs = U64(cur.geometry + 0x20);
        int ib_ok = U32(cur.submesh + 0xc) ? index_addr && index_addr == indices_of(cur.geometry, cur.submesh)
                                           : !index_addr;  /* no indices: drawn as the vertices */
        prog_st[l].ib += ib_ok;
        if (!ib_ok) show_ib(bufs, cb->trace_vb0, index_addr, type, count);
        static _Thread_local int shown;
        if (!same_rows(rows, game, ours) && shown < 6) {
            shown++;
            fprintf(stderr, "yw4: gbuffer: vs %016" PRIx64 " constants 2 rows read %#" PRIx64 " (game, then ours):\n",
                    p->stages[0].key, rows);
            for (int r = 0; r < LOCAL_ROWS; r++)
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

/* Vertex constants 2 (the local matrix block) for the submesh noted last: the renderer's world,
 * view x world, projection x view x world; last frame's two from the game's bound block. */
static void object_constants(const float *game, float out[GBUFFER_OBJECT_FLOATS]) {
    Mat4 w = {{0}};
    memcpy(w.m, cur.world, sizeof cur.world);
    w.m[15] = 1;
    /* view x world, then projection x that: each summed in order without fused multiply-adds, the
     * game's order, bit-identical on every draw checked ((P x V) x W or fused sums are off by a last
     * bit on 2-60% of draws) */
    Mat4 proj = mat4_load(cur.device + 0x280), view = mat4_load(cur.device + 0x2c0);
    Mat4 vw = mat4_mul(&view, &w), wvp = mat4_mul(&proj, &vw);
    memcpy(out, w.m, sizeof w.m);
    memcpy(out + 16, vw.m, sizeof vw.m);
    memcpy(out + 32, wvp.m, sizeof wvp.m);
    memcpy(out + 4 * OUR_ROWS, game + 4 * OUR_ROWS, 16 * (LOCAL_ROWS - OUR_ROWS));
}

int gbuffer_take_draw(CPUState *ctx, const NvnCmdBuf *cb, int indexed, int instanced) {
    if (!cur.armed || __atomic_load_n(&mode, __ATOMIC_ACQUIRE) != MODE_NATIVE) return 0;
    cur.armed = 0;
    native_summary();
    /* DrawElements(cmd, primitive, type, count, address) or DrawArrays(cmd, primitive, first, count);
     * instanced: then (base vertex,) base instance, instances */
    uint32_t prim = (uint32_t)ctx->x[1], type = indexed ? (uint32_t)ctx->x[2] : 0, count = (uint32_t)ctx->x[3];
    uint64_t index_addr = indexed ? ctx->x[4] : 0, first = indexed ? 0 : ctx->x[2];
    int32_t base_vertex = instanced && indexed ? (int32_t)ctx->x[5] : 0;
    uint32_t base_instance = !instanced ? 0 : (uint32_t)ctx->x[indexed ? 6 : 4];
    uint32_t instances = !instanced ? 1 : (uint32_t)ctx->x[indexed ? 7 : 5];
    const NvnProgram *p = cb->program;
    /* any fragment program: our draw changes only what the vertex program reads */
    if (!local_rows(p) || skipped(p)) return note_other(p), nat.left[0]++, 0;
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
    if (base_vertex) return nat.left[5]++, 0;
    memcpy(d.game, &ctx->x[1], sizeof d.game);
    d.game_indexed = indexed;
    d.game_instanced = instanced;
    d.base_instance = base_instance, d.instances = instances;
    if (!cb->trace_ub[0][2]) return nat.left[1]++, 0;
    object_constants((const float *)(uintptr_t)cb->trace_ub[0][2], d.object);
    nvn_record_native(ctx, ctx->x[0], gpu_gbuffer_draw, &d, sizeof d);
    nat.taken++;
    return 1;
}
