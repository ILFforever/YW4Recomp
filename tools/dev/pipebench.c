/* pipebench: times how long the Vulkan driver takes to build a graphics pipeline from one
 * translated fragment shader (work/shaders/glsl/<key>.frag.spv + .iface + .json), cold: the
 * SPIR-V gets a random OpString so the driver's shader cache can't answer (PB_NOSALT=1: as is; PB_SALT=<n>:
 * a fixed one, to test what the cache keys on; PB_FORMAT=<VkFormat>, PB_BLEND=1: other target state).
 *   pipebench.exe <stub vertex .spv> <glsl dir> <key> [<key> ...]
 * Built and run by tools/dev/pipebench.py (README.md).
 * Descriptor bindings follow runtime/src/gpu_draw.c (c: slot, t: 16+slot, d: 96+slot, p: 95,
 * s: 64+n, i: 16+slot; fragment stage = set 1). Vertex stage: a stub that writes gl_Position. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <vulkan/vulkan.h>

static void *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc(*n + 1);
    fread(p, 1, *n, f);
    p[*n] = 0;
    fclose(f);
    return p;
}

#define CHECK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s: %d\n", #x, r_); exit(1); } } while (0)


/* Copy of the SPIR-V with an OpString "<salt>" inserted before the first debug instruction. */
static uint32_t *salted(const uint32_t *w, size_t nw, size_t *out_n, unsigned salt) {
    char s[32];
    int len = snprintf(s, sizeof s, "pipebench%u", salt);
    size_t sw = (size_t)(len + 4) / 4;  /* string words, nul included */
    uint32_t *o = calloc(nw + 2 + sw, 4);
    size_t at = 5;
    while (at < nw) {
        uint32_t op = w[at] & 0xFFFF, wc = w[at] >> 16;
        if (op == 3 || op == 4 || op == 5 || op == 6 || op == 7 || op == 71 || op == 72) break;  /* debug or annotation */
        at += wc;
    }
    memcpy(o, w, at * 4);
    uint32_t id = o[3]++;  /* bound */
    o[at] = (uint32_t)((2 + sw) << 16) | 7;
    o[at + 1] = id;
    memcpy(&o[at + 2], s, (size_t)len);
    memcpy(&o[at + 2 + sw], w + at, (nw - at) * 4);
    *out_n = (nw + 2 + sw) * 4;
    return o;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "pipebench <stub vertex .spv> <glsl dir> <key>...\n"); return 1; }
    const char *dir = argv[2];
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t np = 8;
    VkPhysicalDevice pds[8];
    CHECK(vkEnumeratePhysicalDevices(inst, &np, pds));
    VkPhysicalDevice pd = pds[0];
    for (uint32_t i = 0; i < np; i++) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(pds[i], &pr);
        if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pd = pds[i];
    }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

    size_t vn;
    uint32_t *vspv = read_file(argv[1], &vn);  /* #version 460 / void main() { gl_Position = vec4(0.0); } */
    if (!vspv) { fprintf(stderr, "no stub vertex shader %s\n", argv[1]); return 1; }
    VkShaderModuleCreateInfo vmci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vn, .pCode = vspv};
    VkShaderModule vmod;
    CHECK(vkCreateShaderModule(dev, &vmci, NULL, &vmod));
    VkDescriptorSetLayoutCreateInfo empty = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout set0;
    CHECK(vkCreateDescriptorSetLayout(dev, &empty, NULL, &set0));
    unsigned salt = (unsigned)GetTickCount() ^ (unsigned)GetCurrentProcessId() << 16;
    const char *e = getenv("PB_SALT");
    if (e) salt = (unsigned)atoi(e);
    VkFormat color_fmt = getenv("PB_FORMAT") ? (VkFormat)atoi(getenv("PB_FORMAT")) : VK_FORMAT_R8G8B8A8_UNORM;
    int blend = getenv("PB_BLEND") != NULL;

    for (int a = 3; a < argc; a++) {
        char path[1024];
        size_t n;
        snprintf(path, sizeof path, "%s/%s.frag.spv", dir, argv[a]);
        uint32_t *spv = read_file(path, &n);
        if (!spv) { fprintf(stderr, "%s: missing\n", path); continue; }
        size_t sn;
        uint32_t *s = getenv("PB_NOSALT") ? (sn = n, spv) : salted(spv, n / 4, &sn, e ? salt : salt + (unsigned)a);
        snprintf(path, sizeof path, "%s/%s.iface", dir, argv[a]);
        char *iface = read_file(path, &n);
        VkDescriptorSetLayoutBinding lb[64];
        int nb = 0;
        for (char *line = strtok(iface, "\n"); line; line = strtok(NULL, "\n")) {
            char kind;
            unsigned slot;
            if (sscanf(line, "%c %u", &kind, &slot) < 2 || kind == 'a') continue;
            VkDescriptorSetLayoutBinding *l = &lb[nb++];
            *l = (VkDescriptorSetLayoutBinding){.descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
            switch (kind) {
            case 'c': l->binding = slot, l->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
            case 't': l->binding = 16 + slot, l->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
            case 'd': l->binding = 96 + slot, l->descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
            case 'p': l->binding = 95, l->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
            case 'i': l->binding = 16 + slot, l->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; break;
            case 's': l->binding = 64 + slot, l->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; break;
            default: nb--;
            }
        }
        VkDescriptorSetLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)nb, .pBindings = lb};
        VkDescriptorSetLayout set1;
        CHECK(vkCreateDescriptorSetLayout(dev, &lci, NULL, &set1));
        VkDescriptorSetLayout sets[2] = {set0, set1};
        VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = sets};
        VkPipelineLayout layout;
        CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));

        /* colour outputs from the .json "colors": [..] (count of entries) */
        snprintf(path, sizeof path, "%s/%s.json", dir, argv[a]);
        char *json = read_file(path, &n);
        int ncolor = 0;
        char *c = json ? strstr(json, "\"colors\": [") : NULL;
        if (c) for (c += 11; *c && *c != ']'; c++) if (*c >= '0' && *c <= '9' && (c[1] == ',' || c[1] == ']')) ncolor = *c - '0' + 1;
        VkFormat fmts[8];
        VkPipelineColorBlendAttachmentState att[8];
        for (int i = 0; i < 8; i++) fmts[i] = color_fmt, att[i] = (VkPipelineColorBlendAttachmentState){.colorWriteMask = 0xF, .blendEnable = (VkBool32)blend,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO};

        VkShaderModuleCreateInfo mci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sn, .pCode = s};
        VkShaderModule fmod;
        CHECK(vkCreateShaderModule(dev, &mci, NULL, &fmod));
        VkPipelineShaderStageCreateInfo st[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vmod, .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fmod, .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = (uint32_t)ncolor, .pAttachments = att};
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
        VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = (uint32_t)ncolor, .pColorAttachmentFormats = fmts};
        VkGraphicsPipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cbs, .pDynamicState = &dy, .layout = layout};
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        VkPipeline pipe;
        VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipe);
        QueryPerformanceCounter(&t1);
        printf("%s %8.1f ms%s\n", argv[a], (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, r ? " (FAILED)" : "");
        fflush(stdout);
        if (!r) vkDestroyPipeline(dev, pipe, NULL);
    }
    return 0;
}
