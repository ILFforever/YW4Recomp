/* The draw path: NVN command lists run on Vulkan (decision 0021).
 *
 * Memory: every memory pool is imported with VK_EXT_external_memory_host and covered by
 * one VkBuffer, so an NVN buffer address is (that buffer, offset) and the GPU reads and
 * writes the game's memory in place. Textures are VkImages, one per NVN texture, made at
 * first use: those in CPU-no-access pools (render targets) are only ever written by the
 * GPU; the others are uploaded from the game's memory (de-swizzled with tegra.c) when made
 * and again after WriteTexels changes them.
 *
 * Execution: gpu_execute records one Vulkan command buffer per NVN command list and submits
 * it, signalling a timeline semaphore with the submission's number; NVN syncs wait for
 * numbers. Every image stays in layout GENERAL and a full memory barrier separates the
 * operations: simple and correct; finer barriers when profiling asks for them. Draws are
 * in gpu_draw.c.
 *
 * The NVN -> Vulkan format table follows LAS Recomp (runtime/nvn/vulkan/textures.c; MIT, see
 * third_party/lasrecomp), as does the sampler translation.
 */
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gbuffer.h"
#include "post.h"
#include "gpu_internal.h"
#include "host.h"
#include "shadow.h"
#include "tegra.h"
#include "window.h"

static int ready;
static HostLock gpu_lock = HOST_LOCK_INIT;  /* recording and submitting */
static VkCommandPool cmd_pool;
static VkSemaphore timeline;
static uint64_t submitted;  /* number of the last submission */

/* Command buffers in a ring; each slot also owns what its submission needs until it has
 * run: descriptor pools and staging buffers. */
enum { RING = 16, MAX_DESC_POOLS = 16, MAX_STAGING = 256 };
typedef struct {
    VkCommandBuffer cb;
    uint64_t seq;  /* the submission that last used it */
    VkDescriptorPool desc[MAX_DESC_POOLS];
    int ndesc;
    struct {
        VkBuffer buf;
        VkDeviceMemory mem;
    } staging[MAX_STAGING];
    int nstaging;
} Slot;
static Slot ring[RING];
static int ring_next;
static Slot *cur;  /* the slot being recorded */

/* GPU timing (YW4 performance log): each submission writes a timestamp at its start and end
 * into its slot's pair of queries; the results are read once the submission has completed
 * and added up per frame. */
static VkQueryPool qpool;
static double ts_period_ns;  /* nanoseconds per timestamp tick; 0 = no timestamps */
typedef struct {
    int slot;
    uint64_t seq, frame;
} PendingTime;
static PendingTime pending[64];
static int npending;
typedef struct {
    uint64_t frame, busy, first, last;  /* ticks */
    int used;
} FrameTime;
static FrameTime frame_times[32];
static uint64_t present_count;  /* frames presented */

void gpu_fatal(const char *what, VkResult r) {
    fprintf(stderr, "yw4: gpu: %s failed (VkResult %d)\n", what, (int)r);
    exit(3);
}

void gpu_stop(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "yw4: gpu: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, " (not supported yet)\n");
    va_end(ap);
    exit(3);
}

int gpu_ready(void) { return ready; }

void gpu_init(void) {
    if (!vk.host_import) return;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = vk.queue_family};
    CHECK(vkCreateCommandPool(vk.device, &pci, NULL, &cmd_pool));
    for (int i = 0; i < RING; i++) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        CHECK(vkAllocateCommandBuffers(vk.device, &ai, &ring[i].cb));
    }
    VkSemaphoreTypeCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci};
    CHECK(vkCreateSemaphore(vk.device, &sci, NULL, &timeline));
    if (vk.props.limits.timestampComputeAndGraphics && vk.props.limits.timestampPeriod > 0) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * RING};
        if (vkCreateQueryPool(vk.device, &qi, NULL, &qpool) == VK_SUCCESS)
            ts_period_ns = vk.props.limits.timestampPeriod;
    }
    ready = 1;
    fprintf(stderr, "yw4: gpu: draw path on (decision 0021)\n");
    gpu_prebuild_start();
}

/* ================================================================== submissions */

uint64_t gpu_submitted(void) { return __atomic_load_n(&submitted, __ATOMIC_ACQUIRE); }

static uint64_t completed(void) {
    uint64_t v = 0;
    CHECK(vkGetSemaphoreCounterValue(vk.device, timeline, &v));
    return v;
}

int gpu_wait(uint64_t seq, uint64_t timeout_ns) {
    if (!ready || completed() >= seq) return 0;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &timeline, .pValues = &seq};
    VkResult r = vkWaitSemaphores(vk.device, &wi, timeout_ns);
    if (r == VK_TIMEOUT) return 2;
    if (r != VK_SUCCESS) gpu_fatal("vkWaitSemaphores", r);
    return 1;
}

/* Adds the timestamps of every completed submission to its frame. gpu_lock held. */
static void collect_times(void) {
    if (!ts_period_ns) return;
    uint64_t done = completed();
    for (int i = 0; i < npending;) {
        PendingTime p = pending[i];
        if (p.seq > done) {
            i++;
            continue;
        }
        uint64_t t[2];
        if (vkGetQueryPoolResults(vk.device, qpool, (uint32_t)p.slot * 2, 2, sizeof t, t, sizeof t[0],
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0]) {
            FrameTime *f = &frame_times[p.frame % 32];
            if (!f->used || f->frame != p.frame) *f = (FrameTime){.frame = p.frame, .first = t[0], .used = 1};
            f->busy += t[1] - t[0];
            if (t[0] < f->first) f->first = t[0];
            if (t[1] > f->last) f->last = t[1];
        }
        gpu_passes_collect(p.slot);
        pending[i] = pending[--npending];
    }
}

int gpu_frame_time(uint64_t frame, double *busy_ms, double *span_ms) {
    if (!ready || !ts_period_ns) return 0;
    host_lock(&gpu_lock);
    collect_times();
    FrameTime f = frame_times[frame % 32];
    host_unlock(&gpu_lock);
    if (!f.used || f.frame != frame) return 0;
    *busy_ms = (double)f.busy * ts_period_ns / 1e6;
    *span_ms = (double)(f.last - f.first) * ts_period_ns / 1e6;
    return 1;
}

/* The next slot of the ring, its command buffer begun; waits if the GPU still runs it, then
 * frees what its last submission used. gpu_lock held. */
static void geo_reset_if_full(void);

static VkCommandBuffer begin(void) {
    Slot *s = &ring[ring_next];
    ring_next = (ring_next + 1) % RING;
    gpu_wait(s->seq, UINT64_MAX);
    geo_reset_if_full();
    collect_times();  /* before the slot's queries are reset below */
    for (int i = 0; i < s->ndesc; i++) CHECK(vkResetDescriptorPool(vk.device, s->desc[i], 0));
    for (int i = 0; i < s->nstaging; i++) {
        vkDestroyBuffer(vk.device, s->staging[i].buf, NULL);
        vkFreeMemory(vk.device, s->staging[i].mem, NULL);
    }
    s->nstaging = 0;
    CHECK(vkResetCommandBuffer(s->cb, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(s->cb, &bi));
    if (ts_period_ns) {
        uint32_t q = (uint32_t)(s - ring) * 2;
        vkCmdResetQueryPool(s->cb, qpool, q, 2);
        vkCmdWriteTimestamp(s->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, q);
    }
    gpu_passes_begin(s->cb, (int)(s - ring));
    gpu_dynamic_forget();  /* a new command buffer: no dynamic state set yet */
    s->seq = submitted + 1;
    cur = s;
    return s->cb;
}

static void submit(VkCommandBuffer cb) {
    gpu_passes_end(cb);
    if (ts_period_ns && cur) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qpool, (uint32_t)(cur - ring) * 2 + 1);
    CHECK(vkEndCommandBuffer(cb));
    uint64_t seq = submitted + 1;
    VkTimelineSemaphoreSubmitInfo ts = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &seq};
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ts, .commandBufferCount = 1,
                       .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &timeline};
    host_lock(&vk.queue_lock);
    VkResult r = vkQueueSubmit(vk.queue, 1, &si, VK_NULL_HANDLE);
    host_unlock(&vk.queue_lock);
    if (r == VK_ERROR_DEVICE_LOST) {
        fprintf(stderr, "yw4: gpu: device lost in submission %" PRIu64 "\n", seq);
        exit(3);
    }
    if (r != VK_SUCCESS) gpu_fatal("vkQueueSubmit", r);
    __atomic_store_n(&submitted, seq, __ATOMIC_RELEASE);
    if (ts_period_ns && cur && npending < 64) pending[npending++] = (PendingTime){(int)(cur - ring), seq, present_count + 1};
    cur = NULL;
}

void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL,
                         0, NULL);
}

VkDescriptorSet gpu_alloc_set(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorSetCount = 1,
                                      .pSetLayouts = &layout};
    VkDescriptorSet set;
    if (cur->ndesc) {
        ai.descriptorPool = cur->desc[cur->ndesc - 1];
        VkResult r = vkAllocateDescriptorSets(vk.device, &ai, &set);
        if (r == VK_SUCCESS) return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) gpu_fatal("vkAllocateDescriptorSets", r);
    }
    if (cur->ndesc == MAX_DESC_POOLS) gpu_stop("more than %d descriptor pools in one submission", MAX_DESC_POOLS);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512}};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4096,
                                      .poolSizeCount = 4, .pPoolSizes = sizes};
    CHECK(vkCreateDescriptorPool(vk.device, &pci, NULL, &cur->desc[cur->ndesc]));
    ai.descriptorPool = cur->desc[cur->ndesc++];
    CHECK(vkAllocateDescriptorSets(vk.device, &ai, &set));
    return set;
}

/* A host-visible buffer of `size` bytes, alive until the current submission has run. */
static void *staging(VkDeviceSize size, VkBuffer *buf) {
    if (cur->nstaging == MAX_STAGING) gpu_stop("more than %d uploads in one submission", MAX_STAGING);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    CHECK(vkCreateBuffer(vk.device, &bci, NULL, buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, *buf, &mr);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &mem));
    CHECK(vkBindBufferMemory(vk.device, *buf, mem, 0));
    void *p;
    CHECK(vkMapMemory(vk.device, mem, 0, VK_WHOLE_SIZE, 0, &p));
    cur->staging[cur->nstaging].buf = *buf, cur->staging[cur->nstaging].mem = mem;
    cur->nstaging++;
    return p;
}

/* ================================================================== memory pools */

typedef struct {
    VkDeviceMemory mem;
    VkBuffer buf;
    uint32_t *page_gen;  /* per 4 KiB page: the write generation it was last seen written in */
    int untracked;       /* the host can't track writes to it: every page counts as written */
} GpuPool;

/* CPU writes to the pools (decision 0021): once a frame, the pages written since the last
 * frame get the new write generation (host_written_pages; Windows tracks writes per page).
 * A texture is uploaded again when one of its pages is newer than its last upload. This
 * catches what the game writes without WriteTexels, like the movie decoder's frames. */
static uint32_t write_gen = 1;

static void mark_written(uint64_t page, void *arg) {
    const NvnPool *p = arg;
    ((GpuPool *)p->gpu)->page_gen[(page - p->storage) >> 12] = write_gen;
}

enum { MAX_POOLS = 64 };
static NvnPool *pools[MAX_POOLS];
static int npools;

static NvnPool *pool_of(uint64_t addr) {
    for (int i = 0; i < npools; i++)
        if (addr >= pools[i]->storage && addr < pools[i]->storage + pools[i]->size) return pools[i];
    return NULL;
}

static void scan_writes(void) {
    write_gen++;
    for (int i = 0; i < npools; i++) {
        NvnPool *p = pools[i];
        GpuPool *g = p->gpu;
        if (p->flags & POOL_CPU_NO_ACCESS || g->untracked) continue;
        if (host_written_pages(p->storage, p->size, mark_written, p) < 0) g->untracked = 1;
    }
}

/* Has the CPU written t's storage since its image g was last uploaded? */
static int cpu_changed(const NvnTexture *t, GpuImage *g) {
    if (!g || !t->addr || t->pool_flags & POOL_CPU_NO_ACCESS || g->checked_gen == write_gen) return 0;
    NvnPool *p = pool_of(t->addr);
    if (!p) return 0;
    const GpuPool *gp = p->gpu;
    if (gp->untracked) return 1;
    uint64_t end = t->addr + tegra_total_size(&t->layout);
    if (end > p->storage + p->size) end = p->storage + p->size;
    for (uint64_t a = (t->addr - p->storage) >> 12, z = (end - 1 - p->storage) >> 12; a <= z; a++)
        if (gp->page_gen[a] > g->upload_gen) return 1;
    g->checked_gen = write_gen;
    return 0;
}


void gpu_pool_init(NvnPool *p) {
    if (!ready || !p->size) return;
    void *host = (void *)(uintptr_t)p->storage;
    if (p->storage % vk.host_import_align || p->size % vk.host_import_align) {
        fprintf(stderr, "yw4: gpu: pool %#" PRIx64 " + %#" PRIx64 " is not aligned for import\n", p->storage, p->size);
        exit(3);
    }
    if (npools == MAX_POOLS) gpu_stop("more than %d memory pools", MAX_POOLS);
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    CHECK(vkGetMemoryHostPointerPropertiesEXT(vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host,
                                              &hp));
    VkExternalMemoryBufferCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext, .size = p->size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
    GpuPool *g = calloc(1, sizeof *g);
    CHECK(vkCreateBuffer(vk.device, &bci, NULL, &g->buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, g->buf, &mr);
    uint32_t type = vk_memory_type(mr.memoryTypeBits & hp.memoryTypeBits, 0);
    if (type == UINT32_MAX) gpu_stop("no memory type to import pool %#" PRIx64, p->storage);
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = host};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
                                .allocationSize = p->size, .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &g->mem));
    CHECK(vkBindBufferMemory(vk.device, g->buf, g->mem, 0));
    g->page_gen = calloc(p->size >> 12, sizeof *g->page_gen);
    if (!g->page_gen) gpu_stop("no memory for write tracking");
    p->gpu = g;
    host_written_pages(p->storage, p->size, mark_written, p);  /* writes before now: uploads read them anyway */
    host_lock(&gpu_lock);
    pools[npools++] = p;
    host_unlock(&gpu_lock);
}

void gpu_pool_finalize(NvnPool *p) {
    GpuPool *g = p->gpu;
    if (!g) return;
    gpu_wait(gpu_submitted(), UINT64_MAX);
    host_lock(&gpu_lock);
    for (int i = 0; i < npools; i++)
        if (pools[i] == p) pools[i] = pools[--npools];
    host_unlock(&gpu_lock);
    vkDestroyBuffer(vk.device, g->buf, NULL);
    vkFreeMemory(vk.device, g->mem, NULL);
    free(g->page_gen);
    free(g);
    p->gpu = NULL;
}

/* ---- geometry in video memory (decision 0028; YW4_VRAM_GEOMETRY=0: off): the pools are the
 * game's memory imported as is, so a draw read its vertices and indices from system memory across
 * PCIe, which made the G-buffer and material passes of a busy street take 15 ms each. Here a draw's
 * vertex and index ranges get a copy in one device-local arena, made once and made again when the
 * CPU has written one of the range's pages since (write generations, as for textures). A range
 * the CPU keeps writing (UI, effects) is left in the pool after a few copies. When the arena is
 * full (or its table crowded), it is emptied at the start of the next submission. The arena has two
 * halves, filled in turn: emptying switches to the other half, which the GPU finished with long
 * ago, so the CPU doesn't wait for the work in flight (waiting made one 50 ms frame). */
enum { GEO_ARENA = 1u << 30, GEO_HALF = GEO_ARENA / 2, GEO_SLOTS = 1 << 18, GEO_DYNAMIC = 4 };
typedef struct {
    uint64_t addr, size, at;     /* the range, and where its copy is in the arena */
    uint32_t copied_gen, checked_gen;
    uint32_t copies;             /* made again this many times: GEO_DYNAMIC and it stays in the pool */
} GeoCopy;
static GeoCopy geo[GEO_SLOTS];
static VkBuffer geo_buf;
static VkDeviceMemory geo_mem;
static uint64_t geo_used;   /* in the current half */
static uint32_t geo_count;  /* ranges in the table */
static int geo_half;        /* the half being filled */
static uint64_t geo_retired[2];  /* the last submission that read each half before it was left */
static int geo_on = -1, geo_full;

int gpu_vram_geometry(void) {
    if (geo_on < 0) geo_on = !getenv("YW4_VRAM_GEOMETRY") || atoi(getenv("YW4_VRAM_GEOMETRY"));
    return geo_on;
}

/* begin(): the half filled up: go on in the other one, once the submissions that read it have run
 * (they ran long ago: a half lasts minutes, so the wait is almost always none). */
static void geo_reset_if_full(void) {
    if (!geo_full) return;
    geo_retired[geo_half] = submitted;
    geo_half ^= 1;
    int waited = gpu_wait(geo_retired[geo_half], UINT64_MAX);
    memset(geo, 0, sizeof geo);
    geo_used = 0, geo_full = 0, geo_count = 0;
    fprintf(stderr, "yw4: gpu: geometry arena emptied (now half %d%s)\n", geo_half, waited ? ", waited for the GPU" : "");
}

static GeoCopy *geo_find(uint64_t addr, uint64_t size, int add) {
    uint32_t h = (uint32_t)((addr ^ size * 0x9E3779B97F4A7C15ull) * 0x9E3779B97F4A7C15ull >> 32) & (GEO_SLOTS - 1);
    for (uint32_t i = 0; i < 64; i++, h = (h + 1) & (GEO_SLOTS - 1)) {
        GeoCopy *c = &geo[h];
        if (c->size && c->addr == addr && c->size == size) return c;
        if (!c->size) {
            if (!add) return NULL;
            uint64_t at = (geo_used + 255) & ~255ull;
            if (at + size > GEO_HALF) {
                geo_full = 1;
                return NULL;
            }
            geo_used = at + size;
            *c = (GeoCopy){.addr = addr, .size = size, .at = (uint64_t)geo_half * GEO_HALF + at};
            if (++geo_count % 8192 == 0)
                fprintf(stderr, "yw4: gpu: geometry table: %u ranges, %" PRIu64 " MiB\n", geo_count, geo_used >> 20);
            return c;
        }
    }
    if (add && !geo_full) {  /* no free slot near: empty the table as if the arena were full (the
                              * table fills first: ~1.5 KB per range in a long session) */
        geo_full = 1;
        fprintf(stderr, "yw4: gpu: geometry table crowded (%u ranges, %" PRIu64 " MiB of the arena used)\n", geo_count,
                geo_used >> 20);
    }
    return NULL;
}

/* Does the range's copy need making (again)? Its pages, checked once a frame. */
static int geo_stale(GeoCopy *c, const NvnPool *p) {
    if (!c->copied_gen) return 1;
    if (c->checked_gen == write_gen) return 0;
    const GpuPool *gp = p->gpu;
    for (uint64_t a = (c->addr - p->storage) >> 12, z = (c->addr + c->size - 1 - p->storage) >> 12; a <= z; a++)
        if (gp->page_gen[a] > c->copied_gen) return 1;
    c->checked_gen = write_gen;
    return 0;
}

/* gpu_draw.c, before rendering: 1 if the range would be copied now (rendering must end first). */
int gpu_geometry_needs_copy(uint64_t addr, uint64_t size) {
    if (!gpu_vram_geometry() || !size) return 0;
    NvnPool *p = pool_of(addr);
    if (!p || addr + size > p->storage + p->size || ((GpuPool *)p->gpu)->untracked) return 0;
    GeoCopy *c = geo_find(addr, size, 1);
    return c && c->copies < GEO_DYNAMIC && geo_stale(c, p);
}

/* gpu_draw.c, outside rendering: makes the range's copy if it needs one. */
void gpu_geometry_copy(VkCommandBuffer cb, uint64_t addr, uint64_t size) {
    if (!gpu_geometry_needs_copy(addr, size)) return;
    if (!geo_buf) {
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GEO_ARENA,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
        CHECK(vkCreateBuffer(vk.device, &bci, NULL, &geo_buf));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk.device, geo_buf, &mr);
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                    .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        CHECK(vkAllocateMemory(vk.device, &mai, NULL, &geo_mem));
        CHECK(vkBindBufferMemory(vk.device, geo_buf, geo_mem, 0));
        fprintf(stderr, "yw4: gpu: geometry in video memory (%u MiB arena)\n", GEO_ARENA >> 20);
    }
    NvnPool *p = pool_of(addr);
    GeoCopy *c = geo_find(addr, size, 0);
    if (!c) return;
    VkBufferCopy bc = {.srcOffset = addr - p->storage, .dstOffset = c->at, .size = size};
    vkCmdCopyBuffer(cb, ((GpuPool *)p->gpu)->buf, geo_buf, 1, &bc);
    if (c->copied_gen && ++c->copies == GEO_DYNAMIC) {
        static int logged;
        if (logged++ < 8)
            fprintf(stderr, "yw4: gpu: geometry range %#" PRIx64 " (%" PRIu64 " bytes) rewritten %d times: left in system memory\n",
                    addr, size, GEO_DYNAMIC);
    }
    c->copied_gen = c->checked_gen = write_gen;
}

/* The range's copy in video memory if it has a current one, else 0. */
static int geometry_at(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (!gpu_vram_geometry() || !geo_buf) return 0;
    GeoCopy *c = geo_find(addr, size, 0);
    if (!c || !c->copied_gen || c->checked_gen != write_gen || c->copies >= GEO_DYNAMIC) return 0;
    *buf = geo_buf, *off = c->at;
    return 1;
}

int gpu_geometry_at(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    return geometry_at(addr, size, buf, off) || gpu_buffer_at(addr, size, buf, off);
}

int gpu_buffer_at(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    for (int i = 0; i < npools; i++) {
        const NvnPool *p = pools[i];
        if (addr >= p->storage && addr + size <= p->storage + p->size) {
            *buf = ((GpuPool *)p->gpu)->buf;
            *off = addr - p->storage;
            return 1;
        }
    }
    return 0;
}

/* ================================================================== formats */

VkFormat gpu_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x31: return VK_FORMAT_S8_UINT;
    case 0x32: return VK_FORMAT_D16_UNORM;
    case 0x33: return VK_FORMAT_D32_SFLOAT;  /* DEPTH24 */
    case 0x34: return VK_FORMAT_D32_SFLOAT;
    case 0x35: return VK_FORMAT_D24_UNORM_S8_UINT;
    case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x41: case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK;
    case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK;
    case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK;
    case 0x4A: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 0x4B: return VK_FORMAT_BC5_UNORM_BLOCK;
    case 0x4C: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4D: return VK_FORMAT_BC7_UNORM_BLOCK;
    case 0x4E: return VK_FORMAT_BC7_SRGB_BLOCK;
    case 0x4F: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    /* RGBX / BGRX: alpha unused, reads as 1 (gpu_format_alpha_one) */
    case 0x37: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x6D: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x71: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x76: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x98: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x9A: return VK_FORMAT_B8G8R8A8_SRGB;
    case 0x99: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x9B: return VK_FORMAT_B8G8R8A8_SRGB;
    default: return VK_FORMAT_UNDEFINED;
    }
}

int gpu_format_alpha_one(uint32_t f) {
    return f == 0x37 || f == 0x6D || f == 0x71 || f == 0x76 || f == 0x98 || f == 0x9A;
}

static VkImageAspectFlags aspect_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

/* ================================================================== images */

/* Copies t's texels from the game's memory into its image: every level and layer. */
static void upload(VkCommandBuffer cb, NvnTexture *t, GpuImage *g) {
    gpu_passes_note(2);
    const TexLayout *L = &t->layout;
    const FormatInfo *f = L->fmt;
    if (g->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return;  /* depth/stencil: rendered, not loaded */
    g->upload_gen = g->checked_gen = write_gen;
    g->uploaded_frame = present_count;
    VkDeviceSize total = 0;
    for (uint32_t l = 0; l < g->levels; l++) {
        LevelLayout lv = tegra_level(L, l);
        total += (VkDeviceSize)lv.wb * lv.hb * lv.d * f->bpb * g->layers;
    }
    VkBuffer buf;
    uint8_t *dst = staging(total, &buf);
    VkBufferImageCopy copies[16];
    uint32_t ncopies = 0;
    VkDeviceSize off = 0;
    for (uint32_t l = 0; l < g->levels && l < 16; l++) {
        LevelLayout lv = tegra_level(L, l);
        VkDeviceSize layer_bytes = (VkDeviceSize)lv.wb * lv.hb * lv.d * f->bpb;
        for (uint32_t k = 0; k < g->layers; k++)
            tegra_read_region(L, (const uint8_t *)(uintptr_t)t->addr, l, k, 0, 0, 0, lv.w, lv.h, lv.d,
                              dst + off + k * layer_bytes);
        copies[ncopies++] = (VkBufferImageCopy){
            .bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, g->layers},
            .imageExtent = {lv.w, lv.h, lv.d}};
        off += layer_bytes * g->layers;
    }
    /* YW4_DUMP_UPLOAD=<w>x<h>: write level 0 of uploads of that size to work/dump/ (raw texels) */
    static const char *dump = (const char *)1;
    if (dump == (const char *)1) dump = getenv("YW4_DUMP_UPLOAD");
    char want[32];
    snprintf(want, sizeof want, "%ux%u", g->width, g->height);
    static int n;  /* ... the first 8 from frame YW4_DUMP_AT (default 0) on */
    static uint64_t dump_at = UINT64_MAX;
    if (dump_at == UINT64_MAX) dump_at = getenv("YW4_DUMP_AT") ? strtoull(getenv("YW4_DUMP_AT"), NULL, 10) : 0;
    if (dump && !strcmp(dump, want) && n < 8 && present_count >= dump_at) {
        char path[128];
        snprintf(path, sizeof path, "work/dump/tex_%" PRIx64 "_%03d_fmt%x.raw", t->addr, n++, t->format);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            LevelLayout lv = tegra_level(L, 0);
            fwrite(dst, 1, (size_t)lv.wb * lv.hb * f->bpb, fp);
            fclose(fp);
            fprintf(stderr, "yw4: gpu: dumped %s\n", path);
        }
        /* ... and the swizzled bytes as they are in the game's memory, with the layout used */
        snprintf(path, sizeof path, "work/dump/tex_%" PRIx64 "_%03d_fmt%x_bh%u.tegra", t->addr, n - 1, t->format,
                 L->block_height_log2);
        if ((fp = fopen(path, "wb"))) {
            fwrite((const void *)(uintptr_t)t->addr, 1, (size_t)tegra_total_size(L), fp);
            fclose(fp);
        }
    }
    full_barrier(cb);
    vkCmdCopyBufferToImage(cb, buf, g->image, VK_IMAGE_LAYOUT_GENERAL, ncopies, copies);
    full_barrier(cb);
}

uint64_t gpu_frames(void) { return present_count; }

/* YW4_GPU_REUPLOAD=1 (diagnostic): upload every CPU-visible texture again once per frame,
 * to find textures whose data the game writes without telling NVN. Slow. */
static int reupload(void) {
    static int on = -1;
    if (on < 0) on = getenv("YW4_GPU_REUPLOAD") && atoi(getenv("YW4_GPU_REUPLOAD"));
    return on;
}

static int reupload_due(const NvnTexture *t, GpuImage *g) {
    if (reupload()) return g && t->addr && !(t->pool_flags & POOL_CPU_NO_ACCESS) && g->uploaded_frame != present_count;
    return cpu_changed(t, g);
}

/* ---- images by description (ImageKey): texture objects come and go, the memory stays */

enum { IMAGE_BUCKETS = 4096 };
static GpuImage *images[IMAGE_BUCKETS];

/* Shadow quality: the shadow maps (depth, 1024 x 1024, 3 layers: one per cascade; rendered by
 * the GPU only) made 2048 or 4096 instead. 4096 by default (the owner's pick, with PCSS x2);
 * YW4_SHADOW_RES=1024/2048/4096 sets it at start, the settings panel (F1) while playing. The size is part of the image key, so after a switch the next frame's draws and
 * samples use an image of the new size (the old one is freed with other unused images). Draws
 * into it get their viewport and scissor scaled (gpu_dynamic_state); the shaders reading it
 * sample in 0..1 coordinates, with their filter spread scaled to match (gpu_draw.c). */
static int shadow_res = -1;

int gpu_shadow_res(void) {
    int r = __atomic_load_n(&shadow_res, __ATOMIC_ACQUIRE);
    if (r < 0) {
        const char *e = getenv("YW4_SHADOW_RES");
        int v = e ? atoi(e) : 4096;
        r = v == 1024 || v == 2048 ? v : 4096;
        __atomic_store_n(&shadow_res, r, __ATOMIC_RELEASE);
    }
    return r;
}

void gpu_shadow_res_cycle(void) {
    int r = gpu_shadow_res();
    r = r == 1024 ? 2048 : r == 2048 ? 4096 : 1024;
    __atomic_store_n(&shadow_res, r, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: shadow maps %dx%d\n", r, r);
}

/* Resolution of the 3D view (docs/frame.md, "What depends on the screen size"): the game renders the
 * scene, and the UI over it, into 1600x900 targets and smaller copies (1/2, 1/3, 1/4, 1/8, ~1/13),
 * then copies the result into the 1920x1080 window image by sampling. These targets are made
 * height / 900 times larger: 900 (the game's), 1080, 1440 or 2160. The shaders read them in 0..1
 * coordinates, so they follow; draws into them get their viewport and scissor scaled
 * (gpu_dynamic_state). YW4_RES=<height> at start, the settings panel (F1) while playing. */
static int res_height = -1;

int gpu_res_height(void) {
    int r = __atomic_load_n(&res_height, __ATOMIC_ACQUIRE);
    if (r < 0) {
        const char *e = getenv("YW4_RES");
        int v = e ? atoi(e) : 900;
        r = v == 1080 || v == 1440 || v == 2160 ? v : 900;
        __atomic_store_n(&res_height, r, __ATOMIC_RELEASE);
    }
    return r;
}

void gpu_res_cycle(void) {
    int r = gpu_res_height();
    r = r == 900 ? 1080 : r == 1080 ? 1440 : r == 1440 ? 2160 : 900;
    __atomic_store_n(&res_height, r, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: 3D view %dx%d\n", r * 16 / 9, r);
}

/* Upscaling (decision 0039): the screen-sized targets of the 3D get the render height, output / ratio; the
 * window images (presented) and, from the anti-aliasing point to the present, the scene image the UI is drawn
 * into get the output height. The anti-aliasing point (gpu_aa.c) makes the full-size picture: FSR when that is
 * the anti-aliasing, else the chosen anti-aliasing at the render size and a plain stretch. */
static int upscale_mode = -1;
static const float upscale_ratio[4] = {1.0f, 1.5f, 1.7f, 2.0f};
static uint64_t upscaled_scene, upscaled_depth;  /* the scene's colour and depth images' addresses while they
                                                  * name output-sized images (GPU thread) */

int gpu_upscale_setting(void) {
    int m = __atomic_load_n(&upscale_mode, __ATOMIC_ACQUIRE);
    if (m < 0) {
        const char *e = getenv("YW4_UPSCALE");
        m = e ? atoi(e) : 0;
        if (m < 0 || m > 3) m = 0;
        __atomic_store_n(&upscale_mode, m, __ATOMIC_RELEASE);
    }
    return m;
}

/* Upscaling needs FSR to rebuild the detail; without it the choice is kept but not used (owner, 2026-10-29) */
int gpu_upscale(void) { return gpu_aa() == 4 ? gpu_upscale_setting() : 0; }

void gpu_upscale_cycle(void) {
    int m = (gpu_upscale_setting() + 1) % 4;
    __atomic_store_n(&upscale_mode, m, __ATOMIC_RELEASE);
    static const char *const names[4] = {"off", "quality (1.5x)", "balanced (1.7x)", "performance (2x)"};
    fprintf(stderr, "yw4: gpu: upscaling %s: the 3D at %d lines\n", names[m], gpu_render_height());
}

int gpu_render_height(void) {
    int h = gpu_res_height(), m = gpu_upscale();
    return m ? (int)((float)h / upscale_ratio[m] + 0.5f) : h;
}

void gpu_upscale_begin(uint64_t color, uint64_t depth) {
    __atomic_store_n(&upscaled_depth, depth, __ATOMIC_RELEASE);
    __atomic_store_n(&upscaled_scene, color, __ATOMIC_RELEASE);
}

int gpu_upscaled(const NvnTexture *t) {
    uint64_t a = __atomic_load_n(&upscaled_scene, __ATOMIC_ACQUIRE), d = __atomic_load_n(&upscaled_depth, __ATOMIC_ACQUIRE);
    return t && t->addr && (t->addr == a || t->addr == d);
}

/* The screen-sized targets: GPU-only 2D images of one of these sizes. The window image (1920x1080)
 * too: the game copies the 3D view into its top-left 1600x900 and presents that crop (gpu_present
 * scales the crop with it). The engine can also render the scene at 1920x1080 (it makes its targets
 * that size first at start, then 1600x900 at the same addresses), with smaller copies of that:
 * 960x540, 639x359, 480x270 seen so far; all get the same factor so their ratios hold. */
/* The battle's start grows the 3D view in steps of 32 x 18 from 1600x900 towards 1920x1080 (1632x918 ...
 * 1888x1062, seen 2026-10-28), with its smaller copies at 1/2, 1/3, 1/4 and 1/8 (rounded down): the
 * divisor of w x h in that chain, 0 if it is not one. Without it those targets kept the game's shape: a
 * 16:9 battle with the swirl before it left at the sides for a few seconds (widescreen). They are widened
 * but not made larger (image_scale). */
static int zoom_chain(uint32_t w, uint32_t h) {
    static const uint32_t divs[] = {1, 2, 3, 4, 8};
    for (uint32_t k = 1; k < 10; k++)
        for (size_t i = 0; i < sizeof divs / sizeof *divs; i++)
            if (w == (1600 + 32 * k) / divs[i] && h == (900 + 18 * k) / divs[i]) return (int)divs[i];
    return 0;
}

static int screen_sized(const NvnTexture *t) {
    static const uint16_t sizes[][2] = {{1600, 900}, {800, 450}, {533, 300}, {400, 225}, {200, 112}, {120, 67},
                                        {1920, 1080}, {960, 540}, {639, 359}, {480, 270}};
    if (t->target != TARGET_2D || t->layout.levels > 1 || t->layout.layers > 1 || !(t->pool_flags & POOL_CPU_NO_ACCESS))
        return 0;
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++)
        if (t->layout.width == sizes[i][0] && t->layout.height == sizes[i][1]) return 1;
    return zoom_chain(t->layout.width, t->layout.height) != 0;
}

/* How much larger t's image is made (0 / 0: not). */
/* Effects resolution: the game draws its shadow mask, lamp lights, ambient occlusion and effects into
 * half-size targets (800x450, 960x540 in the 1920x1080 chain) and spreads them over the picture, which
 * softens shadow edges and makes them blocky up close. FULL makes those targets twice as large as the
 * rest (the 3D view's size); their readers sample them in 0..1 coordinates. YW4_EFFECTS=0 (the game's,
 * half) / 1 (full) at start. */
static int effects_full = -1;

int gpu_effects_full(void) {
    int f = __atomic_load_n(&effects_full, __ATOMIC_ACQUIRE);
    if (f < 0) {
        const char *e = getenv("YW4_EFFECTS");
        f = e && atoi(e) == 1;
        __atomic_store_n(&effects_full, f, __ATOMIC_RELEASE);
    }
    return f;
}

void gpu_effects_cycle(void) {
    int f = !gpu_effects_full();
    __atomic_store_n(&effects_full, f, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: effects at %s resolution\n", f ? "full" : "half (the game's)");
}

/* The distance blur's half-size images stay half size even then: its blur comes mostly from that low
 * resolution, so full-size images would weaken it (+60% distant detail in the bamboo forest) and blur its
 * own setting (gpu_dof). gpu_draw.c names them from the draws that use them; the image is made again at the
 * right size from the next use on. */
enum { KEEP_RATIO = 16 };
static uint64_t keep_ratio[KEEP_RATIO];

void gpu_keep_game_ratio(const NvnTexture *t) {
    if (!t || !t->addr) return;
    for (int i = 0; i < KEEP_RATIO; i++) {
        uint64_t a = __atomic_load_n(&keep_ratio[i], __ATOMIC_ACQUIRE);
        if (a == t->addr) return;
        if (!a && __atomic_compare_exchange_n(&keep_ratio[i], &a, t->addr, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            fprintf(stderr, "yw4: gpu: distance-blur image at %#" PRIx64 " (%ux%u) keeps the game's size ratio\n", t->addr,
                    t->layout.width, t->layout.height);
            return;
        }
    }
}

static int half_sized(const NvnTexture *t) {
    if (!((t->layout.width == 800 && t->layout.height == 450) || (t->layout.width == 960 && t->layout.height == 540)))
        return 0;
    for (int i = 0; i < KEEP_RATIO; i++)
        if (__atomic_load_n(&keep_ratio[i], __ATOMIC_ACQUIRE) == t->addr) return 0;
    /* YW4_EFFECTS_KEEP=<hex addr>[,...]: these keep the game's ratio too (to find which image an effect
     * that breaks at full size reads) */
    static uint64_t keep[16];
    static int nkeep = -1;
    if (nkeep < 0) {
        int n = 0;
        const char *e = getenv("YW4_EFFECTS_KEEP");
        while (e && *e && n < 16) {
            char *end;
            keep[n] = strtoull(e, &end, 16);
            if (end == e) break;
            n++;
            e = *end == ',' ? end + 1 : end;
        }
        __atomic_store_n(&nkeep, n, __ATOMIC_RELEASE);
    }
    for (int i = 0; i < nkeep; i++)
        if (keep[i] == t->addr) return 0;
    return 1;
}

static void image_scale(const NvnTexture *t, uint32_t *num, uint32_t *den) {
    *num = *den = 0;
    int res = gpu_shadow_res(), h = gpu_render_height();
    if (gpu_upscale() && (gpu_is_presented(t->addr) || gpu_upscaled(t))) h = gpu_res_height();  /* output-sized */
    if (res != 1024 && t->layout.width == 1024 && t->layout.height == 1024 && t->layout.layers == 3 &&
        t->layout.levels <= 1 && t->pool_flags & POOL_CPU_NO_ACCESS &&
        aspect_of(gpu_format(t->format)) & VK_IMAGE_ASPECT_DEPTH_BIT)
        *num = (uint32_t)res, *den = 1024;
    /* the battle's zoom chain is widened (key_of) but keeps the game's resolution: each step is a new size,
     * so a new set of images, and nine sets at 4K ran the GPU out of memory (2026-10-28) */
    else if (zoom_chain(t->layout.width, t->layout.height))
        return;
    else if (screen_sized(t) && gpu_effects_full() && half_sized(t))
        *num = 2 * (uint32_t)h, *den = 900;
    else if (h != 900 && screen_sized(t))
        *num = (uint32_t)h, *den = 900;
}

static ImageKey key_of(const NvnTexture *t) {
    ImageKey k;
    memset(&k, 0, sizeof k);
    k.addr = t->addr, k.format = t->format, k.target = t->target;
    k.width = t->layout.width, k.height = t->layout.height, k.depth = t->layout.depth;
    k.layers = t->layout.layers, k.levels = t->layout.levels;
    k.block_height_log2 = t->layout.block_height_log2, k.block_depth_log2 = t->layout.block_depth_log2;
    k.linear = (t->flags & FLAG_LINEAR) != 0;
    image_scale(t, &k.scale_num, &k.scale_den);
    if (gpu_wide() && screen_sized(t)) gpu_wide_factor(&k.wide_num, &k.wide_den);
    return k;
}

static uint32_t key_hash(const ImageKey *k) {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint8_t *p = (const uint8_t *)k;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return (uint32_t)(h ^ h >> 32) % IMAGE_BUCKETS;
}

/* Bumped when an image is freed: a texture's remembered image (found) is only used while its
 * epoch is current, and its description must still match. Draws look their textures up several
 * times each; the hash and the bucket's list cost more than the compare. */
static uint32_t image_epoch = 1;

GpuImage *image_find(const NvnTexture *t) {
    ImageKey k = key_of(t);
    GpuImage *m = t->found;
    if (m && t->found_epoch == image_epoch && !memcmp(&m->key, &k, sizeof k)) return m;
    for (GpuImage *g = images[key_hash(&k)]; g; g = g->next)
        if (!memcmp(&g->key, &k, sizeof k)) {
            ((NvnTexture *)t)->found = g, ((NvnTexture *)t)->found_epoch = image_epoch;
            return g;
        }
    return NULL;
}

int gpu_needs_transfer(const NvnTexture *t) {
    GpuImage *g = image_find(t);
    return !g || t->cpu_written || reupload_due(t, g);
}

static void image_destroy(GpuImage *g);  /* below, after GpuView */

/* Frees images no texture has used for a while (resolution changes leave old render
 * targets behind) once the GPU is done with them. gpu_lock held. */
static void evict_images(void) {
    enum { UNUSED_FRAMES = 300 };
    uint64_t done = completed();
    int freed = 0;
    for (int b = 0; b < IMAGE_BUCKETS; b++)
        for (GpuImage **pp = &images[b]; *pp;) {
            GpuImage *g = *pp;
            if (g->last_frame + UNUSED_FRAMES < present_count && g->last_seq <= done) {
                *pp = g->next;
                image_destroy(g);
                freed++;
            } else {
                pp = &g->next;
            }
        }
    if (freed && nvn_stats_enabled()) fprintf(stderr, "yw4: gpu: freed %d unused images\n", freed);
}

GpuImage *image_of(VkCommandBuffer cb, NvnTexture *t) {
    GpuImage *g = image_find(t);
    if (g) {
        g->last_frame = present_count, g->last_seq = cur ? cur->seq : g->last_seq;
        if (reupload_due(t, g) || t->cpu_written) {
            t->cpu_written = 0;
            upload(cb, t, g);
        }
        return g;
    }
    VkFormat fmt = gpu_format(t->format);
    if (fmt == VK_FORMAT_UNDEFINED || t->target == TARGET_BUFFER || t->samples > 1)
        gpu_stop("texture format %#x target %u samples %u", t->format, t->target, t->samples);
    if (t->levels > 16) gpu_stop("texture with %u levels", t->levels);
    g = calloc(1, sizeof *g);
    g->key = key_of(t);
    g->last_frame = present_count, g->last_seq = cur ? cur->seq : 0;
    uint32_t bucket = key_hash(&g->key);
    g->next = images[bucket];
    images[bucket] = g;
    g->format = fmt;
    g->aspect = aspect_of(fmt);
    g->width = t->layout.width, g->height = t->layout.height, g->depth = t->layout.depth ? t->layout.depth : 1;
    g->layers = t->layout.layers ? t->layout.layers : 1, g->levels = t->layout.levels ? t->layout.levels : 1;
    g->scale_num = g->key.scale_den ? g->key.scale_num : 1, g->scale_den = g->key.scale_den ? g->key.scale_den : 1;
    g->wide_num = g->key.wide_den ? g->key.wide_num : 1, g->wide_den = g->key.wide_den ? g->key.wide_den : 1;
    if (g->key.scale_den || g->key.wide_den) {
        g->width = gpu_scaled(gpu_scaled(g->width, g->scale_num, g->scale_den), g->wide_num, g->wide_den);
        g->height = gpu_scaled(g->height, g->scale_num, g->scale_den);
        static int logged;
        int half = (t->layout.width == 800 || t->layout.width == 960) && g->width >= 2 * t->layout.width - 2;
        if (logged++ < 16 || half)  /* the effects-resolution ones always: they are few */
            fprintf(stderr, "yw4: gpu: target at %#" PRIx64 " format %#x made %ux%u (the game's is %ux%u)\n", t->addr,
                    t->format, g->width, g->height, t->layout.width, t->layout.height);
    } else if (gpu_res_height() != 900 && t->pool_flags & POOL_CPU_NO_ACCESS && t->target == TARGET_2D) {
        /* GPU-only 2D images of other sizes stay the game's: named once per size, to find screen-sized
         * targets of scenes not measured yet (docs/frame.md) */
        static uint32_t seen[64];
        uint32_t wh = t->layout.width << 16 | t->layout.height, i;
        for (i = 0; i < 64 && seen[i] && seen[i] != wh; i++) {}
        if (i < 64 && !seen[i]) {
            seen[i] = wh;
            fprintf(stderr, "yw4: gpu: GPU-only target %ux%u format %#x kept at the game's size\n", t->layout.width,
                    t->layout.height, t->format);
        }
    }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk.gpu, fmt, &fp);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    int cube = (t->target == TARGET_CUBE || t->target == TARGET_CUBE_ARRAY) && g->layers % 6 == 0;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
                 (t->target == TARGET_3D ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0),
        .imageType = t->target == TARGET_3D ? VK_IMAGE_TYPE_3D
                     : t->target == TARGET_1D || t->target == TARGET_1D_ARRAY ? VK_IMAGE_TYPE_1D
                                                                                : VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = {g->width, g->height, g->depth}, .mipLevels = g->levels, .arrayLayers = g->layers,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(vk.device, &ici, NULL, &g->image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk.device, g->image, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &g->mem));
    CHECK(vkBindImageMemory(vk.device, g->image, g->mem, 0));
    VkImageSubresourceRange all = {g->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g->image, .subresourceRange = all};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1,
                         &b);
    if (nvn_stats_enabled())
        fprintf(stderr, "yw4: gpu: image %ux%u format %#x flags %#x stride %" PRIu64 " block height log2 %u pool flags %#x at %#" PRIx64
                        " (frame %" PRIu64 ")%s\n",
                g->width, g->height, t->format, t->flags, t->stride, t->layout.block_height_log2, t->pool_flags, t->addr,
                present_count,
                t->cpu_written ? ", WriteTexels before first use" : "");
    t->cpu_written = 0;
    if (t->addr && !(t->pool_flags & POOL_CPU_NO_ACCESS)) {
        upload(cb, t, g);
    } else {  /* rendered by the GPU: starts as zeros */
        if (g->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue zero = {{0}};
            vkCmdClearColorImage(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        } else {
            VkClearDepthStencilValue zero = {0};
            vkCmdClearDepthStencilImage(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        }
        full_barrier(cb);
    }
    return g;
}

struct GpuView {
    GpuView *next;
    VkImageViewType type;
    VkFormat format;
    uint32_t level0, nlevels, layer0, nlayers;
    VkComponentMapping sw;
    VkImageAspectFlags aspect;
    VkImageView view;
};

static void image_destroy(GpuImage *g) {
    image_epoch++;
    for (GpuView *v = g->views, *n; v; v = n) {
        n = v->next;
        vkDestroyImageView(vk.device, v->view, NULL);
        free(v);
    }
    vkDestroyImage(vk.device, g->image, NULL);
    vkFreeMemory(vk.device, g->mem, NULL);
    free(g);
}

static VkImageView view_of(GpuImage *g, VkImageViewType type, VkFormat fmt, uint32_t level0, uint32_t nlevels,
                           uint32_t layer0, uint32_t nlayers, VkComponentMapping sw, VkImageAspectFlags aspect) {
    for (GpuView *v = g->views; v; v = v->next)
        if (v->type == type && v->format == fmt && v->level0 == level0 && v->nlevels == nlevels && v->layer0 == layer0 &&
            v->nlayers == nlayers && !memcmp(&v->sw, &sw, sizeof sw) && v->aspect == aspect)
            return v->view;
    GpuView *v = calloc(1, sizeof *v);
    *v = (GpuView){g->views, type, fmt, level0, nlevels, layer0, nlayers, sw, aspect, VK_NULL_HANDLE};
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g->image, .viewType = type,
                                .format = fmt, .components = sw,
                                .subresourceRange = {aspect, level0, nlevels, layer0, nlayers}};
    CHECK(vkCreateImageView(vk.device, &ci, NULL, &v->view));
    g->views = v;
    return v->view;
}

VkImageView gpu_render_view(VkCommandBuffer cb, NvnTexture *t, const NvnTextureView *v, int has_view, uint32_t *w,
                            uint32_t *h) {
    GpuImage *g = image_of(cb, t);
    uint32_t level = has_view && v->setmask & 1 ? v->base_level : 0;
    uint32_t layer0 = has_view && v->setmask & 2 ? v->min_layer : 0;
    uint32_t nlayers = has_view && v->setmask & 2 && v->num_layers ? v->num_layers : 1;
    VkFormat fmt = has_view && v->setmask & 4 ? gpu_format(v->format) : g->format;
    if (fmt == VK_FORMAT_UNDEFINED) gpu_stop("render target view format %#x", v->format);
    if (level >= g->levels || layer0 + nlayers > g->layers) gpu_stop("render target view outside its texture");
    *w = g->width >> level ? g->width >> level : 1;
    *h = g->height >> level ? g->height >> level : 1;
    VkComponentMapping id = {0};
    return view_of(g, nlayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, fmt, level, 1, layer0, nlayers,
                   id, g->aspect);
}

VkImageView gpu_sample_view(VkCommandBuffer cb, NvnTexture *t, const NvnTextureView *v, int has_view,
                            VkImageViewType type) {
    GpuImage *g = image_of(cb, t);
    if ((type == VK_IMAGE_VIEW_TYPE_3D) != (t->target == TARGET_3D)) return gpu_dummy_view(cb, type);
    uint32_t level0 = has_view && v->setmask & 1 ? v->base_level : 0;
    uint32_t nlevels = has_view && v->setmask & 1 && v->num_levels ? v->num_levels : g->levels - level0;
    uint32_t layer0 = has_view && v->setmask & 2 ? v->min_layer : 0;
    uint32_t nlayers = has_view && v->setmask & 2 && v->num_layers ? v->num_layers : g->layers - layer0;
    if (level0 >= g->levels || layer0 >= g->layers) return gpu_dummy_view(cb, type);
    if (level0 + nlevels > g->levels) nlevels = g->levels - level0;
    if (layer0 + nlayers > g->layers) nlayers = g->layers - layer0;
    if (type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_3D) nlayers = 1;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE) {
        /* A view of a cube texture counts its layers in cubes: the game's ambient cube map has a
         * view of layers 0+1, which is the whole cube (6 faces), not one face. */
        if (t->target == TARGET_CUBE || t->target == TARGET_CUBE_ARRAY) {
            if (nlayers < 6) layer0 *= 6;
        }
        if (layer0 + 6 > g->layers) return gpu_dummy_view(cb, type);
        nlayers = 6;
    }
    VkFormat fmt = has_view && v->setmask & 4 ? gpu_format(v->format) : g->format;
    if (fmt == VK_FORMAT_UNDEFINED) gpu_stop("texture view format %#x", v->format);
    VkImageAspectFlags aspect = g->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : g->aspect;
    VkComponentMapping sw = {0};  /* identity */
    if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        /* NVN swizzle: 0 zero, 1 one, 2..5 r g b a; all zero means never set (identity) */
        const uint8_t *s = has_view && v->setmask & 8 ? v->swizzle : t->swizzle;
        int set = s[0] | s[1] | s[2] | s[3];
        uint8_t c[4];
        for (int i = 0; i < 4; i++) c[i] = set && s[i] <= 5 ? s[i] : (uint8_t)(2 + i);
        if (gpu_format_alpha_one(has_view && v->setmask & 4 ? v->format : t->format))
            for (int i = 0; i < 4; i++)
                if (c[i] == 5) c[i] = 1;
        sw = (VkComponentMapping){(VkComponentSwizzle)(c[0] + 1), (VkComponentSwizzle)(c[1] + 1),
                                  (VkComponentSwizzle)(c[2] + 1), (VkComponentSwizzle)(c[3] + 1)};
    }
    return view_of(g, type, fmt, level0, nlevels, layer0, nlayers, sw, aspect);
}

/* A black 1x1 texture of each view type, for unbound texture slots. */
VkImageView gpu_dummy_view(VkCommandBuffer cb, VkImageViewType type) {
    static NvnTexture dummies[8];
    NvnTexture *t = &dummies[type & 7];
    if (!t->layout.width) {
        int cube = type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        t->target = type == VK_IMAGE_VIEW_TYPE_3D ? TARGET_3D : cube ? TARGET_CUBE : TARGET_2D_ARRAY;
        t->format = 0x25;  /* RGBA8 */
        t->pool_flags = POOL_CPU_NO_ACCESS;
        t->layout.width = t->layout.height = t->layout.depth = t->layout.levels = 1;
        t->layout.layers = cube ? 6 : 1;
        t->width = t->height = t->depth = t->levels = 1;
    }
    return gpu_sample_view(cb, t, NULL, 0, type);
}

/* The image stays: copies of the texture (texture pool entries, recorded render targets)
 * may still use it, as the memory stays on the Switch. Unused images are freed later
 * (evict_images). */
void gpu_texture_finalize(NvnTexture *t) {
    if (t) t->gpu = NULL;
}

/* ================================================================== samplers */

static VkCompareOp compare_op(uint32_t f) {
    static const VkCompareOp ops[9] = {VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS,
                                       VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER,
                                       VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return f < 9 ? ops[f] : VK_COMPARE_OP_ALWAYS;
}

/* Texture filtering: every sampler the game makes has anisotropic filtering off and picks one mip
 * level per texel (linear within it, nearest between levels: min filter 3), so distant and slanted
 * surfaces drop to small, blurry levels early, in steps (2026-10-27, YW4_SAMPLER_LOG). With this on,
 * mipmapped samplers that don't compare (not the shadow maps) blend between levels and filter
 * anisotropically with up to this many taps. YW4_ANISO=0 (the game's) / 2 / 4 / 8 / 16 (the default)
 * at start, the settings panel (F1) while playing. */
static int aniso = -1;

int gpu_aniso(void) {
    if (!vk.device) return 0;  /* asked before the device exists: decide later */
    int a = __atomic_load_n(&aniso, __ATOMIC_ACQUIRE);
    if (a < 0) {
        const char *e = getenv("YW4_ANISO");
        int v = e ? atoi(e) : 16;
        a = v == 0 || v == 2 || v == 4 || v == 8 ? v : 16;
        if (!vk.sampler_anisotropy) a = 0;
        __atomic_store_n(&aniso, a, __ATOMIC_RELEASE);
    }
    return a;
}

void gpu_aniso_cycle(void) {
    int a = gpu_aniso();
    a = a == 0 ? 2 : a == 2 ? 4 : a == 4 ? 8 : a == 8 ? 16 : 0;
    if (!vk.sampler_anisotropy) a = 0;
    __atomic_store_n(&aniso, a, __ATOMIC_RELEASE);
    fprintf(stderr, "yw4: gpu: texture filtering %s\n", a ? (a == 2 ? "x2" : a == 4 ? "x4" : a == 8 ? "x8" : "x16")
                                                                 : "the game's");
}

/* Upscaling's texture bias (decision 0039): drawn smaller, the 3D would pick smaller mip levels, where fine
 * patterns (nets, perforated fences) average out to a flat colour that FSR has nothing to rebuild from (seen
 * 2026-10-29: the schoolyard's goal net gone, the high fence's panels empty at performance). So the 3D's
 * mipmapped textures are read as at the output's size, and one level finer as AMD recommends: log2(render /
 * output) - 1 (quality -1.58, balanced -1.77, performance -2). Not for the draws after the anti-aliasing point
 * (the UI, drawn at the output's size). YW4_FSR_MIP_BIAS=<x> replaces the -1. */
static float upscale_lod_bias(void) {
    int m = gpu_upscale();
    if (!m || __atomic_load_n(&upscaled_scene, __ATOMIC_ACQUIRE)) return 0.0f;
    static float extra = 1.0f;  /* 1.0: not read yet */
    if (extra > 0.5f) extra = getenv("YW4_FSR_MIP_BIAS") ? (float)atof(getenv("YW4_FSR_MIP_BIAS")) : -1.0f;
    return log2f(upscale_ratio[m] > 0.0f ? 1.0f / upscale_ratio[m] : 1.0f) + extra;
}

VkSampler gpu_sampler(const NvnSamplerDesc *d) {
    enum { MAX_SAMPLERS = 1024 };
    static struct {
        NvnSamplerDesc desc;
        int aniso;   /* gpu_aniso() it was made with */
        float bias;  /* ... and upscale_lod_bias() */
        VkSampler s;
    } cache[MAX_SAMPLERS];
    static int n;
    int af = gpu_aniso();
    float ub = upscale_lod_bias();
    for (int i = 0; i < n; i++)
        if (cache[i].aniso == af && cache[i].bias == ub && !memcmp(&cache[i].desc, d, sizeof *d)) return cache[i].s;
    if (n == MAX_SAMPLERS) gpu_stop("more than %d different samplers", MAX_SAMPLERS);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->min_filter;
    si.minFilter = mf == 1 || mf == 3 || mf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    int border = 0;
    for (int i = 0; i < 3; i++) switch (d->wrap[i]) {
        case 0: case 7: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 2: case 3: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; break;
        case 4: case 5: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, border = 1; break;
        case 6: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    si.addressModeU = modes[0], si.addressModeV = modes[1], si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= 1) si.maxLod = si.minLod + 0.25f;  /* no mipmapping: the base level */
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.mipLodBias = d->lod_bias;
    if (d->max_aniso > 1) si.anisotropyEnable = VK_TRUE, si.maxAnisotropy = fminf(d->max_aniso, 16);
    if (af && mf >= 2 && !d->compare_mode && si.maxLod > si.minLod + 0.25f) {  /* better filtering (above) */
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        if (si.minFilter == VK_FILTER_LINEAR && (float)af > d->max_aniso)
            si.anisotropyEnable = VK_TRUE, si.maxAnisotropy = fminf((float)af, vk.props.limits.maxSamplerAnisotropy);
    }
    if (ub != 0.0f && mf >= 2 && !d->compare_mode && si.maxLod > si.minLod + 0.25f) si.mipLodBias += ub;  /* above */
    if (d->compare_mode) si.compareEnable = VK_TRUE, si.compareOp = compare_op(d->compare_func);
    if (border) {  /* nearest standard border colour (custom colours: to do) */
        const float *c = d->border;
        si.borderColor = c[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                         : c[0] + c[1] + c[2] > 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                     : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    }
    static int log_samplers = -1;
    if (log_samplers < 0) log_samplers = getenv("YW4_SAMPLER_LOG") && atoi(getenv("YW4_SAMPLER_LOG"));
    if (log_samplers)  /* YW4_SAMPLER_LOG=1: each new sampler as the game set it */
        fprintf(stderr, "yw4: sampler %d: min %u mag %u wrap %u,%u lod %g..%g bias %g aniso %g compare %u\n", n,
                d->min_filter, d->mag_filter, d->wrap[0], d->wrap[1], d->lod_min, d->lod_max, d->lod_bias, d->max_aniso,
                d->compare_mode);
    cache[n].desc = *d, cache[n].aniso = af, cache[n].bias = ub;
    CHECK(vkCreateSampler(vk.device, &si, NULL, &cache[n].s));
    return cache[n++].s;
}

/* ================================================================== command lists */

static VkImageSubresourceRange target_range(const GpuImage *g, const NvnTextureView *v, int has_view) {
    VkImageSubresourceRange r = {g->aspect, 0, 1, 0, g->layers};
    if (has_view && v->setmask & 1) r.baseMipLevel = v->base_level;
    if (has_view && v->setmask & 2) r.baseArrayLayer = v->min_layer, r.layerCount = v->num_layers ? v->num_layers : 1;
    return r;
}

void gpu_execute(uint64_t handle) {
    host_lock(&gpu_lock);
    VkCommandBuffer cb = begin();
    full_barrier(cb);  /* after the previous submission, and the CPU's writes before it */
    /* GPU state belongs to the queue, not to a list: it carries over between submissions */
    static GpuState s;
    static int state_ready;
    if (!state_ready) gpu_state_init(&s), state_ready = 1;
    s.after_composite = 0;  /* ... except this: the UI follows the composite in the same list (gpu_draw.c) */
    uint64_t at = handle;
    static uint64_t exec;  /* which run of a list this is (gpu_ab.c) */
    exec++;
    for (int n = 0;; n++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)at;
        if (r->op >= CMD_COUNT || r->size < sizeof(CmdRec) || r->size & 7 || n > 10000000) {
            fprintf(stderr, "yw4: gpu: command list %#" PRIx64 " is broken at %#" PRIx64 " (op %u, size %u)\n", handle,
                    at, r->op, r->size);
            exit(3);
        }
        if (r->op == CMD_END) break;
        if (r->op == CMD_JUMP) {
            at = r->x[0];
            continue;
        }
        switch (r->op) {
        /* ---- state */
        case CMD_BIND_PROGRAM: s.program = (NvnProgram *)(uintptr_t)r->x[0]; break;
        case CMD_BIND_BLEND: memcpy(s.blend[r->extra[0] & 7], r->extra, 8); break;
        case CMD_BIND_COLOR: memcpy(s.color, r->extra, 4); break;
        case CMD_BIND_CHANNEL_MASK: memcpy(&s.channel_mask, r->extra, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(s.depth_stencil, r->extra, 8); break;
        case CMD_BIND_POLYGON: memcpy(s.polygon, r->extra, 4); break;
        case CMD_BIND_MULTISAMPLE: break;  /* single-sampled for now */
        case CMD_BIND_VERTEX_ATTRIB:
            s.nattribs = r->x[0] < 16 ? (uint32_t)r->x[0] : 16;
            memcpy(s.attribs, r->extra, s.nattribs * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            s.nstreams = r->x[0] < 16 ? (uint32_t)r->x[0] : 16;
            memcpy(s.streams, r->extra, s.nstreams * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:  /* (index, address, size) */
            if (r->x[0] < 16) s.vbuf[r->x[0]] = (GpuRange){r->x[1], r->x[2]};
            break;
        case CMD_BIND_UNIFORM_BUFFERS:  /* (stage, index, address, size) */
            if (r->x[0] < STAGES && r->x[1] < UBOS) s.ubo[r->x[0]][r->x[1]] = (GpuRange){r->x[2], r->x[3]};
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            if (r->x[0] < STAGES && r->x[1] < SSBOS) s.ssbo[r->x[0]][r->x[1]] = (GpuRange){r->x[2], r->x[3]};
            break;
        case CMD_BIND_TEXTURES:  /* (stage, index, handle) */
            if (r->x[0] < STAGES && r->x[1] < TEXTURES) s.tex[r->x[0]][r->x[1]] = r->x[2];
            break;
        case CMD_BIND_IMAGES:
            if (r->x[0] < STAGES && r->x[1] < TEXTURES) s.img[r->x[0]][r->x[1]] = r->x[2];
            break;
        case CMD_SET_TEXTURE_POOL:
        case CMD_SET_SAMPLER_POOL: {
            const NvnDescPool **p = r->op == CMD_SET_TEXTURE_POOL ? &s.tex_pool : &s.sampler_pool;
            if (*p != (const NvnDescPool *)(uintptr_t)r->x[0] && nvn_trace_texpool(UINT32_MAX) > 0)
                fprintf(stderr, "yw4: texpool: frame %" PRIu64 ": %s pool %p -> %p\n", gpu_frames(),
                        r->op == CMD_SET_TEXTURE_POOL ? "texture" : "sampler", (const void *)*p, (const void *)(uintptr_t)r->x[0]);
            *p = (const NvnDescPool *)(uintptr_t)r->x[0];
            break;
        }
        case CMD_SET_RENDER_TARGETS:
            gpu_end_rendering(cb, &s);
            gpu_taa_targets_end(cb);  /* TAA: the G-buffer's motion, before the game reuses it */
            memcpy(&s.rt, r->extra, sizeof s.rt);
            render_targets_fix(&s.rt);  /* point at our own copies of the textures */
            s.ncolor = (int)r->x[0];
            s.targets_noted = 0;  /* noted at the first draw, with the state the game draws with */
            gpu_ab_targets(cb, &s);
            gpu_passes_targets(cb, &s);
            break;
        case CMD_SET_VIEWPORTS:  /* (x, y, w, h) */
            for (int i = 0; i < 4; i++) s.viewport[i] = (int32_t)r->x[i];
            s.have_viewport = 1;
            break;
        case CMD_SET_SCISSORS:
            for (int i = 0; i < 4; i++) s.scissor[i] = (int32_t)r->x[i];
            s.have_scissor = 1;
            break;
        case CMD_SET_DEPTH_RANGES: s.depth_range[0] = r->f[0], s.depth_range[1] = r->f[1]; break;
        case CMD_SET_BLEND_COLOR: memcpy(s.blend_color, r->extra, 16); break;
        case CMD_SET_STENCIL: {  /* (faces, value); x[6]: 0 reference, 1 write mask, 2 value mask */
            uint32_t *v = r->x[6] == 0 ? s.stencil_ref : r->x[6] == 1 ? s.stencil_mask : s.stencil_value_mask;
            if (r->x[0] & 1) v[0] = (uint32_t)r->x[1];
            if (r->x[0] & 2) v[1] = (uint32_t)r->x[1];
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(s.polygon_offset, r->f, 12); break;
        case CMD_SET_RASTERIZER_DISCARD: s.rasterizer_discard = (int)r->x[0]; break;
        case CMD_SET_DEPTH_CLAMP: s.depth_clamp = (int)r->x[0]; break;

        /* ---- work */
        case CMD_CLEAR_COLOR: {  /* (index, colour, mask) */
            int i = (int)r->x[0];
            NvnTexture *t = i < s.ncolor ? s.rt.color[i] : NULL;
            if (!t) break;
            gpu_end_rendering(cb, &s);
            if ((r->x[2] & 0xF) != 0xF) {
                static int logged;
                if (!logged++)
                    fprintf(stderr, "yw4: gpu: clear with channel mask %#x: whole texels for now\n", (unsigned)r->x[2]);
            }
            GpuImage *g = image_of(cb, t);
            VkClearColorValue color;
            memcpy(color.float32, r->extra, 16);
            VkImageSubresourceRange range = target_range(g, &s.rt.views[i], s.rt.has_view[i]);
            vkCmdClearColorImage(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
            full_barrier(cb);
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL: {  /* (depth in s0; depth mask, stencil, stencil mask in x1..x3) */
            NvnTexture *t = s.rt.depth;
            if (!t) break;
            gpu_end_rendering(cb, &s);
            GpuImage *g = image_of(cb, t);
            VkImageSubresourceRange range = target_range(g, &s.rt.depth_view, s.rt.has_depth_view);
            range.aspectMask = 0;
            if (r->x[0] && g->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) range.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if ((r->x[2] & 0xFF) && g->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            if (!range.aspectMask) break;
            VkClearDepthStencilValue v = {.depth = r->f[0], .stencil = (uint32_t)r->x[1] & 0xFF};
            vkCmdClearDepthStencilImage(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            full_barrier(cb);
            break;
        }
        case CMD_DRAW_ARRAYS: case CMD_DRAW_ELEMENTS:
            if (!s.targets_noted) gpu_shadow_note_targets(&s), s.targets_noted = 1;
            /* (primitive, type, count, ...) / (primitive, first, count, ...): count, and the instances */
            gpu_passes_draw(s.program, r->x[2] * (r->op == CMD_DRAW_ELEMENTS ? (r->x[6] ? r->x[6] : 1) : (r->x[4] ? r->x[4] : 1)));
            gpu_draw(cb, &s, r);
            break;
        case CMD_DRAW_ARRAYS_INDIRECT: case CMD_DRAW_ELEMENTS_INDIRECT: case CMD_DRAW_TRANSFORM_FEEDBACK: {
            static int logged;
            if (!logged++) fprintf(stderr, "yw4: gpu: indirect / transform feedback draws skipped for now\n");
            break;
        }
        case CMD_DISPATCH: {
            gpu_passes_other(cb, "compute", s.program ? s.program->stages[5].key : 0);
            gpu_dispatch(cb, &s, r);
            gpu_passes_targets(cb, &s);
            break;
        }
        case CMD_BARRIER:
            gpu_end_rendering(cb, &s);
            full_barrier(cb);
            break;
        case CMD_NATIVE:  /* a pass of our own renderer (decision 0024) */
            if ((NvnNativePass)(uintptr_t)r->x[0] == gpu_ab_native) {  /* same-frame A/B: may run a range again */
                if (gpu_ab_marker(cb, &s, r->extra, (uint32_t)r->x[1], exec, at + r->size, &at)) continue;
                break;
            }
            if ((NvnNativePass)(uintptr_t)r->x[0] == gpu_shadow_pass) {  /* our shadow pass: its own time */
                gpu_passes_other(cb, "our shadow pass", 0);
                gpu_shadow_pass(cb, &s, r->extra, (uint32_t)r->x[1]);
                gpu_passes_targets(cb, &s);
                break;
            }
            if ((NvnNativePass)(uintptr_t)r->x[0] == gpu_gbuffer_draw) {  /* a draw, in place */
                /* noted as the game's draws are: our shadow pass draws where a layer's draws went
                 * (a cutscene's second shadow map), and a layer can be all in-place draws */
                if (!s.targets_noted) gpu_shadow_note_targets(&s), s.targets_noted = 1;
                const GBufferDraw *d = (const GBufferDraw *)r->extra;
                gpu_passes_draw(s.program, (uint64_t)(d->index_count ? d->index_count : d->vertex_count) * (d->instances ? d->instances : 1));
            }
            ((NvnNativePass)(uintptr_t)r->x[0])(cb, &s, r->extra, (uint32_t)r->x[1]);
            break;
        case CMD_REPORT_COUNTER: {  /* (counter, address): NVNcounterData {u64 value, u64 GPU ticks at 614.4 MHz} */
            static uint32_t logged;
            if (r->x[0] < 32 && !(logged & 1u << r->x[0])) {
                logged |= 1u << r->x[0];
                fprintf(stderr, "yw4: gpu: ReportCounter type %u (first use)\n", (unsigned)r->x[0]);
            }
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            out[0] = 0;
            out[1] = (uint64_t)((unsigned __int128)(uint64_t)host_now_ns() * 384 / 625);
            break;
        }
        }
        at += r->size;
    }
    gpu_end_rendering(cb, &s);
    submit(cb);
    host_unlock(&gpu_lock);
}

/* ================================================================== present */

/* YW4_DUMP_IMAGE=<guest addresses in hex, comma separated> (with YW4_DUMP_IMAGE_AT=<frame>): at
 * that frame, reads back every image at those addresses (depth, RGBA8, RGBA16F or R11G11B10F),
 * prints per layer the minimum, maximum and how much of it still holds the first texel's value
 * (red for colour), and writes work/dump/image_<addr>_<n>.pgm (scaled to its own range) and, for
 * colour, image_<addr>_<n>.rgbaf (u32 width, height, then RGBA floats). Images are reused within a
 * frame: this is the last thing drawn into them (YW4_CAPTURE_FS, gpu_draw.c, is mid-frame).
 * gpu_lock held. */
static int dump_request;
void gpu_request_dump(void) { __atomic_store_n(&dump_request, 1, __ATOMIC_RELEASE); }

static float dump_half(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? 65504.0f : ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

/* an unsigned small float with a 5-bit exponent and mbits of mantissa (R11G11B10) */
static float dump_small(uint32_t v, int mbits) {
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    return e == 0 ? ldexpf((float)m, -14 - mbits) : e == 31 ? 65504.0f : ldexpf((float)(m | (1u << mbits)), (int)e - 15 - mbits);
}

static void dump_images(uint64_t frame) {
    int all = __atomic_exchange_n(&dump_request, 0, __ATOMIC_ACQ_REL);
    /* YW4_DUMP_IMAGE_AT may list several frames (comma separated): then the files get the frame
     * in their name (image_<addr>_f<frame>_<layer>), to compare frames of one run */
    static uint64_t want[16], at[8];
    static int nwant = -1, nat;
    if (nwant < 0) {
        nwant = 0;
        const char *a = getenv("YW4_DUMP_IMAGE");
        for (char *end; a && *a && nwant < 16; a = *end ? end + 1 : end) want[nwant++] = strtoull(a, &end, 16);
        a = getenv("YW4_DUMP_IMAGE_AT");
        for (char *end; a && *a && nat < 8; a = *end ? end + 1 : end) at[nat++] = strtoull(a, &end, 10);
        if (!nat) at[nat++] = 0;
    }
    int listed = 0;
    for (int i = 0; i < nat; i++) listed |= frame == at[i];
    if (!all && (!nwant || !listed)) return;
    char tag[24] = "";
    if (nat > 1) snprintf(tag, sizeof tag, "_f%" PRIu64, frame);
    for (int b = 0; b < IMAGE_BUCKETS; b++)
        for (GpuImage *g = images[b]; g; g = g->next) {
            int wanted = 0;
            for (int k = 0; k < nwant; k++) wanted |= g->key.addr == want[k];
            if (!all && !wanted) continue;
            if (all && (g->layers < 2 || !(g->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))) continue;
            uint64_t addr = g->key.addr;
            int depth = g->aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
            int half = g->format == VK_FORMAT_R16G16B16A16_SFLOAT, packed = g->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
            uint32_t bpp = depth ? (g->format == VK_FORMAT_D16_UNORM ? 2 : 4) : half ? 8 : 4;
            int d24 = g->format == VK_FORMAT_D24_UNORM_S8_UINT;
            if (depth && !d24 && g->format != VK_FORMAT_D32_SFLOAT && g->format != VK_FORMAT_D16_UNORM &&
                g->format != VK_FORMAT_D32_SFLOAT_S8_UINT) { fprintf(stderr, "yw4: image %#" PRIx64 ": depth format %d not dumped\n", addr, (int)g->format); continue; }
            if (!depth && !half && !packed && g->format != VK_FORMAT_R8G8B8A8_UNORM && g->format != VK_FORMAT_R8G8B8A8_SRGB) { fprintf(stderr, "yw4: image %#" PRIx64 ": format %d not dumped\n", addr, (int)g->format); continue; }
            size_t layer_bytes = (size_t)g->width * g->height * bpp, total = layer_bytes * g->layers;
            VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = total,
                                      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VkBuffer buf;
            CHECK(vkCreateBuffer(vk.device, &bci, NULL, &buf));
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(vk.device, buf, &mr);
            VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                        .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits,
                                                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
            VkDeviceMemory mem;
            CHECK(vkAllocateMemory(vk.device, &mai, NULL, &mem));
            CHECK(vkBindBufferMemory(vk.device, buf, mem, 0));
            VkCommandBuffer cb = begin();
            full_barrier(cb);
            VkBufferImageCopy cp = {.imageSubresource = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                                         g->layers},
                                    .imageExtent = {g->width, g->height, 1}};
            vkCmdCopyImageToBuffer(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &cp);
            full_barrier(cb);
            submit(cb);
            gpu_wait(gpu_submitted(), UINT64_MAX);
            void *p;
            CHECK(vkMapMemory(vk.device, mem, 0, VK_WHOLE_SIZE, 0, &p));
            for (uint32_t l = 0; l < g->layers; l++) {
                const uint8_t *src = (const uint8_t *)p + l * layer_bytes;
                size_t n = (size_t)g->width * g->height;
                double mn = 1e30, mx = -1e30;
                size_t same = 0;
                double first = 0;
                double *v = malloc(n * sizeof *v);
                for (size_t i = 0; i < n; i++) {
                    double x;
                    if (d24) x = (((const uint32_t *)src)[i] & 0xFFFFFF) / 16777215.0;
                    else if (depth && bpp == 4) x = ((const float *)src)[i];
                    else if (depth) x = ((const uint16_t *)src)[i] / 65535.0;
                    else if (half) x = dump_half(((const uint16_t *)src)[i * 4]);  /* red */
                    else if (packed) x = dump_small(((const uint32_t *)src)[i] & 0x7ff, 6);
                    else x = src[i * 4] / 255.0;  /* red */
                    v[i] = x;
                    if (x < mn) mn = x;
                    if (x > mx) mx = x;
                    if (i == 0) first = x;
                    same += x == first;
                }
                fprintf(stderr, "yw4: image %#" PRIx64 " frame %" PRIu64 " %ux%u format %d layer %u: min %g max %g, %.1f%% equal to the first texel (%g)\n",
                        addr, frame, g->width, g->height, (int)g->format, l, mn, mx, 100.0 * (double)same / (double)n, first);
                char path[256];
                snprintf(path, sizeof path, "work/dump/image_%" PRIx64 "%s_%u.pgm", addr, tag, l);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P5\n%u %u\n255\n", g->width, g->height);
                    for (size_t i = 0; i < n; i++) {
                        double u = mx > mn ? (v[i] - mn) / (mx - mn) : 0;
                        fputc((int)(u * 255.0 + 0.5), f);
                    }
                    fclose(f);
                }
                if (depth) {  /* depth as raw floats, for tools */
                    snprintf(path, sizeof path, "work/dump/image_%" PRIx64 "%s_%u.depthf", addr, tag, l);
                    f = fopen(path, "wb");
                    if (f) {
                        fwrite(&g->width, 4, 1, f), fwrite(&g->height, 4, 1, f);
                        for (size_t i = 0; i < n; i++) {
                            float x = (float)v[i];
                            fwrite(&x, 4, 1, f);
                        }
                        fclose(f);
                    }
                }
                if (!depth) {  /* colour as raw RGBA floats, for tools */
                    snprintf(path, sizeof path, "work/dump/image_%" PRIx64 "%s_%u.rgbaf", addr, tag, l);
                    f = fopen(path, "wb");
                    if (f) {
                        fwrite(&g->width, 4, 1, f), fwrite(&g->height, 4, 1, f);
                        for (size_t i = 0; i < n * 4; i++) {
                            uint32_t w_ = packed ? ((const uint32_t *)src)[i / 4] : 0;
                            float x = half ? dump_half(((const uint16_t *)src)[i])
                                    : packed ? (i % 4 == 0 ? dump_small(w_ & 0x7ff, 6) : i % 4 == 1 ? dump_small((w_ >> 11) & 0x7ff, 6)
                                                : i % 4 == 2 ? dump_small(w_ >> 22, 5) : 1.0f)
                                    : src[i] / 255.0f;
                            fwrite(&x, 4, 1, f);
                        }
                        fclose(f);
                    }
                }
                free(v);
            }
            vkDestroyBuffer(vk.device, buf, NULL);
            vkFreeMemory(vk.device, mem, NULL);
        }
}

/* The textures presented lately: the game alternates two window images (widescreen's UI rule, gpu_draw.c). */
static uint64_t presented[4];

int gpu_is_presented(uint64_t addr) {
    for (int i = 0; i < 4; i++)
        if (addr && __atomic_load_n(&presented[i], __ATOMIC_RELAXED) == addr) return 1;
    return 0;
}

void gpu_present(NvnTexture *t, uint64_t frame, const int crop[4]) {
    host_lock(&gpu_lock);
    present_count++;
    gpu_upscale_begin(0, 0);  /* the scene's images are the 3D's again (upscaling) */
    scan_writes();
    if (present_count % 300 == 0) evict_images();
    if (!gpu_is_presented(t->addr)) {
        static int next;
        __atomic_store_n(&presented[next++ % 4], t->addr, __ATOMIC_RELAXED);
    }
    GpuImage *g = image_find(t);
    if (!g) {  /* never drawn to: make it (black) */
        VkCommandBuffer cb = begin();
        g = image_of(cb, t);
        submit(cb);
    }
    g->last_frame = present_count;
    if (gpu_capture_pending()) {
        gpu_wait(gpu_submitted(), UINT64_MAX);
        gpu_capture_flush();
    }
    if (gpu_ab_pending()) {
        gpu_wait(gpu_submitted(), UINT64_MAX);
        gpu_ab_flush();
    }
    if (gpu_post_pending()) {
        gpu_wait(gpu_submitted(), UINT64_MAX);
        gpu_post_flush();
    }
    dump_images(present_count);
    if (getenv("YW4_COMPUTE_TRACE")) {
        gpu_wait(gpu_submitted(), UINT64_MAX);
        gpu_compute_trace(present_count);
    }
    host_unlock(&gpu_lock);
    VkRect2D r = {{0, 0}, {g->width, g->height}};
    /* the crop, in the game's pixels: scaled with the window image (YW4_RES) */
    uint32_t cx = gpu_scaled(gpu_scaled(crop[0] > 0 ? (uint32_t)crop[0] : 0, g->scale_num, g->scale_den), g->wide_num,
                             g->wide_den);
    uint32_t cy = gpu_scaled(crop[1] > 0 ? (uint32_t)crop[1] : 0, g->scale_num, g->scale_den);
    uint32_t cw = gpu_scaled(gpu_scaled(crop[2] > 0 ? (uint32_t)crop[2] : 0, g->scale_num, g->scale_den), g->wide_num,
                             g->wide_den);
    uint32_t ch = gpu_scaled(crop[3] > 0 ? (uint32_t)crop[3] : 0, g->scale_num, g->scale_den);
    if (crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 && cx + cw <= g->width && cy + ch <= g->height)
        r = (VkRect2D){{(int32_t)cx, (int32_t)cy}, {cw, ch}};
    window_present_image(g->image, g->format, r, frame);
}
