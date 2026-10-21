/* NVN objects, builders and state objects: everything but command recording (decision 0017).
 *
 * Headless for now: objects record what the game asks for; nothing is drawn. Only the NVN
 * functions the game uses are here (work/recon/nvn.md lists 177); any other one stops with
 * its name (nvn.c).
 *
 * Builder field offsets, state packing and the stage detection in ProgramSetShaders are
 * adapted from LAS Recomp (runtime/nvn/objects.c; MIT, see third_party/lasrecomp).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "host.h"
#include "kernel.h"
#include "nvn.h"
#include "pixels.h"
#include "window.h"
#include "yw4rt.h"

static void *alloc(size_t n) {
    void *p = calloc(1, n);
    if (!p) {
        fprintf(stderr, "yw4: NVN: out of host memory\n");
        exit(3);
    }
    return p;
}

/* ================================================================== device */

NVN(nvnDeviceBuilderSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnDeviceBuilderSetFlags) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }
NVN(nvnDeviceInitialize) {
    fprintf(stderr, "yw4: NVN device initialised (flags %#x), headless\n", G(uint32_t, A(1), 0));
    RET(1);
}
/* Window origin (lower/upper left) and clip-space depth (-w..w or 0..w): the draw path
 * follows them (gpu_draw.c). */
uint32_t nvn_window_origin, nvn_depth_mode;
NVN(nvnDeviceSetWindowOriginMode) {
    if (nvn_window_origin != (uint32_t)A(1))
        fprintf(stderr, "yw4: NVN window origin %s\n", A(1) ? "upper left" : "lower left");
    nvn_window_origin = (uint32_t)A(1);
}
NVN(nvnDeviceSetDepthMode) {
    if (nvn_depth_mode != (uint32_t)A(1)) fprintf(stderr, "yw4: NVN depth mode: near at %s\n", A(1) ? "0" : "-w");
    nvn_depth_mode = (uint32_t)A(1);
}
NVN(nvnDeviceRegisterFastClearDepth) { RET(1); }  /* (device, depth): a hint for compression */
NVN(nvnDeviceFinalizeCommandHandle) { (void)c; }
/* YW4_TRACE_TEXPOOL=<id>: texture and sampler pools (diagnostic). Every pool, registration
 * counts per pool, and in detail what touches texture ids id-10..id+10 (1: around 3003). */
int nvn_trace_texpool(uint32_t id) {
    static int on = -1;
    static uint32_t center;
    if (on < 0) {
        const char *e = getenv("YW4_TRACE_TEXPOOL");
        on = e && *e && *e != '0';
        center = on ? (uint32_t)strtoul(e, NULL, 0) : 0;
        if (center <= 1) center = 3003;
    }
    if (!on) return 0;
    return id == UINT32_MAX || (id + 10 >= center && id <= center + 10) ? 1 : -1;
}
/* "caller <- caller's caller" of a guest call, for the trace */
static const char *trace_callers(const CPUState *c, char buf[320], uint64_t *hash) {
    uint64_t st[6];
    int n = yw4_call_stack(c, st, 6);
    char w[48];
    buf[0] = 0;
    uint64_t h = 0xCBF29CE484222325ull;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(buf);
        snprintf(buf + l, 320 - l, "%s%s", i ? " <- " : "", yw4_describe(st[i] - 4, w));
        h = (h ^ st[i]) * 0x100000001B3ull;
    }
    if (hash) *hash = h;
    return buf;
}

/* Handles a shader uses to name descriptors: texture ID in the low 20 bits, sampler ID above. */
NVN(nvnDeviceGetTextureHandle) {
    if (nvn_trace_texpool((uint32_t)A(1)) > 0) {  /* once per texture id and call stack */
        enum { SEEN = 1024 };
        static uint64_t seen[SEEN];
        static int nseen;
        char b[320];
        uint64_t h;
        trace_callers(c, b, &h);
        h ^= A(1) * 0x9E3779B97F4A7C15ull;
        int i = 0;
        while (i < nseen && seen[i] != h) i++;
        if (i == nseen && nseen < SEEN) {
            seen[nseen++] = h;
            fprintf(stderr, "yw4: texpool: GetTextureHandle(tex %u, sampler %u) = %#" PRIx64 " from %s\n", (unsigned)A(1),
                    (unsigned)A(2), (uint64_t)((A(1) & 0xFFFFF) | (A(2) & 0xFFF) << 20), b);
        }
    }
    RET((A(1) & 0xFFFFF) | (A(2) & 0xFFF) << 20);
}
NVN(nvnDeviceGetImageHandle) { RET(A(1) & 0xFFFFF); }
/* (device, const NVNcounterData*): the timestamp at +8 is in GPU ticks, 614.4 MHz. */
NVN(nvnDeviceGetTimestampInNanoseconds) { RET((uint64_t)((unsigned __int128)G(uint64_t, A(1), 8) * 625 / 384)); }

/* ================================================================== queue
 * QueueBuilder: device@0 flags@8 command@0xC compute@0x10 control@0x14 memory@0x18 memsize@0x20
 * flush threshold@0x28 */

NVN(nvnQueueBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnQueueBuilderSetDefaults) {
    memset(AP(0), 0, 0x30);
    G(uint32_t, A(0), 0x0C) = 0x10000;
    G(uint32_t, A(0), 0x10) = 0x40000;
    G(uint32_t, A(0), 0x14) = 0x4000;
    G(uint32_t, A(0), 0x28) = 0x8000;
}
NVN(nvnQueueBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetCommandMemorySize) { G(uint32_t, A(0), 0x0C) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetComputeMemorySize) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetCommandFlushThreshold) { G(uint32_t, A(0), 0x28) = (uint32_t)A(1); }
NVN(nvnQueueBuilderSetQueueMemory) { G(uint64_t, A(0), 0x18) = A(1); G(uint64_t, A(0), 0x20) = A(2); }
/* How much queue memory the game must provide for these settings. The driver keeps its own
 * command and compute memory in it; ours needs none, but the game must allocate something. */
NVN(nvnQueueBuilderGetQueueMemorySize) { RET(0x10000); }

NVN(nvnQueueInitialize) { RET(1); }
NVN(nvnQueueFinalize) { (void)c; }

static uint64_t frames;

NVN(nvnQueueSubmitCommands) {  /* (queue, count, const NVNcommandHandle*) */
    int n = (int)A(1);
    const uint64_t *handles = AP(2);
    for (int i = 0; i < n; i++) nvn_execute(handles[i]);
}
/* The display flips at vsync and shows each frame for `interval` vsyncs; NVN only hands
 * out a window texture again once the display is done with it. Headless, presenting waits
 * for that flip directly, which paces the game as the Switch does (60 / interval). */
static uint64_t next_flip;

/* The presented texture, read back from the game's memory as RGBA8 rows for the window. */
static void show(NvnTexture *t, uint64_t frame, const int crop[4]) {
    static uint8_t *texels, *rgba;
    static size_t cap;
    static uint32_t logged_format;
    if (t && gpu_ready()) {
        gpu_present(t, frame, crop);
        return;
    }
    if (!t || !t->addr || t->target != 1 /* 2D */ || t->layout.fmt->bw != 1) return;
    uint32_t w = t->width, h = t->height, bpb = t->layout.fmt->bpb;
    size_t n = (size_t)w * h;
    if (n > cap) {
        free(texels), free(rgba);
        texels = malloc(n * bpb), rgba = malloc(n * 4);
        if (!texels || !rgba) exit(3);
        cap = n;
    }
    tegra_read_region(&t->layout, (const uint8_t *)(uintptr_t)t->addr, 0, 0, 0, 0, 0, w, h, 1, texels);
    for (size_t i = 0; i < n; i++)
        if (!pixel_to_rgba8(t->format, texels + i * bpb, rgba + 4 * i)) {
            if (logged_format != t->format) fprintf(stderr, "yw4: NVN: can't show window format %#x yet\n", t->format);
            logged_format = t->format;
            return;
        }
    window_present_rgba(rgba, w, h, frame);
}

NVN(nvnQueuePresentTexture) {  /* (queue, window, texture index) */
    NvnWindow *w = HOST(NvnWindow, A(1));
    int index = (int)A(2);
    int64_t perf_enter = host_now_ns();
    if (index >= 0 && index < w->num) show(HOST(NvnTexture, w->textures[index]), frames + 1, w->crop);
    int64_t perf_shown = host_now_ns();
    yw4_vsync_start();
    uint64_t now = yw4_vsync_count();
    if (next_flip < now + 1) next_flip = now + 1;
    yw4_wait_vsync(next_flip);
    next_flip += (uint64_t)(w->interval > 0 ? w->interval : 1);
    static int64_t last_log;  /* host time of the last logged frame, for the rate since then */
    static uint64_t last_frames;
    frames++;
    uint64_t draws = nvn_take_draw_count();
    yw4_perf_frame(frames, perf_enter, perf_shown, host_now_ns(), draws);
    nvn_stats_frame(frames);
    if (frames <= 3 || frames % 300 == 0) {
        int64_t now = host_now_ns();
        double fps = last_log ? (double)(frames - last_frames) * 1e9 / (double)(now - last_log) : 0;
        fprintf(stderr, "yw4: NVN frame %" PRIu64 " presented (texture %d, %" PRIu64 " draws, %.1f frames/s)\n", frames,
                (int)A(2), draws, fps);
        last_log = now, last_frames = frames;
    }
}

/* Syncs: a sync keeps, in the game's NVNsync at +0, the number of the GPU submission it
 * waits for (gpu.c, decision 0021). Fencing takes the latest submission. Headless, GPU work
 * is done the moment it is submitted and every sync is signalled. One queue: the GPU never
 * has to wait for another queue (QueueWaitSync). */
NVN(nvnQueueFenceSync) { G(uint64_t, A(1), 0) = gpu_submitted(); }  /* (queue, sync, condition, flags) */
NVN(nvnQueueWaitSync) { RET(1); }  /* (queue, sync) */
NVN(nvnSyncInitialize) { G(uint64_t, A(0), 0) = 0; RET(1); }
NVN(nvnSyncFinalize) { (void)c; }
/* (sync, timeout ns) -> ALREADY_SIGNALED 0, CONDITION_SATISFIED 1, TIMEOUT_EXPIRED 2 */
NVN(nvnSyncWait) { RET(gpu_wait(G(uint64_t, A(0), 0), A(1))); }

/* ================================================================== memory pools
 * Builder: device@0 flags@8 storage@0x10 size@0x18. GPU addresses are guest addresses. */

NVN(nvnMemoryPoolBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnMemoryPoolBuilderSetDefaults) { memset(AP(0), 0, 0x20); G(uint32_t, A(0), 8) = 0x9; }
NVN(nvnMemoryPoolBuilderSetFlags) { G(uint32_t, A(0), 8) = (uint32_t)A(1); }
NVN(nvnMemoryPoolBuilderSetStorage) { G(uint64_t, A(0), 0x10) = A(1); G(uint64_t, A(0), 0x18) = A(2); }

NVN(nvnMemoryPoolInitialize) {
    NvnPool *p = alloc(sizeof *p);
    p->storage = G(uint64_t, A(1), 0x10);
    p->size = G(uint64_t, A(1), 0x18);
    p->flags = G(uint32_t, A(1), 8);
    nvn_stats_pool(p->flags, p->size);
    if (nvn_stats_enabled())
        fprintf(stderr, "yw4: NVN pool %#llx + %#llx flags %#x\n", (unsigned long long)p->storage,
                (unsigned long long)p->size, p->flags);
    gpu_pool_init(p);
    SET_HOST(A(0), p);
    RET(1);
}
NVN(nvnMemoryPoolFinalize) {
    gpu_pool_finalize(HOST(NvnPool, A(0)));
    free(HOST(NvnPool, A(0)));
    SET_HOST(A(0), 0);
}
NVN(nvnMemoryPoolMap) { RET(HOST(NvnPool, A(0))->storage); }
NVN(nvnMemoryPoolGetFlags) { RET(HOST(NvnPool, A(0))->flags); }
NVN(nvnMemoryPoolFlushMappedRange) { (void)c; }       /* CPU writes visible to the GPU: shared memory already */
NVN(nvnMemoryPoolInvalidateMappedRange) { (void)c; }

/* ================================================================== buffers
 * Builder: device@0 pool@8 offset@0x10 size@0x18. Buffer: address@0 pool@8 offset@0x10 size@0x18 */

NVN(nvnBufferBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnBufferBuilderSetDefaults) { memset((uint8_t *)AP(0) + 8, 0, 0x18); }
NVN(nvnBufferBuilderSetStorage) { G(uint64_t, A(0), 8) = A(1); G(uint64_t, A(0), 0x10) = A(2); G(uint64_t, A(0), 0x18) = A(3); }
NVN(nvnBufferInitialize) {
    uint64_t pool = G(uint64_t, A(1), 8), off = G(uint64_t, A(1), 0x10);
    G(uint64_t, A(0), 0) = HOST(NvnPool, pool)->storage + off;
    G(uint64_t, A(0), 8) = pool;
    G(uint64_t, A(0), 0x10) = off;
    G(uint64_t, A(0), 0x18) = G(uint64_t, A(1), 0x18);
    RET(1);
}
NVN(nvnBufferFinalize) { (void)c; }
NVN(nvnBufferMap) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferGetAddress) { RET(G(uint64_t, A(0), 0)); }
NVN(nvnBufferFlushMappedRange) { (void)c; }
NVN(nvnBufferInvalidateMappedRange) { (void)c; }

/* ================================================================== textures
 * TextureBuilder: device@0 flags@8 target@0xC width@0x10 height@0x14 depth@0x18 levels@0x1C
 *   format@0x20 samples@0x24 swizzle@0x28 ds mode@0x2C stride@0x30 pool@0x38 offset@0x40
 *   packaged layout@0x50 */

#define TB(T, off) G(T, A(0), off)

NVN(nvnTextureBuilderSetDevice) { TB(uint64_t, 0) = A(1); }
NVN(nvnTextureBuilderSetDefaults) {
    memset((uint8_t *)AP(0) + 8, 0, 0x58);
    TB(uint32_t, 0xC) = TARGET_2D;
    TB(uint32_t, 0x10) = TB(uint32_t, 0x14) = TB(uint32_t, 0x18) = TB(uint32_t, 0x1C) = 1;
    TB(uint8_t, 0x28) = 2, TB(uint8_t, 0x29) = 3, TB(uint8_t, 0x2A) = 4, TB(uint8_t, 0x2B) = 5;  /* R G B A */
}
NVN(nvnTextureBuilderSetFlags) { TB(uint32_t, 8) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetTarget) { TB(uint32_t, 0xC) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSize3D) { TB(uint32_t, 0x10) = (uint32_t)A(1), TB(uint32_t, 0x14) = (uint32_t)A(2), TB(uint32_t, 0x18) = (uint32_t)A(3); }
NVN(nvnTextureBuilderSetLevels) { TB(uint32_t, 0x1C) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetFormat) { TB(uint32_t, 0x20) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSamples) { TB(uint32_t, 0x24) = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSwizzle) {
    for (int i = 0; i < 4; i++) TB(uint8_t, 0x28 + i) = (uint8_t)A(1 + i);
}
NVN(nvnTextureBuilderSetStorage) { TB(uint64_t, 0x38) = A(1); TB(uint64_t, 0x40) = A(2); }
NVN(nvnTextureBuilderSetPackagedTextureLayout) { TB(uint64_t, 0x50) = A(1) ? *(const uint64_t *)AP(1) : 0; }

static const FormatInfo *format_of(const CPUState *c, uint32_t format) {
    const FormatInfo *f = nvn_format_info(format);
    if (!f) nvn_fatal(c, "texture format %#x not known", format);
    return f;
}

/* The texture's storage layout from a builder. Multisampled textures are stored as one
 * bigger surface: 2x as 2x1 samples per pixel, 4x as 2x2, 8x as 4x2 (Maxwell). */
static void builder_layout(const CPUState *c, uint64_t b, TexLayout *t) {
    uint32_t target = G(uint32_t, b, 0xC), samples = G(uint32_t, b, 0x24);
    memset(t, 0, sizeof *t);
    t->fmt = format_of(c, G(uint32_t, b, 0x20));
    t->width = G(uint32_t, b, 0x10);
    t->height = target == TARGET_1D || target == TARGET_1D_ARRAY ? 1 : G(uint32_t, b, 0x14);
    t->depth = target == TARGET_3D ? G(uint32_t, b, 0x18) : 1;
    t->layers = target == TARGET_1D_ARRAY ? G(uint32_t, b, 0x14)
              : target == TARGET_CUBE ? 6
              : target == TARGET_2D_ARRAY || target == TARGET_2D_MS_ARRAY || target == TARGET_CUBE_ARRAY ? G(uint32_t, b, 0x18)
              : 1;
    if (!t->layers) t->layers = 1;
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    if (samples > 1) {
        if (samples != 2 && samples != 4 && samples != 8) nvn_fatal(c, "%u samples per pixel", samples);
        t->width *= samples == 8 ? 4 : 2;
        if (samples >= 4) t->height *= 2;
    }
    tegra_layout_init(t);
    /* Packaged textures were laid out offline with their own block height: the low word of
     * NVNpackagedTextureLayout is log2(GOBs per block). */
    uint64_t packaged = G(uint64_t, b, 0x50);
    if (packaged && (uint32_t)packaged <= 5) t->block_height_log2 = (uint32_t)packaged;
}

static int builder_linear(uint64_t b) { return (G(uint32_t, b, 8) & FLAG_LINEAR) || G(uint32_t, b, 0xC) == TARGET_BUFFER; }

NVN(nvnTextureBuilderGetStorageSize) {
    uint64_t b = A(0);
    if (G(uint32_t, b, 0xC) == TARGET_BUFFER) {
        RET((uint64_t)G(uint32_t, b, 0x10) * format_of(c, G(uint32_t, b, 0x20))->bpb);
    } else if (builder_linear(b)) {
        RET(G(uint64_t, b, 0x30) * G(uint32_t, b, 0x14));  /* stride x height */
    } else {
        TexLayout t;
        builder_layout(c, b, &t);
        RET(tegra_total_size(&t));
    }
}
NVN(nvnTextureBuilderGetStorageAlignment) { RET(builder_linear(A(0)) ? 32 : 512); }

NVN(nvnTextureInitialize) {
    uint64_t b = A(1);
    NvnTexture *t = alloc(sizeof *t);
    t->flags = G(uint32_t, b, 8);
    t->target = G(uint32_t, b, 0xC);
    t->width = G(uint32_t, b, 0x10), t->height = G(uint32_t, b, 0x14), t->depth = G(uint32_t, b, 0x18);
    t->levels = G(uint32_t, b, 0x1C) ? G(uint32_t, b, 0x1C) : 1;
    t->format = G(uint32_t, b, 0x20);
    t->samples = G(uint32_t, b, 0x24);
    memcpy(t->swizzle, (const void *)(uintptr_t)(b + 0x28), 4);
    t->ds_mode = G(uint32_t, b, 0x2C);
    t->stride = G(uint64_t, b, 0x30);
    uint64_t pool = G(uint64_t, b, 0x38);
    t->addr = pool ? HOST(NvnPool, pool)->storage + G(uint64_t, b, 0x40) : 0;
    t->pool_flags = pool ? HOST(NvnPool, pool)->flags : 0;
    if (t->target != TARGET_BUFFER) builder_layout(c, b, &t->layout);
    if (nvn_stats_enabled() && (t->format == 0x4d || t->flags & FLAG_LINEAR))
        fprintf(stderr, "yw4: NVN texture %ux%u format %#x flags %#x packaged %#llx stride %llu block height log2 %u\n",
                t->width, t->height, t->format, t->flags, (unsigned long long)G(uint64_t, b, 0x50),
                (unsigned long long)t->stride, t->layout.block_height_log2);
    SET_HOST(A(0), t);
    RET(1);
}
NVN(nvnTextureFinalize) {
    static long finalized_registered;
    if (HOST(NvnTexture, A(0))->registered && finalized_registered++ < 5)
        fprintf(stderr, "yw4: NVN: texture %ux%u finalized while registered in a texture pool\n",
                HOST(NvnTexture, A(0))->width, HOST(NvnTexture, A(0))->height);
    gpu_texture_finalize(HOST(NvnTexture, A(0)));
    free(HOST(NvnTexture, A(0)));
    SET_HOST(A(0), 0);
}
NVN(nvnTextureGetFlags) { RET(HOST(NvnTexture, A(0))->flags); }

/* (texture, view or NULL, const NVNcopyRegion*, data): tightly packed linear texels into the
 * texture's storage. Region: x, y, z offsets, then width, height, depth, in texels; for
 * array and cube textures z and depth pick layers. The view can choose the level (its base
 * level) and the first layer. */
NVN(nvnTextureWriteTexels) {
    NvnTexture *t = HOST(NvnTexture, A(0));
    const NvnTextureView *v = AP(1);
    const int32_t *r = AP(2);
    if (t->flags & FLAG_LINEAR || t->target == TARGET_BUFFER) nvn_fatal(c, "WriteTexels to a linear texture");
    if (v && v->setmask & ~0xFu) nvn_fatal(c, "WriteTexels through a view with target or depth-stencil mode set (%#x)", v->setmask);
    if (v && v->setmask & 4) {  /* a view format: the bytes go in as they are if its blocks have the same size */
        const FormatInfo *vf = format_of(c, v->format);
        if (vf->bpb != t->layout.fmt->bpb || vf->bw != t->layout.fmt->bw || vf->bh != t->layout.fmt->bh)
            nvn_fatal(c, "WriteTexels through a view of format %#x, texture format %#x", v->format, t->format);
    }  /* a view swizzle changes how shaders read, not the bytes */
    if (!r || r[0] < 0 || r[1] < 0 || r[2] < 0 || r[3] <= 0 || r[4] <= 0 || r[5] <= 0) nvn_fatal(c, "WriteTexels region");
    uint32_t level = v && v->setmask & 1 ? v->base_level : 0, layer = v && v->setmask & 2 ? v->min_layer : 0;
    uint32_t w = t->layout.width >> level, h = t->layout.height >> level, d = t->layout.depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    int layered = t->layout.layers > 1 || t->target == TARGET_2D_ARRAY || t->target == TARGET_CUBE ||
                  t->target == TARGET_CUBE_ARRAY || t->target == TARGET_1D_ARRAY;
    uint32_t x = (uint32_t)r[0], y = (uint32_t)r[1], z = (uint32_t)r[2], rw = (uint32_t)r[3], rh = (uint32_t)r[4],
             rd = (uint32_t)r[5];
    if (level >= t->layout.levels || x + rw > w || y + rh > h ||
        (layered ? layer + z + rd > t->layout.layers : z + rd > d))
        nvn_fatal(c, "WriteTexels region (%u,%u,%u)+(%u,%u,%u) outside level %u (%ux%ux%u, %u layers)", x, y, z, rw, rh,
                  rd, level, w, h, d, t->layout.layers);
    const FormatInfo *f = t->layout.fmt;
    uint64_t slice = (uint64_t)((rw + f->bw - 1) / f->bw) * ((rh + f->bh - 1) / f->bh) * f->bpb;
    const uint8_t *src = AP(3);
    nvn_stats_texels(slice * rd);
    t->cpu_written = 1;
    if (layered) {
        for (uint32_t k = 0; k < rd; k++)
            tegra_write_region(&t->layout, (uint8_t *)(uintptr_t)t->addr, level, layer + z + k, x, y, 0, rw, rh, 1,
                               src + k * slice);
    } else {
        tegra_write_region(&t->layout, (uint8_t *)(uintptr_t)t->addr, level, layer, x, y, z, rw, rh, rd, src);
    }
}

/* Texture views: our layout of the 0x28-byte NVNtextureView, a set-mask and the fields */
NVN(nvnTextureViewSetDefaults) { memset(AP(0), 0, 0x28); }
NVN(nvnTextureViewSetLevels) {
    NvnTextureView *v = AP(0);
    v->setmask |= 1, v->base_level = (uint16_t)A(1), v->num_levels = (uint16_t)A(2);
}
NVN(nvnTextureViewSetLayers) {
    NvnTextureView *v = AP(0);
    v->setmask |= 2, v->min_layer = (uint16_t)A(1), v->num_layers = (uint16_t)A(2);
}
NVN(nvnTextureViewSetFormat) { NvnTextureView *v = AP(0); v->setmask |= 4, v->format = (uint32_t)A(1); }
NVN(nvnTextureViewSetSwizzle) {
    NvnTextureView *v = AP(0);
    v->setmask |= 8;
    for (int i = 0; i < 4; i++) v->swizzle[i] = (uint8_t)A(1 + i);
}

/* ================================================================== samplers
 * SamplerBuilder: device@0, then an NvnSamplerDesc at 8 (fits in the 0x60-byte builder) */

#define SD ((NvnSamplerDesc *)((uint8_t *)AP(0) + 8))
NVN(nvnSamplerBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnSamplerBuilderSetDefaults) {
    NvnSamplerDesc *d = SD;
    memset(d, 0, sizeof *d);
    d->wrap[0] = d->wrap[1] = d->wrap[2] = 1;  /* REPEAT */
    d->lod_max = 1000.0f;
    d->max_aniso = 1.0f;
    d->compare_func = 4;
}
NVN(nvnSamplerBuilderSetMinMagFilter) { SD->min_filter = (uint32_t)A(1), SD->mag_filter = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetWrapMode) { for (int i = 0; i < 3; i++) SD->wrap[i] = (uint32_t)A(1 + i); }
NVN(nvnSamplerBuilderSetLodClamp) { SD->lod_min = AF(0), SD->lod_max = AF(1); }
NVN(nvnSamplerBuilderSetLodBias) { SD->lod_bias = AF(0); }
NVN(nvnSamplerBuilderSetCompare) { SD->compare_mode = (uint32_t)A(1), SD->compare_func = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetBorderColor) { memcpy(SD->border, AP(1), 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy) { SD->max_aniso = AF(0); }

/* ================================================================== texture and sampler pools
 * Pool: host@0 memory pool@8 offset@16 count@24 */

static void desc_pool_init(CPUState *c, size_t entry) {  /* (pool, memory pool, offset, count) */
    NvnDescPool *d = alloc(sizeof *d);
    d->addr = HOST(NvnPool, A(1))->storage + A(2);
    d->count = (int)A(3);
    d->entries = alloc((size_t)d->count * entry);
    if (nvn_trace_texpool(UINT32_MAX) > 0)
        fprintf(stderr, "yw4: texpool: %s pool %p (guest %#" PRIx64 ") at %#" PRIx64 ", %d entries\n",
                entry == sizeof(NvnTexEntry) ? "texture" : "sampler", (void *)d, A(0), d->addr, d->count);
    SET_HOST(A(0), d);
    G(uint64_t, A(0), 8) = A(1);
    G(uint64_t, A(0), 16) = A(2);
    G(uint32_t, A(0), 24) = (uint32_t)A(3);
    RET(1);
}
static void desc_pool_finalize(CPUState *c) {
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    if (nvn_trace_texpool(UINT32_MAX) > 0) fprintf(stderr, "yw4: texpool: pool %p finalized\n", (void *)d);
    if (d) free(d->entries), free(d);
    SET_HOST(A(0), 0);
}
static void *desc_entry(CPUState *c, size_t entry) {  /* (pool, id, ...) */
    NvnDescPool *d = HOST(NvnDescPool, A(0));
    int id = (int)A(1);
    if (id < 0 || id >= d->count) nvn_fatal(c, "descriptor %d outside a pool of %d", id, d->count);
    return (uint8_t *)d->entries + (size_t)id * entry;
}

NVN(nvnTexturePoolInitialize) { desc_pool_init(c, sizeof(NvnTexEntry)); }
NVN(nvnTexturePoolFinalize) { desc_pool_finalize(c); }
static void register_texture(CPUState *c) {  /* (pool, id, texture, view or NULL) */
    NvnTexEntry *e = desc_entry(c, sizeof *e);
    NvnTexture *t = HOST(NvnTexture, A(2));
    t->registered++;
    e->snap = *t;  /* a copy, as the hardware's descriptor is */
    e->snap.gpu = NULL;
    e->tex = &e->snap;
    e->has_view = A(3) != 0;
    if (e->has_view) memcpy(&e->view, AP(3), sizeof e->view);
    int tr = nvn_trace_texpool((uint32_t)A(1));
    if (tr) {
        enum { POOLS = 16 };
        static const void *pools[POOLS];
        static long regs[POOLS], detail;
        const void *d = HOST(NvnDescPool, A(0));
        int i = 0;
        while (i < POOLS && pools[i] && pools[i] != d) i++;
        if (i < POOLS) {
            pools[i] = d;
            if (!(++regs[i] & (regs[i] - 1)))  /* powers of two */
                fprintf(stderr, "yw4: texpool: pool %p: %ld registrations\n", d, regs[i]);
        }
        char b[320];
        if (tr > 0 && detail++ < 400)
            fprintf(stderr, "yw4: texpool: pool %p id %u <- %ux%u format %#x at %#" PRIx64 " target %u levels %u%s from %s\n",
                    d, (unsigned)A(1), t->width, t->height, t->format, t->addr, t->target, t->levels,
                    e->has_view ? " (view)" : "", trace_callers(c, b, NULL));
    }
}
NVN(nvnTexturePoolRegisterTexture) { register_texture(c); }
NVN(nvnTexturePoolRegisterImage) { register_texture(c); }

NVN(nvnSamplerPoolInitialize) { desc_pool_init(c, sizeof(NvnSamplerDesc)); }
NVN(nvnSamplerPoolFinalize) { desc_pool_finalize(c); }
NVN(nvnSamplerPoolRegisterSamplerBuilder) {  /* (pool, id, builder) */
    *(NvnSamplerDesc *)desc_entry(c, sizeof(NvnSamplerDesc)) = *(const NvnSamplerDesc *)((uint8_t *)AP(2) + 8);
}

/* ================================================================== programs */

NVN(nvnProgramInitialize) { SET_HOST(A(0), alloc(sizeof(NvnProgram))); RET(1); }
NVN(nvnProgramFinalize) {
    gpu_program_finalize(HOST(NvnProgram, A(0)));
    free(HOST(NvnProgram, A(0)));
    SET_HOST(A(0), 0);
}
/* (program, count, const NVNshaderData*): each {u64 code address, const void *control}. The
 * stage comes from the Maxwell shader program header after the 0x30-byte NVN header; code
 * without the header magic is compute. */
NVN(nvnProgramSetShaders) {
    NvnProgram *p = HOST(NvnProgram, A(0));
    int n = (int)A(1);
    const uint64_t *data = AP(2);
    gpu_program_finalize(p);
    memset(p, 0, sizeof *p);
    for (int i = 0; i < n; i++) {
        uint64_t code = data[2 * i], control = data[2 * i + 1];
        int stage = 5;
        if (G(uint32_t, code, 0) == 0x12345678) switch ((G(uint32_t, code, 0x30) >> 10) & 0xF) {
            case 1: stage = 0; break;  /* vertex */
            case 5: stage = 1; break;  /* fragment */
            case 4: stage = 2; break;  /* geometry */
            case 2: stage = 3; break;  /* tessellation control */
            case 3: stage = 4; break;  /* tessellation evaluation */
            }
        uint64_t key = nvn_shader_key(control);
        if (!key) nvn_fatal(c, "shader stage %d has no NV_gpu_program5 assembly in its control section", stage);
        static const char *const names[6] = {"vert", "frag", "geom", "tesc", "tese", "comp"};
        nvn_stats_shader(key, names[stage]);
        p->stages[stage] = (NvnShaderStage){code, control, key};
        p->stage_mask |= 1u << stage;
    }
    RET(1);
}

/* ================================================================== window
 * WindowBuilder: device@0 native window@8 count@0x10 textures@0x18 interval@0x20 active@0x24 */

NVN(nvnWindowBuilderSetDevice) { G(uint64_t, A(0), 0) = A(1); }
NVN(nvnWindowBuilderSetDefaults) { memset((uint8_t *)AP(0) + 8, 0, 0x20); G(uint32_t, A(0), 0x20) = 1; }
NVN(nvnWindowBuilderSetNativeWindow) { G(uint64_t, A(0), 8) = A(1); }
NVN(nvnWindowBuilderSetTextures) { G(uint32_t, A(0), 0x10) = (uint32_t)A(1); G(uint64_t, A(0), 0x18) = A(2); }

NVN(nvnWindowInitialize) {
    NvnWindow *w = alloc(sizeof *w);
    w->num = (int)G(uint32_t, A(1), 0x10);
    if (w->num < 1 || w->num > 8) nvn_fatal(c, "window with %d textures", w->num);
    memcpy(w->textures, (const void *)(uintptr_t)G(uint64_t, A(1), 0x18), (size_t)w->num * 8);
    w->interval = (int)G(uint32_t, A(1), 0x20);
    w->active = G(uint32_t, A(1), 0x24) ? (int)G(uint32_t, A(1), 0x24) : w->num;
    SET_HOST(A(0), w);
    fprintf(stderr, "yw4: NVN window with %d textures\n", w->num);
    RET(1);
}
NVN(nvnWindowFinalize) { free(HOST(NvnWindow, A(0))); SET_HOST(A(0), 0); }
NVN(nvnWindowSetCrop) { NvnWindow *w = HOST(NvnWindow, A(0)); for (int i = 0; i < 4; i++) w->crop[i] = (int)A(1 + i); }
NVN(nvnWindowSetPresentInterval) {
    HOST(NvnWindow, A(0))->interval = (int)A(1);
    fprintf(stderr, "yw4: NVN present interval %d (%d frames/s at most)\n", (int)A(1), A(1) ? 60 / (int)A(1) : 60);
}
NVN(nvnWindowAcquireTexture) {  /* (window, sync signalled when it's free, int *index) */
    NvnWindow *w = HOST(NvnWindow, A(0));
    if (A(1)) G(uint64_t, A(1), 0) = gpu_submitted();  /* free once what was submitted has run */
    *(int32_t *)AP(2) = w->next;
    w->next = (w->next + 1) % w->active;
    RET(0);  /* NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS */
}

/* ================================================================== small state objects
 * The game binds these by value (the command buffer copies them), so their fields live in
 * the guest object, packed into its ABI size. */

/* BlendState (8 bytes): target, src colour, dst colour, src alpha, dst alpha, eq colour, eq alpha */
NVN(nvnBlendStateSetDefaults) {
    static const uint8_t d[8] = {0, 2, 1, 2, 1, 1, 1, 0};  /* ONE, ZERO, ONE, ZERO, ADD, ADD */
    memcpy(AP(0), d, 8);
}
NVN(nvnBlendStateSetBlendTarget) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnBlendStateSetBlendFunc) { for (int i = 0; i < 4; i++) G(uint8_t, A(0), 1 + i) = (uint8_t)A(1 + i); }
NVN(nvnBlendStateSetBlendEquation) { G(uint8_t, A(0), 5) = (uint8_t)A(1); G(uint8_t, A(0), 6) = (uint8_t)A(2); }

/* ChannelMaskState (4 bytes): 4 bits per colour target */
NVN(nvnChannelMaskStateSetDefaults) { G(uint32_t, A(0), 0) = 0xFFFFFFFFu; }
NVN(nvnChannelMaskStateSetChannelMask) {  /* (state, target, r, g, b, a) */
    unsigned t = (unsigned)A(1) & 7;
    uint32_t m = (A(2) ? 1u : 0) | (A(3) ? 2u : 0) | (A(4) ? 4u : 0) | (A(5) ? 8u : 0);
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~(0xFu << (4 * t))) | m << (4 * t);
}

/* ColorState (4 bytes): blend enable per target, logic op, alpha test */
NVN(nvnColorStateSetDefaults) { G(uint8_t, A(0), 0) = 0; G(uint8_t, A(0), 1) = 3; G(uint8_t, A(0), 2) = 8; }
NVN(nvnColorStateSetBlendEnable) {
    uint8_t bit = (uint8_t)(1u << (A(1) & 7));
    G(uint8_t, A(0), 0) = A(2) ? G(uint8_t, A(0), 0) | bit : G(uint8_t, A(0), 0) & ~bit;
}

/* DepthStencilState (8 bytes): depth test, depth write, depth func, stencil test, stencil func
 * (front | back << 4), then the stencil ops (fail, depth fail, pass) in 4-bit fields, front then back */
NVN(nvnDepthStencilStateSetDefaults) {
    static const uint8_t d[8] = {0, 1, 2, 0, 0x88, 0x11, 0x11, 0x11};
    memcpy(AP(0), d, 8);
}
NVN(nvnDepthStencilStateSetDepthTestEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthWriteEnable) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetDepthFunc) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilTestEnable) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilFunc) {  /* (state, face: 1 front 2 back 3 both, func) */
    uint8_t *d = AP(0);
    if (A(1) & 1) d[4] = (uint8_t)((d[4] & 0xF0) | (A(2) & 0xF));
    if (A(1) & 2) d[4] = (uint8_t)((d[4] & 0x0F) | (A(2) & 0xF) << 4);
}
NVN(nvnDepthStencilStateSetStencilOp) {  /* (state, face, fail, depth fail, pass) */
    uint8_t *d = AP(0);
    uint32_t ops = (uint32_t)((A(2) & 0xF) | (A(3) & 0xF) << 4 | (A(4) & 0xF) << 8);
    uint32_t packed = d[5] | d[6] << 8 | d[7] << 16;
    if (A(1) & 1) packed = (packed & ~0xFFFu) | ops;
    if (A(1) & 2) packed = (packed & 0xFFFu) | ops << 12;
    d[5] = (uint8_t)packed, d[6] = (uint8_t)(packed >> 8), d[7] = (uint8_t)(packed >> 16);
}

/* MultisampleState (0x18 bytes): enable@0 samples@4 ... */
NVN(nvnMultisampleStateSetDefaults) { memset(AP(0), 0, 0x18); G(uint8_t, A(0), 0) = 1; G(uint8_t, A(0), 0xD) = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }

/* PolygonState (4 bytes): cull face, front face, polygon mode, offset enables */
NVN(nvnPolygonStateSetDefaults) { G(uint8_t, A(0), 0) = 0; G(uint8_t, A(0), 1) = 1; G(uint8_t, A(0), 2) = 2; G(uint8_t, A(0), 3) = 0; }
NVN(nvnPolygonStateSetCullFace) { G(uint8_t, A(0), 0) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetFrontFace) { G(uint8_t, A(0), 1) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonMode) { G(uint8_t, A(0), 2) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonOffsetEnables) { G(uint8_t, A(0), 3) = (uint8_t)A(1); }

/* VertexAttribState (4 bytes): format bits 0-7, stream bits 8-12, relative offset bits 13-31 */
NVN(nvnVertexAttribStateSetDefaults) { G(uint32_t, A(0), 0) = 0; }
NVN(nvnVertexAttribStateSetFormat) {  /* (state, format, relative offset) */
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & 0x1F00u) | (uint32_t)(A(1) & 0xFF) | (uint32_t)(A(2) << 13);
}
NVN(nvnVertexAttribStateSetStreamIndex) {
    G(uint32_t, A(0), 0) = (G(uint32_t, A(0), 0) & ~0x1F00u) | (uint32_t)((A(1) & 0x1F) << 8);
}

/* VertexStreamState (8 bytes): stride, divisor */
NVN(nvnVertexStreamStateSetDefaults) { G(uint64_t, A(0), 0) = 0; }
NVN(nvnVertexStreamStateSetStride) { G(uint32_t, A(0), 0) = (uint32_t)A(1); }

const NvnImpl nvn_object_impls[] = {
    IMPL(nvnDeviceBuilderSetDefaults), IMPL(nvnDeviceBuilderSetFlags), IMPL(nvnDeviceInitialize),
    IMPL(nvnDeviceSetWindowOriginMode), IMPL(nvnDeviceSetDepthMode), IMPL(nvnDeviceRegisterFastClearDepth),
    IMPL(nvnDeviceFinalizeCommandHandle), IMPL(nvnDeviceGetTextureHandle), IMPL(nvnDeviceGetImageHandle),
    IMPL(nvnDeviceGetTimestampInNanoseconds),
    IMPL(nvnQueueBuilderSetDevice), IMPL(nvnQueueBuilderSetDefaults), IMPL(nvnQueueBuilderSetFlags),
    IMPL(nvnQueueBuilderSetCommandMemorySize), IMPL(nvnQueueBuilderSetComputeMemorySize),
    IMPL(nvnQueueBuilderSetCommandFlushThreshold), IMPL(nvnQueueBuilderSetQueueMemory),
    IMPL(nvnQueueBuilderGetQueueMemorySize),
    IMPL(nvnQueueInitialize), IMPL(nvnQueueFinalize), IMPL(nvnQueueSubmitCommands), IMPL(nvnQueuePresentTexture),
    IMPL(nvnQueueFenceSync), IMPL(nvnQueueWaitSync), IMPL(nvnSyncInitialize), IMPL(nvnSyncFinalize), IMPL(nvnSyncWait),
    IMPL(nvnMemoryPoolBuilderSetDevice), IMPL(nvnMemoryPoolBuilderSetDefaults), IMPL(nvnMemoryPoolBuilderSetFlags),
    IMPL(nvnMemoryPoolBuilderSetStorage), IMPL(nvnMemoryPoolInitialize), IMPL(nvnMemoryPoolFinalize),
    IMPL(nvnMemoryPoolMap), IMPL(nvnMemoryPoolGetFlags), IMPL(nvnMemoryPoolFlushMappedRange),
    IMPL(nvnMemoryPoolInvalidateMappedRange),
    IMPL(nvnBufferBuilderSetDevice), IMPL(nvnBufferBuilderSetDefaults), IMPL(nvnBufferBuilderSetStorage),
    IMPL(nvnBufferInitialize), IMPL(nvnBufferFinalize), IMPL(nvnBufferMap), IMPL(nvnBufferGetAddress),
    IMPL(nvnBufferFlushMappedRange), IMPL(nvnBufferInvalidateMappedRange),
    IMPL(nvnTextureBuilderSetDevice), IMPL(nvnTextureBuilderSetDefaults), IMPL(nvnTextureBuilderSetFlags),
    IMPL(nvnTextureBuilderSetTarget), IMPL(nvnTextureBuilderSetSize3D), IMPL(nvnTextureBuilderSetLevels),
    IMPL(nvnTextureBuilderSetFormat), IMPL(nvnTextureBuilderSetSamples), IMPL(nvnTextureBuilderSetSwizzle),
    IMPL(nvnTextureBuilderSetStorage), IMPL(nvnTextureBuilderSetPackagedTextureLayout),
    IMPL(nvnTextureBuilderGetStorageSize), IMPL(nvnTextureBuilderGetStorageAlignment),
    IMPL(nvnTextureInitialize), IMPL(nvnTextureFinalize), IMPL(nvnTextureGetFlags), IMPL(nvnTextureWriteTexels),
    IMPL(nvnTextureViewSetDefaults), IMPL(nvnTextureViewSetLevels), IMPL(nvnTextureViewSetLayers),
    IMPL(nvnTextureViewSetFormat), IMPL(nvnTextureViewSetSwizzle),
    IMPL(nvnSamplerBuilderSetDevice), IMPL(nvnSamplerBuilderSetDefaults), IMPL(nvnSamplerBuilderSetMinMagFilter),
    IMPL(nvnSamplerBuilderSetWrapMode), IMPL(nvnSamplerBuilderSetLodClamp), IMPL(nvnSamplerBuilderSetLodBias),
    IMPL(nvnSamplerBuilderSetCompare), IMPL(nvnSamplerBuilderSetBorderColor), IMPL(nvnSamplerBuilderSetMaxAnisotropy),
    IMPL(nvnTexturePoolInitialize), IMPL(nvnTexturePoolFinalize), IMPL(nvnTexturePoolRegisterTexture),
    IMPL(nvnTexturePoolRegisterImage), IMPL(nvnSamplerPoolInitialize), IMPL(nvnSamplerPoolFinalize),
    IMPL(nvnSamplerPoolRegisterSamplerBuilder),
    IMPL(nvnProgramInitialize), IMPL(nvnProgramFinalize), IMPL(nvnProgramSetShaders),
    IMPL(nvnWindowBuilderSetDevice), IMPL(nvnWindowBuilderSetDefaults), IMPL(nvnWindowBuilderSetNativeWindow),
    IMPL(nvnWindowBuilderSetTextures), IMPL(nvnWindowInitialize), IMPL(nvnWindowFinalize), IMPL(nvnWindowSetCrop),
    IMPL(nvnWindowSetPresentInterval), IMPL(nvnWindowAcquireTexture),
    IMPL(nvnBlendStateSetDefaults), IMPL(nvnBlendStateSetBlendTarget), IMPL(nvnBlendStateSetBlendFunc),
    IMPL(nvnBlendStateSetBlendEquation),
    IMPL(nvnChannelMaskStateSetDefaults), IMPL(nvnChannelMaskStateSetChannelMask),
    IMPL(nvnColorStateSetDefaults), IMPL(nvnColorStateSetBlendEnable),
    IMPL(nvnDepthStencilStateSetDefaults), IMPL(nvnDepthStencilStateSetDepthTestEnable),
    IMPL(nvnDepthStencilStateSetDepthWriteEnable), IMPL(nvnDepthStencilStateSetDepthFunc),
    IMPL(nvnDepthStencilStateSetStencilTestEnable), IMPL(nvnDepthStencilStateSetStencilFunc),
    IMPL(nvnDepthStencilStateSetStencilOp),
    IMPL(nvnMultisampleStateSetDefaults), IMPL(nvnMultisampleStateSetMultisampleEnable),
    IMPL(nvnPolygonStateSetDefaults), IMPL(nvnPolygonStateSetCullFace), IMPL(nvnPolygonStateSetFrontFace),
    IMPL(nvnPolygonStateSetPolygonMode), IMPL(nvnPolygonStateSetPolygonOffsetEnables),
    IMPL(nvnVertexAttribStateSetDefaults), IMPL(nvnVertexAttribStateSetFormat), IMPL(nvnVertexAttribStateSetStreamIndex),
    IMPL(nvnVertexStreamStateSetDefaults), IMPL(nvnVertexStreamStateSetStride),
    {NULL, NULL},
};
