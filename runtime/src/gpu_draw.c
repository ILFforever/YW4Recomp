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
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gbuffer.h"
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
    uint32_t cb8_bytes;  /* ... and how much of cbuf 8 it reads */
} Shader;

static Shader *shaders;

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

/* Our replacements for game programs (decision 0026): readable GLSL in runtime/shaders with the
 * translated program's interface, used in its place when the G-buffer pass is on (F6). */
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
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char kind, dim[32] = "";
        unsigned slot;
        if (sscanf(line, "%c %u %31s", &kind, &slot, dim) < 2) continue;
        if (kind == 'a') {
            if (slot < 32) s->inputs[slot] = dim[0];
            continue;
        }
        if (kind == 'r') continue;  /* rows read from a constant buffer: for gbuffer.c */
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
    /* The shaders that sample a shadow map average 5 taps spread by a cbuf 8 value the game sets
     * (half a texel of the 1024 map and up): cbuf 8 d[31].zw in most, d[8].zw in the rest. Found
     * in the GLSL, so shadow quality can scale it with the map (bind_stage_set). */
    s->pcf_float = -1;
    for (int i = 0; i < s->nb; i++)
        if (s->b[i].kind == 't' && s->b[i].shadow) {
            snprintf(path, sizeof path, "%s/%016" PRIx64 ".%s", shader_dir(), key, stage_names[stage]);
            char *glsl = read_file(path, &n);
            if (glsl) {
                s->pcf_float = strstr(glsl, "cb8.d[31].zw") ? 31 * 4 + 2 : strstr(glsl, "cb8.d[8].zw") ? 8 * 4 + 2 : -1;
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
            break;
        }
    VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = (uint32_t)s->nb, .pBindings = lb};
    CHECK(vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &s->layout));
    s->ours = replacement(key, stage);
    return s;
}

static Shader *shader(uint64_t key, int stage) {
    for (Shader *s = shaders; s; s = s->next)
        if (s->key == key && s->stage == stage) return s;
    Shader *s = load_shader(key, stage);
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
} Program;

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
    CHECK(vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &ci, NULL, &g->comp_pipe));
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

static VkPipeline pipeline(Program *g, const PipeKey *k) {
    for (Pipe *p = g->pipes; p; p = p->next)
        if (!memcmp(&p->key, k, sizeof *k)) return p->pipeline;
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
    Pipe *p = calloc(1, sizeof *p);
    p->key = *k;
    int64_t t0 = host_now_ns();
    CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pci, NULL, &p->pipeline));
    int64_t took = host_now_ns() - t0;
    if (took > 100000000)  /* a slow build stalls the frame: say which shaders */
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
        fprintf(stderr, "yw4: gpu: prebuilt pipeline (frag %016" PRIx64 ") in %lld ms\n", prebuilt[i].frag,
                (long long)((host_now_ns() - t0) / 1000000));
    }
}

void gpu_prebuild_start(void) {
    const char *e = getenv("YW4_PREBUILD");
    if (e && *e == '0') return;
    if (host_thread_start(prebuild_thread, NULL, 1 << 20))
        fprintf(stderr, "yw4: gpu: cannot start the pipeline prebuild thread\n");
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
    if (s->rt.depth) {
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
static NvnTexEntry *tex_entry(const GpuState *s, uint64_t handle) {
    uint32_t id = (uint32_t)(handle & 0xFFFFF);
    if (!s->tex_pool || !handle || (int)id >= s->tex_pool->count) return NULL;
    NvnTexEntry *e = (NvnTexEntry *)s->tex_pool->entries + id;
    return e->tex ? e : NULL;
}

static const NvnSamplerDesc *sampler_entry(const GpuState *s, uint64_t handle) {
    uint32_t id = (uint32_t)(handle >> 20 & 0xFFF);
    if (!s->sampler_pool || (int)id >= s->sampler_pool->count) return NULL;
    return (const NvnSamplerDesc *)s->sampler_pool->entries + id;
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
    ii->sampler = gpu_sampler(&d);
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
/* Shadow quality (gpu.c, F8): with shadow maps larger than the game's, a shader that samples one
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

static int pcf_scaled_copy(uint64_t addr, VkDeviceSize range, int pcf_float, VkBuffer *buf, VkDeviceSize *offset) {
    int res = gpu_shadow_res();
    if (res == 1024 || range > 65536 || (VkDeviceSize)(pcf_float + 2) * 4 > range) return 0;
    uint8_t *p = ring_alloc(range, buf, offset);
    if (!p) return 0;
    memcpy(p, (const void *)(uintptr_t)addr, (size_t)range);
    float *spread = (float *)p + pcf_float;
    spread[0] *= 1024.0f / (float)res, spread[1] *= 1024.0f / (float)res;
    return 1;
}

/* Soft shadows (PCSS, yw4_shadow in recomp/shader.py): off, or on with a strength. x2 by default
 * (the owner's pick, with 4096 maps); F7 cycles it, YW4_PCSS=0..3 sets it at start. The settings go to each draw from the ring:
 *   mode  = on, (unused), largest radius, smallest radius
 *   scale = radius per unit of depth gap for cascades 0..3 (YW4_PCSS_SCALE=a,b,c,d to try others)
 *   tune  = smallest depth gap that counts as a caster (YW4_PCSS_EPS)
 * Radii are in map coordinates (0..1), so they don't change with the map's size (F8); the smallest
 * is half a texel of the current map, the game's own filter spread. */
static int pcss_level = -1;
static float pcss_scale[4] = {0.01f, 0.005f, 0.0025f, 0.0025f}, pcss_eps, pcss_max = 6.0f / 1024.0f;
static int pcss_debug;  /* YW4_PCSS_DEBUG: 1 shows casters found / 16, 2 the radius / largest (white = 1) */

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
        __atomic_store_n(&pcss_level, l, __ATOMIC_RELEASE);
    }
    return l;
}

void gpu_pcss_cycle(void) {
    int l = (gpu_pcss_level() + 1) % 4;
    __atomic_store_n(&pcss_level, l, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: PCSS %s (F7)\n", l ? (l == 1 ? "x1" : l == 2 ? "x2" : "x4") : "off");
}

static int pcss_settings(VkBuffer *buf, VkDeviceSize *offset) {
    float *f = (float *)ring_alloc(48, buf, offset);
    if (!f) return 0;
    int l = gpu_pcss_level();
    float k = (float)(1 << (l ? l - 1 : 0));
    f[0] = l ? 1.0f : 0.0f, f[1] = 0.0f, f[2] = pcss_max, f[3] = 0.5f / (float)gpu_shadow_res();
    for (int i = 0; i < 4; i++) f[4 + i] = pcss_scale[i] * k;
    f[8] = pcss_eps, f[9] = (float)pcss_debug, f[10] = f[11] = 0.0f;
    return 1;
}

/* A draw of our G-buffer pass (gbuffer.h): its own vertex and index buffers and vertex constants 2
 * in place of the bound ones. */
static _Thread_local const GBufferDraw *own;

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
                if (own && ns == 0 && b->kind == 'c' && b->slot == 2) {  /* our object constants */
                    uint8_t *p = ring_alloc(sizeof own->object, &bi[i].buffer, &bi[i].offset);
                    if (p) memcpy(p, own->object, sizeof own->object), range = sizeof own->object;
                    else bi[i].buffer = dummy_buffer(), bi[i].offset = 0, range = 65536;
                } else if (b->kind == 'c' && b->slot == 8 && sh->pcf_float >= 0 && r.size &&
                    pcf_scaled_copy(r.addr, range < sh->cb8_bytes ? range : sh->cb8_bytes, sh->pcf_float,
                                    &bi[i].buffer, &bi[i].offset)) {
                    /* a copy with the shadow filter spread scaled to the shadow maps' size */
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
                bi[i].range = 48;
                w[i].dstBinding = 95;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                w[i].pBufferInfo = &bi[i];
            } else if (b->kind == 't' || b->kind == 'd') {
                uint64_t handle = b->slot < TEXTURES ? s->tex[ns][b->slot] : 0;
                NvnTexEntry *e = tex_entry(s, handle);
                const NvnSamplerDesc *sd = sampler_entry(s, handle);
                NvnSamplerDesc d = sd ? *sd : (NvnSamplerDesc){.min_filter = 1, .mag_filter = 1, .lod_max = 1000};
                if (b->shadow && !d.compare_mode) d.compare_mode = 1, d.compare_func = 4;  /* LEQUAL */
                if (!b->shadow) d.compare_mode = 0;
                if (b->kind == 'd') d.min_filter = d.mag_filter = 0;  /* raw depth: nearest texel */
                ii[i].imageView = e ? gpu_sample_view(cb, e->tex, &e->view, e->has_view, b->view)
                                    : gpu_dummy_view(cb, b->view);
                ii[i].sampler = gpu_sampler(&d);
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

/* YW4_CAPTURE_FS=<fragment shader key prefixes, comma separated> with YW4_DUMP_IMAGE_AT=<frame>:
 * after each draw with such a fragment shader in that frame, its colour targets are copied to host
 * memory and written as work/dump/cap_<n>_<fs>_t<target>.raw (u32 width, height, VkFormat, then
 * the texels). Unlike YW4_DUMP_IMAGE this sees a target mid-frame, before later passes reuse it. */
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
    static uint64_t at;
    static int init;
    if (!init) {
        init = 1;
        want = getenv("YW4_CAPTURE_FS");
        at = getenv("YW4_DUMP_IMAGE_AT") ? strtoull(getenv("YW4_DUMP_IMAGE_AT"), NULL, 10) : 0;
    }
    if (!want || gpu_frames() != at || !g->sh[ST_FRAG]) return;
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
    snprintf(name, sizeof name, "cap_%02d_%.8s", ncaps, key);
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

void gpu_cull(VkCommandBuffer cb, uint8_t cull_mode, uint8_t front_face) {
    static const VkCullModeFlags cull[4] = {VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT,
                                            VK_CULL_MODE_FRONT_AND_BACK};
    vkCmdSetCullMode(cb, cull[cull_mode & 3]);
    int ccw = front_face == 1;
    if (nvn_window_origin != 1) ccw = !ccw;
    vkCmdSetFrontFace(cb, ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
}

/* The dynamic state of the pipelines (gpu_draw's and the native passes'), from s: viewport
 * (flipped for the upper-left origin), scissor, cull, front face, depth, stencil, bias. */
void gpu_dynamic_state(VkCommandBuffer cb, const GpuState *s, VkFormat depth, uint32_t w, uint32_t h) {
    int flip = nvn_window_origin == 1;
    /* a target made larger than the game's texture (YW4_SHADOW_RES): the game's viewport and
     * scissor scaled with it */
    const GpuImage *tg = s->rt.depth ? image_find(s->rt.depth) : NULL;
    int64_t k = tg && tg->scale > 1 ? tg->scale : 1;
    float vx = 0, vy = 0, vw = (float)w, vh = (float)h;
    if (s->have_viewport) vx = (float)(s->viewport[0] * k), vy = (float)(s->viewport[1] * k),
                          vw = (float)(s->viewport[2] * k), vh = (float)(s->viewport[3] * k);
    VkViewport vp = {vx, flip ? vy + vh : vy, vw, flip ? -vh : vh, s->depth_range[0], s->depth_range[1]};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc = {{0, 0}, {w, h}};
    if (s->have_scissor) {
        int64_t x = s->scissor[0] < 0 ? 0 : s->scissor[0] * k, y = s->scissor[1] < 0 ? 0 : s->scissor[1] * k;
        int64_t sw = s->scissor[2] > 0 ? s->scissor[2] * k : 0, sh = s->scissor[3] > 0 ? s->scissor[3] * k : 0;
        if (k > 1) {  /* the game's "whole target" scissor can be huge: keep it inside the target */
            if (x > w) x = w;
            if (y > h) y = h;
            if (sw > w - x) sw = w - x;
            if (sh > h - y) sh = h - y;
        }
        sc = (VkRect2D){{(int32_t)x, (int32_t)y}, {(uint32_t)sw, (uint32_t)sh}};
    }
    vkCmdSetScissor(cb, 0, 1, &sc);
    static const VkCullModeFlags cull[4] = {VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT,
                                            VK_CULL_MODE_FRONT_AND_BACK};
    (void)cull;
    gpu_cull(cb, s->polygon[0], s->polygon[1]);
    const uint8_t *ds = s->depth_stencil;
    int has_depth = depth != VK_FORMAT_UNDEFINED && depth != VK_FORMAT_S8_UINT;
    int has_stencil = depth == VK_FORMAT_S8_UINT || depth == VK_FORMAT_D24_UNORM_S8_UINT ||
                      depth == VK_FORMAT_D32_SFLOAT_S8_UINT;
    vkCmdSetDepthTestEnable(cb, has_depth && ds[0]);
    vkCmdSetDepthWriteEnable(cb, has_depth && ds[1]);
    vkCmdSetDepthCompareOp(cb, compare_fn(ds[2]));
    vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    vkCmdSetStencilTestEnable(cb, has_stencil && ds[3]);
    uint32_t ops = ds[5] | ds[6] << 8 | ds[7] << 16;
    for (int face = 0; face < 2; face++) {
        uint32_t o = ops >> (12 * face);
        VkStencilFaceFlags f = face ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilOp(cb, f, stencil_op(o & 0xF), stencil_op(o >> 8 & 0xF), stencil_op(o >> 4 & 0xF),
                          compare_fn(face ? ds[4] >> 4 : ds[4] & 0xF));
        vkCmdSetStencilReference(cb, f, s->stencil_ref[face]);
        vkCmdSetStencilWriteMask(cb, f, s->stencil_mask[face]);
        vkCmdSetStencilCompareMask(cb, f, s->stencil_value_mask[face]);
    }
    vkCmdSetDepthBiasEnable(cb, (s->polygon[3] & 4) != 0);
    vkCmdSetDepthBias(cb, s->polygon_offset[1], s->polygon_offset[2], s->polygon_offset[0]);
    vkCmdSetBlendConstants(cb, s->blend_color);
    vkCmdSetRasterizerDiscardEnable(cb, s->rasterizer_discard != 0);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
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
    /* our G-buffer programs in place of the game's (decision 0026; F6) */
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
        if (!vr.size || !gpu_buffer_at(vr.addr, vr.size, &buf, &off)) buf = dummy_buffer(), off = 0;
        vkCmdBindVertexBuffers(cb, i, 1, &buf, &off);
    }

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(g, &k));
    bind_sets(cb, s, g);

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

    gpu_dynamic_state(cb, s, k.depth, w, h);
    uint32_t pos_fix = nvn_depth_mode == 0 ? (g->sh[ST_GEOM] ? 2u : 1u) : 0u;
    vkCmdPushConstants(cb, g->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, 4, &pos_fix);

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
        if (!gpu_buffer_at(r->x[3], (uint64_t)r->x[2] * size, &buf, &off))
            gpu_stop("index buffer %#" PRIx64 " outside the memory pools", r->x[3]);
        vkCmdBindIndexBuffer(cb, buf, off, type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, (uint32_t)r->x[2], (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1, 0, (int32_t)r->x[4],
                         (uint32_t)r->x[5]);
    }
    capture_after_draw(cb, s, g);
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
