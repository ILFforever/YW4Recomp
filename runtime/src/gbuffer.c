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

/* The constant buffer slots a translated shader reads (its .iface "r <slot> <rows>" lines), as a
 * bit mask; remembered per shader. */
static uint32_t read_cbuf_slots(uint64_t key) {
    char path[1024], line[256];
    snprintf(path, sizeof path, "%s/%016" PRIx64 ".iface", shader_dir(), key);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    uint32_t mask = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned slot;
        if (line[0] == 'r' && sscanf(line + 1, "%u", &slot) == 1 && slot < 32) mask |= 1u << slot;
    }
    fclose(f);
    return mask;
}
static uint32_t cbuf_slots(uint64_t key) {
    enum { SLOTS = 1 << 12 };
    static struct { uint64_t key; uint32_t mask; } seen[SLOTS];  /* mask | 1 << 31 once read */
    uint32_t h = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 52);
    for (uint32_t i = 0; i < SLOTS; i++, h = (h + 1) & (SLOTS - 1)) {
        uint64_t k = __atomic_load_n(&seen[h].key, __ATOMIC_ACQUIRE);
        if (k == key) {
            uint32_t m = __atomic_load_n(&seen[h].mask, __ATOMIC_ACQUIRE);
            if (m) return m & ~(1u << 31);
            break;
        }
        if (!k) {
            uint64_t zero = 0;
            if (!__atomic_compare_exchange_n(&seen[h].key, &zero, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
                zero != key)
                continue;
            uint32_t m = read_cbuf_slots(key);
            __atomic_store_n(&seen[h].mask, m | 1u << 31, __ATOMIC_RELEASE);
            return m;
        }
    }
    return read_cbuf_slots(key);
}

/* The texture slots a translated shader reads (.iface "t <slot> <dim>"), as a bit mask; remembered. */
static uint32_t read_tex_slots(uint64_t key) {
    char path[1024], line[256];
    snprintf(path, sizeof path, "%s/%016" PRIx64 ".iface", shader_dir(), key);
    FILE *f = fopen(path, "r");
    if (!f) return ~0u;  /* unknown: as if it read everything */
    uint32_t mask = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned slot;
        if (line[0] == 't' && sscanf(line + 1, "%u", &slot) == 1 && slot < 32) mask |= 1u << slot;
    }
    fclose(f);
    return mask;
}
static uint32_t tex_slots(uint64_t key) {
    enum { SLOTS = 1 << 12 };
    static struct { uint64_t key; uint64_t mask; } seen[SLOTS];  /* mask | 1 << 32 once read */
    uint32_t h = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 52);
    for (uint32_t i = 0; i < SLOTS; i++, h = (h + 1) & (SLOTS - 1)) {
        uint64_t k = __atomic_load_n(&seen[h].key, __ATOMIC_ACQUIRE);
        if (k == key) {
            uint64_t m = __atomic_load_n(&seen[h].mask, __ATOMIC_ACQUIRE);
            if (m) return (uint32_t)m;
            break;
        }
        if (!k) {
            uint64_t zero = 0;
            if (!__atomic_compare_exchange_n(&seen[h].key, &zero, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
                zero != key)
                continue;
            uint32_t m = read_tex_slots(key);
            __atomic_store_n(&seen[h].mask, m | 1ull << 32, __ATOMIC_RELEASE);
            return m;
        }
    }
    return read_tex_slots(key);
}

/* The NVN programs the engine has made, by their vertex and fragment code (decision 0030): learned
 * from the game's own draws (the engine makes one program per combination of stage shaders and keeps
 * it), so a takeover draw can bind the program the engine would have. 0 if not seen yet. */
enum { PROGRAMS = 1 << 12 };
static struct { uint64_t vs, fs, guest; } programs[PROGRAMS];
static void learn_program(const NvnCmdBuf *cb) {
    const NvnProgram *p = cb->program;
    if (!p || !cb->program_guest || p->stage_mask != 3) return;
    uint64_t vs = p->stages[0].code, fs = p->stages[1].code;
    uint32_t h = (uint32_t)((vs ^ fs * 0x9E3779B97F4A7C15ull) * 0x9E3779B97F4A7C15ull >> 52);
    for (uint32_t i = 0; i < PROGRAMS; i++, h = (h + 1) & (PROGRAMS - 1)) {
        uint64_t k = __atomic_load_n(&programs[h].vs, __ATOMIC_ACQUIRE);
        if (k == vs && __atomic_load_n(&programs[h].fs, __ATOMIC_ACQUIRE) == fs) {
            __atomic_store_n(&programs[h].guest, cb->program_guest, __ATOMIC_RELEASE);
            return;
        }
        if (!k) {
            uint64_t zero = 0;
            if (!__atomic_compare_exchange_n(&programs[h].vs, &zero, vs, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
            __atomic_store_n(&programs[h].fs, fs, __ATOMIC_RELEASE);
            __atomic_store_n(&programs[h].guest, cb->program_guest, __ATOMIC_RELEASE);
            return;
        }
    }
}
static uint64_t program_of(uint64_t vs, uint64_t fs) {
    uint32_t h = (uint32_t)((vs ^ fs * 0x9E3779B97F4A7C15ull) * 0x9E3779B97F4A7C15ull >> 52);
    for (uint32_t i = 0; i < PROGRAMS; i++, h = (h + 1) & (PROGRAMS - 1)) {
        uint64_t k = __atomic_load_n(&programs[h].vs, __ATOMIC_ACQUIRE);
        if (!k) return 0;
        if (k == vs && __atomic_load_n(&programs[h].fs, __ATOMIC_ACQUIRE) == fs)
            return __atomic_load_n(&programs[h].guest, __ATOMIC_ACQUIRE);
    }
    return 0;
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

/* The engine keeps the NVN state objects a draw binds in its state ([device+0x10d0]), from +0x240:
 * blend states (8 bytes per colour target), +0x280 channel mask, +0x284 colour state, +0x288 depth
 * stencil, +0x290 multisample (0x18 bytes), +0x2a8 polygon, then plain fields up to +0x2f2. */
#define NVN_STATE_AT 0x240
#define NVN_STATE_BYTES 0xb8

/* The layer being drawn on this thread, and the submesh about to be drawn. */
static _Thread_local struct {
    int active;  /* a G-buffer layer */
    uint64_t device;
    Mat4 vp;     /* projection x view, from the render state */
    int armed;   /* a submesh noted: the next draw recorded on this thread is its */
    uint64_t object, geometry, submesh;
    uint64_t sp, ret;  /* the model renderer's stack and which of its two call sites */
    float world[12];   /* the renderer's world for this draw ([sp+0x30]+0x40), read at the hook */
    float world_row3[4];  /* ... its bottom row as the game stores it (0 0 0 1, sometimes with -0), +0x70 */
    /* check mode: what a takeover draw would use, captured at the hook (before the engine's commit) */
    float hook_object[GBUFFER_OBJECT_FLOATS];  /* constants 2 */
    uint8_t hook_old_bones[3 * 256 * 16];      /* last frame's bones */
    uint64_t hook_tex[TAKEOVER_TEXTURES];      /* the other fragment textures (slot_texture) */
    uint8_t hook_samp[TAKEOVER_TEXTURES][15];
    int have_world;
    uint64_t list;     /* the layer's draw list */
    uint64_t item;     /* the draw item (0x48 bytes) of this submesh, 0 if not found */
    uint8_t kind;      /* the layer's kind */
    uint8_t layer_state[33];  /* the render state block (device+0xeb9) after the layer's own setup */
    uint32_t bones_size;      /* check mode: the bone palette as read at the submesh's hook, 0 if none */
    float bones[3 * 256 * 4];
    int layer_fresh;          /* no submesh of the layer seen yet */
    int game_drew;            /* the game drew a submesh of this layer (its commit bound the targets) */
    uint8_t layer_nvn[NVN_STATE_BYTES];  /* the engine's NVN state objects ([device+0x10d0]+0x240) then */
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
static _Thread_local struct {
    uint64_t vs;
    long n, exact, ib, item, mat_n, mat_ok[2], mat_read, mat_any;
    long tex_n, tex_ok, tex_other, tex_unknown;  /* material textures: ours bound at slot k, another, not decoded */
    long smp_n, smp_state, smp_desc;             /* their samplers: our state = the engine's, our settings = bound */
    long rs_ok, rs_bytes[33];                    /* render state block = the engine's; draws where byte i differs */
    long pg_n, pg_eff, pg_stages;                /* shader effect ours; vertex and fragment programs ours */
    long vl_n, vl_ok, vl_other;                  /* vertex layouts: ours = the bound attributes and strides; other streams */
    long mm_n, mm_ok, mm_rows[22];               /* model material block (constants 3): ours; draws per differing row */
    long st_n, st_tex, st_samp;                  /* other fragment textures: read, texture ours, sampler ours */
    long nv_ok, nv_regions[8];                   /* NVN state objects = the engine's; draws where each part differs */
    long bn_n, bn_hook, bn_old_n, bn_late;       /* bone palettes: this frame's ours; last frame's ours */
} prog_st[PROGRAM_STATS];
/* game: the game's bound vertex constants 2, for last frame's rows */
static void object_constants(const float *game, float out[GBUFFER_OBJECT_FLOATS]);
static uint64_t indices_of(uint64_t geometry, uint64_t submesh);

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
    cur.kind = U8(item + 8);
    cur.layer_fresh = 1;
    cur.game_drew = 0;
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

/* The material's textures (decision 0030): the guest NVNtexture of each, in the order the engine
 * binds them, texture k to fragment slot k (check mode: equal on all 5.7 million slots read, four saves).
 * The material record's +6 (start) and +0xc (count) pick 6-byte references in the item's +0x28;
 * a reference's byte 0 is an entry of the model record's texture table (+0xf8, 16 bytes: resource,
 * then a u16 index). The resource's +0x18 is a Level-5 G4TX container; its 0x18-byte texture
 * records start at 4 x (u16 +0xa + u16 +0x3e), and a record's first word + 0x18 is the NVN texture
 * the engine made for it. The container holds byte +0x20 images; an index past them (and past byte
 * +0x23 more) is a region of an atlas, whose record (at 4 x (u16 +0xa + u16 +0x42)) starts with the
 * image it is in. Not decoded: the engine's other table (+0xf0, when the geometry's +0x108 is set;
 * not met in four saves): 0 there. Returns the count. */
static unsigned material_textures(uint64_t item, uint64_t out[8]) {
    uint64_t model = U64(item + 8);
    uint64_t rec = U64(item + 0x18) + 16ull * U8(cur.submesh + 0x43);
    uint64_t table = U64(model + 0xf8), refs = U64(item + 0x28) + 6ull * U16(rec + 6);
    unsigned n = U8(rec + 0xc) < 8 ? U8(rec + 0xc) : 8;
    for (unsigned k = 0; k < n; k++) {
        uint64_t entry = table + 16ull * U8(refs + 6ull * k), r = U64(entry);
        unsigned index = U16(entry + 8);
        out[k] = 0;
        if (!guest_ptr(r) || !readable(r, 0x48)) continue;
        uint64_t hdr = U64(r + 0x18);
        if (!guest_ptr(hdr)) continue;
        unsigned images = U8(hdr + 0x20), base = images + U8(hdr + 0x23);
        if (index >= base)  /* a sub-image (atlas region): its 0x18-byte record names the image */
            index = U16(hdr + 4ull * U16(hdr + 0xa) + 4ull * U16(hdr + 0x42) + 0x18ull * (index - base));
        if (index >= images) continue;
        out[k] = U64(hdr + 4ull * U16(hdr + 0xa) + 4ull * U16(hdr + 0x3e) + 0x18ull * index) + 0x18;
    }
    return n;
}

/* The sampler of material texture k (decision 0030). The engine keeps a 15-byte sampler state per
 * texture slot (device+0x1120 + 0xf x slot) and makes an NVN sampler from it at the draw (f_976ab0).
 * The state starts each layer from defaults (device+0x1300 + 0x1e0 x level + 0xf x slot, level =
 * byte device+0x4631); a texture reference's byte 1 counts (field, value) byte pairs, from u16 +4,
 * in the geometry header blob (geometry+0x18) at 4 x (u16 +0xa + u16 +0x6e), each setting one byte
 * of the state. Only the first 12 references have one. Returns 0 if there is none. */
static int sampler_state(uint64_t item, unsigned k, uint8_t out[15]) {
    if (k >= 12) return 0;
    uint64_t rec = U64(item + 0x18) + 16ull * U8(cur.submesh + 0x43);
    uint64_t ref = U64(item + 0x28) + 6ull * (U16(rec + 6) + k), blob = U64(U64(item) + 0x18);
    if (!guest_ptr(blob)) return 0;
    memcpy(out, (const void *)(uintptr_t)(cur.device + 0x1300 + 0x1e0ull * U8(cur.device + 0x4631) + 0xfull * k), 15);
    uint64_t pairs = blob + 4ull * (U16(blob + 0xa) + U16(blob + 0x6e)) + 2ull * U16(ref + 4);
    for (unsigned i = 0; i < U8(ref + 1); i++)
        if (U8(pairs + 2 * i) < 15) out[U8(pairs + 2 * i)] = U8(pairs + 2 * i + 1);
    return 1;
}

/* The NVN sampler settings f_976ab0 makes from a sampler state (its constants read from the game).
 * Bytes: 1-3 wrap s, t, r; 4 and 5 minification and magnification (low 4 bits; bit 4 of either:
 * anisotropic, bit 5: mipmaps linear); 6 anisotropy; 7, 8 level-of-detail clamp; 9 bias (signed);
 * 0xa depth compare function (0: off); 0xb-0xe border colour. The device's texture quality
 * settings (+0x4620 mode, +0x4624 anisotropy, +0x4625 on) can raise the filter and anisotropy. */
static void sampler_desc(const uint8_t s[15], NvnSamplerDesc *d) {
    static uint64_t base;
    if (!base)
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, "main")) base = *yw4_code[i].base;
    float c16, c17, c18, c19;
    memcpy(&c16, (const void *)(uintptr_t)(base + 0x11fdba0), 4);
    memcpy(&c17, (const void *)(uintptr_t)(base + 0x11fdbe8), 4);
    memcpy(&c18, (const void *)(uintptr_t)(base + 0x11fdf24), 4);
    memcpy(&c19, (const void *)(uintptr_t)(base + 0x11fe17c), 4);
    uint8_t min = s[4], mag = s[5];
    unsigned aniso_level = s[6], minsel, magsel;
    int aniso = (min >> 4 & 1) || (mag >> 4 & 1);
    if (aniso) minsel = magsel = 2;
    else minsel = min & 0xf, magsel = mag & 0xf;
    int linear = (magsel & 3) == 2, filter;
    if (minsel & 1) filter = min & 0x20 ? 4 : 2;
    else if (minsel >> 1 & 1) filter = min & 0x20 ? 5 : 3;
    else filter = 0;
    if (U8(cur.device + 0x4625)) {
        uint32_t qmode = U32(cur.device + 0x4620);
        if (qmode & 1) {
            if (filter == 4) filter = 2;
            else if (filter == 5) filter = 3;
        } else if (qmode >> 1 & 1) {
            if (filter == 2) filter = 4;
            else if (filter == 3) filter = 5;
        }
        int8_t q = (int8_t)U8(cur.device + 0x4624);
        if (q >= 0) {
            if (aniso_level <= (unsigned)(uint8_t)q) aniso_level = (uint8_t)q;
            if (min == 2 && mag == 2) {
                linear = 1;
                if (filter == 0) filter = 1;
                else if (filter == 4) filter = 5;
                else if (filter == 2) filter = 3;
            }
        }
    }
    memset(d, 0, sizeof *d);
    d->min_filter = (uint32_t)filter, d->mag_filter = (uint32_t)linear;
    d->max_aniso = aniso ? (float)aniso_level * c17 * 15.0f + 1.0f : 1.0f;
    d->wrap[0] = s[1], d->wrap[1] = s[2], d->wrap[2] = s[3];
    for (int i = 0; i < 4; i++) d->border[i] = (float)s[0xb + i] / c18;
    d->lod_min = (float)s[7] / c18 * 15.0f, d->lod_max = (float)s[8] / c18 * 15.0f;
    d->lod_bias = (float)(int8_t)s[9] * c16 * c19;
    d->compare_mode = s[0xa] != 0, d->compare_func = s[0xa] ? s[0xa] : 8;
}

/* The render state block (33 bytes, device+0xeb9) at a submesh's draw (decision 0030). The model
 * renderer saves the block before each material and restores it after the draw, so the block a
 * draw sees is the layer's plus the material's own entries: (id, value) byte pairs, the material
 * record's +4 (start) and +0xb (count), in the geometry header blob at 4 x (u16 +0xa + u16 +0x6c).
 * Setting id 20 without 21 and 22 while byte 0x14 is 2 also sets bytes 0x15 = 1 and 0x16 = 2. In
 * the G-buffer and shadow layers (kinds 2 and 1) a material's entries count only when they leave
 * byte 5 set, and then byte 0x10 is cleared. */
static void render_state(uint64_t item, uint8_t out[33]) {
    memcpy(out, cur.layer_state, 33);
    uint64_t rec = U64(item + 0x18) + 16ull * U8(cur.submesh + 0x43), blob = U64(U64(item) + 0x18);
    if (!guest_ptr(blob)) return;
    uint64_t pairs = blob + 4ull * (U16(blob + 0xa) + U16(blob + 0x6c)) + 2ull * U16(rec + 4);
    uint32_t set = 0;
    for (unsigned i = 0; i < U8(rec + 0xb); i++) {
        unsigned id = U8(pairs + 2 * i);
        if (!id || id >= 33) continue;
        out[id] = U8(pairs + 2 * i + 1);
        set |= 1u << id;
    }
    if ((set >> 20 & 1) && !(set & 0x600000) && out[0x14] == 2) out[0x15] = 1, out[0x16] = 2;
    if (cur.kind == LAYER_SHADOW || cur.kind == LAYER_GBUFFER) {
        if (!out[5]) memcpy(out, cur.layer_state, 33);
        else out[0x10] = 0;
    }
}

/* The material's shader effect id (decision 0030): per object overrides first (model record +0xe8,
 * u16 +0x20a of them, 0x18 bytes: u16 +0x10 the submesh, u16 +0x12 the id), else the model
 * record's table (+0x110) at the submesh's byte +0x42. The engine looks the effect up by it. */
static unsigned effect_id(uint64_t item) {
    uint64_t model = U64(item + 8), ov = U64(model + 0xe8);
    unsigned sub = U16(item + 0x40);
    for (unsigned i = 0; ov && i < U16(model + 0x20a); i++)
        if (U16(ov + 0x18ull * i + 0x10) == sub) return U16(ov + 0x18ull * i + 0x12);
    return U16(U64(model + 0x110) + 2ull * U8(cur.submesh + 0x42));
}

/* The effect's pass for this layer (f_977e30): the passes (+0x30, 8 bytes: key, u16 record) whose
 * key is the layer's pass key (device+0x45d4); the (device+0x45d0 + 1)-th of them, or the first;
 * with none, the pass with the engine's default key (main+0x5865650) if device+0x4628 is set.
 * Returns the pass record (+0x38, 0x10 bytes) or 0. */
static _Thread_local unsigned effect_pass_count;  /* of the pass effect_pass chose last: the commit draws
                                                    * that many consecutive pass records (f_977e30) */
static uint64_t effect_pass(uint64_t effect) {
    static uint64_t base;
    if (!base)
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, "main")) base = *yw4_code[i].base;
    uint64_t passes = U64(effect + 0x30);
    unsigned n = U8(effect + 0x48), sel = 0xffff, matches = 0;
    if (!passes) return 0;
    for (unsigned i = 0; i < n; i++)
        if (U32(passes + 8ull * i) == U32(cur.device + 0x45d4)) {
            if (++matches > U32(cur.device + 0x45d0)) { sel = i; break; }
            if (sel == 0xffff) sel = i;
        }
    if (sel == 0xffff && U8(cur.device + 0x4628))
        for (unsigned i = 0; i < n && sel == 0xffff; i++)
            if (U32(passes + 8ull * i) == U32(base + 0x5865650)) sel = i;
    effect_pass_count = sel == 0xffff ? 0 : U16(passes + 8ull * sel + 6);
    return sel == 0xffff ? 0 : U64(effect + 0x38) + 0x10ull * U16(passes + 8ull * sel + 4);
}

/* The material's shader effect: the effect manager (main+0x58656e8) keeps them in an array (+0x10,
 * 0x28 bytes each, the effect object first) by id from 1 (f_a9edf0). 0 if none. */
static uint64_t effect_of(uint64_t item) {
    static uint64_t base;
    if (!base)
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, "main")) base = *yw4_code[i].base;
    unsigned id = effect_id(item);
    return id ? U64(U64(U64(base + 0x58656e8) + 0x10) + 0x28ull * (id - 1)) : 0;
}

/* The shader code of the effect's pass for this layer: the pass record names the stage shader
 * objects by index into the effect manager's arrays (u16 +4: vertex, manager +0x30, 0x20 bytes each,
 * count u16 +0x4c; u16 +6: fragment, +0x78, 0x10 bytes, count +0x94; f_a9ee60, f_a9eec0); a stage
 * object's first word is its code (the address the NVN program is made from, which names our
 * translated shader). The engine combines the stages into one NVN program (f_965880, cached by the
 * stages' ids). 0 if there is no pass. */
static int pass_shaders(uint64_t effect, uint64_t *vs, uint64_t *fs) {
    static uint64_t base;
    if (!base)
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, "main")) base = *yw4_code[i].base;
    uint64_t pass = effect ? effect_pass(effect) : 0, mgr = U64(base + 0x58656e8);
    if (!pass) return 0;
    *vs = U16(pass + 4) < U16(mgr + 0x4c) ? U64(U64(U64(mgr + 0x30) + 0x20ull * U16(pass + 4))) : 0;
    *fs = U16(pass + 6) < U16(mgr + 0x94) ? U64(U64(U64(mgr + 0x78) + 0x10ull * U16(pass + 6))) : 0;
    return *vs && *fs;
}

/* A skinned submesh's bone palette (3x4 floats per bone, vertex constants 4). The model record's
 * per-submesh records (+0xc8, 0x140 bytes, by the submesh's index in the geometry's submesh table)
 * hold three palettes, 0x38 bytes apart from +0x70 (each: a buffer object at +8, its data at
 * [B+0x40] + [B+0x58]; the size at +0x28), written in turn by the animation; +0x118 points at the
 * newest, which the model renderer copies into vertex constants 4 at the commit (equal on every
 * skinned draw checked, read at our hook). The shadow pass's first palette (+0x78 always) was the
 * right one a third of the time. 0 if none. */
static int bone_palette(uint64_t item, uint64_t *data, uint32_t *size) {
    uint64_t model = U64(item + 8), recs = U64(model + 0xc8), hdr = U64(cur.geometry + 0x18);
    if (!recs || !guest_ptr(hdr)) return 0;
    uint64_t subs = hdr + 4ull * U16(hdr + 0xa) + 4ull * U16(hdr + 0x60);
    if (cur.submesh < subs || (cur.submesh - subs) % 0x50 || (cur.submesh - subs) / 0x50 >= U16(model + 0x200))
        return 0;
    uint64_t rec = recs + 0x140 * ((cur.submesh - subs) / 0x50), pal = U64(rec + 0x118);
    if (pal < rec + 0x70 || (pal - (rec + 0x70)) % 0x38 || pal > rec + 0xe0) return 0;
    uint64_t b = U64(pal + 8);
    if (!guest_ptr(b) || !U64(b + 0x40)) return 0;
    *data = U64(b + 0x40) + U32(b + 0x58);
    *size = U32(pal + 0x28);
    return *size && *size % 48 == 0 && *size <= 3 * 256 * 16;
}

/* Last frame's bones (vertex constants 5, for motion vectors): the palette before the newest; on
 * 0.5% of skinned draws the game binds the newest there too (not decoded). */
static int bone_palette_previous(uint64_t item, uint64_t *data) {
    uint64_t model = U64(item + 8), hdr = U64(cur.geometry + 0x18);
    uint64_t subs = hdr + 4ull * U16(hdr + 0xa) + 4ull * U16(hdr + 0x60);
    uint64_t rec = U64(model + 0xc8) + 0x140 * ((cur.submesh - subs) / 0x50), pal = U64(rec + 0x118);
    uint64_t prev = rec + 0x70 + 0x38 * (((pal - (rec + 0x70)) / 0x38 + 2) % 3), b = U64(prev + 8);
    if (!guest_ptr(b) || !U64(b + 0x40)) return 0;
    *data = U64(b + 0x40) + U32(b + 0x58);
    return 1;
}

/* The draw's vertex layout (f_965070): the effect's handle in the manager's array ([mgr+0x10] +
 * 0x28 x (id-1), the effect object at +0) lists vertex elements at +0x10 (u16 +0x22 of them, 8 bytes:
 * semantic, offset in the vertex, stream, then the NVN format at +4); the pass's program entry (the
 * handle's +8 table, 0x22 bytes by the pass record's byte +0xd) maps a semantic (u16 at +2 + 2 x
 * semantic) to the shader's attribute, or -1. Each attribute of stream 0 reads the submesh's vertices
 * through a stream of its own, the attribute's number. out: our packing of the attribute states
 * (nvn_objects.c: format, stream << 8, offset << 13), 0 where unused. Returns the attributes used
 * (bit mask), 0 if there is no layout; *other: elements on other streams (left to the game). */
static uint32_t vertex_layout(uint64_t item, uint32_t out[16], int *other) {
    static uint64_t base;
    if (!base)
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, "main")) base = *yw4_code[i].base;
    unsigned id = effect_id(item);
    *other = 0;
    if (!id) return 0;
    uint64_t handle = U64(U64(base + 0x58656e8) + 0x10) + 0x28ull * (id - 1), pass = effect_pass(U64(handle));
    uint64_t elems = U64(handle + 0x10), progs = U64(handle + 8);
    if (!pass || !elems || !progs) return 0;
    uint64_t map = progs + 0x22ull * U8(pass + 0xd);
    uint32_t used = 0;
    memset(out, 0, 16 * sizeof *out);
    for (unsigned i = 0; i < U16(handle + 0x22); i++) {
        uint64_t e = elems + 8ull * i;
        int16_t attr = (int16_t)U16(map + 2 + 2ull * U8(e));
        if (attr < 0 || attr >= 16) continue;
        if (U8(e + 2)) {
            (*other)++;
            continue;
        }
        out[attr] = (U32(e + 4) & 0xFF) | (uint32_t)attr << 8 | (uint32_t)U8(e + 1) << 13;
        used |= 1u << attr;
    }
    return used;
}

/* The per-draw material block, constants 3 (CBUSE_UB_MODEL_MATERIAL, 0x160 bytes), as the model
 * renderer builds it on its stack and uploads it (f_977550, slot u16 list+0x74). Rows (16 bytes):
 *   0 u_ubParam: 1, 0, the submesh's index + u16 model+0x1fe (an integer), the alpha test
 *     reference ((field 3 + 0.1) / 255 while field 1 is set, else -1; the material's block);
 *   1-3 u_diffuse, u_ambient, u_speculer: a 0x30-byte colour record (the item's +0x20 table by the
 *     material record's u16 +0, or the model record's per-object +0x68 table by material when
 *     +0x219 bit 0); diffuse times the object's colour (+0xe0..+0xec) and the fade (object +0x1bc
 *     times the model record's per-submesh +0x138, 1 past its u16 +0x200): on red, green and blue
 *     when fields 0x13-0x15 are all 2, else on alpha;
 *   4-15 texProj: 2x3 UV transforms, 0x20 bytes per texture (the model record's +0x98 table, +0x90
 *     when +0x219 bit 1, by the material record's u16 +0xe; identity without a table);
 *   16-21 u_uvRange: per texture (0, 0, 1, 1), or an atlas region's rectangle over its image's size.
 * Rows past the material's textures keep what an earlier draw left (the engine reuses the block);
 * *ntex is how many are this draw's. */
static void model_material(uint64_t item, const uint8_t state[33], uint8_t out[0x160], unsigned *ntex) {
    static uint64_t base;
    if (!base)
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, "main")) base = *yw4_code[i].base;
    float c01, c255, w[4] = {0};
    memcpy(&c01, (const void *)(uintptr_t)(base + 0x11fddb8), 4);
    memcpy(&c255, (const void *)(uintptr_t)(base + 0x11fdf24), 4);
    uint64_t model = U64(item + 8), obj = cur.object;
    unsigned m = U8(cur.submesh + 0x43), sub = U16(item + 0x40);
    uint64_t rec = U64(item + 0x18) + 16ull * m;
    memset(out, 0, 0x160);
    float one = 1.0f, ref = state[1] ? ((float)state[3] + c01) / c255 : -1.0f;
    uint32_t index = (sub + U16(model + 0x1fe)) & 0xffff;
    memcpy(out, &one, 4), memcpy(out + 8, &index, 4), memcpy(out + 12, &ref, 4);
    uint64_t colour = U8(model + 0x219) & 1 && U64(model + 0x68) ? U64(model + 0x68) + 0x30ull * m
                                                                  : U64(item + 0x20) + 0x30ull * U16(rec);
    memcpy(out + 16, (const void *)(uintptr_t)colour, 0x30);
    memcpy(w, out + 16, 16);
    for (int i = 0; i < 4; i++) w[i] *= *(const float *)(uintptr_t)(obj + 0xe0 + 4 * i);
    float fade = sub < U16(model + 0x200) ? *(const float *)(uintptr_t)(U64(model + 0x138) + 4ull * sub) : 1.0f;
    float f = *(const float *)(uintptr_t)(obj + 0x1bc) * fade;
    if (state[0x13] == 2 && state[0x14] == 2 && state[0x15] == 2) w[0] *= f, w[1] *= f, w[2] *= f;
    else w[3] *= f;
    memcpy(out + 16, w, 16);
    unsigned n = U8(rec + 0xc) < 6 ? U8(rec + 0xc) : 6;
    *ntex = n;
    uint64_t uv = U64(model + (U8(model + 0x219) & 2 ? 0x90 : 0x98));
    static const float ident[8] = {1, 0, 0, 0, 0, 1, 0, 0}, range[4] = {0, 0, 1, 1};
    for (unsigned k = 0; k < n; k++)
        memcpy(out + 0x40 + 0x20 * k, uv ? (const void *)(uintptr_t)(uv + 0x20ull * (U16(rec + 0xe) + k)) : ident, 0x20);
    uint64_t table = U64(model + (U64(obj + 0x108) ? 0xf0 : 0xf8)), refs = U64(item + 0x28) + 6ull * U16(rec + 6);
    for (unsigned k = 0; k < n; k++) {
        memcpy(out + 0x100 + 0x10 * k, range, 16);
        uint64_t entry = table + 16ull * U8(refs + 6ull * k), r = U64(entry);
        if (!guest_ptr(r)) continue;
        uint64_t hdr = U64(r + 0x18);
        unsigned idx = U16(entry + 8), first = (U8(hdr + 0x23) + U8(hdr + 0x20)) & 0xff;
        if (idx < first) continue;
        uint64_t subr = hdr + 4ull * U16(hdr + 0xa) + 4ull * U16(hdr + 0x42) + 0x18ull * (idx - first);
        uint64_t img = hdr + 4ull * U16(hdr + 0xa) + 4ull * U16(hdr + 0x3a) + 0x30ull * U16(subr);
        float W = (float)U16(img + 0x18), H = (float)U16(img + 0x1a);
        float rr[4] = {(float)U16(subr + 4) / W, (float)U16(subr + 6) / H, (float)U16(subr + 8) / W, (float)U16(subr + 10) / H};
        memcpy(out + 0x100 + 0x10 * k, rr, 16);
    }
}

/* A fragment texture slot's texture as the engine will bind it at the commit (f_966120): the pending
 * texture record the model renderer left for the slot (device+0x3e08 + 0x18 x slot, when its int at +8
 * is 0: the record's second word is a texture view), else the view already in the engine's slot
 * (state+0xb80 + 0x10 x slot); the view's first word + 0x18 is the guest NVNtexture (the per-object
 * reflection probe in slot 9: equal on every draw checked). Its sampler: the slot's 15-byte sampler
 * state (device+0x1120 + 0xf x slot). 0 if none. */
static uint64_t slot_texture(unsigned slot, uint8_t samp[15]) {
    uint64_t pend = cur.device + 0x3e08 + 0x18ull * slot, state = U64(cur.device + 0x10d0), view = 0;
    if (!U32(pend + 8) && guest_ptr(U64(pend)) && U64(U64(pend))) view = U64(U64(pend) + 8);
    else if (guest_ptr(state)) view = U64(state + 0xb80 + 0x10ull * slot);
    if (samp) memcpy(samp, (const void *)(uintptr_t)(cur.device + 0x1120 + 0xfull * slot), 15);
    return guest_ptr(view) && guest_ptr(U64(view)) ? U64(view) + 0x18 : 0;
}

/* The render state block the draw is made with: the material's (render_state), then the effect
 * pass's own (id, value) pairs (record +0xc of them from u16 +0xa, in the effect's +0x40), then what
 * the commit (f_9759d0) changes by the device's settings: +0x4629 flips the cull face (byte 0x1b
 * 1 <-> 2), +0x462a clears byte 5, +0x462b byte 0x10, +0x462d adds to byte 0x1c (up to 0xff). */
static void draw_state(uint64_t item, uint64_t effect, uint8_t out[33]) {
    render_state(item, out);
    uint64_t pass = effect ? effect_pass(effect) : 0;
    for (unsigned i = 0; pass && i < U8(pass + 0xc); i++) {
        uint64_t p = U64(effect + 0x40) + 2ull * (U16(pass + 0xa) + i);
        if (U8(p) < 33) out[U8(p)] = U8(p + 1);
    }
    if (U8(cur.device + 0x4629) && out[0x1b]) out[0x1b] = out[0x1b] == 1 ? 2 : 1;
    if (U8(cur.device + 0x462a)) out[5] = 0;
    if (U8(cur.device + 0x462b)) out[0x10] = 0;
    unsigned add = U8(cur.device + 0x462d);
    if (add && out[0x1c] != 0xff) out[0x1c] = (uint8_t)(out[0x1c] + (add < (uint8_t)~out[0x1c] ? add : (uint8_t)~out[0x1c]));
}

/* An NVN depth or stencil compare function from a block byte, as the engine's handlers do: 2-8 are
 * NVN's LESS..ALWAYS, anything else NEVER (1). */
static uint8_t nvn_compare(uint8_t b) { return b >= 2 && b <= 8 ? b : 1; }

/* The NVN state objects a draw binds, made from its render state block as the engine's per-field
 * handlers do at the commit (the table at main+0x14d23b0), into our packing of the objects
 * (nvn_objects.c); out starts as the layer's objects, for what the block doesn't drive (the other
 * colour targets' blend states, the colour state's logic op). Offsets from state+0x240.
 *   f_979950 (bytes 0xf-0x16), colour target 0: channel mask (0xf, bits r g b a), blend enable
 *     (0x10), blend equations colour and alpha (0x11, 0x12) and factors (0x13-0x16);
 *   f_979a90, f_979ba0 (4-0xe): depth test (4), depth write (5), depth compare (6), stencil test (7),
 *     stencil compare both faces (9), stencil ops fail, depth fail, pass (0xc, 0xe, 0xd), stencil
 *     reference (0xb, +0x2b8) and mask (0xa when 8 is set, +0x2b4);
 *   f_979c70 (0x1b, 0x1d, 0x1e): cull face, front face counter-clockwise, polygon mode and the
 *     polygon offset enables for it (main+0x12be4ac), +0x2f1 set by 0x1d;
 *   f_979da0 (0x1c): depth bias -byte at +0x2bc, +0x2c0 cleared;
 *   f_979d40 (0x1f, 0x20): +0x2f0, multisample enable. */
static void nvn_state(const uint8_t b[33], uint8_t out[NVN_STATE_BYTES]) {
    static uint64_t base;
    if (!base)
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, "main")) base = *yw4_code[i].base;
    memcpy(out, cur.layer_nvn, NVN_STATE_BYTES);
    uint8_t *blend = out, *color = out + 0x44, *ds = out + 0x48, *ms = out + 0x50, *poly = out + 0x68;
    uint32_t mask, u;
    memcpy(&mask, out + 0x40, 4);
    mask = (mask & ~0xFu) | (b[0xf] & 0xF);
    memcpy(out + 0x40, &mask, 4);
    color[0] = b[0x10] ? color[0] | 1 : color[0] & ~1;
    blend[5] = b[0x11], blend[6] = b[0x12];
    for (int i = 0; i < 4; i++) blend[1 + i] = b[0x13 + i];
    ds[0] = b[4] != 0, ds[1] = b[5] != 0, ds[2] = nvn_compare(b[6]), ds[3] = b[7] != 0;
    uint8_t f = nvn_compare(b[9]);
    ds[4] = (uint8_t)(f | f << 4);
    uint32_t ops = (uint32_t)((b[0xc] & 0xF) | (b[0xe] & 0xF) << 4 | (b[0xd] & 0xF) << 8), packed = ops | ops << 12;
    ds[5] = (uint8_t)packed, ds[6] = (uint8_t)(packed >> 8), ds[7] = (uint8_t)(packed >> 16);
    u = b[0xb];
    memcpy(out + 0x78, &u, 4);
    u = b[8] ? b[0xa] : 0;
    memcpy(out + 0x74, &u, 4);
    poly[0] = b[0x1b], poly[1] = 1, poly[2] = b[0x1e];
    poly[3] = b[0x1e] < 3 ? (uint8_t)U32(base + 0x12be4ac + 4ull * b[0x1e]) : 0;
    out[0xb1] = b[0x1d] != 0;
    float bias = -(float)b[0x1c];
    memcpy(out + 0x7c, &bias, 4);
    memset(out + 0x80, 0, 8);
    out[0xb0] = b[0x1f] != 0;
    ms[0] = b[0x20] != 0;
}

/* ---- the takeover (decision 0030): a draw made from the game's data alone, at the submesh-draw
 * hook, in place of the engine's state work (its commit) and draw */

enum { TK_DRAWN, TK_OFF, TK_FIRST, TK_ITEM, TK_PRIMITIVE, TK_PROGRAM, TK_READS, TK_PASSES, TK_TEXTURE, TK_LAYOUT,
       TK_CONSTANTS, TK_BONES, TK_REASONS };
static const char *const tk_reason[TK_REASONS] = {
    "", "off", "first draw of the layer", "no draw item or world", "not triangles", "program not known",
    "vertex textures or split instances", "more than one pass", "texture or sampler",
    "vertex layout", "constant blocks", "bones"};
static _Thread_local struct { long taken, left[TK_REASONS], bytes; } tk;  /* bytes: our records' sizes */

static void takeover_summary(void) {
    static _Thread_local uint64_t last;
    uint64_t frame = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    if (frame < last + 300) return;
    last = frame;
    fprintf(stderr, "yw4: takeover: %s: %ld draws ours (%ld bytes each); the game's:", pass_name, tk.taken,
            tk.taken ? tk.bytes / tk.taken : 0);
    for (int i = TK_FIRST; i < TK_REASONS; i++)
        if (tk.left[i]) fprintf(stderr, " %s %ld;", tk_reason[i], tk.left[i]);
    fprintf(stderr, "\n");
    memset(&tk, 0, sizeof tk);
}

static int takeover_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("YW4_TAKEOVER") && atoi(getenv("YW4_TAKEOVER"));
    return on;
}
/* YW4_TAKEOVER_ALTERNATE=1: only on odd presented frames, the game draws on the even ones (for
 * comparisons within one run: tools.dev.frame_ab) */
static int takeover_frame(void) {
    static int alt = -1;
    if (alt < 0) alt = getenv("YW4_TAKEOVER_ALTERNATE") && atoi(getenv("YW4_TAKEOVER_ALTERNATE"));
    return !alt || (__atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED) & 1);
}

/* The texture pool entry a takeover draw can copy for guest NVNtexture g: the one it was registered at
 * last, still naming it, and still a picture of it as it is now (a texture made again after loading
 * gets registered again by the engine's commit: until then the entry is old). NULL if not. */
static const NvnTexEntry *fresh_entry(const NvnCmdBuf *cb, uint64_t g) {
    const NvnTexture *nt = g ? HOST(NvnTexture, g) : NULL;
    if (!nt || !cb || !cb->trace_texpool || nt->pool_id < 0 || nt->pool_id >= cb->trace_texpool->count) return NULL;
    const NvnTexEntry *e = (const NvnTexEntry *)cb->trace_texpool->entries + nt->pool_id;
    if (!e->tex || e->guest != g || e->snap.addr != nt->addr || e->snap.width != nt->width ||
        e->snap.height != nt->height || e->snap.depth != nt->depth || e->snap.format != nt->format ||
        e->snap.levels != nt->levels)
        return NULL;
    return e;
}

static void add_ubo(TakeoverDraw *t, unsigned slot, unsigned stages, uint32_t offset, uint32_t size) {
    t->ubo[t->nubo].slot = (uint8_t)slot, t->ubo[t->nubo].stages = (uint8_t)stages;
    t->ubo[t->nubo].offset = offset, t->ubo[t->nubo].size = size;
    t->nubo++;
}

/* TK_DRAWN (0): ours is recorded; else the reason the draw stays the game's. */
static int takeover(const CPUState *ctx) {
    int m = __atomic_load_n(&mode, __ATOMIC_ACQUIRE);
    if (!takeover_on() || !takeover_frame() || (m != MODE_NATIVE && m != MODE_CHECK) ||
        (cur.kind != LAYER_GBUFFER && strcmp(pass_name, "material")))
        return TK_OFF;
    if (!cur.game_drew || cur.layer_fresh) return TK_FIRST;
    if (!cur.item || !cur.have_world) return TK_ITEM;
    if (U8(cur.submesh + 0x3d) != 1) return TK_PRIMITIVE;
    uint64_t effect = effect_of(cur.item), vs = 0, fs = 0;
    if (!pass_shaders(effect, &vs, &fs)) return TK_PROGRAM;
    uint64_t guest = program_of(vs, fs);
    const NvnProgram *p = guest ? HOST(NvnProgram, guest) : NULL;
    if (!p || p->stages[0].code != vs || p->stages[1].code != fs || p->stage_mask != 3) return TK_PROGRAM;
    uint32_t cv = cbuf_slots(p->stages[0].key), cf = cbuf_slots(p->stages[1].key);
    uint64_t rows = local_rows(p);
    uint32_t tf = tex_slots(p->stages[1].key);
    if (!rows || tex_slots(p->stages[0].key)) return TK_READS;
    if (effect_pass_count != 1) return TK_PASSES;
    /* instanced draws: the copies' count the model renderer left for the submesh draw (f_a75e90; the
     * other path, device+0x3230 set, draws split copies another way) */
    uint32_t instances = U32(cur.device + 0x45cc);
    if (U64(cur.device + 0x3230)) return TK_READS;

    static _Thread_local uint8_t buf[512 * 1024];
    TakeoverDraw *t = (TakeoverDraw *)buf;
    memset(t, 0, sizeof *t);
    uint32_t at = (sizeof *t + 15) & ~15u;
    t->program = guest, t->vs_code = vs, t->fs_code = fs;
    /* textures and samplers, in the fragment slots the program reads */
    uint64_t texs[8];
    unsigned ntex = material_textures(cur.item, texs);
    const NvnCmdBuf *cb = HOST(NvnCmdBuf, U64(cur.device + 0x10d0) + 0xd8);
    t->tex_at = at;  /* the TakeoverTex in slot order: the two loops go up the slots */
    for (unsigned k = 0; k < ntex && k < TAKEOVER_TEXTURES; k++) {
        if (!(tf >> k & 1)) continue;
        const NvnTexEntry *e = fresh_entry(cb, texs[k]);
        uint8_t ss[15];
        if (!e || !sampler_state(cur.item, k, ss)) return TK_TEXTURE;
        TakeoverTex *x = (TakeoverTex *)(buf + at);
        t->tex_mask |= 1u << k;
        x->e = *e;
        sampler_desc(ss, &x->samp);
        at += sizeof *x;
    }
    /* the other fragment textures it reads (the scene's, the per-object reflection probe) */
    for (unsigned k = ntex; k < TAKEOVER_TEXTURES; k++) {
        if (!(tf >> k & 1)) continue;
        uint8_t ss[15];
        uint64_t g = slot_texture(k, ss);
        if (!g) continue;  /* none: what the game bound stays */
        const NvnTexEntry *e = fresh_entry(cb, g);
        if (!e) return TK_TEXTURE;
        TakeoverTex *x = (TakeoverTex *)(buf + at);
        t->tex_mask |= 1u << k;
        x->e = *e;
        sampler_desc(ss, &x->samp);
        at += sizeof *x;
    }
    at = (at + 15) & ~15u;
    /* vertex layout and buffers */
    int other;
    if (!vertex_layout(cur.item, t->attribs, &other) || other) return TK_LAYOUT;
    t->nattribs = 16;
    t->stride = U8(cur.submesh + 0x3e);
    GBufferDraw *d = &t->d;
    uint64_t bufs = U64(cur.geometry + 0x20);
    d->vertices = bufs ? U64(bufs) + U32(cur.submesh) : 0;
    d->vertex_count = U32(cur.submesh + 8);
    d->vertex_bytes = d->vertex_count * t->stride;
    d->index_count = U32(cur.submesh + 0xc);
    d->indices = d->index_count ? indices_of(cur.geometry, cur.submesh) : 0;
    d->index_type = !d->index_count ? 0 : U8(cur.submesh + 0x3f) & 2 ? 2 : 1;
    d->primitive = 4;
    d->instances = instances ? instances : 1;
    if (!d->vertices || !t->stride || (d->index_count && !d->indices)) return TK_LAYOUT;
    object_constants(NULL, d->object);
    /* render state */
    uint8_t rs[33], ms[33];
    draw_state(cur.item, effect, rs);
    nvn_state(rs, t->nvn);
    /* constant blocks: the model material block, the material constants, the bones */
    unsigned mmslot = U16(cur.list + 0x74), mslot = U16(cur.list + 0x7a), n;
    uint8_t stages;
    if (mmslot < 16 && (stages = (uint8_t)((cv >> mmslot & 1) | (cf >> mmslot & 1) << 1))) {
        render_state(cur.item, ms);
        model_material(cur.item, ms, buf + at, &n);
        add_ubo(t, mmslot, stages, at, 0x160);
        at += 0x160;
    }
    if (mslot < 16 && mslot != mmslot && (stages = (uint8_t)((cv >> mslot & 1) | (cf >> mslot & 1) << 1))) {
        if (!material_constants(cur.item, buf + at)) return TK_CONSTANTS;
        add_ubo(t, mslot, stages, at, 0x80);
        at += 0x80;
    }
    if ((cv | cf) & (1u << 4 | 1u << 5)) {
        uint64_t pal, prev;
        uint32_t size;
        if (!bone_palette(cur.item, &pal, &size) || !bone_palette_previous(cur.item, &prev)) return TK_BONES;
        for (int sl = 4; sl <= 5; sl++) {
            if (!((cv | cf) >> sl & 1)) continue;
            memcpy(buf + at, (const void *)(uintptr_t)(sl == 4 ? pal : prev), size);
            add_ubo(t, (unsigned)sl, (cv >> sl & 1) | (cf >> sl & 1) << 1, at, size);
            at += size;
        }
    }
    /* every other constant block the program reads: what the model renderer left pending for the slot
     * in the stage's set (device+13000 + 0x150 x set + 0x18 x slot: buffer object, size at +8; vertex
     * set 0, fragment set 2), copied now, as the engine's commit binds them (f_965eb0, slots 0-13):
     * the per-object ones (10 point lights, 11 instance copies, 12 USER_DATA02, 9 a character's rim
     * settings, ...) and the layer's alike. A slot with nothing pending keeps what the game bound. */
    uint32_t ours[2] = {1u << 2, 0};
    for (uint32_t u = 0; u < t->nubo; u++)
        for (int st = 0; st < 2; st++)
            if (t->ubo[u].stages >> st & 1) ours[st] |= 1u << t->ubo[u].slot;
    for (int st = 0; st < 2; st++)
        for (int sl = 0; sl < 14; sl++) {
            if (!((st ? cf : cv) >> sl & 1) || ours[st] >> sl & 1) continue;
            uint64_t slot = cur.device + 13000 + 0x150ull * (st ? 2 : 0) + 0x18ull * (unsigned)sl, b = U64(slot);
            uint32_t size = U32(slot + 8);
            if (!guest_ptr(b) || !U64(b + 0x40) || !size) continue;
            /* already bound as it is (not a per-draw copy, and the command buffer's last binding of the
             * slot): the executor's state has it, nothing to copy */
            if (!U8(slot + 0x15) && cb && cb->trace_ub[st][sl] == U64(b + 0x40) + U32(b + 0x58)) continue;
            if (size > 0x10000 || t->nubo == TAKEOVER_UBOS || at + size > sizeof buf) return TK_CONSTANTS;
            memcpy(buf + at, (const void *)(uintptr_t)(U64(b + 0x40) + U32(b + 0x58)), size);
            add_ubo(t, (unsigned)sl, st ? 2u : 1u, at, size);
            at += (size + 15) & ~15u;
        }
    nvn_record_native(ctx, U64(cur.device + 0x10d0) + 0xd8, gpu_takeover_draw, buf, at);
    tk.bytes += at;
    return TK_DRAWN;
}

void gbuffer_disarm(void) { cur.armed = 0; }

int gbuffer_hook_world(const CPUState *ctx, float out[12]) {
    uint64_t rw = U64(ctx->sp + 0x30);
    if (!guest_ptr(rw) || !readable(rw + 0x40, 48)) return 0;
    memcpy(out, (const void *)(uintptr_t)(rw + 0x40), 48);
    return 1;
}

int gbuffer_submesh(const CPUState *ctx) {
    if (!cur.active) return 0;
    static uint64_t base;
    if (!base)
        for (size_t i = 0; i < yw4_ncode; i++)
            if (!strcmp(yw4_code[i].name, "main")) base = *yw4_code[i].base;
    uint64_t ret = ctx->x[30] - base;
    if (ret != 0xa4bf94 && ret != 0xa4c2d0) return 0;  /* from the model renderer */
    /* the object being drawn is in the model renderer's stack slot sp+0x390 */
    cur.object = U64(ctx->sp + 0x390), cur.geometry = ctx->x[1], cur.submesh = ctx->x[2];
    cur.sp = ctx->sp, cur.ret = ret;
    cur.have_world = gbuffer_hook_world(ctx, cur.world);
    if (cur.have_world)
        memcpy(cur.world_row3, (const void *)(uintptr_t)(U64(ctx->sp + 0x30) + 0x70), sizeof cur.world_row3);
    cur.item = find_item(ctx->sp, ret == 0xa4c2d0);
    /* the layer's render state: the model renderer saved the block (the newest of its 0x21-byte
     * copies at device+0xefb, byte device+0x4630 of them) before this first submesh's material
     * changed it; the shadow and material layers have pushed one of their own before that */
    unsigned level = U8(cur.device + 0x4630);
    if (cur.layer_fresh && level) {
        memcpy(cur.layer_state, (const void *)(uintptr_t)(cur.device + 0xefb + 0x21ull * (level - 1)),
               sizeof cur.layer_state);
        memcpy(cur.layer_nvn, (const void *)(uintptr_t)(U64(cur.device + 0x10d0) + NVN_STATE_AT), NVN_STATE_BYTES);
        cur.layer_fresh = 0;
    }
    uint64_t bones;
    cur.bones_size = 0;
    if (mode == MODE_CHECK && cur.item && bone_palette(cur.item, &bones, &cur.bones_size))
        memcpy(cur.bones, (const void *)(uintptr_t)bones, cur.bones_size);
    st.submeshes++;
    if (mode == MODE_CHECK && cur.have_world) {  /* what the takeover would use, for the checks */
        object_constants(NULL, cur.hook_object);
        for (unsigned k = 0; k < TAKEOVER_TEXTURES; k++) cur.hook_tex[k] = slot_texture(k, cur.hook_samp[k]);
        uint64_t prev;
        if (cur.bones_size && bone_palette_previous(cur.item, &prev))
            memcpy(cur.hook_old_bones, (const void *)(uintptr_t)prev, cur.bones_size);
    }
    int why = takeover(ctx);
    if (takeover_on()) {
        if (why != TK_DRAWN) tk.left[why]++;
        else tk.taken++;
        takeover_summary();
    }
    if (why == TK_DRAWN) return cur.armed = 0, 1;  /* ours: the engine's commit and draw are skipped */
    cur.game_drew = 1;
    cur.armed = 1;
    return 0;
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
                " ours %ld; draw item found %ld, material constants %ld: ours bound in vs %ld, fs %ld; read %ld, ours %ld; textures %ld:"
                " ours bound %ld, another %ld, not decoded %ld; samplers %ld: state ours %ld, settings ours %ld\n",
                pass_name, prog_st[i].vs, prog_st[i].n, prog_st[i].exact, prog_st[i].ib, prog_st[i].item,
                prog_st[i].mat_n, prog_st[i].mat_ok[0], prog_st[i].mat_ok[1], prog_st[i].mat_read, prog_st[i].mat_any, prog_st[i].tex_n, prog_st[i].tex_ok,
                prog_st[i].tex_other, prog_st[i].tex_unknown, prog_st[i].smp_n, prog_st[i].smp_state,
                prog_st[i].smp_desc);
    for (int i = 0; i < PROGRAM_STATS && prog_st[i].n; i++) {
        fprintf(stderr, "yw4: %s:   vs %016" PRIx64 ": effect ours %ld of %ld, programs ours %ld; bones %ld: ours %ld, last frame's %ld: ours %ld; render state ours %ld of %ld; bytes differing:", pass_name,
                prog_st[i].vs, prog_st[i].pg_eff, prog_st[i].pg_n, prog_st[i].pg_stages, prog_st[i].bn_n, prog_st[i].bn_hook,
                prog_st[i].bn_old_n, prog_st[i].bn_late, prog_st[i].rs_ok, prog_st[i].item);
        for (int b = 0; b < 33; b++)
            if (prog_st[i].rs_bytes[b]) fprintf(stderr, " %#x:%ld", b, prog_st[i].rs_bytes[b]);
        fprintf(stderr, "; vertex layouts %ld: ours %ld, other streams %ld", prog_st[i].vl_n, prog_st[i].vl_ok,
                prog_st[i].vl_other);
        fprintf(stderr, "; other textures %ld: ours %ld, samplers ours %ld", prog_st[i].st_n, prog_st[i].st_tex,
                prog_st[i].st_samp);
        fprintf(stderr, "; model material %ld: ours %ld, rows differing:", prog_st[i].mm_n, prog_st[i].mm_ok);
        for (int row = 0; row < 22; row++)
            if (prog_st[i].mm_rows[row]) fprintf(stderr, " %d:%ld", row, prog_st[i].mm_rows[row]);
        fprintf(stderr, "; NVN state ours %ld, parts differing:", prog_st[i].nv_ok);
        for (int q = 0; q < 8; q++)
            if (prog_st[i].nv_regions[q]) fprintf(stderr, " %d:%ld", q, prog_st[i].nv_regions[q]);
        fprintf(stderr, "\n");
    }
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
    learn_program(cb);  /* the takeover may run in check mode too */
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
        prog_st[l].exact += same_rows(rows, game, cur.hook_object);  /* as captured at the hook */
        /* the material's textures, against the guest NVNtexture the game bound at fragment slot k (a
         * slot the program doesn't read may hold an older texture: "another" is not by itself wrong) */
        uint64_t tex[8];
        unsigned ntex = cur.item ? material_textures(cur.item, tex) : 0;
        /* the slots the fragment program reads: the engine's program ([device+0x10d0]+0x2f8) keeps per
         * stage (+0xc8, +0xd0, +0xd8: fragment) a table of (engine slot, binding) pairs at +0xb8, count
         * +0xc0, for the textures the shader uses (engine slot = binding in every program seen) */
        uint32_t reads = 0;
        uint64_t eprog = U64(cur.device + 0x10d0), fsobj = 0, tab;
        if (guest_ptr(eprog) && guest_ptr(eprog = U64(eprog + 0x2f8))) fsobj = U64(eprog + 0xd8);
        if (guest_ptr(fsobj) && guest_ptr(tab = U64(fsobj + 0xb8)))
            for (unsigned j = 0; j < U32(fsobj + 0xc0) && j < 32; j++)
                if (U32(tab + 8ull * j) < 32) reads |= 1u << U32(tab + 8ull * j);
        for (unsigned k = 0; k < ntex && cb->trace_texpool; k++) {
            if (!(reads >> k & 1)) continue;
            uint64_t h = cb->trace_tex[1][k];
            uint32_t id = (uint32_t)(h & 0xFFFFF);
            const NvnTexEntry *e = h && (int)id < cb->trace_texpool->count
                                       ? (const NvnTexEntry *)cb->trace_texpool->entries + id : NULL;
            prog_st[l].tex_n++;
            if (!tex[k]) prog_st[l].tex_unknown++;
            else if (e && e->tex && e->guest == tex[k]) prog_st[l].tex_ok++;
            else prog_st[l].tex_other++;
            /* its sampler: our state against the engine's at the draw, our settings against the
             * sampler bound with the texture (the handle's high bits) */
            uint8_t ss[15];
            uint32_t sid = (uint32_t)(h >> 20);
            if (!h || !sampler_state(cur.item, k, ss)) continue;
            prog_st[l].smp_n++;
            prog_st[l].smp_state += !memcmp(ss, (const void *)(uintptr_t)(cur.device + 0x1120 + 0xfull * k), 15);
            if (!cb->trace_samplerpool || (int)sid >= cb->trace_samplerpool->count) continue;
            NvnSamplerDesc ours, game = ((const NvnSamplerDesc *)cb->trace_samplerpool->entries)[sid];
            sampler_desc(ss, &ours);
            ours.reduction = game.reduction;  /* not set by the engine */
            if (!memcmp(&ours, &game, sizeof ours)) prog_st[l].smp_desc++;
            else {
                static int shown;
                if (shown++ < 6)
                    fprintf(stderr, "yw4: gbuffer: sampler differs: game min %u mag %u wrap %u %u %u cmp %u %u lod %g %g"
                            " bias %g aniso %g; ours min %u mag %u wrap %u %u %u cmp %u %u lod %g %g bias %g aniso %g\n",
                            game.min_filter, game.mag_filter, game.wrap[0], game.wrap[1], game.wrap[2],
                            game.compare_mode, game.compare_func, game.lod_min, game.lod_max, game.lod_bias,
                            game.max_aniso, ours.min_filter, ours.mag_filter, ours.wrap[0], ours.wrap[1], ours.wrap[2],
                            ours.compare_mode, ours.compare_func, ours.lod_min, ours.lod_max, ours.lod_bias,
                            ours.max_aniso);
            }
        }
        {  /* what the engine changes per draw beyond what ours provides (constants 2, the material
            * constants and model material slots, bones 4 and 5; material textures 0..n-1): what a
            * draw of ours that skips the engine's would still miss */
            enum { P = 4 };
            static long n[P], ub[P][2][16], tx[P][2][32];
            static uint64_t lastd;
            int pi = !strcmp(pass_name, "gbuffer") ? 0 : !strcmp(pass_name, "material") ? 1 : !strcmp(pass_name, "shadow") ? 2 : 3;
            unsigned mslot = U16(cur.list + 0x7a);
            unsigned mmslot = U16(cur.list + 0x74);
            uint32_t ours_ub = 1u << 2 | 1u << 4 | 1u << 5 | (mslot < 16 ? 1u << mslot : 0) | (mmslot < 16 ? 1u << mmslot : 0);
            uint64_t t8[8];
            unsigned nt = cur.item ? material_textures(cur.item, t8) : 0;
            uint32_t ours_tex = (1u << nt) - 1;
            __atomic_fetch_add(&n[pi], 1, __ATOMIC_RELAXED);
            /* only what changed since this thread's previous draw (rebinding the same is free to skip);
             * for constants, the contents: the engine uploads blocks into fresh memory each time */
            static _Thread_local uint64_t prev_tex[2][32];
            static _Thread_local uint8_t prev_ub[2][16][256];
            for (int st = 0; st < 2; st++) {
                uint32_t u = cb->bound_ub[st] & ~ours_ub, t = cb->bound_tex[st] & ~(st ? ours_tex : 0);
                if (p) u &= cbuf_slots(p->stages[st].key);
                for (int k = 0; k < 16; k++)
                    if (u >> k & 1) {
                        const void *now = (const void *)(uintptr_t)cb->trace_ub[st][k];
                        if (now && !memcmp(prev_ub[st][k], now, 256)) u &= ~(1u << k);
                        if (now) memcpy(prev_ub[st][k], now, 256);
                    }
                for (int k = 0; k < 32 && k < TRACE_TEXTURES; k++)
                    if (t >> k & 1) {
                        if (prev_tex[st][k] == cb->trace_tex[st][k]) t &= ~(1u << k);
                        prev_tex[st][k] = cb->trace_tex[st][k];
                    }
                for (int k = 0; k < 16; k++) if (u >> k & 1) __atomic_fetch_add(&ub[pi][st][k], 1, __ATOMIC_RELAXED);
                for (int k = 0; k < 32; k++) if (t >> k & 1) __atomic_fetch_add(&tx[pi][st][k], 1, __ATOMIC_RELAXED);
            }
            uint64_t frame = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
            if (frame >= lastd + 1800 && frame > 2000) {
                lastd = frame;
                static const char *nm[P] = {"gbuffer", "material", "shadow", "stages"};
                for (int q = 0; q < P; q++) {
                    fprintf(stderr, "yw4: gbuffer: per-draw changes not ours: %s %ld draws; constants read:", nm[q], n[q]);
                    for (int st = 0; st < 2; st++)
                        for (int k = 0; k < 16; k++)
                            if (ub[q][st][k]) fprintf(stderr, " %s%d:%ld", st ? "fs" : "vs", k, ub[q][st][k]);
                    fprintf(stderr, "; textures:");
                    for (int st = 0; st < 2; st++)
                        for (int k = 0; k < 32; k++)
                            if (tx[q][st][k]) fprintf(stderr, " %s%d:%ld", st ? "fs" : "vs", k, tx[q][st][k]);
                    fprintf(stderr, "\n");
                }
            }
        }
        ((NvnCmdBuf *)cb)->bound_ub[0] = ((NvnCmdBuf *)cb)->bound_ub[1] = 0;
        ((NvnCmdBuf *)cb)->bound_tex[0] = ((NvnCmdBuf *)cb)->bound_tex[1] = 0;
        /* the fragment textures beyond the material's (scene textures, the reflection probe), against the
         * game's binding: texture and sampler, as captured at the hook */
        if (p && cb->trace_texpool && cb->trace_samplerpool) {
            uint64_t mt[8];
            unsigned nmat = cur.item ? material_textures(cur.item, mt) : 0;
            uint32_t reads = tex_slots(p->stages[1].key);
            for (unsigned k = nmat; k < TAKEOVER_TEXTURES && k < TRACE_TEXTURES; k++) {
                if (!(reads >> k & 1)) continue;
                uint64_t h = cb->trace_tex[1][k];
                uint32_t id = (uint32_t)(h & 0xFFFFF), sid = (uint32_t)(h >> 20);
                if (!h || (int)id >= cb->trace_texpool->count || (int)sid >= cb->trace_samplerpool->count) continue;
                NvnSamplerDesc ours, game = ((const NvnSamplerDesc *)cb->trace_samplerpool->entries)[sid];
                sampler_desc(cur.hook_samp[k], &ours);
                ours.reduction = game.reduction;
                prog_st[l].st_n++;
                prog_st[l].st_tex += ((const NvnTexEntry *)cb->trace_texpool->entries)[id].guest == cur.hook_tex[k];
                prog_st[l].st_samp += !memcmp(&ours, &game, sizeof ours);
            }
        }
        /* the bone palettes, against vertex constants 4 (this frame's) and 5 (last frame's) the game bound */
        uint64_t prev;
        if (cur.bones_size && cb->trace_ub[0][4] && cur.item) {
            prog_st[l].bn_n++;
            prog_st[l].bn_hook += !memcmp(cur.bones, (const void *)(uintptr_t)cb->trace_ub[0][4], cur.bones_size);
            if (cb->trace_ub[0][5] && bone_palette_previous(cur.item, &prev)) {  /* as captured at the hook */
                prog_st[l].bn_old_n++;
                prog_st[l].bn_late += !memcmp(cur.hook_old_bones, (const void *)(uintptr_t)cb->trace_ub[0][5], cur.bones_size);
            }
        }
        /* the shader effect, the program and the render state block, against the engine's at the draw */
        if (cur.item) {
            uint8_t rs[33];
            uint64_t effect = effect_of(cur.item), vs = 0, fs = 0;
            draw_state(cur.item, effect, rs);
            prog_st[l].pg_n++;
            prog_st[l].pg_eff += effect == U64(cur.device + 0x27a0);
            prog_st[l].pg_stages += pass_shaders(effect, &vs, &fs) && cb->program && vs == cb->program->stages[0].code &&
                                    fs == cb->program->stages[1].code;
            int same = 1;
            for (int b = 0; b < 33; b++)
                if (rs[b] != U8(cur.device + 0xeb9 + b)) prog_st[l].rs_bytes[b]++, same = 0;
            prog_st[l].rs_ok += same;
            /* the NVN state objects made from our block, against the engine's (blend 0, channel mask,
             * colour, depth stencil, multisample, polygon, stencil reference and mask and depth bias,
             * the two flags) */
            static const struct { uint8_t at, n; } part[8] = {{0, 8}, {0x40, 4}, {0x44, 4}, {0x48, 8}, {0x50, 0x18},
                                                              {0x68, 4}, {0x74, 0x14}, {0xb0, 2}};
            uint8_t nv[NVN_STATE_BYTES];
            const uint8_t *eng = (const uint8_t *)(uintptr_t)(U64(cur.device + 0x10d0) + NVN_STATE_AT);
            nvn_state(rs, nv);
            int nsame = 1;
            for (int q = 0; q < 8; q++)
                if (memcmp(nv + part[q].at, eng + part[q].at, part[q].n)) prog_st[l].nv_regions[q]++, nsame = 0;
            prog_st[l].nv_ok += nsame;
            /* the model material block, against what the game bound at its slot (list+0x74), in the
             * stage(s) whose shader reads it; rows 4-21 only for this draw's textures */
            unsigned mslot = U16(cur.list + 0x74), ntex;
            for (int st = 0; st < 2 && mslot < 16; st++) {
                if (!p || !(cbuf_slots(p->stages[st].key) >> mslot & 1) || !cb->trace_ub[st][mslot]) continue;
                uint8_t mm[0x160];
                uint8_t mat_state[33];
                render_state(cur.item, mat_state);
                model_material(cur.item, mat_state, mm, &ntex);
                const uint8_t *g = (const uint8_t *)(uintptr_t)cb->trace_ub[st][mslot];
                int msame = 1;
                for (int row = 0; row < 22; row++) {
                    if (row >= 4 && row < 16 && (unsigned)(row - 4) / 2 >= ntex) continue;
                    if (row >= 16 && (unsigned)(row - 16) >= ntex) continue;
                    if (memcmp(mm + 16 * row, g + 16 * row, 16)) prog_st[l].mm_rows[row]++, msame = 0;
                }
                prog_st[l].mm_n++;
                prog_st[l].mm_ok += msame;
                static int shown;
                if (!msame && shown++ < 6) {
                    fprintf(stderr, "yw4: gbuffer: model material differs (stage %d, %u textures):", st, ntex);
                    for (int row = 0; row < 4; row++) {
                        const float *a = (const float *)(g + 16 * row), *b = (const float *)(mm + 16 * row);
                        fprintf(stderr, " | row %d game %g %g %g %g ours %g %g %g %g", row, a[0], a[1], a[2], a[3], b[0], b[1],
                                b[2], b[3]);
                    }
                    fprintf(stderr, "\n");
                }
                break;
            }
            /* the vertex layout, against the attribute and stream states the game bound */
            uint32_t attrs[16];
            int other;
            uint32_t used = vertex_layout(cur.item, attrs, &other), stride = U8(cur.submesh + 0x3e);
            if (used) {
                int vsame = !other && cb->trace_nattribs <= 16;
                for (uint32_t a = 0; vsame && a < cb->trace_nattribs; a++)
                    vsame = attrs[a] == cb->trace_attribs[a] && (!(used >> a & 1) || a >= 8 ||
                                                                 (uint32_t)cb->trace_streams[a] == stride);
                prog_st[l].vl_n++;
                prog_st[l].vl_ok += vsame;
                prog_st[l].vl_other += other != 0;
                static int shown;
                if (!vsame && !other && shown++ < 6) {
                    fprintf(stderr, "yw4: gbuffer: vertex layout differs: stride %u, game:", stride);
                    for (uint32_t a = 0; a < cb->trace_nattribs; a++)
                        fprintf(stderr, " %#x/%u", cb->trace_attribs[a], a < 8 ? (uint32_t)cb->trace_streams[a] : 0);
                    fprintf(stderr, "; ours:");
                    for (int a = 0; a < 16; a++) if (attrs[a]) fprintf(stderr, " %d=%#x", a, attrs[a]);
                    fprintf(stderr, "\n");
                }
            }
        }
        /* the material constants, against what the game bound at the draw list's slot (+0x7a) */
        uint8_t mat[0x80];
        unsigned slot = U16(cur.list + 0x7a);
        if (cur.item && slot < 16 && material_constants(cur.item, mat)) {
            prog_st[l].mat_n++;
            int reads = 0, eq_all = 1;
            for (int stage = 0; stage < 2; stage++) {
                int eq = cb->trace_ub[stage][slot] && !memcmp((const void *)(uintptr_t)cb->trace_ub[stage][slot], mat, 0x80);
                prog_st[l].mat_ok[stage] += eq;
                /* only the stages whose shader reads the slot: the other may hold an older block */
                if (p && cbuf_slots(p->stages[stage].key) >> slot & 1) reads = 1, eq_all &= eq;
            }
            prog_st[l].mat_read += reads;
            prog_st[l].mat_any += reads && eq_all;
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
 * view x world, projection x view x world; last frame's world is this frame's (on every draw checked,
 * walking characters too: they move through their bones), with the record's own bottom row (-0
 * where the game has it); last frame's view x world is the renderer's record's +0x240, which the
 * engine writes at the commit (after our hook): read when the draw is recorded. */
static void object_constants(const float *game, float out[GBUFFER_OBJECT_FLOATS]) {
    (void)game;
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
    memcpy(out + 48, cur.world, sizeof cur.world);
    memcpy(out + 60, cur.world_row3, sizeof cur.world_row3);
    /* last frame's view x world: the device's derived matrix 10 (f_977680, computed at the commit
     * into the matrix cache at [device+0xe90] - 0x180, our record's +0x240): last frame's view (the
     * device's matrix 4, +0x380) times last frame's world (matrix 6: this frame's, with its bottom row) */
    Mat4 w6 = w, oldview = mat4_load(cur.device + 0x380);
    memcpy(w6.m + 12, cur.world_row3, sizeof cur.world_row3);
    Mat4 ovw = mat4_mul(&oldview, &w6);
    memcpy(out + 64, ovw.m, sizeof ovw.m);
}

int gbuffer_take_draw(CPUState *ctx, const NvnCmdBuf *cb, int indexed, int instanced) {
    if (cur.armed) learn_program(cb);
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
