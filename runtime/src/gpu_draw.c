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

#include "gpu_internal.h"

enum { ST_VERT, ST_FRAG, ST_GEOM, NSETS = 3 };
static const char *const stage_names[NSETS] = {"vert", "frag", "geom"};
static const VkShaderStageFlagBits stage_bits[NSETS] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                                                        VK_SHADER_STAGE_GEOMETRY_BIT};
/* NVN shader stage numbers used by Bind*(stage, ...) and NvnProgram.stages */
static const int nvn_stage[NSETS] = {0, 1, 2};

/* ================================================================== shaders */

enum { MAX_BINDINGS = 64 };
typedef struct {
    char kind;  /* c constant buffer, t texture, i image, s storage buffer */
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

static Shader *shader(uint64_t key, int stage) {
    for (Shader *s = shaders; s; s = s->next)
        if (s->key == key && s->stage == stage) return s;
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
        case 'i': l->binding = 16 + slot, l->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; break;
        case 's': l->binding = 64 + slot, l->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; break;
        default: gpu_stop("shader interface line '%s'", line);
        }
    }
    free(text);
    VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = (uint32_t)s->nb, .pBindings = lb};
    CHECK(vkCreateDescriptorSetLayout(vk.device, &lci, NULL, &s->layout));
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
} Program;

static Program *program_of(NvnProgram *p) {
    if (p->gpu) return p->gpu;
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
                                                          .module = i == ST_FRAG ? debug_fragment(g->sh[i])
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
    CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pci, NULL, &p->pipeline));
    p->next = g->pipes;
    g->pipes = p;
    return p->pipeline;
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
static void begin_rendering(VkCommandBuffer cb, GpuState *s, VkFormat color[8], VkFormat *depth, uint32_t *w,
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
    if (aw == UINT32_MAX) gpu_stop("a draw without render targets");
    *w = aw, *h = ah;
    if (s->rendering) return;
    int has_depth = *depth != VK_FORMAT_UNDEFINED && *depth != VK_FORMAT_S8_UINT;
    int has_stencil = *depth == VK_FORMAT_S8_UINT || *depth == VK_FORMAT_D24_UNORM_S8_UINT ||
                      *depth == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {aw, ah}}, .layerCount = 1,
                          .colorAttachmentCount = n, .pColorAttachments = ca,
                          .pDepthAttachment = has_depth ? &da : NULL, .pStencilAttachment = has_stencil ? &da : NULL};
    vkCmdBeginRendering(cb, &ri);
    s->rendering = 1;
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

static void bind_sets(VkCommandBuffer cb, const GpuState *s, const Program *g) {
    for (int st = 0; st < NSETS; st++) {
        const Shader *sh = g->sh[st];
        if (!sh || !sh->nb) continue;
        int ns = nvn_stage[st];
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
                if (!r.size || !gpu_buffer_at(r.addr, range, &bi[i].buffer, &bi[i].offset)) {
                    bi[i].buffer = dummy_buffer(), bi[i].offset = 0, range = 65536;
                } else {
                    VkDeviceSize align = b->kind == 'c' ? vk.props.limits.minUniformBufferOffsetAlignment
                                                        : vk.props.limits.minStorageBufferOffsetAlignment;
                    if (bi[i].offset % align) gpu_stop("buffer address %#" PRIx64 " not %llu-aligned", r.addr,
                                                       (unsigned long long)align);
                }
                bi[i].range = range;
                w[i].dstBinding = b->kind == 'c' ? b->slot : 64 + b->slot;
                w[i].descriptorType = b->kind == 'c' ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[i].pBufferInfo = &bi[i];
            } else if (b->kind == 't') {
                uint64_t handle = b->slot < TEXTURES ? s->tex[ns][b->slot] : 0;
                NvnTexEntry *e = tex_entry(s, handle);
                const NvnSamplerDesc *sd = sampler_entry(s, handle);
                NvnSamplerDesc d = sd ? *sd : (NvnSamplerDesc){.min_filter = 1, .mag_filter = 1, .lod_max = 1000};
                if (b->shadow && !d.compare_mode) d.compare_mode = 1, d.compare_func = 4;  /* LEQUAL */
                if (!b->shadow) d.compare_mode = 0;
                ii[i].imageView = e ? gpu_sample_view(cb, e->tex, &e->view, e->has_view, b->view)
                                    : gpu_dummy_view(cb, b->view);
                ii[i].sampler = gpu_sampler(&d);
                ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                w[i].dstBinding = 16 + b->slot;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = &ii[i];
            } else {
                gpu_stop("image bindings in draws");
            }
        }
        vkUpdateDescriptorSets(vk.device, (uint32_t)sh->nb, w, 0, NULL);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->layout, (uint32_t)st, 1, &set, 0, NULL);
    }
}

void gpu_draw(VkCommandBuffer cb, GpuState *s, const CmdRec *r) {
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
    begin_rendering(cb, s, k.color, &k.depth, &w, &h);
    k.ncolor = (uint8_t)s->ncolor;
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
        if (!s->vbuf[i].size || !gpu_buffer_at(s->vbuf[i].addr, s->vbuf[i].size, &buf, &off))
            buf = dummy_buffer(), off = 0;
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
    if (trace > 0 && gpu_frames() >= (uint64_t)trace_at) {
        trace--;
        fprintf(stderr, "yw4: gpu draw: vs %016" PRIx64 " fs %016" PRIx64 " prim %u count %u target %ux%u (%d colour, %s depth)"
                " viewport %s%d,%d %dx%d scissor %s%d,%d %dx%d blend %#x\n",
                vs->key, g->sh[ST_FRAG] ? g->sh[ST_FRAG]->key : 0, (unsigned)r->x[0],
                (unsigned)(r->op == CMD_DRAW_ARRAYS ? r->x[2] : r->x[2]), w, h, s->ncolor, s->rt.depth ? "with" : "no",
                s->have_viewport ? "" : "(none) ", s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3],
                s->have_scissor ? "" : "(none) ", s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3],
                s->color[0]);
        for (int st = 0; st < NSETS; st++)
            for (int i = 0; g->sh[st] && i < g->sh[st]->nb; i++) {
                const Binding *b = &g->sh[st]->b[i];
                if (b->kind != 't') continue;
                NvnTexEntry *e = tex_entry(s, s->tex[nvn_stage[st]][b->slot]);
                uint64_t h = s->tex[nvn_stage[st]][b->slot];
                fprintf(stderr, "yw4: gpu draw:   %s texture %u: handle %#" PRIx64 " (id %u, sampler %u) -> ", stage_names[st],
                        b->slot, h, (unsigned)(h & 0xFFFFF), (unsigned)(h >> 20 & 0xFFF));
                if (e) fprintf(stderr, "%ux%u format %#x at %#" PRIx64 "\n", e->tex->width, e->tex->height, e->tex->format,
                               e->tex->addr);
                else fprintf(stderr, "none\n");
            }
    }

    /* Dynamic state */
    int flip = nvn_window_origin == 1;
    float vx = 0, vy = 0, vw = (float)w, vh = (float)h;
    if (s->have_viewport) vx = (float)s->viewport[0], vy = (float)s->viewport[1], vw = (float)s->viewport[2],
                          vh = (float)s->viewport[3];
    VkViewport vp = {vx, flip ? vy + vh : vy, vw, flip ? -vh : vh, s->depth_range[0], s->depth_range[1]};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc = {{0, 0}, {w, h}};
    if (s->have_scissor) {
        int32_t x = s->scissor[0] < 0 ? 0 : s->scissor[0], y = s->scissor[1] < 0 ? 0 : s->scissor[1];
        sc = (VkRect2D){{x, y}, {(uint32_t)(s->scissor[2] > 0 ? s->scissor[2] : 0), (uint32_t)(s->scissor[3] > 0 ? s->scissor[3] : 0)}};
    }
    vkCmdSetScissor(cb, 0, 1, &sc);
    static const VkCullModeFlags cull[4] = {VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT,
                                            VK_CULL_MODE_FRONT_AND_BACK};
    vkCmdSetCullMode(cb, cull[s->polygon[0] & 3]);
    int ccw = s->polygon[1] == 1;
    if (!flip) ccw = !ccw;
    vkCmdSetFrontFace(cb, ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    const uint8_t *ds = s->depth_stencil;
    int has_depth = k.depth != VK_FORMAT_UNDEFINED && k.depth != VK_FORMAT_S8_UINT;
    int has_stencil = k.depth == VK_FORMAT_S8_UINT || k.depth == VK_FORMAT_D24_UNORM_S8_UINT ||
                      k.depth == VK_FORMAT_D32_SFLOAT_S8_UINT;
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
}
