/* Draws on Vulkan (decision 0021): translated shaders, pipelines, descriptors.
 *
 * Shaders: an NVN program's stages are named by the key of their assembly (0020); their
 * SPIR-V and interface (<key>.<stage>.spv, <key>.iface from recomp/shader.py) are read
 * from YW4_SHADER_DIR (default work/shaders/glsl) the first time a program is drawn with.
 *
 * Bindings (0020): descriptor set = stage (vertex 0, fragment 1, geometry 2); constant
 * buffer n -> binding n, texture slot k -> 16 + k, storage buffer n -> 64 + n. A texture
 * slot k is what BindTexture(stage, k, handle) binds: handle = texture id | sampler id << 20
 * in the current texture and sampler pools.
 *
 * Pipelines are cached per program on the state Vulkan can't set dynamically: attachment
 * formats, blending, channel masks, polygon mode, depth clamp, topology, vertex input.
 * Everything else (viewport, scissor, cull, front face, depth and stencil) is dynamic state.
 *
 * Window origin: with NVN's upper-left origin, clip-space y = +1 is the top row, so the
 * viewport is flipped (negative height). With the OpenGL lower-left origin, the rows the
 * Switch writes come out the same without a flip, but Vulkan's area rule (y down) then
 * sees every triangle with the opposite winding, so the front face is inverted.
 *
 * Enum values follow LAS Recomp (runtime/nvn/vulkan/exec.c; MIT, see third_party/lasrecomp).
 */
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gbuffer.h"
#include "post.h"
#include "gpu_internal.h"
#include "host.h"
#include "kernel.h"

enum { ST_VERT, ST_FRAG, ST_GEOM, NSETS = 3, ST_COMP = 3 };  /* NSETS: the graphics stages; compute is separate */
static const char *const stage_names[4] = {"vert", "frag", "geom", "comp"};
static const VkShaderStageFlagBits stage_bits[4] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                                                    VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
/* NVN shader stage numbers used by Bind*(stage, ...) and NvnProgram.stages */
static const int nvn_stage[NSETS] = {0, 1, 2};

/* ================================================================== shaders */

enum { MAX_BINDINGS = 64 };
typedef struct {
    char kind;  /* c constant buffer, t texture, i image, s storage buffer, d a shadow map's raw depth
                 * (texture slot, no compare), p the soft shadow settings (yw4_shadow in recomp/shader.py) */
    uint32_t slot;
    VkImageViewType view;  /* textures */
    int shadow;
} Binding;

typedef struct Shader {
    struct Shader *next;
    uint64_t key;
    int stage;
    VkShaderModule module;
    Binding b[MAX_BINDINGS];
    int nb;
    char inputs[32];  /* vertex inputs: 0 unused, 'f' float, 'u' integer */
    VkDescriptorSetLayout layout;
    VkShaderModule ours;  /* our readable program with the same interface, if we have one (0: none) */
    int pcf_float;  /* samples a shadow map: the float in cbuf 8 where its filter spread starts (2), else -1 */
    int screen_taps;  /* reads cbuf 8 row 32 (tap offsets in the screen-sized targets) */
    int inv_proj;     /* a fragment program reading cbuf 8 rows 20-23 (the inverse projection): positions from depth */
    int dof;          /* the distance blur's composite (gpu_dof) */
    uint32_t cb8_bytes;  /* how much of cbuf 8 it reads (with pcf_float or screen_taps) */
    int model;  /* a vertex program with the engine's local-matrix block in slot 2: it draws 3D objects */
    /* a vertex program whose position is one constant buffer's rows dotted with input 0 (the UI's):
     * that buffer + 1 and the rows of x, y, z, w; or input 0 as it is (pos_input: full-screen passes);
     * neither: the iface has no line m */
    uint32_t pos_cbuf, pos_rows[4];
    int pos_input;
} Shader;

static Shader *shaders;
/* the draw being bound reads a screen-sized image: after the composite it is not UI (widescreen; gpu_dynamic_state) */
static int dyn_reads_screen;
/* the draw being bound is a UI layer over the game's whole screen: stretched, not kept 16:9 (widescreen) */
static int dyn_full_screen;
/* ... in an image narrower than 16:9 (16:10), where the UI is fitted to the width with a strip left above and
 * below: the next gpu_dynamic_state is for a whole-screen layer's second or third drawing, which fills the strip
 * at the top (1) or the bottom (2) from the layer's edge */
static int dyn_edge;

/* Distance blur (depth of field): the game blurs the scene by distance. Fragment shader 27b63a2b... reads
 * depth, turns it into an amount with cbuf 6 row 0 (a, b, c, w: clamp((a / (b z + cam) + c) w)) and draws
 * a blurred half-size image over the scene with that amount as opacity (2026-10-27: it takes 63% of the
 * fine detail of a schoolyard's background and none of the foreground). The blurred image comes from
 * 800x450 passes whose taps are in pixels, so a larger 3D view (YW4_RES) weakens it. YW4_DOF=2 the
 * game's, 1 half (w halved; the default, the owner's pick), 0 off (the draw skipped). */
enum { DOF_SHADER = 0x27b63a2bf65deaf0ull, DOF_BLUR_SHADER = 0xb8856cfd2725bc25ull };  /* composite; the blur passes */
enum { COMPOSITE_SHADER = 0x58b87f4410e7bc5dull };  /* the bloom composite: the scene is done (gpu_aa.c) */
static int dof_level = -1;

int gpu_dof(void) {
    int l = __atomic_load_n(&dof_level, __ATOMIC_ACQUIRE);
    if (l < 0) {
        const char *e = getenv("YW4_DOF");
        l = e ? atoi(e) : 1;
        if (l < 0 || l > 2) l = 2;
        __atomic_store_n(&dof_level, l, __ATOMIC_RELEASE);
    }
    return l;
}

void gpu_dof_cycle(void) {
    int l = (gpu_dof() + 2) % 3;  /* 2 -> 1 -> 0 -> 2 */
    __atomic_store_n(&dof_level, l, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: distance blur %s\n", l == 2 ? "the game's" : l == 1 ? "half" : "off");
}

static const char *shader_dir(void) {
    const char *d = getenv("YW4_SHADER_DIR");
    return d && *d ? d : "work/shaders/glsl";
}

static void *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc((size_t)n + 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) gpu_stop("cannot read %s", path);
    fclose(f);
    p[n] = 0;
    *size = (size_t)n;
    return p;
}

static VkImageViewType view_type(const char *dim, int *shadow) {
    *shadow = !strncmp(dim, "SHADOW", 6);
    if (*shadow) dim += 6;
    if (!strcmp(dim, "2D")) return VK_IMAGE_VIEW_TYPE_2D;
    if (!strcmp(dim, "ARRAY2D")) return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (!strcmp(dim, "3D")) return VK_IMAGE_VIEW_TYPE_3D;
    if (!strcmp(dim, "CUBE")) return VK_IMAGE_VIEW_TYPE_CUBE;
    gpu_stop("texture target %s", dim);
}

/* The shadow mask's fragment program (docs/frame.md "Lighting stage"). */
#define SHADOW_MASK_FS 0x06d03ad703b4bee5ull

/* The screen passes: the game's full-screen programs between the G-buffer and the UI that ours replace in
 * the engine's own draws (the shadow mask's way, decision 0044): same inputs, targets and state.
 * YW4_SCREEN_PASSES=game: the game's. */
static const uint64_t screen_fs[] = {
    0x95ea1b3181bdf579ull,  /* colour and depth copy (depth_copy.frag) */
    0x6b4575fb57b97548ull,  /* depth-only copy */
    0xd2aaa383b27842ecull,  /* fill with the quad's colour */
    0x2571d0a66582ab9bull,  /* copy, negatives to 0 */
    0x2d68c875031cfe11ull,  /* copy times the quad's colour */
    0x79077436c58013f9ull,  /* the lighting inputs (f_7f4c90) */
    0x3c94bd209d61eddcull,  /* edge-aware upsample */
    0x22d01556d2366ea7ull,  /* tone mapping */
    0x42759b97412ca8acull,  /* lamp lights (a light volume per lamp, not a quad) */
    0x3f9fc96541a9bd43ull,  /* the game's own FXAA */
    0x627287ec450d41fdull,  /* sky and aerial perspective */
};

static int is_screen_fs(uint64_t key) {
    for (size_t i = 0; i < sizeof screen_fs / sizeof *screen_fs; i++)
        if (screen_fs[i] == key) return 1;
    return 0;
}

static int screen_on = -1;

static int screen_native(void) {
    int on = __atomic_load_n(&screen_on, __ATOMIC_ACQUIRE);
    if (on < 0) {
        on = !(getenv("YW4_SCREEN_PASSES") && !strcmp(getenv("YW4_SCREEN_PASSES"), "game"));
        __atomic_store_n(&screen_on, on, __ATOMIC_RELEASE);
    }
    return on;
}

int screen_passes_native(void) { return screen_native(); }

/* the settings panel (DEVELOPER OPTIONS, SCREEN PASSES) */
void screen_passes_toggle(void) {
    int on = !screen_native();
    __atomic_store_n(&screen_on, on, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: screen passes: %s (settings panel)\n", on ? "ours" : "the game's");
}

/* gpu.c, at each draw: YW4_AB_PASS=screen compares this draw, ours against the game's, when its fragment
 * program starts with YW4_AB_FS (hex) and it is one of the screen passes (YW4_AB_AT picks the frame), and
 * it is the frame's YW4_AB_DRAW-th such draw (default 1). YW4_AB_FS=all: any screen pass, the frame's
 * (frame % 32 + 1)-th, so 32 frames in a row in YW4_AB_AT compare each of a frame's screen draws in turn.
 * The second run of the draw is not counted. */
int gpu_screen_ab_draw(const GpuState *s) {
    static int wanted = -1, all;
    static uint64_t prefix, frame;
    static int digits, nth, seen;
    if (wanted < 0) {
        const char *e = getenv("YW4_AB_FS");
        wanted = gpu_ab_wanted("screen") && e && *e;
        all = wanted && !strcmp(e, "all");
        if (wanted && !all) prefix = strtoull(e, NULL, 16), digits = (int)strlen(e);
        nth = getenv("YW4_AB_DRAW") ? atoi(getenv("YW4_AB_DRAW")) : 1;
    }
    if (!wanted || !s->program || (!all && (digits < 1 || digits > 16))) return 0;
    uint64_t fs = s->program->stages[1].key;
    if (!is_screen_fs(fs) || (!all && fs >> (64 - 4 * digits) != prefix)) return 0;
    if (gpu_ab_game) return 1;  /* the draw again, the game's: the range's end follows it */
    if (frame != gpu_frames()) frame = gpu_frames(), seen = 0;
    return ++seen == (all ? (int)(frame % 32) + 1 : nth);
}

/* Our replacements for game programs (decision 0026): readable GLSL in runtime/shaders with the
 * translated program's interface, used in its place when the G-buffer pass is on (settings panel). */
static const struct {
    uint64_t key;
    int stage;
    const char *name;
} replacements[] = {
    {0xbc358b88a500029full, ST_VERT, "gbuffer_main.vert"},
    {0xa93492ef0b558e21ull, ST_FRAG, "gbuffer_main.frag"},
    {0x01fd748a298cf8b6ull, ST_FRAG, "gbuffer_main_cutout.frag"},
    {0x53ce6660dbaa7332ull, ST_FRAG, "gbuffer_gloss.frag"},
    {0xca3b58d6b4fb04f7ull, ST_FRAG, "gbuffer_gloss_cutout.frag"},
    {0x2c5e1e076da5b271ull, ST_FRAG, "gbuffer_plain.frag"},
    {0xb742aa7d4094cd28ull, ST_VERT, "gbuffer_skinned.vert"},
    {0x046b2a15a631058full, ST_VERT, "material_main.vert"},
    {0x5eb3b2cacbdb9824ull, ST_FRAG, "material_main.frag"},
    {0xb0aa5d810b96a62eull, ST_FRAG, "gbuffer_skinned.frag"},
    {SHADOW_MASK_FS, ST_FRAG, "shadow_mask.frag"},  /* its own switch (shadow_mask.c) */
    /* the screen passes' (screen_native) */
    {0x95ea1b3181bdf579ull, ST_FRAG, "depth_copy.frag"},
    {0x6b4575fb57b97548ull, ST_FRAG, "depth_only_copy.frag"},
    {0xd2aaa383b27842ecull, ST_FRAG, "vertex_colour.frag"},
    {0x2571d0a66582ab9bull, ST_FRAG, "clamp_copy.frag"},
    {0x2d68c875031cfe11ull, ST_FRAG, "tinted_copy.frag"},
    {0x79077436c58013f9ull, ST_FRAG, "lighting_inputs.frag"},
    {0x3c94bd209d61eddcull, ST_FRAG, "edge_upsample.frag"},
    {0x22d01556d2366ea7ull, ST_FRAG, "tonemap.frag"},
    {0x42759b97412ca8acull, ST_FRAG, "lamp_light.frag"},
    {0x3f9fc96541a9bd43ull, ST_FRAG, "game_fxaa.frag"},
    {0x627287ec450d41fdull, ST_FRAG, "sky_fog.frag"},
};

static VkShaderModule replacement(uint64_t key, int stage) {
    for (size_t i = 0; i < sizeof replacements / sizeof *replacements; i++) {
        if (replacements[i].key != key || replacements[i].stage != stage) continue;
        const char *dir = getenv("YW4_NATIVE_SHADER_DIR");
        char path[1024];
        size_t n;
        snprintf(path, sizeof path, "%s/%s.spv", dir && *dir ? dir : "work/build/shaders", replacements[i].name);
        uint32_t *spv = read_file(path, &n);
        if (!spv) {
            fprintf(stderr, "yw4: gpu: no %s (python -m recomp.build compiles runtime/shaders): the game's program stays\n",
                    path);
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo mci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = spv};
        VkShaderModule m;
        CHECK(vkCreateShaderModule(vk.device, &mci, NULL, &m));
        free(spv);
        return m;
    }
    return VK_NULL_HANDLE;
}

/* The next non-empty line of *p (NUL-terminated in place), or NULL at the end; *p moves past it. */
static char *next_line(char **p) {
    char *s = *p;
    while (*s == '\n' || *s == '\r') s++;
    if (!*s) return NULL;
    char *e = s + strcspn(s, "\n");
    if (*e) *e++ = 0;
    *p = e;
    return s;
}

/* A translated shader's module and descriptor set layout, from the files (not kept in the list). */
static Shader *load_shader(uint64_t key, int stage) {
    Shader *s = calloc(1, sizeof *s);
    s->key = key, s->stage = stage;
    char path[1024];
    size_t n;
    snprintf(path, sizeof path, "%s/%016" PRIx64 ".%s.spv", shader_dir(), key, stage_names[stage]);
    uint32_t *spv = read_file(path, &n);
    if (!spv) gpu_stop("no translated shader %s (run python -m recomp.shader --check)", path);
    VkShaderModuleCreateInfo mci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = spv};
    CHECK(vkCreateShaderModule(vk.device, &mci, NULL, &s->module));
    free(spv);

    snprintf(path, sizeof path, "%s/%016" PRIx64 ".iface", shader_dir(), key);
    char *text = read_file(path, &n);
    if (!text) gpu_stop("no shader interface %s", path);
    VkDescriptorSetLayoutBinding lb[MAX_BINDINGS];
    /* lines split by hand, not strtok: the prewarm threads load shaders at the same time (gpu_prebuild_start) */
    char *rest = text;
    for (char *line; (line = next_line(&rest)) != NULL;) {
        char kind, dim[32] = "";
        unsigned slot, rx, ry, rz, rw;
        if (!strncmp(line, "m in", 4)) {  /* the position: input 0 as it is */
            s->pos_input = 1;
            continue;
        }
        if (sscanf(line, "%c %u %31s", &kind, &slot, dim) < 2) continue;
        if (kind == 'a') {
            if (slot < 32) s->inputs[slot] = dim[0];
            continue;
        }
        if (kind == 'r') continue;  /* rows read from a constant buffer: for gbuffer.c */
        if (kind == 'm') {  /* the position: a matrix, buffer and rows of x y z w */
            if (sscanf(line, "m %u %u %u %u %u", &slot, &rx, &ry, &rz, &rw) == 5 && slot < UBOS)
                s->pos_cbuf = slot + 1, s->pos_rows[0] = rx, s->pos_rows[1] = ry, s->pos_rows[2] = rz, s->pos_rows[3] = rw;
            continue;
        }
        if (kind == 'c' && slot == 2 && stage == ST_VERT && !strcmp(dim, "CBUSE_UB_LOCAL_MATRIX_IDX")) s->model = 1;
        if (s->nb == MAX_BINDINGS) gpu_stop("shader %016" PRIx64 " has too many bindings", key);
        Binding *b = &s->b[s->nb];
        b->kind = kind, b->slot = slot;
        VkDescriptorSetLayoutBinding *l = &lb[s->nb++];
        *l = (VkDescriptorSetLayoutBinding){.descriptorCount = 1, .stageFlags = stage_bits[stage]};
        switch (kind) {
        case 'c': l->binding = slot, l->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
        case 't':
            l->binding = 16 + slot, l->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b->view = view_type(dim, &b->shadow);
            break;
        case 'd':
            l->binding = 96 + slot, l->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b->view = view_type(dim, &b->shadow);
            break;
        case 'p': l->binding = 95, l->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
        case 'i': l->binding = 16 + slot, l->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; break;
        case 's': l->binding = 64 + slot, l->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; break;
        default: gpu_stop("shader interface line '%s'", line);
        }
    }
    free(text);
    /* Two cbuf 8 (camera) values are in pixels of a target we may make larger (bind_stage_set scales
     * them in a copy):
     * - the shaders that sample a shadow map average 5 taps spread by a value the game sets (half a
     *   texel of the 1024 map and up): d[31].zw in most, d[8].zw in the rest (shadow quality);
     * - row 32 = 1.5/1600, 1.5/900, 0.75/533, 0.75/300: tap offsets in the screen-sized targets
     *   (resolution of the 3D view, F4; docs/frame.md).
     * Found in the GLSL. */
    s->pcf_float = -1, s->screen_taps = 0, s->inv_proj = 0;
    s->dof = stage == ST_FRAG && key == DOF_SHADER;
    int has_cb8 = 0, has_shadow = 0;
    for (int i = 0; i < s->nb; i++) {
        if (s->b[i].kind == 'c' && s->b[i].slot == 8) has_cb8 = 1;
        if (s->b[i].kind == 't' && s->b[i].shadow) has_shadow = 1;
    }
    if (has_cb8) {
        {
            snprintf(path, sizeof path, "%s/%016" PRIx64 ".%s", shader_dir(), key, stage_names[stage]);
            char *glsl = read_file(path, &n);
            if (glsl) {
                if (has_shadow)
                    s->pcf_float = strstr(glsl, "cb8.d[31].zw") ? 31 * 4 + 2 : strstr(glsl, "cb8.d[8].zw") ? 8 * 4 + 2 : -1;
                s->screen_taps = strstr(glsl, "cb8.d[32]") != NULL;
                s->inv_proj = stage == ST_FRAG && strstr(glsl, "cb8.d[20]") && strstr(glsl, "cb8.d[23]");
                /* the copy only needs what the shader reads: up to its highest constant index
                 * (any computed index: all of it) */
                s->cb8_bytes = 0;
                for (const char *p = glsl; (p = strstr(p, "cb8.d[")) != NULL && s->cb8_bytes != 65536; p += 6) {
                    char *end;
                    unsigned long k = strtoul(p + 6, &end, 10);
                    if (end == p + 6 || *end != ']') s->cb8_bytes = 65536;
                    else if ((k + 1) * 16 > s->cb8_bytes) s->cb8_bytes = (uint32_t)(k + 1) * 16;
                }
                free(glsl);
            }
        }
    }
    VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = (uint32_t)s->nb, .pBindings = lb};
    CHECK(vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &s->layout));
    s->ours = replacement(key, stage);
    return s;
}

/* ---- every translated shader loaded in the background at start (gpu_prebuild_start), so a new area's
 * first draws find theirs ready: loading one from its files (SPIR-V, interface, GLSL) and making its module
 * took 5-10 ms on the GPU thread (2026-11-08, town: 12 new shaders = 110 ms, a visible hitch). The slots are
 * made before the threads start and are read-only after, but for s: NULL not loaded yet, &pre_taken taken
 * by the GPU thread (shader()), else a loaded Shader waiting to be taken. YW4_SHADER_PRELOAD=0: off. */
enum { PRE_CAP = 8192 };
typedef struct {
    uint64_t key;  /* 0: empty slot */
    int stage;
    Shader *s;
} PreSlot;
static PreSlot *pre;
static uint32_t *pre_order;  /* the slots in directory order, for the threads */
static size_t pre_n, pre_next;
static Shader pre_taken;
static int64_t pre_t0;
static int pre_left;

static size_t pre_hash(uint64_t key, int stage) {
    return (size_t)(((key ^ (uint64_t)stage * 0x9E3779B97F4A7C15ull) * 0xff51afd7ed558ccdull) >> 40) & (PRE_CAP - 1);
}

static PreSlot *pre_find(uint64_t key, int stage) {
    if (!pre || !key) return NULL;
    for (size_t i = pre_hash(key, stage);; i = (i + 1) & (PRE_CAP - 1)) {
        if (!pre[i].key) return NULL;
        if (pre[i].key == key && pre[i].stage == stage) return &pre[i];
    }
}

/* The preloaded shader for (key, stage), taken (the slot then stays taken), or NULL: load it here. */
static Shader *pre_take(uint64_t key, int stage) {
    PreSlot *p = pre_find(key, stage);
    if (!p) return NULL;
    Shader *s = __atomic_exchange_n(&p->s, &pre_taken, __ATOMIC_ACQ_REL);
    return s == &pre_taken ? NULL : s;
}

static Shader *shader(uint64_t key, int stage) {
    for (Shader *s = shaders; s; s = s->next)
        if (s->key == key && s->stage == stage) return s;
    Shader *s = pre_take(key, stage);
    if (s) {
        s->next = shaders;
        shaders = s;
        return s;
    }
    int64_t t0 = host_now_ns();
    s = load_shader(key, stage);
    gpu_stall_add(STALL_SHADER, host_now_ns() - t0);
    s->next = shaders;
    shaders = s;
    return s;
}

static VkDescriptorSetLayout empty_layout(void) {
    static VkDescriptorSetLayout l;
    if (!l) {
        VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        CHECK(vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &l));
    }
    return l;
}

/* ================================================================== programs and pipelines */

typedef struct {
    VkFormat color[8], depth;
    uint8_t blend[8][8];
    uint8_t color_state[4];
    uint32_t channel_mask;
    uint8_t polygon_mode, topology, depth_clamp, ncolor;  /* ncolor: as in the rendering, empty slots too */
    uint8_t ours;  /* our replacement programs in place of the translated ones (Shader.ours) */
    VkVertexInputAttributeDescription attr[16];
    uint32_t nattr;
    VkVertexInputBindingDescription bind[16];
    uint32_t nbind;
} PipeKey;

typedef struct Pipe {
    struct Pipe *next;
    PipeKey key;
    VkPipeline pipeline;
} Pipe;

typedef struct {
    Shader *sh[NSETS];
    VkPipelineLayout layout;
    Pipe *pipes;
    Shader *comp;  /* the compute stage, if any (set 0, no push constants) */
    VkPipelineLayout comp_layout;
    VkPipeline comp_pipe;
    int warm;  /* a prewarm thread's copy (gpu_prebuild_start): not journalled, not counted as a frame's stall */
} Program;

static void journal_add(const Program *g, const PipeKey *k);

static Program *program_of(NvnProgram *p) {
    if (p->gpu) return ((Program *)p->gpu)->sh[ST_VERT] ? p->gpu : NULL;  /* a compute-only program has no draws */
    if (!(p->stage_mask & 1)) return NULL;  /* compute only, or nothing */
    if (p->stage_mask & 0x18) gpu_stop("tessellation shaders");
    Program *g = calloc(1, sizeof *g);
    VkDescriptorSetLayout layouts[NSETS];
    uint32_t nsets = 2;
    for (int i = 0; i < NSETS; i++) {
        if (p->stage_mask & (1u << nvn_stage[i])) g->sh[i] = shader(p->stages[nvn_stage[i]].key, i);
        layouts[i] = g->sh[i] ? g->sh[i]->layout : empty_layout();
    }
    if (g->sh[ST_GEOM]) nsets = 3;
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, 4};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = nsets,
                                      .pSetLayouts = layouts, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    CHECK(vkCreatePipelineLayout(vk.device, &pli, NULL, &g->layout));
    p->gpu = g;
    return g;
}

/* The program with its compute pipeline made (a compute-only program is a Program with no graphics stages). */
static Program *compute_program_of(NvnProgram *p) {
    if (!(p->stage_mask & 0x20)) return NULL;
    Program *g = p->gpu;
    if (!g) p->gpu = g = calloc(1, sizeof *g);
    if (g->comp_pipe) return g;
    g->comp = shader(p->stages[5].key, ST_COMP);
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &g->comp->layout};
    CHECK(vkCreatePipelineLayout(vk.device, &pli, NULL, &g->comp_layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = g->comp->module,
                                                .pName = "main"},
                                      .layout = g->comp_layout};
    CHECK(vkCreateComputePipelines(vk.device, gpu_pcache(), 1, &ci, NULL, &g->comp_pipe));
    return g;
}

void gpu_program_finalize(NvnProgram *p) {
    /* Pipelines and layouts are small; they are kept (a program's memory is reused for
     * another program with SetShaders, so only the link is dropped). */
    if (p) p->gpu = NULL;
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: gpu_stop("blend factor %#x", f);
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 1: return VK_BLEND_OP_ADD;
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: gpu_stop("blend equation %u", e);
    }
}

static VkCompareOp compare_fn(uint8_t f) {
    static const VkCompareOp ops[9] = {VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS,
                                       VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER,
                                       VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return f < 9 ? ops[f] : VK_COMPARE_OP_ALWAYS;
}

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int integer_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SINT:
    case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R32G32B32_UINT:
    case VK_FORMAT_R32G32B32_SINT:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    default: return gpu_format(f);
    }
}

/* A shader reading a vertex input as integers (`u`) needs an integer format: a 32-bit float
 * format is read as its bits, which is what the Switch's registers hold. */
static VkFormat input_format(VkFormat f, char want, uint32_t loc) {
    if (want != 'u' || integer_format(f)) {
        if (want != 'u' && integer_format(f)) {
            static uint32_t logged;
            if (!(logged & 1u << loc)) fprintf(stderr, "yw4: gpu: vertex input %u: integer format read as floats\n", loc);
            logged |= 1u << loc;
            switch (f) {  /* the values, as floats: the nearest Vulkan allows */
            case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_USCALED;
            case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SSCALED;
            case VK_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_USCALED;
            case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SSCALED;
            /* 32-bit integers have no scaled format. The shader only gets bits to copy (a plain MOV
             * says nothing about the type), so fetch them as the float of the same width. */
            case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: return VK_FORMAT_R32_SFLOAT;
            case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_SFLOAT;
            case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: return VK_FORMAT_R32G32B32_SFLOAT;
            case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_SFLOAT;
            default: gpu_stop("vertex input %u: integer format %d read as floats", loc, (int)f);
            }
        }
        return f;
    }
    switch (f) {
    case VK_FORMAT_R32_SFLOAT: return VK_FORMAT_R32_UINT;
    case VK_FORMAT_R32G32_SFLOAT: return VK_FORMAT_R32G32_UINT;
    case VK_FORMAT_R32G32B32_SFLOAT: return VK_FORMAT_R32G32B32_UINT;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_UINT;
    default: gpu_stop("vertex input %u: format %d read as integers", loc, (int)f);
    }
}

static int topology(uint32_t mode, VkPrimitiveTopology *t) {
    static const int map[14] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, -1,
                                VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, -1, -1, -1,
                                VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,
                                VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY,
                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY};
    if (mode >= 14 || map[mode] < 0) return 0;
    *t = (VkPrimitiveTopology)map[mode];
    return 1;
}

/* Quads (NVN primitive 7) as triangles: indices 4q+0,1,2 and 4q+0,2,3 for up to QUADS
 * quads, used with the draw's first vertex as the vertex offset. */
enum { QUADS = 65536 };
static VkBuffer quad_indices(void) {
    static VkBuffer b;
    if (!b) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = QUADS * 6 * 4,
                                  .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
        CHECK(vkCreateBuffer(vk.device, &bci, NULL, &b));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk.device, b, &mr);
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
            .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VkDeviceMemory m;
        CHECK(vkAllocateMemory(vk.device, &mai, NULL, &m));
        CHECK(vkBindBufferMemory(vk.device, b, m, 0));
        uint32_t *p;
        CHECK(vkMapMemory(vk.device, m, 0, VK_WHOLE_SIZE, 0, (void **)&p));
        static const uint32_t pattern[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t q = 0; q < QUADS; q++)
            for (int i = 0; i < 6; i++) p[q * 6 + i] = q * 4 + pattern[i];
    }
    return b;
}

/* Debugging a draw's shading: YW4_GPU_DEBUG_FS=<fragment key>=<name>[,...] draws programs
 * with that fragment shader using YW4_SHADER_DIR/../debug/<name>.frag.spv instead (for
 * example "magenta": where the geometry lands whatever the shading does; or one that shows
 * an input). Applies to pipelines made after start-up. */
static VkShaderModule debug_fragment(const Shader *fs) {
    static const char *spec = (const char *)1;
    if (spec == (const char *)1) spec = getenv("YW4_GPU_DEBUG_FS");
    char key[17];
    snprintf(key, sizeof key, "%016" PRIx64, fs->key);
    const char *at = spec ? strstr(spec, key) : NULL;
    if (!at || at[16] != '=') return fs->module;
    char name[64];
    size_t len = strcspn(at + 17, ",");
    snprintf(name, sizeof name, "%.*s", (int)(len < 63 ? len : 63), at + 17);
    char path[1024];
    size_t n;
    snprintf(path, sizeof path, "%s/../debug/%s.frag.spv", shader_dir(), name);
    uint32_t *spv = read_file(path, &n);
    if (!spv) gpu_stop("YW4_GPU_DEBUG_FS needs %s", path);
    VkShaderModuleCreateInfo mci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = spv};
    VkShaderModule m;
    CHECK(vkCreateShaderModule(vk.device, &mci, NULL, &m));
    free(spv);
    fprintf(stderr, "yw4: gpu: fragment shader %s replaced by debug/%s\n", key, name);
    return m;
}

/* Asks the driver for the pipeline of g with state k (through the cache on disk); *took: how long. */
static VkPipeline build_pipeline(Program *g, const PipeKey *k, int64_t *took) {
    VkPipelineShaderStageCreateInfo st[NSETS];
    uint32_t nst = 0;
    for (int i = 0; i < NSETS; i++)
        if (g->sh[i])
            st[nst++] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                          .stage = stage_bits[i],
                                                          .module = k->ours && g->sh[i]->ours ? g->sh[i]->ours
                                                                    : i == ST_FRAG    ? debug_fragment(g->sh[i])
                                                                                      : g->sh[i]->module,
                                                          .pName = "main"};
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = k->nbind,
        .pVertexBindingDescriptions = k->bind, .vertexAttributeDescriptionCount = k->nattr,
        .pVertexAttributeDescriptions = k->attr};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = (VkPrimitiveTopology)k->topology};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .depthClampEnable = k->depth_clamp,
        .polygonMode = k->polygon_mode == 0   ? VK_POLYGON_MODE_POINT
                       : k->polygon_mode == 1 ? VK_POLYGON_MODE_LINE
                                              : VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .maxDepthBounds = 1.0f};
    VkPipelineColorBlendAttachmentState att[8];
    uint32_t ncolor = k->ncolor;
    for (uint32_t i = 0; i < ncolor; i++) {
        const uint8_t *b = k->blend[i];
        int enable = (k->color_state[0] >> i & 1) && k->color[i] && !integer_format(k->color[i]);
        att[i] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = (VkBool32)enable,
            .srcColorBlendFactor = enable ? blend_factor(b[1]) : VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = enable ? blend_factor(b[2]) : VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = enable ? blend_op(b[5]) : VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = enable ? blend_factor(b[3]) : VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = enable ? blend_factor(b[4]) : VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = enable ? blend_op(b[6]) : VK_BLEND_OP_ADD,
            .colorWriteMask = (k->channel_mask >> (4 * i)) & 0xF};
    }
    VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = ncolor, .pAttachments = att};
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    VkFormat depth = k->depth;
    VkPipelineRenderingCreateInfo ri = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = ncolor,
        .pColorAttachmentFormats = k->color,
        .depthAttachmentFormat = depth == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : depth,
        .stencilAttachmentFormat =
            depth == VK_FORMAT_S8_UINT || depth == VK_FORMAT_D24_UNORM_S8_UINT || depth == VK_FORMAT_D32_SFLOAT_S8_UINT
                ? depth
                : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = nst, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cbs, .pDynamicState = &dy,
        .layout = g->layout};
    VkPipeline pl;
    int64_t t0 = host_now_ns();
    CHECK(vkCreateGraphicsPipelines(vk.device, gpu_pcache(), 1, &pci, NULL, &pl));
    *took = host_now_ns() - t0;
    gpu_pcache_built();
    return pl;
}

/* ---- pipelines built in the background (2026-11-08)
 *
 * A pipeline the driver has never compiled costs 40-300 ms on a Radeon (one shader pair has up to four
 * variants, by vertex format and blending: 234 pipelines for 118 pairs in the town). Built in the frame, each
 * is a visible hitch. So when a 3D object's draw (a vertex program with the engine's local-matrix block,
 * Shader.model: redrawn every frame) needs a pipeline that neither this run nor the journal has seen, the
 * build goes to a background thread and the draw is skipped until it is ready: the object shows a few frames
 * late, the very first time only (then the cache on disk has it). Everything else (the UI, full-screen
 * passes, render-to-texture that may be drawn once) still builds in its frame.
 *
 * Off unless YW4_ASYNC_PIPELINES=1 (the owner, 2026-11-12): an object missing for a few frames is worse than a
 * hitch that happens once, and with the cache and the journal's prewarm a pipeline is new only the first time
 * it is ever drawn. Off, every pipeline builds in its frame, as before. */
enum { ASYNC_THREADS = 3, ASYNC_QUEUE = 1024 };
typedef struct {
    Program *g;
    Pipe *p;
} AsyncJob;
static AsyncJob async_q[ASYNC_QUEUE];
static unsigned async_head, async_tail;  /* under async_lock */
static HostLock async_lock;
static HostCond async_cond;
static int async_on = -1;
static uint64_t async_built, async_skipped;

static void async_thread(void *arg) {
    (void)arg;
    for (;;) {
        host_lock(&async_lock);
        while (async_head == async_tail) host_cond_wait(&async_cond, &async_lock, -1);
        AsyncJob j = async_q[async_tail++ % ASYNC_QUEUE];
        host_unlock(&async_lock);
        int64_t took;
        VkPipeline pl = build_pipeline(j.g, &j.p->key, &took);
        journal_add(j.g, &j.p->key);
        __atomic_store_n(&j.p->pipeline, pl, __ATOMIC_RELEASE);
        uint64_t n = __atomic_add_fetch(&async_built, 1, __ATOMIC_RELAXED);
        if (n % 50 == 1)
            fprintf(stderr, "yw4: gpu: %" PRIu64 " new pipelines built in the background (last %lld ms), %" PRIu64
                    " draws waited for one\n", n, (long long)(took / 1000000),
                    __atomic_load_n(&async_skipped, __ATOMIC_RELAXED));
    }
}

static int async_enabled(void) {
    if (async_on < 0) {
        const char *e = getenv("YW4_ASYNC_PIPELINES");
        async_on = e && *e && *e != '0';
        if (async_on) {
            host_lock_init(&async_lock);
            host_cond_init(&async_cond);
            for (int i = 0; i < ASYNC_THREADS; i++)
                if (host_thread_start(async_thread, NULL, 1 << 20)) {
                    if (!i) async_on = 0;
                    break;
                }
        }
    }
    return async_on;
}

static int journal_knows(const Program *g, const PipeKey *k);

/* The pipeline of g with state k. VK_NULL_HANDLE (draw nothing) while a background build of it is running. */
static VkPipeline pipeline(Program *g, const PipeKey *k) {
    for (Pipe *p = g->pipes; p; p = p->next)
        if (!memcmp(&p->key, k, sizeof *k)) {
            VkPipeline pl = __atomic_load_n(&p->pipeline, __ATOMIC_ACQUIRE);
            if (!pl) __atomic_add_fetch(&async_skipped, 1, __ATOMIC_RELAXED);
            return pl;
        }
    Pipe *p = calloc(1, sizeof *p);
    p->key = *k;
    if (!g->warm && g->sh[ST_VERT] && g->sh[ST_VERT]->model && async_enabled() && !journal_knows(g, k)) {
        host_lock(&async_lock);
        int room = async_head - async_tail < ASYNC_QUEUE;
        if (room) {
            async_q[async_head++ % ASYNC_QUEUE] = (AsyncJob){g, p};
            host_cond_signal(&async_cond);
        }
        host_unlock(&async_lock);
        if (room) {
            p->next = g->pipes;  /* listed now, its pipeline 0 until the thread is done */
            g->pipes = p;
            __atomic_add_fetch(&async_skipped, 1, __ATOMIC_RELAXED);
            return VK_NULL_HANDLE;
        }
    }
    int64_t took;
    p->pipeline = build_pipeline(g, k, &took);
    if (!g->warm) {
        journal_add(g, k);
        gpu_stall_add(STALL_PIPE, took);
    }
    if (took > 100000000 && !g->warm)  /* a slow build stalls the frame: say which shaders */
        fprintf(stderr, "yw4: gpu: pipeline took %lld ms (vert %016" PRIx64 ", frag %016" PRIx64 ")\n",
                (long long)(took / 1000000), g->sh[ST_VERT] ? g->sh[ST_VERT]->key : 0,
                g->sh[ST_FRAG] ? g->sh[ST_FRAG]->key : 0);
    p->next = g->pipes;
    g->pipes = p;
    return p->pipeline;
}

/* Pipelines built at start-up, so their first draw doesn't wait for the driver's compiler: the
 * soft-shadow mask (yw4_shadow in recomp/shader.py) builds in ~0.2-0.3 s, a visible stall the
 * first time a scene loads on a cold driver cache. The key is the game's draw (the same in every
 * traced frame: one B10G11R11 target, D32 depth, no blending, quads as a triangle list, three
 * vertex inputs). A background thread builds it while the game boots: the driver's disk cache
 * keeps the result, and the game's own build of it is then ~1 ms. The
 * thread uses its own Shader copies (load_shader), so it never touches the shared list. */
static const struct {
    uint64_t vert, frag;
} prebuilt[] = {{0x98c09b816c2787a2ull, 0x06d03ad703b4bee5ull}};

static void prebuild_thread(void *arg) {
    (void)arg;
    host_thread_set_priority(NULL, 50);  /* background shader warmup: yield to boot/intro playback */
    for (size_t i = 0; i < sizeof prebuilt / sizeof *prebuilt; i++) {
        /* both stages translated (load_shader stops the game on a missing file), else nothing to warm */
        int have = 1;
        for (int st = 0; st < 2; st++) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%016" PRIx64 ".%s.spv", shader_dir(), st ? prebuilt[i].frag : prebuilt[i].vert,
                     st ? "frag" : "vert");
            FILE *f = fopen(path, "rb");
            if (f) fclose(f);
            else have = 0;
        }
        if (!have) continue;
        int64_t t0 = host_now_ns();
        Program g = {0};
        g.warm = 1;
        g.sh[ST_VERT] = load_shader(prebuilt[i].vert, ST_VERT);
        g.sh[ST_FRAG] = load_shader(prebuilt[i].frag, ST_FRAG);
        VkDescriptorSetLayout layouts[2] = {g.sh[ST_VERT]->layout, g.sh[ST_FRAG]->layout};
        VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, 4};
        VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2,
                                          .pSetLayouts = layouts, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
        CHECK(vkCreatePipelineLayout(vk.device, &pli, NULL, &g.layout));
        PipeKey k = {.depth = VK_FORMAT_D32_SFLOAT, .channel_mask = 0xFFFFFFFFu, .polygon_mode = 2,
                     .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .ncolor = 1, .nattr = 3, .nbind = 3};
        k.color[0] = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        k.color_state[1] = 3, k.color_state[2] = 8;
        for (int b = 0; b < 8; b++) {
            static const uint8_t none[8] = {0, 2, 1, 2, 1, 1, 1, 0};  /* ONE, ZERO, ONE, ZERO, ADD, ADD */
            memcpy(k.blend[b], none, 8);
        }
        k.attr[0] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
        k.attr[1] = (VkVertexInputAttributeDescription){1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 24};
        k.attr[2] = (VkVertexInputAttributeDescription){2, 2, VK_FORMAT_R32G32_SFLOAT, 16};
        for (uint32_t b = 0; b < 3; b++) k.bind[b] = (VkVertexInputBindingDescription){b, 40, VK_VERTEX_INPUT_RATE_VERTEX};
        pipeline(&g, &k);  /* kept, like every pipeline: a few small objects, once */
        if (g.sh[ST_FRAG]->ours) k.ours = 1, pipeline(&g, &k);  /* and with our program (the shadow mask's) */
        fprintf(stderr, "yw4: gpu: prebuilt pipeline (frag %016" PRIx64 ") in %lld ms\n", prebuilt[i].frag,
                (long long)((host_now_ns() - t0) / 1000000));
    }
}

/* ---- the pipeline journal and the prewarm (2026-11-08)
 *
 * The cache on disk (gpu_pcache.c) makes a pipeline built in an earlier run cost ~1 ms instead of 40-300 ms
 * on a Radeon, but the game still asks for each one in the middle of a frame: 117 at once when the town first
 * appears. So every pipeline the game builds is also written to a journal, work/pipelines.journal: its stage
 * programs (by key) and its PipeKey. At start-up, while the game boots and shows its title, background
 * threads build every pipeline in the journal into the cache; when the game then asks for one, it is a cache
 * hit. A pipeline never seen before still compiles in its frame, once: from the next run on it is warm.
 *
 * Records are written only for new pipelines (a hash set of what the file already holds), appended and
 * flushed one by one so a crash keeps what came before. A file from another build (another PipeKey size) is
 * set aside as .old and a new one started; one from another GPU, driver or set of shaders is deleted with the
 * cache (gpu_pcache.c, decision 0063). YW4_PIPELINE_JOURNAL=<file> moves it, =none turns it off. */
typedef struct {
    uint64_t key[NSETS];
    PipeKey k;
} JournalRec;

static const char JOURNAL_MAGIC[8] = {'Y', 'W', '4', 'P', 'I', 'P', 'E', '1'};
static FILE *journal;
static HostLock journal_lock;
static uint64_t *journal_set;  /* open addressing; 0 = empty */
static size_t journal_cap, journal_count;
static JournalRec *warm_recs;
static size_t warm_n, warm_next;

static uint64_t rec_hash(const JournalRec *r) {
    uint64_t h = 1469598103934665603ull;
    const unsigned char *p = (const unsigned char *)r;
    for (size_t i = 0; i < sizeof *r; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h ? h : 1;
}

/* 1 if h was new (now in the set) */
static int set_add(uint64_t h) {
    if (2 * (journal_count + 1) > journal_cap) {
        size_t cap = journal_cap ? 2 * journal_cap : 4096;
        uint64_t *n = calloc(cap, sizeof *n);
        if (!n) return 0;
        for (size_t i = 0; i < journal_cap; i++)
            if (journal_set[i]) {
                size_t j = journal_set[i] & (cap - 1);
                while (n[j]) j = (j + 1) & (cap - 1);
                n[j] = journal_set[i];
            }
        free(journal_set);
        journal_set = n, journal_cap = cap;
    }
    size_t j = h & (journal_cap - 1);
    while (journal_set[j]) {
        if (journal_set[j] == h) return 0;
        j = (j + 1) & (journal_cap - 1);
    }
    journal_set[j] = h;
    journal_count++;
    return 1;
}

static void journal_rec(JournalRec *r, const Program *g, const PipeKey *k) {
    memset(r, 0, sizeof *r);
    for (int i = 0; i < NSETS; i++) r->key[i] = g->sh[i] ? g->sh[i]->key : 0;
    r->k = *k;
}

/* 1 if this pipeline was built in an earlier run (it's in the journal, so in the cache on disk) or this one */
static int journal_knows(const Program *g, const PipeKey *k) {
    if (!journal) return 0;
    JournalRec r;
    journal_rec(&r, g, k);
    uint64_t h = rec_hash(&r);
    host_lock(&journal_lock);
    int found = 0;
    if (journal_cap) {
        size_t j = h & (journal_cap - 1);
        while (journal_set[j] && !found) {
            found = journal_set[j] == h;
            j = (j + 1) & (journal_cap - 1);
        }
    }
    host_unlock(&journal_lock);
    return found;
}

static void journal_add(const Program *g, const PipeKey *k) {
    if (!journal) return;
    JournalRec r;
    journal_rec(&r, g, k);
    host_lock(&journal_lock);
    if (set_add(rec_hash(&r)) && fwrite(&r, sizeof r, 1, journal) == 1) fflush(journal);
    host_unlock(&journal_lock);
}

/* Reads the journal into warm_recs and opens it for appending. */
static void journal_open(void) {
    const char *path = gpu_pcache_journal_path();  /* gpu_pcache.c: a stale one is gone by now */
    if (!path) return;
    host_lock_init(&journal_lock);
    uint32_t want = (uint32_t)sizeof(JournalRec);
    FILE *f = fopen(path, "rb");
    if (f) {
        char magic[8];
        uint32_t size = 0;
        if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, JOURNAL_MAGIC, 8) && fread(&size, 4, 1, f) == 1 &&
            size == want) {
            JournalRec r;
            size_t cap = 0;
            while (fread(&r, sizeof r, 1, f) == 1) {
                if (!set_add(rec_hash(&r))) continue;
                if (warm_n == cap) {
                    cap = cap ? 2 * cap : 1024;
                    JournalRec *n = realloc(warm_recs, cap * sizeof *n);
                    if (!n) break;
                    warm_recs = n;
                }
                warm_recs[warm_n++] = r;
            }
            fclose(f);
        } else {  /* another build's: kept beside, a new one started */
            fclose(f);
            char old[1040];
            snprintf(old, sizeof old, "%s.old", path);
            remove(old);
            rename(path, old);
        }
    }
    int fresh = !warm_n && journal_count == 0;
    journal = fopen(path, fresh ? "wb" : "ab");
    if (journal && fresh) {
        fwrite(JOURNAL_MAGIC, 1, 8, journal);
        fwrite(&want, 4, 1, journal);
        fflush(journal);
    }
    fprintf(stderr, "yw4: gpu: pipeline journal %s: %zu pipelines to prewarm\n", journal ? path : "off", warm_n);
}

static int shader_file(uint64_t key, int stage) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%016" PRIx64 ".%s.spv", shader_dir(), key, stage_names[stage]);
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void free_shader(Shader *s) {
    if (!s) return;
    vkDestroyShaderModule(vk.device, s->module, NULL);
    if (s->ours) vkDestroyShaderModule(vk.device, s->ours, NULL);
    vkDestroyDescriptorSetLayout(vk.device, s->layout, NULL);
    free(s);
}

static void preload_thread(void *arg) {
    (void)arg;
    host_thread_set_priority(NULL, 50);  /* as prebuild_thread: below the game's boot and intro playback (232829c) */
    for (;;) {
        size_t i = __atomic_fetch_add(&pre_next, 1, __ATOMIC_RELAXED);
        if (i >= pre_n) break;
        PreSlot *p = &pre[pre_order[i]];
        if (__atomic_load_n(&p->s, __ATOMIC_ACQUIRE)) continue;  /* the GPU thread got there first */
        Shader *s = load_shader(p->key, p->stage), *expect = NULL;
        if (!__atomic_compare_exchange_n(&p->s, &expect, s, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) free_shader(s);
    }
    if (__atomic_sub_fetch(&pre_left, 1, __ATOMIC_ACQ_REL) == 0)
        fprintf(stderr, "yw4: gpu: preloaded %zu shaders in %lld ms\n", pre_n,
                (long long)((host_now_ns() - pre_t0) / 1000000));
}

/* The slots from the shader folder's *.spv names (<16 hex>.<stage>.spv) that have their interface file too
 * (load_shader stops the game without one), then the threads. */
static void preload_start(void) {
    const char *e = getenv("YW4_SHADER_PRELOAD");
    if (e && *e == '0') return;
    DIR *d = opendir(shader_dir());
    if (!d) return;
    pre = calloc(PRE_CAP, sizeof *pre);
    pre_order = malloc(PRE_CAP * sizeof *pre_order);
    if (!pre || !pre_order) {
        closedir(d);
        free(pre), free(pre_order), pre = NULL;
        return;
    }
    for (struct dirent *de; (de = readdir(d)) != NULL && pre_n < PRE_CAP * 3 / 4;) {
        const char *n = de->d_name;
        size_t len = strlen(n);
        if (len < 22 || strcmp(n + len - 4, ".spv") || n[16] != '.') continue;
        char hex[17];
        memcpy(hex, n, 16), hex[16] = 0;
        char *end;
        uint64_t key = strtoull(hex, &end, 16);
        if (*end || !key) continue;
        int stage = -1;
        for (int st = 0; st < 4; st++)
            if (len == 17 + strlen(stage_names[st]) + 4 && !strncmp(n + 17, stage_names[st], strlen(stage_names[st])))
                stage = st;
        if (stage < 0 || pre_find(key, stage)) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%016" PRIx64 ".iface", shader_dir(), key);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fclose(f);
        size_t i = pre_hash(key, stage);
        while (pre[i].key) i = (i + 1) & (PRE_CAP - 1);
        pre[i].key = key, pre[i].stage = stage;
        pre_order[pre_n++] = (uint32_t)i;
    }
    closedir(d);
    if (!pre_n) return;
    /* 4 threads: the 3D's own threads use about two cores, the 5700X3D's class of CPU has 16 */
    int n = host_cpu_count() / 4;
    if (n > 4) n = 4;
    if (n < 1) n = 1;
    pre_t0 = host_now_ns();
    pre_left = n;
    for (int i = 0; i < n; i++)
        if (host_thread_start(preload_thread, NULL, 1 << 20)) {
            fprintf(stderr, "yw4: gpu: cannot start a shader preload thread\n");
            if (__atomic_sub_fetch(&pre_left, 1, __ATOMIC_ACQ_REL) == 0) break;
        }
}

static int64_t warm_t0;
static int warm_threads_left;

/* One prewarm thread: takes journal records in order and builds each into the cache; the pipelines and their
 * shaders are destroyed again (the cache keeps the driver's result). */
static void warm_thread(void *arg) {
    (void)arg;
    host_thread_set_priority(NULL, 50);  /* as prebuild_thread: below the game's boot and intro playback (232829c) */
    for (;;) {
        size_t i = __atomic_fetch_add(&warm_next, 1, __ATOMIC_RELAXED);
        if (i >= warm_n) break;
        const JournalRec *r = &warm_recs[i];
        int ok = r->key[ST_VERT] != 0;
        for (int st = 0; st < NSETS && ok; st++)
            if (r->key[st] && !shader_file(r->key[st], st)) ok = 0;
        if (!ok) continue;
        Program g = {0};
        g.warm = 1;
        VkDescriptorSetLayout layouts[NSETS];
        for (int st = 0; st < NSETS; st++) {
            if (r->key[st]) g.sh[st] = load_shader(r->key[st], st);
            layouts[st] = g.sh[st] ? g.sh[st]->layout : empty_layout();
        }
        VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, 4};
        VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount = g.sh[ST_GEOM] ? 3u : 2u, .pSetLayouts = layouts,
                                          .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
        CHECK(vkCreatePipelineLayout(vk.device, &pli, NULL, &g.layout));
        if (!r->k.ours || (g.sh[ST_VERT] && g.sh[ST_VERT]->ours) || (g.sh[ST_FRAG] && g.sh[ST_FRAG]->ours))
            pipeline(&g, &r->k);
        for (Pipe *p = g.pipes, *next; p; p = next) {
            next = p->next;
            vkDestroyPipeline(vk.device, p->pipeline, NULL);
            free(p);
        }
        vkDestroyPipelineLayout(vk.device, g.layout, NULL);
        for (int st = 0; st < NSETS; st++) free_shader(g.sh[st]);
    }
    if (__atomic_sub_fetch(&warm_threads_left, 1, __ATOMIC_ACQ_REL) == 0) {
        fprintf(stderr, "yw4: gpu: prewarmed %zu pipelines in %lld ms\n", warm_n,
                (long long)((host_now_ns() - warm_t0) / 1000000));
        gpu_pcache_save();
    }
}

void gpu_prebuild_start(void) {
    const char *e = getenv("YW4_PREBUILD");
    if (e && *e == '0') return;
    if (host_thread_start(prebuild_thread, NULL, 1 << 20))
        fprintf(stderr, "yw4: gpu: cannot start the pipeline prebuild thread\n");
    journal_open();
    preload_start();
    if (!warm_n) return;
    empty_layout();  /* made once here, before the threads share it */
    int n = host_cpu_count() / 2;
    if (n > 6) n = 6;
    if (n < 1) n = 1;
    if ((size_t)n > warm_n) n = (int)warm_n;
    warm_t0 = host_now_ns();
    warm_threads_left = n;
    for (int i = 0; i < n; i++)
        if (host_thread_start(warm_thread, NULL, 1 << 20)) {
            fprintf(stderr, "yw4: gpu: cannot start a pipeline prewarm thread\n");
            if (__atomic_sub_fetch(&warm_threads_left, 1, __ATOMIC_ACQ_REL) == 0) break;
        }
}

/* ================================================================== state and rendering */

void gpu_state_init(GpuState *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 8; i++) {
        static const uint8_t d[8] = {0, 2, 1, 2, 1, 1, 1, 0};  /* ONE, ZERO, ONE, ZERO, ADD, ADD */
        memcpy(s->blend[i], d, 8);
        s->blend[i][0] = (uint8_t)i;
    }
    static const uint8_t color[4] = {0, 3, 8, 0}, ds[8] = {0, 1, 2, 0, 0x88, 0x11, 0x11, 0x11},
                         polygon[4] = {0, 1, 2, 0};
    memcpy(s->color, color, 4);
    memcpy(s->depth_stencil, ds, 8);
    memcpy(s->polygon, polygon, 4);
    s->channel_mask = 0xFFFFFFFFu;
    s->depth_range[1] = 1.0f;
    s->stencil_mask[0] = s->stencil_mask[1] = s->stencil_value_mask[0] = s->stencil_value_mask[1] = 0xFF;
}

void gpu_end_rendering(VkCommandBuffer cb, GpuState *s) {
    if (!s->rendering) return;
    gpu_passes_note(1);
    vkCmdEndRendering(cb);
    full_barrier(cb);
    s->rendering = 0;
}

/* The attachments of s->rt; begins rendering into them unless already. Fills the formats. */
/* 0 if no render target is set (nothing to draw into). */
static int begin_rendering(VkCommandBuffer cb, GpuState *s, VkFormat color[8], VkFormat *depth, uint32_t *w,
                           uint32_t *h) {
    VkRenderingAttachmentInfo ca[8] = {0}, da = {0};
    uint32_t aw = UINT32_MAX, ah = UINT32_MAX, n = (uint32_t)s->ncolor;
    for (uint32_t i = 0; i < n; i++) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!s->rt.color[i]) continue;
        if (s->after_composite) gpu_upscale_also(s->rt.color[i]);  /* upscaling: the UI's images are output-sized */
        uint32_t tw, th;
        ca[i].imageView = gpu_render_view(cb, s->rt.color[i], &s->rt.views[i], s->rt.has_view[i], &tw, &th);
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        GpuImage *g = image_find(s->rt.color[i]);
        const NvnTextureView *v = &s->rt.views[i];
        color[i] = s->rt.has_view[i] && v->setmask & 4 ? gpu_format(v->format) : g->format;
        if (tw < aw) aw = tw;
        if (th < ah) ah = th;
    }
    *depth = VK_FORMAT_UNDEFINED;
    /* upscaling: the UI draws into the output-sized scene image; its depth is output-sized too (gpu_aa.c
     * stretches the 3D's into it). Should it not be, a smaller depth would shrink the render area: left out */
    if (s->rt.depth && !(n && gpu_upscaled(s->rt.color[0]) && !gpu_upscaled(s->rt.depth))) {
        uint32_t tw, th;
        da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        da.imageView = gpu_render_view(cb, s->rt.depth, &s->rt.depth_view, s->rt.has_depth_view, &tw, &th);
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        *depth = image_find(s->rt.depth)->format;
        if (tw < aw) aw = tw;
        if (th < ah) ah = th;
    }
    if (aw == UINT32_MAX) return 0;
    *w = aw, *h = ah;
    if (s->rendering) return 1;
    int has_depth = *depth != VK_FORMAT_UNDEFINED && *depth != VK_FORMAT_S8_UINT;
    int has_stencil = *depth == VK_FORMAT_S8_UINT || *depth == VK_FORMAT_D24_UNORM_S8_UINT ||
                      *depth == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {aw, ah}}, .layerCount = 1,
                          .colorAttachmentCount = n, .pColorAttachments = ca,
                          .pDepthAttachment = has_depth ? &da : NULL, .pStencilAttachment = has_stencil ? &da : NULL};
    vkCmdBeginRendering(cb, &ri);
    s->rendering = 1;
    return 1;
}

int gpu_begin_targets(VkCommandBuffer cb, GpuState *s, VkFormat color[8], VkFormat *depth, uint32_t *w, uint32_t *h) {
    if (!s->rendering) {  /* images first: made or uploaded outside rendering */
        for (int i = 0; i < s->ncolor; i++)
            if (s->rt.color[i]) image_of(cb, s->rt.color[i]);
        if (s->rt.depth) image_of(cb, s->rt.depth);
    }
    return begin_rendering(cb, s, color, depth, w, h);
}

/* The texture and sampler a handle names in the current pools. */
/* A takeover draw's textures (gpu_takeover_draw): handles with the top bit set name its copies. */
static _Thread_local NvnTexEntry *own_tex;

static NvnTexEntry *tex_entry(const GpuState *s, uint64_t handle) {
    if (handle >> 63) return own_tex && own_tex[handle & 0xFF].tex ? &own_tex[handle & 0xFF] : NULL;
    uint32_t id = (uint32_t)(handle & 0xFFFFF);
    if (!s->tex_pool || !handle || (int)id >= s->tex_pool->count) return NULL;
    NvnTexEntry *e = (NvnTexEntry *)s->tex_pool->entries + id;
    return e->tex ? e : NULL;
}

static const NvnSamplerDesc *sampler_entry(const GpuState *s, uint64_t handle) {
    if (handle >> 63) return NULL;  /* a takeover draw's: its own sampler settings (bind_stage_set) */
    uint32_t id = (uint32_t)(handle >> 20 & 0xFFF);
    if (!s->sampler_pool || (int)id >= s->sampler_pool->count) return NULL;
    return (const NvnSamplerDesc *)s->sampler_pool->entries + id;
}

/* The sampler settings a handle names in the bound sampler pool (diagnostics); 0 if none. */
int gpu_handle_sampler(const GpuState *s, uint64_t handle, NvnSamplerDesc *out) {
    const NvnSamplerDesc *sd = sampler_entry(s, handle);
    if (!sd) return 0;
    *out = *sd;
    return 1;
}

int gpu_handle_texture(VkCommandBuffer cb, const GpuState *s, uint64_t handle, int upload, VkDescriptorImageInfo *ii) {
    NvnTexEntry *e = tex_entry(s, handle);
    if (!e) return 0;
    if (upload) {
        if (gpu_needs_transfer(e->tex)) image_of(cb, e->tex);
        return 1;
    }
    /* as bind_stage_set binds a 2D texture */
    const NvnSamplerDesc *sd = sampler_entry(s, handle);
    NvnSamplerDesc d = sd ? *sd : (NvnSamplerDesc){.min_filter = 1, .mag_filter = 1, .lod_max = 1000};
    d.compare_mode = 0;
    ii->imageView = gpu_sample_view(cb, e->tex, &e->view, e->has_view, VK_IMAGE_VIEW_TYPE_2D);
    gpu_sampler_scene = !s->after_composite;  /* upscaling's mip bias: the 3D's, not the UI's */
    ii->sampler = gpu_sampler(&d);
    gpu_sampler_scene = 0;
    ii->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    return 1;
}

static VkBuffer dummy_buffer(void) {  /* 64 KiB of zeros for unbound constant buffers */
    static VkBuffer b;
    if (!b) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536,
                                  .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        CHECK(vkCreateBuffer(vk.device, &bci, NULL, &b));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk.device, b, &mr);
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
            .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VkDeviceMemory m;
        CHECK(vkAllocateMemory(vk.device, &mai, NULL, &m));
        CHECK(vkBindBufferMemory(vk.device, b, m, 0));
        void *p;
        CHECK(vkMapMemory(vk.device, m, 0, VK_WHOLE_SIZE, 0, &p));
        memset(p, 0, 65536);
    }
    return b;
}

/* Does drawing with g need texture uploads or new images? Those are copies, which can't
 * happen inside rendering. */
static int needs_transfers(const GpuState *s, const Program *g) {
    for (int st = 0; st < NSETS; st++) {
        const Shader *sh = g->sh[st];
        for (int i = 0; sh && i < sh->nb; i++) {
            if (sh->b[i].kind != 't' || sh->b[i].slot >= TEXTURES) continue;
            NvnTexEntry *e = tex_entry(s, s->tex[nvn_stage[st]][sh->b[i].slot]);
            if (e && gpu_needs_transfer(e->tex)) return 1;
        }
    }
    return 0;
}

/* Writes and binds the descriptor set of one shader stage (ns: its NVN stage number) at set `setno`.
 * 0 if the shader needs something not supported yet (storage images). */
/* Shadow quality (gpu.c, shadow map size): with shadow maps larger than the game's, a shader that samples one
 * gets cbuf 8 from a copy whose filter spread (2 floats at pcf_float) is scaled by 1024 / size, so
 * its 5 taps stay as many texels apart as the game's and edges get sharper with the map. Copies
 * go into a host-visible ring, reused in turn (16 MiB: many frames of these draws). The game's
 * memory is not touched. 0: not wanted (1024) or not possible; bind the game's buffer. */
enum { PCF_RING = 16 << 20 };
static VkBuffer pcf_buf;
static VkDeviceMemory pcf_mem;
static uint8_t *pcf_ptr;
static VkDeviceSize pcf_at;

/* `size` bytes of the ring for one draw's uniform data; NULL if the ring can't be made. */
static uint8_t *ring_alloc(VkDeviceSize size, VkBuffer *buf, VkDeviceSize *offset) {
    if (!pcf_buf) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = PCF_RING,
                                  .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        if (vkCreateBuffer(vk.device, &bci, NULL, &pcf_buf) != VK_SUCCESS) return NULL;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk.device, pcf_buf, &mr);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                    .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits,
                                                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        if (vkAllocateMemory(vk.device, &mai, NULL, &pcf_mem) != VK_SUCCESS) return NULL;
        vkBindBufferMemory(vk.device, pcf_buf, pcf_mem, 0);
        vkMapMemory(vk.device, pcf_mem, 0, VK_WHOLE_SIZE, 0, (void **)&pcf_ptr);
    }
    VkDeviceSize align = vk.props.limits.minUniformBufferOffsetAlignment;
    VkDeviceSize at = (pcf_at + align - 1) / align * align;
    if (at + size > PCF_RING) at = 0;
    pcf_at = at + size;
    *buf = pcf_buf, *offset = at;
    return pcf_ptr + at;
}

/* The same for the screen-sized targets (YW4_RES, F4): cbuf 8 row 32 (tap offsets in them, 0..1
 * coordinates: x, y of the full size, x, y of the third) scaled by 900 / height, and the x ones also by the
 * picture's width factor (widescreen.c), so the taps stay as many pixels apart as the game's. */
/* And with the anti-aliasing's shift (gpu_taa_clip_jitter): a full-screen pass before the composite that works
 * out positions from the depth (rows 20-23, the inverse projection, dotted with the pixel's clip position) gets
 * them with this frame's shift taken out, as if from clip - shift: row.w -= row.x * shift.x + row.y * shift.y.
 * Its result then sits where the shifted 3D does. */
static int bind_unjitter;  /* the draw being bound is such a pass (draw(), before bind_sets) */

static int cb8_scaled_copy(uint64_t addr, VkDeviceSize range, const Shader *sh, VkBuffer *buf, VkDeviceSize *offset) {
    int res = gpu_shadow_res(), h = gpu_render_height();  /* the lit pass reads the 3D's images */
    uint32_t wn, wd;
    gpu_wide_factor(&wn, &wd);
    int pcf = sh->pcf_float >= 0 && res != 1024 && (VkDeviceSize)(sh->pcf_float + 2) * 4 <= range;
    int taps = sh->screen_taps && (h != 900 || wn != wd) && 33 * 16 <= range;
    float cx = 0, cy = 0;
    int unjitter = sh->inv_proj && bind_unjitter && 24 * 16 <= range && gpu_taa_clip_jitter(&cx, &cy);
    if (!(pcf || taps || unjitter) || range > 65536) return 0;
    uint8_t *p = ring_alloc(range, buf, offset);
    if (!p) return 0;
    memcpy(p, (const void *)(uintptr_t)addr, (size_t)range);
    if (pcf) {
        float *spread = (float *)p + sh->pcf_float;
        spread[0] *= 1024.0f / (float)res, spread[1] *= 1024.0f / (float)res;
    }
    if (taps)
        for (int i = 0; i < 4; i++) ((float *)p)[32 * 4 + i] *= 900.0f / (float)h * (i & 1 ? 1.0f : (float)wd / (float)wn);
    if (unjitter)
        for (int row = 20; row < 24; row++) {
            float *m = (float *)p + row * 4;
            m[3] -= m[0] * cx + m[1] * cy;
        }
    return 1;
}

/* Soft shadows (PCSS, yw4_shadow in recomp/shader.py): off, or on with a strength. x2 by default
 * (the owner's pick, with 4096 maps); the settings panel (F1) changes it, YW4_PCSS=0..3 sets it at start. The settings go to each draw from the ring:
 *   mode  = on, (unused), largest radius, smallest radius
 *   scale = radius per unit of depth gap for cascades 0..3 (YW4_PCSS_SCALE=a,b,c,d to try others)
 *   tune  = smallest depth gap that counts as a caster (YW4_PCSS_EPS)
 * Radii are in map coordinates (0..1), so they don't change with the map's size; the smallest
 * is half a texel of the current map, the game's own filter spread. */
static int pcss_level = -1;
static float pcss_scale[4] = {0.01f, 0.005f, 0.0025f, 0.0025f}, pcss_eps, pcss_max = 6.0f / 1024.0f;
static int pcss_debug;  /* YW4_PCSS_DEBUG: 1 shows casters found / 16, 2 the radius / largest (white = 1) */
static int pcss_search_once = 1;  /* YW4_PCSS_SEARCH=each: a blocker search for every lookup, as before */
/* YW4_PCSS_ALTERNATE=<frame>: for 16 presented frames from that one, odd frames search for every lookup and even
 * frames once, to compare the two in one run (with YW4_CAPTURE_FS=06d03ad7) */
static long long pcss_alternate = -1;
static float pcss_block = 2.0f;  /* YW4_PCSS_BLOCK=<n>: n x n pixels share one disk rotation (1: each pixel its own) */

int gpu_pcss_level(void) {
    int l = __atomic_load_n(&pcss_level, __ATOMIC_ACQUIRE);
    if (l < 0) {
        const char *e = getenv("YW4_PCSS");
        l = e ? atoi(e) : 2;
        if (l < 0 || l > 3) l = 2;
        if ((e = getenv("YW4_PCSS_SCALE")) != NULL)
            sscanf(e, "%f,%f,%f,%f", &pcss_scale[0], &pcss_scale[1], &pcss_scale[2], &pcss_scale[3]);
        if ((e = getenv("YW4_PCSS_EPS")) != NULL) pcss_eps = (float)atof(e);
        if ((e = getenv("YW4_PCSS_MAX")) != NULL) pcss_max = (float)atof(e) / 1024.0f;
        if ((e = getenv("YW4_PCSS_DEBUG")) != NULL) pcss_debug = atoi(e);
        if ((e = getenv("YW4_PCSS_SEARCH")) != NULL) pcss_search_once = strcmp(e, "each") != 0;
        if ((e = getenv("YW4_PCSS_ALTERNATE")) != NULL) pcss_alternate = atoll(e);
        if ((e = getenv("YW4_PCSS_BLOCK")) != NULL) pcss_block = (float)atof(e);
        __atomic_store_n(&pcss_level, l, __ATOMIC_RELEASE);
    }
    return l;
}

void gpu_pcss_cycle(void) {
    int l = (gpu_pcss_level() + 1) % 4;
    __atomic_store_n(&pcss_level, l, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: PCSS %s\n", l ? (l == 1 ? "x1" : l == 2 ? "x2" : "x4") : "off");
}

/* Speed-ups of our shadow mask's soft shadows (shadow_mask.frag; the panel's SPEED-UPS rows):
 * - filter once: one soft lookup at the centre instead of at each of the game's five points (default;
 *   YW4_MASK_FILTER=five for the game's)
 * - search 8: the blocker search with every other one of its 16 taps (YW4_PCSS_SEARCH_TAPS=8)
 * - filter 8: the soft filter with every other one of its 16 taps, the disk turned a golden angle each frame so
 *   the anti-aliasing's frame blending (TAA, FSR, DLSS) averages the taps over frames
 *   (YW4_PCSS_FILTER_TAPS=8)
 * They go to the program in mode.y: bit 0 filter once, bit 1 search 8, bit 2 filter 8; bits 3 up the frame
 * (mod 256) for the turn. */
static int mask_once = -1, search8 = -1, filter8 = -1;

int gpu_pcss_filter8(void) {
    int v = __atomic_load_n(&filter8, __ATOMIC_ACQUIRE);
    if (v < 0) {
        const char *e = getenv("YW4_PCSS_FILTER_TAPS");
        v = e && atoi(e) == 8;
        __atomic_store_n(&filter8, v, __ATOMIC_RELEASE);
    }
    return v;
}

void gpu_pcss_filter8_toggle(void) {
    int v = !gpu_pcss_filter8();
    __atomic_store_n(&filter8, v, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: soft shadow filter %d taps\n", v ? 8 : 16);
}

int gpu_mask_filter_once(void) {
    int v = __atomic_load_n(&mask_once, __ATOMIC_ACQUIRE);
    if (v < 0) {
        const char *e = getenv("YW4_MASK_FILTER");
        v = !(e && !strcmp(e, "five"));  /* once by default: the owner saw no difference (2026-10-31) */
        __atomic_store_n(&mask_once, v, __ATOMIC_RELEASE);
    }
    return v;
}

void gpu_mask_filter_toggle(void) {
    int v = !gpu_mask_filter_once();
    __atomic_store_n(&mask_once, v, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: shadow mask soft filter %s\n", v ?"once per pixel" : "at the game's five points");
}

int gpu_pcss_search8(void) {
    int v = __atomic_load_n(&search8, __ATOMIC_ACQUIRE);
    if (v < 0) {
        const char *e = getenv("YW4_PCSS_SEARCH_TAPS");
        v = e && atoi(e) == 8;
        __atomic_store_n(&search8, v, __ATOMIC_RELEASE);
    }
    return v;
}

void gpu_pcss_search8_toggle(void) {
    int v = !gpu_pcss_search8();
    __atomic_store_n(&search8, v, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: soft shadow search %d taps\n", v ? 8 : 16);
}

/* Contact shadows in our shadow mask (shadow_mask.frag contact_shadow): a short march toward the sun across the
 * half-size depth, for the small shading the shadow maps are too coarse for (inside bushes, under ledges and
 * eaves). The settings panel's CONTACT SHADOWS row switches them while playing. YW4_CONTACT=0 / 1 at start (on
 * by default), or YW4_CONTACT=<steps>,<length>,<thickness>[,<debug>] to try other values (length and thickness in
 * view units, about metres; debug 1 shows the contact shadows alone). Decision 0045.
 * YW4_CONTACT_ALTERNATE=1: off on even GPU frames, to compare both in one run (the sun follows the time of day,
 * so two runs differ anyway). */
static float contact[4] = {12, 0.3f, 0.07f, 0};  /* steps x4 in effect (shared in 2x2 blocks); thickness widened for slanted faces */
static int contact_on = -1, contact_alternate;

int gpu_contact(void) {
    int v = __atomic_load_n(&contact_on, __ATOMIC_ACQUIRE);
    if (v < 0) {
        const char *e = getenv("YW4_CONTACT");
        v = 1;
        if (e && strchr(e, ',')) {
            sscanf(e, "%f,%f,%f,%f", &contact[0], &contact[1], &contact[2], &contact[3]);
            v = contact[0] > 0;
        } else if (e) {
            v = atoi(e) != 0;
        }
        contact_alternate = (e = getenv("YW4_CONTACT_ALTERNATE")) != NULL && atoi(e);
        fprintf(stderr, "yw4: gpu: contact shadows %s: %g steps, length %g, thickness %g%s\n", v ? "on" : "off",
                contact[0], contact[1], contact[2], contact[3] == 1 ? ", shown alone" : "");
        __atomic_store_n(&contact_on, v, __ATOMIC_RELEASE);
    }
    return v;
}

void gpu_contact_toggle(void) {
    int v = !gpu_contact();
    __atomic_store_n(&contact_on, v, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: contact shadows %s (settings panel)\n", v ? "on" : "off");
}

static void contact_settings(float *f) {
    memcpy(f, contact, sizeof contact);
    if (!gpu_contact() || (contact_alternate && !(gpu_frames() & 1))) f[0] = 0;
}

/* YW4_MASK_BREAKDOWN=1: what the shadow mask's parts cost, in one run. Frame by frame the mask takes turns
 * in four forms (as set: soft shadows and contact shadows as chosen; no contact shadows; no soft shadows;
 * neither), and the per-pass GPU timer (F5, YW4_GPU_PASSES) has a line for each (gpu.c marks around the
 * draw): its ms divided by its passes per frame is that form's cost. The picture flickers meanwhile. */
static const char *const mask_forms[4] = {"shadow mask: as set", "shadow mask: no contact shadows",
                                          "shadow mask: no soft shadows", "shadow mask: neither"};

static int mask_breakdown(void) {
    static int on = -1;
    if (on < 0) on = getenv("YW4_MASK_BREAKDOWN") && atoi(getenv("YW4_MASK_BREAKDOWN"));
    return on;
}

/* gpu.c, at a draw: the timer's label for this one if it is the shadow mask in a breakdown, else NULL. */
const char *gpu_mask_breakdown_label(const GpuState *s) {
    if (!mask_breakdown() || !s->program || s->program->stages[1].key != SHADOW_MASK_FS) return NULL;
    return mask_forms[gpu_frames() & 3];
}

static void pcss_settings_fill(float *f);

static int pcss_settings(VkBuffer *buf, VkDeviceSize *offset) {
    float *f = (float *)ring_alloc(64, buf, offset);
    if (!f) return 0;
    pcss_settings_fill(f);
    if (mask_breakdown()) {  /* this frame's form: bit 0 no contact shadows, bit 1 no soft shadows */
        int form = (int)(gpu_frames() & 3);
        if (form & 1) f[12] = 0;
        if (form & 2) f[0] = 0;
    }
    return 1;
}

static void pcss_settings_fill(float *f) {
    int l = gpu_pcss_level();
    float k = (float)(1 << (l ? l - 1 : 0));
    f[0] = l ? 1.0f : 0.0f, f[2] = pcss_max, f[3] = 0.5f / (float)gpu_shadow_res();
    f[1] = (float)(gpu_mask_filter_once() | gpu_pcss_search8() << 1 | gpu_pcss_filter8() << 2 | (int)(gpu_frames() & 255) << 3);
    for (int i = 0; i < 4; i++) f[4 + i] = pcss_scale[i] * k;
    f[8] = pcss_eps, f[9] = (float)pcss_debug;
    int once = pcss_search_once;
    uint64_t fr = gpu_frames();
    if (pcss_alternate >= 0 && fr >= (uint64_t)pcss_alternate && fr < (uint64_t)pcss_alternate + 16) once = !(fr & 1);
    f[10] = (float)once, f[11] = pcss_block;
    contact_settings(f + 12);
}

/* A draw of our G-buffer pass (gbuffer.h): its own vertex and index buffers and vertex constants 2
 * in place of the bound ones. */
static _Thread_local const GBufferDraw *own;
/* ... and a takeover draw (decision 0030): also its constant blocks and samplers */
static _Thread_local const TakeoverDraw *own_t;

static int bind_stage_set(VkCommandBuffer cb, const GpuState *s, const Shader *sh, int ns, VkPipelineBindPoint bp,
                          VkPipelineLayout layout, uint32_t setno) {
    {
        VkDescriptorSet set = gpu_alloc_set(sh->layout);
        VkWriteDescriptorSet w[MAX_BINDINGS];
        VkDescriptorBufferInfo bi[MAX_BINDINGS];
        VkDescriptorImageInfo ii[MAX_BINDINGS];
        for (int i = 0; i < sh->nb; i++) {
            const Binding *b = &sh->b[i];
            w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                          .descriptorCount = 1};
            if (b->kind == 'c' || b->kind == 's') {
                GpuRange r = b->kind == 'c' ? (b->slot < UBOS ? s->ubo[ns][b->slot] : (GpuRange){0})
                                            : (b->slot < SSBOS ? s->ssbo[ns][b->slot] : (GpuRange){0});
                VkDeviceSize range = b->kind == 'c' && r.size > 65536 ? 65536 : r.size;
                uint8_t *p6;
                int taken = 0;
                for (uint32_t u = 0; own_t && b->kind == 'c' && u < own_t->nubo; u++)
                    if (own_t->ubo[u].slot == b->slot && own_t->ubo[u].stages >> ns & 1) {  /* a block of ours */
                        uint8_t *p = ring_alloc(own_t->ubo[u].size, &bi[i].buffer, &bi[i].offset);
                        if (p) memcpy(p, (const uint8_t *)own_t + own_t->ubo[u].offset, own_t->ubo[u].size),
                                   range = own_t->ubo[u].size;
                        else bi[i].buffer = dummy_buffer(), bi[i].offset = 0, range = 65536;
                        taken = 1;
                        break;
                    }
                if (taken) {
                } else if (own && ns == 0 && b->kind == 'c' && b->slot == 2) {  /* our object constants */
                    uint8_t *p = ring_alloc(sizeof own->object, &bi[i].buffer, &bi[i].offset);
                    if (p) memcpy(p, own->object, sizeof own->object), range = sizeof own->object;
                    else bi[i].buffer = dummy_buffer(), bi[i].offset = 0, range = 65536;
                } else if (b->kind == 'c' && b->slot == 6 && sh->dof && gpu_dof() == 1 && r.size >= 16 &&
                           (p6 = ring_alloc(range < 32 ? range : 32, &bi[i].buffer, &bi[i].offset)) != NULL) {
                    /* the distance blur at half: its amount's factor (row 0 .w) halved in a copy */
                    range = range < 32 ? range : 32;
                    memcpy(p6, (const void *)(uintptr_t)r.addr, (size_t)range);
                    ((float *)p6)[3] *= 0.5f;
                } else if (b->kind == 'c' && b->slot == 8 && (sh->pcf_float >= 0 || sh->screen_taps || sh->inv_proj) &&
                           r.size &&
                    cb8_scaled_copy(r.addr, range < sh->cb8_bytes ? range : sh->cb8_bytes, sh, &bi[i].buffer,
                                    &bi[i].offset)) {
                    /* a copy with the shadow filter spread and the screen tap offsets scaled to the targets' sizes */
                    if (range > sh->cb8_bytes) range = sh->cb8_bytes;
                } else if (!r.size || !gpu_buffer_at(r.addr, range, &bi[i].buffer, &bi[i].offset)) {
                    bi[i].buffer = dummy_buffer(), bi[i].offset = 0, range = 65536;
                } else {
                    VkDeviceSize align = b->kind == 'c' ? vk.props.limits.minUniformBufferOffsetAlignment
                                                        : vk.props.limits.minStorageBufferOffsetAlignment;
                    if (bi[i].offset % align) gpu_stop("buffer address %#" PRIx64 " not %llu-aligned", r.addr,
                                                       (unsigned long long)align);
                    /* The game binds storage buffers with a size smaller than what its shaders access (the
                     * brightness shaders get 4 bytes and use 256); the Switch's GPU and emulators don't enforce
                     * it, so neither do we: the range runs to the end of the memory pool. */
                    if (b->kind == 's') range = VK_WHOLE_SIZE;
                }
                bi[i].range = range;
                w[i].dstBinding = b->kind == 'c' ? b->slot : 64 + b->slot;
                w[i].descriptorType = b->kind == 'c' ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[i].pBufferInfo = &bi[i];
            } else if (b->kind == 'p') {  /* the soft shadow settings */
                if (!pcss_settings(&bi[i].buffer, &bi[i].offset))
                    bi[i].buffer = dummy_buffer(), bi[i].offset = 0;
                bi[i].range = 64;
                w[i].dstBinding = 95;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w[i].pBufferInfo = &bi[i];
            } else if (b->kind == 't' || b->kind == 'd') {
                uint64_t handle = b->slot < TEXTURES ? s->tex[ns][b->slot] : 0;
                NvnTexEntry *e = tex_entry(s, handle);
                const NvnSamplerDesc *sd = sampler_entry(s, handle);
                if (own_t && ns == 1 && b->slot < TAKEOVER_TEXTURES && own_t->tex_mask >> b->slot & 1)
                    sd = &takeover_tex(own_t, b->slot)->samp;  /* the takeover's sampler, by its settings */
                NvnSamplerDesc d = sd ? *sd : (NvnSamplerDesc){.min_filter = 1, .mag_filter = 1, .lod_max = 1000};
                if (b->shadow && !d.compare_mode) d.compare_mode = 1, d.compare_func = 4;  /* LEQUAL */
                if (!b->shadow) d.compare_mode = 0;
                if (b->kind == 'd') d.min_filter = d.mag_filter = 0;  /* raw depth: nearest texel */
                ii[i].imageView = e ? gpu_sample_view(cb, e->tex, &e->view, e->has_view, b->view)
                                    : gpu_dummy_view(cb, b->view);
                if (e && ns == 1) {  /* widescreen: a fragment program reading a screen-sized image */
                    const GpuImage *ti = image_find(e->tex);
                    if (ti && ti->wide_num != ti->wide_den) dyn_reads_screen = 1;
                }
                gpu_sampler_scene = !s->after_composite;  /* upscaling's mip bias: the 3D's, not the UI's */
                ii[i].sampler = gpu_sampler(&d);
                gpu_sampler_scene = 0;
                ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                w[i].dstBinding = (b->kind == 'd' ? 96 : 16) + b->slot;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = &ii[i];
            } else {
                return 0;
            }
        }
        vkUpdateDescriptorSets(vk.device, (uint32_t)sh->nb, w, 0, NULL);
        vkCmdBindDescriptorSets(cb, bp, layout, setno, 1, &set, 0, NULL);
    }
    return 1;
}

static void bind_sets(VkCommandBuffer cb, const GpuState *s, const Program *g) {
    for (int st = 0; st < NSETS; st++) {
        const Shader *sh = g->sh[st];
        if (!sh || !sh->nb) continue;
        if (!bind_stage_set(cb, s, sh, nvn_stage[st], VK_PIPELINE_BIND_POINT_GRAPHICS, g->layout, (uint32_t)st))
            gpu_stop("image bindings in draws");
    }
}

/* ---- YW4_COMPUTE_TRACE: what the compute shaders were given, and what they left in the buffers */
typedef struct {
    uint64_t key;
    GpuRange ubo0, ssbo[3];
    uint32_t groups[3];
    int ntex;
    struct { uint64_t handle; int found; uint32_t w, h, fmt; } tex[4];
} ComputeTrace;
static ComputeTrace ctrace[16];
static int nctrace;

static void record_compute(const GpuState *s, const Shader *sh, const CmdRec *r) {
    ComputeTrace *t = NULL;
    for (int i = 0; i < nctrace; i++)
        if (ctrace[i].key == sh->key) t = &ctrace[i];
    if (!t && nctrace < 16) t = &ctrace[nctrace++];
    if (!t) return;
    memset(t, 0, sizeof *t);
    t->key = sh->key;
    t->ubo0 = s->ubo[5][0];
    for (int i = 0; i < 3; i++) t->ssbo[i] = s->ssbo[5][i];
    for (int i = 0; i < 3; i++) t->groups[i] = (uint32_t)r->x[i];
    for (int i = 0; i < sh->nb && t->ntex < 4; i++) {
        if (sh->b[i].kind != 't') continue;
        uint64_t h = sh->b[i].slot < TEXTURES ? s->tex[5][sh->b[i].slot] : 0;
        NvnTexEntry *e = tex_entry(s, h);
        int k = t->ntex++;
        t->tex[k].handle = h, t->tex[k].found = e != NULL;
        if (e) t->tex[k].w = e->tex->width, t->tex[k].h = e->tex->height, t->tex[k].fmt = e->tex->format;
    }
}

void gpu_compute_trace(uint64_t frame) {
    static long every = -1;
    if (every < 0) every = getenv("YW4_COMPUTE_TRACE") ? atol(getenv("YW4_COMPUTE_TRACE")) : 0;
    if (!every || frame % (uint64_t)every) return;
    for (int i = 0; i < nctrace; i++) {
        const ComputeTrace *t = &ctrace[i];
        fprintf(stderr, "yw4: compute %016" PRIx64 " frame %" PRIu64 ": groups %ux%ux%u\n", t->key, frame, t->groups[0],
                t->groups[1], t->groups[2]);
        for (int k = 0; k < t->ntex; k++)
            fprintf(stderr, "    texture %d: handle %#" PRIx64 " %s %ux%u format %#x\n", k, t->tex[k].handle,
                    t->tex[k].found ? "found" : "NOT FOUND", t->tex[k].w, t->tex[k].h, t->tex[k].fmt);
        if (t->ubo0.size && host_readable(t->ubo0.addr, 16)) {
            const uint32_t *u = (const uint32_t *)(uintptr_t)t->ubo0.addr;
            fprintf(stderr, "    ubo0 (%#" PRIx64 "): %#x %#x %#x %#x\n", t->ubo0.addr, u[0], u[1], u[2], u[3]);
        }
        for (int b = 0; b < 3; b++) {
            if (!t->ssbo[b].size || !host_readable(t->ssbo[b].addr, 32)) continue;
            const float *f = (const float *)(uintptr_t)t->ssbo[b].addr;
            fprintf(stderr, "    ssbo%d (%#" PRIx64 ", %" PRIu64 " bytes): %g %g %g %g %g %g %g %g\n", b, t->ssbo[b].addr,
                    t->ssbo[b].size, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        }
    }
}

static int trace_request;
void gpu_request_trace(int draws) { __atomic_store_n(&trace_request, draws, __ATOMIC_RELEASE); }

void gpu_dispatch(VkCommandBuffer cb, GpuState *s, const CmdRec *r) {
    Program *g = s->program ? compute_program_of(s->program) : NULL;
    if (!g) {
        static int logged;
        if (logged++ < 8) fprintf(stderr, "yw4: gpu: compute dispatch skipped: no compute program bound\n");
        return;
    }
    gpu_end_rendering(cb, s);
    /* Textures that need uploading or a new image first. */
    for (int i = 0; i < g->comp->nb; i++) {
        const Binding *b = &g->comp->b[i];
        NvnTexEntry *e = b->kind == 't' && b->slot < TEXTURES ? tex_entry(s, s->tex[5][b->slot]) : NULL;
        if (e && gpu_needs_transfer(e->tex)) image_of(cb, e->tex);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->comp_pipe);
    if (g->comp->nb && !bind_stage_set(cb, s, g->comp, 5, VK_PIPELINE_BIND_POINT_COMPUTE, g->comp_layout, 0)) {
        static uint64_t seen[16];
        static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen[i] == g->comp->key;
        if (!known && nseen < 16) {
            seen[nseen++] = g->comp->key;
            fprintf(stderr, "yw4: gpu: compute shader %016" PRIx64 " uses storage images: dispatch skipped for now\n",
                    g->comp->key);
        }
        return;
    }
    record_compute(s, g->comp, r);
    vkCmdDispatch(cb, (uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]);
    full_barrier(cb);
}

/* YW4_CAPTURE_FS=<fragment shader key prefixes, comma separated> with YW4_DUMP_IMAGE_AT=<frame>[,...]:
 * after each draw with such a fragment shader in those frames, its colour targets are copied to host
 * memory and written as work/dump/cap_<n>_<fs>_f<frame>_t<target>.raw (u32 width, height, VkFormat,
 * then the texels). Unlike YW4_DUMP_IMAGE this sees a target mid-frame, before later passes reuse it. */
typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint32_t w, h, format, size;
    char name[48];
} Capture;
static Capture caps[64];
static int ncaps;

int gpu_capture_pending(void) { return ncaps; }

static void capture_targets(VkCommandBuffer cb, GpuState *s, const char *name);
static void capture_after_draw(VkCommandBuffer cb, GpuState *s, const Program *g) {
    static const char *want;
    static uint64_t at[64];  /* 64 frames: one every 4 over 4 s of a replay (a flash's time series, 2026-11-12) */
    static int init, nat;
    if (!init) {
        init = 1;
        want = getenv("YW4_CAPTURE_FS");
        for (const char *e = getenv("YW4_DUMP_IMAGE_AT"); e && *e && nat < 64; e += strcspn(e, ","), e += *e == ',')
            at[nat++] = strtoull(e, NULL, 10);
    }
    if (!want || !g->sh[ST_FRAG]) return;
    uint64_t frame = gpu_frames();
    int now = 0;
    for (int i = 0; i < nat; i++) now |= at[i] == frame;
    if (!now) return;
    char key[17];
    snprintf(key, sizeof key, "%016" PRIx64, g->sh[ST_FRAG]->key);
    int hit = 0;
    for (const char *w = want; *w;) {
        size_t n = strcspn(w, ",");
        if (n && !strncmp(key, w, n)) hit = 1;
        w += n + (w[n] == ',');
    }
    if (!hit) return;
    char name[32];
    snprintf(name, sizeof name, "cap_%02d_%.8s_f%" PRIu64, ncaps, key, frame);
    capture_targets(cb, s, name);
}

/* Copies the bound colour targets for gpu_capture_flush to write as work/dump/<name>_t<i>.raw. */
static void capture_targets(VkCommandBuffer cb, GpuState *s, const char *name) {
    gpu_end_rendering(cb, s);
    for (int i = 0; i < s->ncolor && ncaps < 64; i++) {
        GpuImage *im = s->rt.color[i] ? image_find(s->rt.color[i]) : NULL;
        if (!im) continue;
        uint32_t bpp = im->format == VK_FORMAT_R16G16B16A16_SFLOAT   ? 8
                       : im->format == VK_FORMAT_R32G32B32A32_SFLOAT ? 16
                                                                     : 4;
        Capture *c = &caps[ncaps++];
        c->w = im->width, c->h = im->height, c->format = (uint32_t)im->format, c->size = im->width * im->height * bpp;
        snprintf(c->name, sizeof c->name, "%s_t%d", name, i);
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = c->size,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        CHECK(vkCreateBuffer(vk.device, &bci, NULL, &c->buf));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk.device, c->buf, &mr);
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
            .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        CHECK(vkAllocateMemory(vk.device, &mai, NULL, &c->mem));
        CHECK(vkBindBufferMemory(vk.device, c->buf, c->mem, 0));
        VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {im->width, im->height, 1}};
        vkCmdCopyImageToBuffer(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, c->buf, 1, &cp);
        full_barrier(cb);
    }
}

/* CMD_NATIVE (gbuffer.c, YW4_CAPTURE_LAYER_AT=<GPU frames>): the colour targets as a pass of ours
 * left them, in those frames; `data` is the pass's name. */
void gpu_capture_layer(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    static uint64_t at[32];
    static int nat = -1;
    if (nat < 0) {
        nat = 0;
        const char *e = getenv("YW4_CAPTURE_LAYER_AT");
        for (char *end; e && *e && nat < 32; e = *end ? end + 1 : end) {
            at[nat] = strtoull(e, &end, 10);
            if (end == e) break;
            nat++;
        }
    }
    uint64_t f = gpu_frames();
    int i = 0;
    while (i < nat && at[i] != f) i++;
    if (i == nat) return;
    char name[40];
    snprintf(name, sizeof name, "layer_%.*s_f%" PRIu64, (int)(size < 12 ? size : 12), (const char *)data, f);
    capture_targets(vk_cb, gpu_state, name);
}

void gpu_capture_flush(void) {
    for (int i = 0; i < ncaps; i++) {
        Capture *c = &caps[i];
        void *p;
        CHECK(vkMapMemory(vk.device, c->mem, 0, VK_WHOLE_SIZE, 0, &p));
        char path[96];
        snprintf(path, sizeof path, "work/dump/%s.raw", c->name);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint32_t hdr[3] = {c->w, c->h, c->format};
            fwrite(hdr, 4, 3, f), fwrite(p, 1, c->size, f), fclose(f);
        }
        vkDestroyBuffer(vk.device, c->buf, NULL), vkFreeMemory(vk.device, c->mem, NULL);
        fprintf(stderr, "yw4: captured %s (%ux%u format %u)\n", c->name, c->w, c->h, c->format);
    }
    ncaps = 0;
}

/* The dynamic state last set in the command buffer being recorded: a draw whose state is the
 * same sets none of it (most draws of a pass: ~25 Vulkan calls each). Forgotten when a command
 * buffer begins and after the native passes, which set some themselves (gpu_dynamic_forget). */
typedef struct {
    VkViewport viewport;
    VkRect2D scissor;
    VkCullModeFlags cull;
    VkFrontFace front;
    VkBool32 depth_test, depth_write, stencil_test, bias_enable, discard;
    VkCompareOp depth_op;
    struct { VkStencilOp fail, pass, depth_fail; VkCompareOp op; uint32_t ref, write_mask, compare_mask; } face[2];
    float bias[3], blend[4];
} DynState;
static DynState dyn_last;
static int dyn_model;  /* the next gpu_dynamic_state is for a 3D object's draw (TAA shifts it, gpu_aa.c) */
static VkCommandBuffer dyn_cb;  /* NULL: nothing known */

void gpu_dynamic_forget(void) { dyn_cb = VK_NULL_HANDLE; }

static void cull_of(uint8_t cull_mode, uint8_t front_face, VkCullModeFlags *cull, VkFrontFace *front) {
    static const VkCullModeFlags modes[4] = {VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT,
                                             VK_CULL_MODE_FRONT_AND_BACK};
    *cull = modes[cull_mode & 3];
    int ccw = front_face == 1;
    if (nvn_window_origin != 1) ccw = !ccw;
    *front = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
}

void gpu_cull(VkCommandBuffer cb, uint8_t cull_mode, uint8_t front_face) {
    VkCullModeFlags cull;
    VkFrontFace front;
    cull_of(cull_mode, front_face, &cull, &front);
    vkCmdSetCullMode(cb, cull);
    vkCmdSetFrontFace(cb, front);
    if (cb == dyn_cb) dyn_last.cull = cull, dyn_last.front = front;
}

/* Widescreen: does this draw cover the game's whole screen (2) or its whole width (1)? The UI's full-screen
 * layers (the dimming behind a dialog, the loading screen's black) are one quad whose corners reach the edges of
 * clip space; the diary's background is four quads in one draw, its bars at the top and bottom span the width
 * only. With the program's position matrix (Shader.pos_cbuf) and vertex input 0 the corners are computed here
 * (the loading screen's black: input 0 is the position, Shader.pos_input). Only small draws (up to 4 quads: 24
 * vertices) and float positions; anything else: no.
 * The reads are checked against the memory pools (gpu_buffer_at), not with host_readable: calling
 * host_readable (VirtualQuery) on these vertex and index addresses every frame stopped the game at
 * the title screen in every test run (2026-10-28; cause not found). */
static int covers_screen(const GpuState *s, const Shader *vs, const CmdRec *r) {
    if (!vs || !(vs->pos_cbuf || vs->pos_input) || !s->nattribs || !(s->attribs[0] & 0xFF)) return 0;
    uint32_t count = (uint32_t)r->x[2];
    if (count < 3 || count > 24) return 0;
    VkBuffer pb;
    VkDeviceSize po;
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    static const uint32_t identity_rows[4] = {0, 1, 2, 3};
    const float *m = identity;
    const uint32_t *rows = identity_rows;
    if (vs->pos_cbuf) {
        GpuRange ub = s->ubo[0][vs->pos_cbuf - 1];
        uint32_t top = 0;
        for (int i = 0; i < 4; i++) top = vs->pos_rows[i] > top ? vs->pos_rows[i] : top;
        if (!ub.size || ub.size < (top + 1) * 16ull || !gpu_buffer_at(ub.addr, (top + 1) * 16ull, &pb, &po)) return 0;
        m = (const float *)(uintptr_t)ub.addr, rows = vs->pos_rows;
    }
    uint32_t a = s->attribs[0], stream = a >> 8 & 0x1F, offset = a >> 13, n;
    switch (vertex_format(a & 0xFF)) {
    case VK_FORMAT_R32G32_SFLOAT: n = 2; break;
    case VK_FORMAT_R32G32B32_SFLOAT: n = 3; break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: n = 4; break;
    default: return 0;
    }
    uint32_t stride = stream < s->nstreams ? s->streams[stream][0] : 0;
    GpuRange vb = s->vbuf[stream];
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (uint32_t i = 0; i < count; i++) {
        uint64_t v;
        if (r->op == CMD_DRAW_ARRAYS) {
            v = (uint64_t)r->x[1] + i;
        } else {  /* (primitive, index type, count, index address, base vertex, ...) */
            uint32_t size = r->x[1] == 1 ? 2 : 4;
            if (!gpu_buffer_at(r->x[3] + (uint64_t)i * size, size, &pb, &po)) return 0;
            const uint8_t *ix = (const uint8_t *)(uintptr_t)(r->x[3] + (uint64_t)i * size);
            v = (size == 2 ? *(const uint16_t *)ix : *(const uint32_t *)ix) + (uint64_t)r->x[4];
        }
        uint64_t at = v * stride + offset;
        if (at + n * 4 > vb.size || !gpu_buffer_at(vb.addr + at, n * 4, &pb, &po)) return 0;
        const float *p = (const float *)(uintptr_t)(vb.addr + at);
        float in[4] = {p[0], p[1], n > 2 ? p[2] : 0, n > 3 ? p[3] : 1}, out[4];
        for (int c = 0; c < 4; c++) {
            const float *row = m + rows[c] * 4;
            out[c] = row[0] * in[0] + row[1] * in[1] + row[2] * in[2] + row[3] * in[3];
        }
        if (!(out[3] > 0)) return 0;
        for (int c = 0; c < 2; c++) {
            float q = out[c] / out[3];
            lo[c] = q < lo[c] ? q : lo[c], hi[c] = q > hi[c] ? q : hi[c];
        }
    }
    if (!(lo[0] <= -0.99f && hi[0] >= 0.99f)) return 0;
    return lo[1] <= -0.99f && hi[1] >= 0.99f ? 2 : 1;
}

/* Widescreen: the draw's viewport spans the game's whole width of t (none set: all of it). */
static int whole_width(const GpuState *s, const GpuImage *t) {
    return !s->have_viewport || (s->viewport[0] <= 0 && s->viewport[0] + s->viewport[2] >= (int32_t)t->key.width);
}

/* The dynamic state of the pipelines (gpu_draw's and the native passes'), from s: viewport
 * (flipped for the upper-left origin), scissor, cull, front face, depth, stencil, bias. */
void gpu_dynamic_state(VkCommandBuffer cb, const GpuState *s, VkFormat depth, uint32_t w, uint32_t h) {
    DynState d;
    memset(&d, 0, sizeof d);  /* padding too: the states are compared as bytes */
    int flip = nvn_window_origin == 1;
    /* a target made larger than the game's texture (YW4_SHADOW_RES, YW4_RES, YW4_EFFECTS): the game's
     * viewport and scissor scaled with it: the first colour target's scale, else the depth target's.
     * They can differ: the distance blur draws into its own half-size images (kept at the game's ratio)
     * with the effects' half-size depth attached but unused (depth test and write off) */
    const GpuImage *tc = s->ncolor && s->rt.color[0] ? image_find(s->rt.color[0]) : NULL;
    const GpuImage *td = s->rt.depth ? image_find(s->rt.depth) : NULL;
    const GpuImage *tg = tc ? tc : td;
    /* Scaled differently and the draw uses depth: the colour wins and the depth goes unused for this draw.
     * Met once: the distance blur's first copy (fs 95ea1b) also copies depth into the effects' half-size
     * depth, which the same copy into a matching target redoes two passes later, before anyone reads it. */
    int mismatch = tc && td && (uint64_t)tc->scale_num * td->scale_den != (uint64_t)td->scale_num * tc->scale_den &&
                   (s->depth_stencil[0] || s->depth_stencil[1] || s->depth_stencil[3]);
    if (mismatch) {
        static int logged;
        if (logged++ < 4)
            fprintf(stderr, "yw4: gpu: colour and depth targets scaled differently (%ux%u, %ux%u) in a draw that uses "
                            "depth: drawn without depth\n", tc->width, tc->height, td->width, td->height);
    }
    int64_t kn = tg ? tg->scale_num : 1, kd = tg ? tg->scale_den : 1;
    /* widescreen: across, also times the image's width factor; the UI (what follows the composite in its
     * command list, but the copy into the window's image, draws that read a screen-sized image, like the
     * finished 3D picture's copy, and layers over the whole screen) keeps the game's width in the middle */
    int64_t xn = kn * (tg ? tg->wide_num : 1), xd = kd * (tg ? tg->wide_den : 1), yn = kn, yd = kd;
    float xoff = 0, yoff = 0;
    int full = dyn_full_screen && tg && whole_width(s, tg);
    /* world markers (the "!" above a yo-kai, the icon on a pole): quads the game places with the camera's
     * projection and tests against the scene's depth, drawn after the composite. They belong to the 3D
     * picture, so they take its full width: kept in the middle they sat too near the centre (by 1/4 of
     * their distance from it at 21:9) and were as much narrower (owner, 2026-11-11) */
    int world = td && s->depth_stencil[0];
    if (tg && tg->wide_num != tg->wide_den && s->after_composite && !dyn_reads_screen && !full && !world &&
        !gpu_is_presented(tg->key.addr)) {
        if (tg->wide_num < tg->wide_den) {
            /* an image narrower than 16:9 (16:10): the UI at the game's width would stick out at both sides.
             * It is fitted to the image's width instead (across: the image's factor, as the 3D), shrunk by
             * the same factor down, and moved to the middle from top to bottom */
            yn = xn, yd = xd;
            yoff = ((float)tg->height - (float)tg->key.height * (float)yn / (float)yd) * 0.5f;
        } else {
            xn = kn, xd = kd;
            xoff = ((float)tg->width - (float)tg->key.width * (float)kn / (float)kd) * 0.5f;
        }
    }
    float vx = 0, vy = 0, vw = (float)w, vh = (float)h, xf = (float)xn / (float)xd, yf = (float)yn / (float)yd;
    if (s->have_viewport) vx = xoff + (float)s->viewport[0] * xf, vy = yoff + (float)s->viewport[1] * yf,
                          vw = (float)s->viewport[2] * xf, vh = (float)s->viewport[3] * yf;
    d.viewport = (VkViewport){vx, flip ? vy + vh : vy, vw, flip ? -vh : vh, s->depth_range[0], s->depth_range[1]};
    float jx, jy;
    if (dyn_model && !s->after_composite && gpu_taa_jitter(w, h, &jx, &jy)) d.viewport.x += jx, d.viewport.y += jy;
    dyn_model = dyn_reads_screen = dyn_full_screen = 0;
    int edge = yoff > 0 ? dyn_edge : 0;
    dyn_edge = 0;
    if (edge) {
        /* the strip above or below the fitted UI: the layer 8 times as tall with its top (bottom) edge at the
         * image's, so the strip shows the layer's outermost rows only (its edge drawn out, as a clamp would;
         * the whole layer stretched instead would put its bands where the fitted content's are not) */
        float big = d.viewport.height * 8, top = edge == 1 ? vy - yoff : vy + vh + yoff - vh * 8;
        d.viewport.y = flip ? top + vh * 8 : top, d.viewport.height = big;
    }
    d.scissor = (VkRect2D){{0, 0}, {w, h}};
    if (s->have_scissor) {
        /* edges scaled: left and top down, right and bottom up, so the scissor covers what it did */
        int64_t x0 = s->scissor[0] < 0 ? 0 : s->scissor[0], y0 = s->scissor[1] < 0 ? 0 : s->scissor[1];
        int64_t x1 = x0 + (s->scissor[2] > 0 ? s->scissor[2] : 0), y1 = y0 + (s->scissor[3] > 0 ? s->scissor[3] : 0);
        int64_t x = x0 * xn / xd, y = y0 * yn / yd;
        int64_t sw = (x1 * xn + xd - 1) / xd - x, sh = (y1 * yn + yd - 1) / yd - y;
        x += (int64_t)xoff, y += (int64_t)yoff;
        if (kn != kd || xn != xd || yn != yd || xoff || yoff) {  /* the game's "whole target" scissor can be huge: keep it inside */
            if (x > w) x = w;
            if (y > h) y = h;
            if (sw > w - x) sw = w - x;
            if (sh > h - y) sh = h - y;
        }
        d.scissor = (VkRect2D){{(int32_t)x, (int32_t)y}, {(uint32_t)sw, (uint32_t)sh}};
    }
    if (edge) {
        uint32_t strip = (uint32_t)(yoff + 0.5f);
        if (strip > h) strip = h;
        d.scissor = (VkRect2D){{0, edge == 1 ? 0 : (int32_t)(h - strip)}, {w, strip}};
    }
    cull_of(s->polygon[0], s->polygon[1], &d.cull, &d.front);
    const uint8_t *ds = s->depth_stencil;
    int has_depth = depth != VK_FORMAT_UNDEFINED && depth != VK_FORMAT_S8_UINT;
    int has_stencil = depth == VK_FORMAT_S8_UINT || depth == VK_FORMAT_D24_UNORM_S8_UINT ||
                      depth == VK_FORMAT_D32_SFLOAT_S8_UINT;
    d.depth_test = has_depth && ds[0], d.depth_write = has_depth && ds[1];
    d.depth_op = compare_fn(ds[2]);
    d.stencil_test = has_stencil && ds[3];
    if (mismatch) d.depth_test = d.depth_write = d.stencil_test = 0;  /* above */
    uint32_t ops = ds[5] | ds[6] << 8 | ds[7] << 16;
    for (int face = 0; face < 2; face++) {
        uint32_t o = ops >> (12 * face);
        d.face[face].fail = stencil_op(o & 0xF), d.face[face].pass = stencil_op(o >> 8 & 0xF);
        d.face[face].depth_fail = stencil_op(o >> 4 & 0xF);
        d.face[face].op = compare_fn(face ? ds[4] >> 4 : ds[4] & 0xF);
        d.face[face].ref = s->stencil_ref[face], d.face[face].write_mask = s->stencil_mask[face];
        d.face[face].compare_mask = s->stencil_value_mask[face];
    }
    d.bias_enable = (s->polygon[3] & 4) != 0;
    d.bias[0] = s->polygon_offset[1], d.bias[1] = s->polygon_offset[2], d.bias[2] = s->polygon_offset[0];
    memcpy(d.blend, s->blend_color, sizeof d.blend);
    d.discard = s->rasterizer_discard != 0;
    if (cb == dyn_cb && !memcmp(&d, &dyn_last, sizeof d)) return;  /* all of it set already */

    vkCmdSetViewport(cb, 0, 1, &d.viewport);
    vkCmdSetScissor(cb, 0, 1, &d.scissor);
    vkCmdSetCullMode(cb, d.cull);
    vkCmdSetFrontFace(cb, d.front);
    vkCmdSetDepthTestEnable(cb, d.depth_test);
    vkCmdSetDepthWriteEnable(cb, d.depth_write);
    vkCmdSetDepthCompareOp(cb, d.depth_op);
    vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    vkCmdSetStencilTestEnable(cb, d.stencil_test);
    for (int face = 0; face < 2; face++) {
        VkStencilFaceFlags f = face ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilOp(cb, f, d.face[face].fail, d.face[face].pass, d.face[face].depth_fail, d.face[face].op);
        vkCmdSetStencilReference(cb, f, d.face[face].ref);
        vkCmdSetStencilWriteMask(cb, f, d.face[face].write_mask);
        vkCmdSetStencilCompareMask(cb, f, d.face[face].compare_mask);
    }
    vkCmdSetDepthBiasEnable(cb, d.bias_enable);
    vkCmdSetDepthBias(cb, d.bias[0], d.bias[1], d.bias[2]);
    vkCmdSetBlendConstants(cb, d.blend);
    vkCmdSetRasterizerDiscardEnable(cb, d.discard);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    dyn_last = d, dyn_cb = cb;
}

static void draw(VkCommandBuffer cb, GpuState *s, const CmdRec *r) {
    Program *g = s->program ? program_of(s->program) : NULL;
    if (!g) {
        static int logged;
        if (logged++ < 8)
            fprintf(stderr, "yw4: gpu: draw skipped: %s (stage mask %#x, stage keys %016" PRIx64 " %016" PRIx64 ")\n",
                    s->program ? "program without a vertex stage" : "no program bound",
                    s->program ? s->program->stage_mask : 0, s->program ? s->program->stages[0].key : 0,
                    s->program ? s->program->stages[5].key : 0);
        return;
    }
    if (g->sh[ST_FRAG] && (g->sh[ST_FRAG]->dof || g->sh[ST_FRAG]->key == DOF_BLUR_SHADER)) {
        /* the distance blur's images keep the game's size ratio (gpu.c, effects resolution) */
        NvnTexEntry *e = tex_entry(s, s->tex[nvn_stage[ST_FRAG]][0]);
        if (e) gpu_keep_game_ratio(e->tex);
        if (!g->sh[ST_FRAG]->dof && s->ncolor) gpu_keep_game_ratio(s->rt.color[0]);
        if (g->sh[ST_FRAG]->dof && !gpu_dof()) return;  /* distance blur off */
    }
    if (s->color[2] != 8) {
        static int logged;
        if (!logged++) fprintf(stderr, "yw4: gpu: alpha test %u ignored for now\n", s->color[2]);
    }
    if (needs_transfers(s, g)) gpu_end_rendering(cb, s);
    /* Images and uploads first (outside rendering), then rendering. */
    for (int st = 0; st < NSETS; st++)
        for (int i = 0; g->sh[st] && i < g->sh[st]->nb; i++) {
            const Binding *b = &g->sh[st]->b[i];
            NvnTexEntry *e = b->kind == 't' && b->slot < TEXTURES ? tex_entry(s, s->tex[nvn_stage[st]][b->slot]) : NULL;
            if (e && gpu_needs_transfer(e->tex)) image_of(cb, e->tex);
        }
    if (!s->rendering)
        for (int i = 0; i < s->ncolor; i++)
            if (s->rt.color[i]) image_of(cb, s->rt.color[i]);
    if (!s->rendering && s->rt.depth) image_of(cb, s->rt.depth);
    /* geometry in video memory (gpu.c, YW4_VRAM_GEOMETRY): the vertex streams the program reads and
     * the indices, copied outside rendering when they need it */
    if (gpu_vram_geometry() && g->sh[ST_VERT]) {
        GpuRange geo[17];
        int ngeo = 0, need = 0;
        uint32_t used = 0;
        for (uint32_t loc = 0; loc < 32; loc++) {
            uint32_t a = loc < s->nattribs ? s->attribs[loc] : 0;
            if (g->sh[ST_VERT]->inputs[loc] && a & 0xFF) used |= 1u << (a >> 8 & 0x1F);
        }
        for (uint32_t i = 0; i < 16; i++) {
            GpuRange vr = own ? (GpuRange){own->vertices, own->vertex_bytes} : s->vbuf[i];
            if (used >> i & 1 && vr.size) geo[ngeo++] = vr;
        }
        if (r->op == CMD_DRAW_ELEMENTS && r->x[1]) geo[ngeo++] = (GpuRange){r->x[3], r->x[2] * (r->x[1] == 1 ? 2 : 4)};
        for (int i = 0; i < ngeo; i++) need |= gpu_geometry_needs_copy(geo[i].addr, geo[i].size);
        if (need) {
            gpu_end_rendering(cb, s);
            for (int i = 0; i < ngeo; i++) gpu_geometry_copy(cb, geo[i].addr, geo[i].size);
            full_barrier(cb);
        }
    }

    PipeKey k;
    memset(&k, 0, sizeof k);
    uint32_t w, h;
    if (!begin_rendering(cb, s, k.color, &k.depth, &w, &h)) {
        static int logged;
        if (logged++ < 8)
            fprintf(stderr, "yw4: gpu: draw without render targets skipped (colour targets %d, depth %p, shaders %016" PRIx64
                            " %016" PRIx64 ", rasterizer discard %d, frame %" PRIu64 ")\n",
                    s->ncolor, (void *)s->rt.depth, g->sh[ST_VERT] ? g->sh[ST_VERT]->key : 0,
                    g->sh[ST_FRAG] ? g->sh[ST_FRAG]->key : 0, s->rasterizer_discard, gpu_frames());
        return;
    }
    k.ncolor = (uint8_t)s->ncolor;
    /* our G-buffer programs in place of the game's (decision 0026; F6); the shadow mask's has its own switch */
    if (g->sh[ST_FRAG] && g->sh[ST_FRAG]->key == SHADOW_MASK_FS)
        k.ours = g->sh[ST_FRAG]->ours && shadow_mask_native() && !gpu_ab_game;
    else if (g->sh[ST_FRAG] && is_screen_fs(g->sh[ST_FRAG]->key))
        k.ours = g->sh[ST_FRAG]->ours && screen_native() && !gpu_ab_game;
    else
        k.ours = (own || gbuffer_native_frame(gpu_frames())) &&
                 ((g->sh[ST_VERT] && g->sh[ST_VERT]->ours) || (g->sh[ST_FRAG] && g->sh[ST_FRAG]->ours));
    memcpy(k.blend, s->blend, sizeof k.blend);
    for (int i = 0; i < 8; i++) k.blend[i][0] = 0;
    memcpy(k.color_state, s->color, 4);
    k.channel_mask = s->channel_mask;
    k.polygon_mode = s->polygon[2];
    k.depth_clamp = (uint8_t)(s->depth_clamp != 0);
    VkPrimitiveTopology topo;
    int quads = r->x[0] == 7;
    if (quads) {
        if (r->op != CMD_DRAW_ARRAYS) gpu_stop("indexed quads");
        topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    } else if (!topology((uint32_t)r->x[0], &topo)) {
        gpu_stop("primitive type %u", (unsigned)r->x[0]);
    }
    k.topology = (uint8_t)topo;

    /* Vertex input: the shader's inputs, from the bound attribute and stream state. */
    const Shader *vs = g->sh[ST_VERT];
    uint32_t streams_used = 0;
    for (uint32_t loc = 0; loc < 32; loc++) {
        if (!vs->inputs[loc]) continue;
        uint32_t a = loc < s->nattribs ? s->attribs[loc] : 0;
        if (!(a & 0xFF)) {
            static uint32_t logged;
            if (!(logged & 1u << loc)) fprintf(stderr, "yw4: gpu: vertex input %u has no attribute\n", loc);
            logged |= 1u << loc;
            continue;
        }
        VkFormat f = vertex_format(a & 0xFF);
        if (f == VK_FORMAT_UNDEFINED) gpu_stop("vertex format %#x", a & 0xFF);
        uint32_t stream = a >> 8 & 0x1F;
        k.attr[k.nattr++] = (VkVertexInputAttributeDescription){loc, stream, input_format(f, vs->inputs[loc], loc),
                                                                a >> 13};
        streams_used |= 1u << stream;
    }
    for (uint32_t i = 0; i < 16; i++) {
        if (!(streams_used & 1u << i)) continue;
        uint32_t stride = i < s->nstreams ? s->streams[i][0] : 0, divisor = i < s->nstreams ? s->streams[i][1] : 0;
        if (divisor > 1) gpu_stop("vertex stream divisor %u", divisor);
        k.bind[k.nbind++] = (VkVertexInputBindingDescription){
            i, stride, divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        VkBuffer buf;
        VkDeviceSize off;
        GpuRange vr = own ? (GpuRange){own->vertices, own->vertex_bytes} : s->vbuf[i];  /* ours: every stream */
        if (!vr.size || !gpu_geometry_at(vr.addr, vr.size, &buf, &off)) buf = dummy_buffer(), off = 0;
        vkCmdBindVertexBuffers(cb, i, 1, &buf, &off);
    }

    VkPipeline pl = pipeline(g, &k);
    if (!pl) return;  /* a 3D object whose new pipeline is being built in the background: drawn from the next frame */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    bind_unjitter = !(g->sh[ST_VERT] && g->sh[ST_VERT]->model) && !s->after_composite;
    bind_sets(cb, s, g);

    /* YW4_MOVIE_LOG=1 (testing video pacing): when the movie program's luma texture shows a new picture (a
     * fingerprint of its memory changes), the GPU frame, the host time and how many frames the last one stayed */
    if (g->sh[ST_FRAG] && g->sh[ST_FRAG]->key == 0x2e2b3b7ac92f7848ull && getenv("YW4_MOVIE_LOG")) {
        NvnTexEntry *e = tex_entry(s, s->tex[nvn_stage[ST_FRAG]][0]);
        if (e && e->tex->addr) {
            static uint64_t last_hash, last_frame;
            static int64_t last_ns;
            const uint8_t *y = (const uint8_t *)(uintptr_t)e->tex->addr;
            uint64_t hash = 1469598103934665603ull;
            for (size_t i = 0; i < 1920 * 1080; i += 997) hash = (hash ^ y[i]) * 1099511628211ull;
            if (hash != last_hash) {
                int64_t now = host_now_ns();
                fprintf(stderr, "yw4: movie: frame %" PRIu64 " new picture at %#" PRIx64 ", %.1f ms and %" PRIu64
                        " frames after the last\n", gpu_frames(), e->tex->addr, last_ns ? (double)(now - last_ns) / 1e6 : 0.0,
                        gpu_frames() - last_frame);
                last_hash = hash, last_frame = gpu_frames(), last_ns = now;
            }
        }
    }

    /* YW4_GPU_TRACE=<n>: describe n draws, from frame YW4_GPU_TRACE_AT (default 0) on */
    static long trace = -1, trace_at;
    if (trace < 0) {
        trace = getenv("YW4_GPU_TRACE") ? atol(getenv("YW4_GPU_TRACE")) : 0;
        trace_at = getenv("YW4_GPU_TRACE_AT") ? atol(getenv("YW4_GPU_TRACE_AT")) : 0;
    }
    int requested = __atomic_exchange_n(&trace_request, 0, __ATOMIC_ACQ_REL);
    if (requested) trace = requested, trace_at = 0;
    if (trace > 0 && gpu_frames() >= (uint64_t)trace_at) {
        trace--;
        fprintf(stderr, "yw4: gpu draw: vs %016" PRIx64 " fs %016" PRIx64 " prim %u count %u target %ux%u (%d colour, %s depth)"
                " viewport %s%d,%d %dx%d scissor %s%d,%d %dx%d blend %#x\n",
                vs->key, g->sh[ST_FRAG] ? g->sh[ST_FRAG]->key : 0, (unsigned)r->x[0],
                (unsigned)(r->op == CMD_DRAW_ARRAYS ? r->x[2] : r->x[2]), w, h, s->ncolor, s->rt.depth ? "with" : "no",
                s->have_viewport ? "" : "(none) ", s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3],
                s->have_scissor ? "" : "(none) ", s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3],
                s->color[0]);
        fprintf(stderr, "yw4: gpu draw:   depth range %g..%g clamp %d discard %d polygon %u,%u,%u,%u depth/stencil %u,%u,%u,%u,%#x offset %g,%g,%g\n",
                s->depth_range[0], s->depth_range[1], s->depth_clamp, s->rasterizer_discard, s->polygon[0], s->polygon[1],
                s->polygon[2], s->polygon[3], s->depth_stencil[0], s->depth_stencil[1], s->depth_stencil[2],
                s->depth_stencil[3], s->depth_stencil[4], s->polygon_offset[0], s->polygon_offset[1], s->polygon_offset[2]);
        if (s->rt.depth)
            fprintf(stderr, "yw4: gpu draw:   depth target at %#" PRIx64 " kind %u layers %u format %#x view %s layer %u+%u\n",
                    s->rt.depth->addr, s->rt.depth->target, s->rt.depth->depth, s->rt.depth->format,
                    s->rt.has_depth_view ? "yes" : "no", s->rt.has_depth_view ? s->rt.depth_view.min_layer : 0,
                    s->rt.has_depth_view ? s->rt.depth_view.num_layers : 0);
        for (int ci = 0; ci < s->ncolor && ci < 8; ci++)
            if (s->rt.color[ci])
                fprintf(stderr, "yw4: gpu draw:   colour target %d at %#" PRIx64 " kind %u layers %u format %#x view %s layer %u+%u\n",
                        ci, s->rt.color[ci]->addr, s->rt.color[ci]->target, s->rt.color[ci]->depth,
                        s->rt.color[ci]->format, s->rt.has_view[ci] ? "yes" : "no",
                        s->rt.has_view[ci] ? s->rt.views[ci].min_layer : 0, s->rt.has_view[ci] ? s->rt.views[ci].num_layers : 0);
        for (int st = 0; st < NSETS; st++)
            for (int i = 0; g->sh[st] && i < g->sh[st]->nb; i++) {
                const Binding *b = &g->sh[st]->b[i];
                if (b->kind != 't') continue;
                NvnTexEntry *e = tex_entry(s, s->tex[nvn_stage[st]][b->slot]);
                uint64_t h = s->tex[nvn_stage[st]][b->slot];
                fprintf(stderr, "yw4: gpu draw:   %s texture %u: handle %#" PRIx64 " (id %u, sampler %u) -> ", stage_names[st],
                        b->slot, h, (unsigned)(h & 0xFFFFF), (unsigned)(h >> 20 & 0xFFF));
                if (e) fprintf(stderr, "%ux%u format %#x at %#" PRIx64 " kind %u layers %u view %s layer %u+%u\n", e->tex->width,
                               e->tex->height, e->tex->format, e->tex->addr, e->tex->target, e->tex->depth,
                               e->has_view ? "yes" : "no", e->has_view ? e->view.min_layer : 0,
                               e->has_view ? e->view.num_layers : 0);
                else fprintf(stderr, "none\n");
            }
        /* YW4_GPU_TRACE_SCREEN=1: constants that look like a screen-derived size (1600x900 and the game's
         * smaller copies of it) or 0.25..4 over one (offsets of a few pixels): what resolution scaling
         * must change; coincidences come too (0.01 = 2/200). Once per
         * shader, block and float. */
        static int screen_scan = -1;
        if (screen_scan < 0) screen_scan = getenv("YW4_GPU_TRACE_SCREEN") && atoi(getenv("YW4_GPU_TRACE_SCREEN"));
        for (int st = 0; screen_scan && st < 2; st++) {
            const Shader *sh = g->sh[st];
            if (!sh) continue;
            static const float sizes[] = {1600, 900, 800, 450, 533, 300, 400, 225, 200, 112, 120, 67, 1920, 1080};
            static const float muls[] = {0.25f, 0.5f, 0.75f, 1, 1.25f, 1.5f, 2, 2.5f, 3, 4};
            static uint64_t seen[4096];
            for (int i = 0; i < sh->nb; i++) {
                const Binding *b = &sh->b[i];
                if (b->kind != 'c' || b->slot >= UBOS) continue;
                GpuRange ub = s->ubo[nvn_stage[st]][b->slot];
                if (!ub.size || ub.size > 0x10000 || !host_readable(ub.addr, ub.size)) continue;
                const float *f = (const float *)(uintptr_t)ub.addr;
                for (uint64_t k = 0; k < ub.size / 4; k++) {
                    float v = f[k], a = v < 0 ? -v : v;
                    if (!(a > 1e-5f) || a > 5000) continue;
                    const char *what = NULL;
                    float x = 0, m = 0;
                    for (size_t z = 0; !what && z < sizeof sizes / sizeof *sizes; z++) {
                        if (a == sizes[z]) what = "size", x = sizes[z];
                        for (size_t q = 0; !what && q < sizeof muls / sizeof *muls; q++) {
                            float r = muls[q] / sizes[z];
                            if (a > r * 0.9999f && a < r * 1.0001f) what = "1/size", x = sizes[z], m = muls[q];
                        }
                    }
                    if (!what) continue;
                    uint64_t id = sh->key ^ (uint64_t)b->slot << 56 ^ k << 40 ^ (uint64_t)st << 62;
                    size_t at = (size_t)(id * 0x9E3779B97F4A7C15ull >> 52), n;
                    for (n = 0; n < 4096 && seen[at] && seen[at] != id; n++) at = (at + 1) & 4095;
                    if (n == 4096 || seen[at] == id) continue;
                    seen[at] = id;
                    fprintf(stderr, "yw4: screen const: %s %016" PRIx64 " cbuf %u row %" PRIu64 ".%c = %g (%g/%g) target %ux%u"
                            " row: %g %g %g %g\n", stage_names[st], sh->key, b->slot, k / 4, "xyzw"[k % 4], v, m ? m : x,
                            m ? x : 1, w, h, f[k & ~3ull], f[(k & ~3ull) + 1], f[(k & ~3ull) + 2], f[(k & ~3ull) + 3]);
                }
            }
        }
        /* the vertex stage's constant buffers: where, how big, and their first 16 floats */
        for (int i = 0; i < vs->nb; i++) {
            const Binding *b = &vs->b[i];
            if (b->kind != 'c' || b->slot >= UBOS) continue;
            GpuRange ub = s->ubo[0][b->slot];
            fprintf(stderr, "yw4: gpu draw:   vs cbuf %u at %#" PRIx64 " size %" PRIu64 ":", b->slot, ub.addr, ub.size);
            if (ub.size >= 192 && host_readable(ub.addr, 192)) {  /* floats 32..47: where the shadow shaders keep a matrix */
                const float *f = (const float *)(uintptr_t)ub.addr;
                for (int k = 32; k < 48; k++) fprintf(stderr, " %g", f[k]);
                /* a matrix whose second row is 0 0 0 1 and third row 0: the broken shadow cascade */
                static int scanned;
                #define NEAR0(x) ((x) > -1e-4f && (x) < 1e-4f)
                if (!scanned && vs->nb && NEAR0(f[36]) && NEAR0(f[37]) && NEAR0(f[38]) && f[39] > 0.999f && f[39] < 1.001f &&
                    NEAR0(f[40]) && NEAR0(f[41]) && NEAR0(f[42]) && NEAR0(f[43]) && !NEAR0(f[32])) {
                    scanned = 1;
                    fprintf(stderr, "\nyw4: scan: broken matrix in the cbuf at %#" PRIx64 ": write watch on its matrix %#" PRIx64 "\n",
                            ub.addr, ub.addr + 128);
                    /* watch disabled: the command-level watch is armed in the generated code */  
                }
            }
            fprintf(stderr, "\n");
        }
    }

    gpu_taa_note_targets(s);
    dyn_model = g->sh[ST_VERT] && g->sh[ST_VERT]->model;
    if (dyn_model && !s->after_composite) gpu_note_model_draw();
    /* A layer over the whole screen (widescreen), by the textures it samples:
     * - a 16:9 texture from the game's files, a picture made for the screen (the map's backdrop 960x540,
     *   the copyright notice 1920x1080, movies: 1920x1080 and 960x540 planes): kept 16:9 in the middle
     *   with black bars at the sides, also over the 3D world the game still draws behind the map;
     * - a 16:9 texture the game renders into (the map's UI, drawn into a 1920x1080 image first): kept
     *   16:9 like the rest of the UI;
     * - anything else is stretched: the dimming behind a dialog (a part of a 2048x2304 sheet), the
     *   loading screen's black (no texture). A full-screen pass program (input 0 is the position) only
     *   with no texture at all: it also copies other images. */
    int samples = 0, picture = 0, layer = 0;
    for (int i = 0; g->sh[ST_FRAG] && i < g->sh[ST_FRAG]->nb; i++) {
        const Binding *b = &g->sh[ST_FRAG]->b[i];
        if (b->kind != 't' || b->slot >= TEXTURES) continue;
        samples = 1;
        NvnTexEntry *e = tex_entry(s, s->tex[nvn_stage[ST_FRAG]][b->slot]);
        if (e && (uint64_t)e->tex->width * 9 == (uint64_t)e->tex->height * 16) {
            if (e->tex->pool_flags & POOL_CPU_NO_ACCESS) layer = 1;
            else picture = 1;
        }
    }
    int cover = s->after_composite && gpu_wide() && !dyn_reads_screen && !layer ? covers_screen(s, vs, r) : 0;
    int full = cover == 2;  /* a 16:9 picture gets bars only when it fills the screen; a band of one stays 16:9 */
    /* a band across the width stays 16:9 in a frame whose picture got black bars (the map's bar at the bottom) */
    static uint64_t bars_frame = UINT64_MAX;
    int barred = __atomic_load_n(&bars_frame, __ATOMIC_RELAXED) == gpu_frames();
    dyn_full_screen = (full || (cover && !barred)) && !picture && !(vs->pos_input && samples);
    const GpuImage *ti = s->ncolor && s->rt.color[0] ? image_find(s->rt.color[0]) : NULL;
    int bars = full && picture && ti && whole_width(s, ti) && !gpu_is_presented(ti->key.addr);
    if (bars) __atomic_store_n(&bars_frame, gpu_frames(), __ATOMIC_RELAXED);
    if (dyn_full_screen || bars) {
        static int logged;
        if (logged++ < 8)
            fprintf(stderr, "yw4: gpu: widescreen: UI layer over the whole %s %s (vs %016" PRIx64 " fs %016" PRIx64
                            ", frame %" PRIu64 ")\n", full ? "screen" : "width",
                    bars ? "kept 16:9 with black bars" : "stretched", vs->key,
                    g->sh[ST_FRAG] ? g->sh[ST_FRAG]->key : 0, gpu_frames());
    }
    /* an image narrower than 16:9 (16:10): the UI is fitted to the width, smaller, with a strip left above and
     * below (gpu_dynamic_state). A layer over the whole screen is fitted like the rest, so its bands stay where
     * the content on them is, and drawn twice more for the strips (dyn_edge); a band across the width is
     * simply fitted (across, fitting and stretching are the same there) */
    int edges = 0;
    if (dyn_full_screen && ti && ti->wide_num < ti->wide_den && !gpu_is_presented(ti->key.addr) &&
        s->have_viewport && whole_width(s, ti)) {
        dyn_full_screen = 0;
        edges = full ? 2 : 0;
    }
    gpu_dynamic_state(cb, s, k.depth, w, h);
    if (bars) {  /* the sides black, then the picture in the middle */
        uint32_t inner = gpu_scaled(ti->key.width, ti->scale_num, ti->scale_den), side = w > inner ? (w - inner) / 2 : 0;
        VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
                                .clearValue.color = {{0, 0, 0, 1}}};
        VkClearRect cr[2] = {{{{0, 0}, {side, h}}, 0, 1}, {{{(int32_t)(w - side), 0}, {side, h}}, 0, 1}};
        if (ti->wide_num < ti->wide_den) {  /* narrower than 16:9: the picture is fitted to the width, bars above and below */
            inner = gpu_scaled(gpu_scaled(ti->key.height, ti->scale_num, ti->scale_den), ti->wide_num, ti->wide_den);
            side = h > inner ? (h - inner) / 2 : 0;
            cr[0].rect = (VkRect2D){{0, 0}, {w, side}}, cr[1].rect = (VkRect2D){{0, (int32_t)(h - side)}, {w, side}};
        }
        if (side) vkCmdClearAttachments(cb, 1, &ca, 2, cr);
    }
    uint32_t pos_fix = nvn_depth_mode == 0 ? (g->sh[ST_GEOM] ? 2u : 1u) : 0u;
    vkCmdPushConstants(cb, g->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, 4, &pos_fix);

    for (int pass = 0; pass <= edges; pass++) {  /* edges: the strips above and below too (16:10) */
    if (pass) dyn_edge = pass, gpu_dynamic_state(cb, s, k.depth, w, h);
    if (quads) {  /* (primitive, first, count, base instance, instances) */
        uint32_t nquads = (uint32_t)r->x[2] / 4;
        if (nquads > QUADS) gpu_stop("%u quads in one draw", nquads);
        vkCmdBindIndexBuffer(cb, quad_indices(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, nquads * 6, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1, 0, (int32_t)r->x[1],
                         (uint32_t)r->x[3]);
    } else if (r->op == CMD_DRAW_ARRAYS) {  /* (primitive, first, count, base instance, instances) */
        vkCmdDraw(cb, (uint32_t)r->x[2], (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1, (uint32_t)r->x[1],
                  (uint32_t)r->x[3]);
    } else {  /* (primitive, index type, count, index address, base vertex, base instance, instances) */
        uint32_t type = (uint32_t)r->x[1], size = type == 1 ? 2 : 4;
        if (type == 0) gpu_stop("8-bit indices");
        VkBuffer buf;
        VkDeviceSize off;
        if (!gpu_geometry_at(r->x[3], (uint64_t)r->x[2] * size, &buf, &off))
            gpu_stop("index buffer %#" PRIx64 " outside the memory pools", r->x[3]);
        vkCmdBindIndexBuffer(cb, buf, off, type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, (uint32_t)r->x[2], (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1, 0, (int32_t)r->x[4],
                         (uint32_t)r->x[5]);
    }
    }
    capture_after_draw(cb, s, g);
    /* the bloom composite, the last 3D pass before the UI: anti-aliasing here (gpu_aa.c) */
    if (g->sh[ST_FRAG] && g->sh[ST_FRAG]->dof) gpu_dof_after_composite(cb, s);  /* YW4_DOF_PASS=check */
    if (g->sh[ST_FRAG] && g->sh[ST_FRAG]->key == COMPOSITE_SHADER) {
        gpu_bloom_after_composite(cb, s);  /* YW4_BLOOM=check: the game's results, before anti-aliasing */
        s->after_composite = 1;
        if (gpu_aa() || gpu_upscale()) gpu_aa_apply(cb, s);
    }
}

void gpu_draw(VkCommandBuffer cb, GpuState *s, const CmdRec *r) { draw(cb, s, r); }

/* CMD_NATIVE: a G-buffer draw of ours (gbuffer.c). On the game's frames of YW4_GBUFFER_ALTERNATE,
 * the game's DrawElements it replaced instead, with the game's programs and buffers. */
void gpu_gbuffer_draw(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    const GBufferDraw *d = data;
    if (size < sizeof *d) return;
    CmdRec r;
    memset(&r, 0, sizeof r);
    r.size = sizeof r;
    if (!gbuffer_native_frame(gpu_frames())) {
        memcpy(r.x, d->game, sizeof r.x);
        r.op = d->game_indexed ? CMD_DRAW_ELEMENTS : CMD_DRAW_ARRAYS;
        if (d->game_instanced) {
            /* the game's instanced call: its base vertex, base instance and instances as recorded */
        } else if (d->game_indexed) {
            r.x[4] = 0, r.x[5] = 0, r.x[6] = 1;  /* base vertex 0, base instance 0, one instance */
        } else {
            r.x[3] = 0, r.x[4] = 1;  /* base instance 0, one instance */
        }
        draw(vk_cb, gpu_state, &r);
        return;
    }
    if (d->index_count) {  /* (primitive, index type, count, index address, base vertex, base instance, instances) */
        r.op = CMD_DRAW_ELEMENTS;
        r.x[0] = d->primitive, r.x[1] = d->index_type, r.x[2] = d->index_count, r.x[3] = d->indices;
        r.x[5] = d->base_instance, r.x[6] = d->instances;
    } else {  /* (primitive, first, count, base instance, instances) */
        r.op = CMD_DRAW_ARRAYS;
        r.x[0] = d->primitive, r.x[2] = d->vertex_count, r.x[3] = d->base_instance, r.x[4] = d->instances;
    }
    own = d;
    draw(vk_cb, gpu_state, &r);
    own = NULL;
}

/* CMD_NATIVE: a takeover draw (gbuffer.c, decision 0030). The engine did no state work for it: its
 * program, NVN state objects, vertex layout, material textures (fragment slots) and constant blocks
 * go into the executor's state for this one draw and are put back after, so the game's next draw
 * finds the state the game's own commands left. Everything else (targets, scene constants and
 * textures, stencil reference, depth bias) is what the game bound for the layer. */
void gpu_takeover_draw(void *vk_cb, void *gpu_state, const void *data, uint32_t size) {
    const TakeoverDraw *t = data;
    GpuState *s = gpu_state;
    if (size < sizeof *t) return;
    NvnProgram *prog = t->program ? (NvnProgram *)(uintptr_t)*(const uint64_t *)(uintptr_t)t->program : NULL;
    if (!prog || prog->stages[0].code != t->vs_code || prog->stages[1].code != t->fs_code) {
        static int logged;
        if (logged++ < 4) fprintf(stderr, "yw4: takeover: program %#" PRIx64 " is no longer the one recorded: draw lost\n",
                                  t->program);
        return;
    }
    /* what we change, saved */
    NvnProgram *program = s->program;
    uint8_t blend[8][8], color[4], ds[8], polygon[4];
    uint32_t mask = s->channel_mask, attribs[16], nattribs = s->nattribs, streams[16][2], nstreams = s->nstreams;
    uint64_t tex[TAKEOVER_TEXTURES];
    uint32_t sref[2], smask[2];
    float offset[3];
    memcpy(sref, s->stencil_ref, sizeof sref), memcpy(smask, s->stencil_mask, sizeof smask);
    memcpy(offset, s->polygon_offset, sizeof offset);
    memcpy(blend, s->blend, sizeof blend), memcpy(color, s->color, 4), memcpy(ds, s->depth_stencil, 8);
    memcpy(polygon, s->polygon, 4), memcpy(attribs, s->attribs, sizeof attribs), memcpy(streams, s->streams, sizeof streams);
    memcpy(tex, s->tex[1], sizeof tex);
    /* ours */
    s->program = prog;
    memcpy(s->blend, t->nvn, 0x40);
    memcpy(&s->channel_mask, t->nvn + 0x40, 4);
    memcpy(s->color, t->nvn + 0x44, 4);
    memcpy(s->depth_stencil, t->nvn + 0x48, 8);
    memcpy(s->polygon, t->nvn + 0x68, 4);
    memcpy(s->attribs, t->attribs, sizeof t->attribs);
    s->nattribs = t->nattribs;
    for (int i = 0; i < 16; i++) s->streams[i][0] = t->stride, s->streams[i][1] = 0;
    s->nstreams = 16;
    /* the textures: our copies of their pool entries, each pointing at its own snapshot */
    static _Thread_local NvnTexEntry ents[TAKEOVER_TEXTURES];
    for (int k = 0; k < TAKEOVER_TEXTURES; k++)
        if (t->tex_mask >> k & 1) {
            ents[k] = takeover_tex(t, (unsigned)k)->e;
            ents[k].tex = &ents[k].snap;
            s->tex[1][k] = 1ull << 63 | (uint64_t)k;
        }
    own_tex = ents;
    /* stencil reference and write mask, polygon offset: what the engine's commit sets from the state it
     * keeps (f_974cd0: state+0x2b8, +0x2b4; +0x2c0, +0x2bc, +0x2c4 = factor, units, clamp) */
    uint32_t ref, wmask;
    memcpy(&ref, t->nvn + 0x78, 4), memcpy(&wmask, t->nvn + 0x74, 4);
    s->stencil_ref[0] = s->stencil_ref[1] = ref, s->stencil_mask[0] = s->stencil_mask[1] = wmask;
    memcpy(&s->polygon_offset[0], t->nvn + 0x80, 4), memcpy(&s->polygon_offset[1], t->nvn + 0x7c, 4);
    memcpy(&s->polygon_offset[2], t->nvn + 0x84, 4);
    CmdRec r;
    memset(&r, 0, sizeof r);
    r.size = sizeof r;
    const GBufferDraw *d = &t->d;
    if (d->index_count) {  /* (primitive, index type, count, index address, base vertex, base instance, instances) */
        r.op = CMD_DRAW_ELEMENTS;
        r.x[0] = d->primitive, r.x[1] = d->index_type, r.x[2] = d->index_count, r.x[3] = d->indices, r.x[6] = d->instances ? d->instances : 1;
    } else {  /* (primitive, first, count, base instance, instances) */
        r.op = CMD_DRAW_ARRAYS;
        r.x[0] = d->primitive, r.x[2] = d->vertex_count, r.x[4] = d->instances ? d->instances : 1;
    }
    own = d, own_t = t;
    draw(vk_cb, s, &r);
    own = NULL, own_t = NULL, own_tex = NULL;
    /* the game's, back */
    s->program = program;
    memcpy(s->blend, blend, sizeof blend), memcpy(s->color, color, 4), memcpy(s->depth_stencil, ds, 8);
    memcpy(s->polygon, polygon, 4), memcpy(s->attribs, attribs, sizeof attribs), memcpy(s->streams, streams, sizeof streams);
    s->channel_mask = mask, s->nattribs = nattribs, s->nstreams = nstreams;
    memcpy(s->tex[1], tex, sizeof tex);
    memcpy(s->stencil_ref, sref, sizeof sref), memcpy(s->stencil_mask, smask, sizeof smask);
    memcpy(s->polygon_offset, offset, sizeof offset);
}
