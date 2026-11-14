/* The game's occlusion queries, and the draws it makes conditional on them (decision 0072).
 *
 * What the game does (measured, YW4_COUNTER_LOG): before each shadow cascade and before the main view
 * it draws one 36-vertex box per object into a 240x135 depth-only target, each between two
 * ReportCounter(SAMPLES_PASSED) calls that write to addresses 16 bytes apart (two NVNcounterData
 * {u64 value, u64 timestamp}). Later in the same frame its model renderer (f_a45890) wraps that
 * object's draws in SetRenderEnableConditional(RENDER_IF_NOT_EQUAL, first address): the GPU itself
 * compares the two values and skips the draws when they are equal, i.e. when no sample of the box
 * passed. The CPU never has to read the counts, so the answer is needed on the GPU within the frame.
 *
 * Here: a pair of reports becomes one Vulkan occlusion query around the draws between them. NVN's
 * counter runs on; ours writes 0 as the first value and the query's count as the second, so the
 * difference (all anyone uses) is right. The count goes into the game's own memory on the GPU
 * (vkCmdCopyQueryPoolResults into the imported pool, decision 0021): no wait, no read-back. The
 * conditional draws use VK_EXT_conditional_rendering on the second value (its low 32 bits: is it 0?),
 * around each single draw, so nothing of ours (passes of our renderer, copies, upscaling) falls
 * under a condition by accident.
 *
 * Vulkan's rules that shape this: a query begun inside a render pass ends inside the same one, and
 * resets and result copies happen outside. So the query begins at the first draw after the first
 * report (rendering is on by then), results are copied when rendering next ends, and queries are
 * reset a block at a time. When a pass ends inside a pair and more draws follow (not seen), the
 * answer is "visible". Without the extension, or for an address no pair of ours wrote, the draws
 * are simply made, as before.
 *
 * All of it runs on the thread that records a submission, gpu_lock held. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_internal.h"
#include "host.h"

enum { QUERIES = 4096, BLOCK = 64, PENDING = 256, KNOWN = 8192 };
enum { PAIR_NONE, PAIR_WAITING, PAIR_COUNTING, PAIR_CLOSED };  /* waiting: for its first draw; closed: by a pass's end */
enum { RESULT_COUNT, RESULT_ZERO, RESULT_VISIBLE };

static VkQueryPool pool;
static int pool_state;  /* 0 not tried, 1 made, -1 none */
static uint32_t next_query, fresh;  /* fresh: reset queries left in the current block */
static struct {
    uint64_t begin;  /* the first report's address */
    uint32_t query;
    int state, lost;  /* lost: draws came after the query had to end */
    int draws;
} pair;
static struct {
    uint64_t addr;
    uint32_t query;
    int kind;
} pending[PENDING];  /* second values to write, once rendering has ended */
static int npending;
static uint64_t known[KNOWN];  /* first addresses of the pairs we wrote: what a condition may name */
static int cond_open;

/* YW4_OCCLUSION=0: the counts stay real, but the game's conditional draws are all made */
static int cull_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("YW4_OCCLUSION");
        on = !(e && *e == '0');
        fprintf(stderr, "yw4: gpu: occlusion queries: %s counts, conditional draws %s\n",
                vk.occlusion_precise ? "exact" : "zero-or-not",
                !vk.conditional_rendering ? "always drawn (no VK_EXT_conditional_rendering)"
                : on ? "skipped on the GPU when hidden" : "always drawn (YW4_OCCLUSION=0)");
    }
    return on && vk.conditional_rendering;
}

/* ---- YW4_COUNTER_LOG=<from>-<to>: in those GPU frames, one line per pair and per condition, and a
 * summary per frame. A count shown is what the game's memory holds at that moment: the GPU writes it
 * later, so it is usually the count of an earlier frame at the same address. */
static uint64_t log_from, log_to;
static struct {
    uint64_t frame;
    int pairs, conds, cond_draws, cond_draws_zero, cond_unknown;
} tally;
static int log_on(void) {
    static int read;
    if (!read) {
        read = 1;
        const char *e = getenv("YW4_COUNTER_LOG");
        if (e) sscanf(e, "%" SCNu64 "-%" SCNu64, &log_from, &log_to);
    }
    if (!log_to) return 0;
    uint64_t f = gpu_frames();
    if (tally.frame != f) {
        if (tally.pairs || tally.conds)
            fprintf(stderr, "yw4: counter: frame %" PRIu64 ": %d query pairs, %d conditions, %d draws under a condition"
                    " (%d with a count of 0 in memory, %d with an address no pair wrote)\n", tally.frame, tally.pairs,
                    tally.conds, tally.cond_draws, tally.cond_draws_zero, tally.cond_unknown);
        memset(&tally, 0, sizeof tally);
        tally.frame = f;
    }
    return f >= log_from && f <= log_to;
}
static uint64_t count_in_memory(uint64_t begin) {
    const uint64_t *v = (const uint64_t *)(uintptr_t)begin;
    return v[2] - v[0];
}

static int known_slot(uint64_t addr) {
    unsigned i = (unsigned)((addr >> 4) * 0x9E3779B1u) % KNOWN;
    for (int n = 0; n < 64; n++, i = (i + 1) % KNOWN)
        if (known[i] == addr || !known[i]) return (int)i;
    return -1;
}

static int have_pool(void) {
    if (!pool_state) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = QUERIES};
        pool_state = vkCreateQueryPool(vk.device, &qi, NULL, &pool) == VK_SUCCESS ? 1 : -1;
        if (pool_state < 0) fprintf(stderr, "yw4: gpu: no occlusion query pool: every query answers \"visible\"\n");
    }
    return pool_state > 0;
}

/* Writes the second values that are due. Outside rendering only. */
static void flush(VkCommandBuffer cb) {
    for (int i = 0; i < npending; i++) {
        VkBuffer buf;
        VkDeviceSize off;
        uint64_t addr = pending[i].addr;
        if (!gpu_buffer_at(addr, 8, &buf, &off) || off & 7) {  /* not in a pool (not seen): by hand, "visible" */
            *(uint64_t *)(uintptr_t)addr = pending[i].kind != RESULT_ZERO;
            continue;
        }
        if (pending[i].kind == RESULT_COUNT) {
            vkCmdCopyQueryPoolResults(cb, pool, pending[i].query, 1, buf, off, 8,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        } else {
            vkCmdFillBuffer(cb, buf, off, 4, pending[i].kind == RESULT_VISIBLE);
            vkCmdFillBuffer(cb, buf, off + 4, 4, 0);
        }
    }
    npending = 0;
}

void gpu_query_rendering_ends(VkCommandBuffer cb) {
    if (pair.state != PAIR_COUNTING) return;
    vkCmdEndQuery(cb, pool, pair.query);
    pair.state = PAIR_CLOSED, pair.lost = 0;
}

void gpu_query_rendering_ended(VkCommandBuffer cb) {
    if (npending) flush(cb);
}

void gpu_query_flush(VkCommandBuffer cb, GpuState *s) {
    if (!npending) return;
    if (s->rendering) {
        gpu_end_rendering(cb, s);  /* flushes, then its barrier */
    } else {
        flush(cb);
        full_barrier(cb);
    }
}

static void pair_abandon(VkCommandBuffer cb) {
    if (pair.state == PAIR_COUNTING) vkCmdEndQuery(cb, pool, pair.query);
    if (pair.state) {
        static int logged;
        if (!logged++)
            fprintf(stderr, "yw4: gpu: a SAMPLES_PASSED report at %#" PRIx64 " without its second one 16 bytes on: "
                    "that count stays 0 (logged once)\n", pair.begin);
    }
    pair.state = PAIR_NONE;
}

void gpu_query_report(VkCommandBuffer cb, GpuState *s, uint64_t addr) {
    if (pair.state && addr == pair.begin + 16) {  /* the pair's second report */
        int kind = RESULT_COUNT;
        if (pair.state == PAIR_COUNTING) vkCmdEndQuery(cb, pool, pair.query);
        else if (pair.state == PAIR_WAITING) kind = RESULT_ZERO;  /* nothing was drawn between the two */
        else if (pair.lost) kind = RESULT_VISIBLE;
        if (pair.lost) {
            static int logged;
            if (!logged++)
                fprintf(stderr, "yw4: gpu: an occlusion query at %#" PRIx64 " spans two render passes with draws in "
                        "both: answered \"visible\" (logged once)\n", pair.begin);
        }
        if (log_on()) {
            tally.pairs++;
            const NvnTexture *t = s->rt.depth ? s->rt.depth : s->rt.color[0];
            fprintf(stderr, "yw4: counter: frame %" PRIu64 ": pair at %#" PRIx64 ", %d draw%s into %ux%u (%d colour, depth "
                    "%#" PRIx64 "), %s; count in memory %" PRIu64 "\n", gpu_frames(), pair.begin, pair.draws,
                    pair.draws == 1 ? "" : "s", t ? t->width : 0, t ? t->height : 0, s->ncolor,
                    s->rt.depth ? s->rt.depth->addr : 0,
                    kind == RESULT_COUNT ? "counted" : kind == RESULT_ZERO ? "nothing drawn: 0" : "visible",
                    count_in_memory(pair.begin));
        }
        if (npending == PENDING) gpu_query_flush(cb, s);
        pending[npending].addr = addr, pending[npending].query = pair.query, pending[npending].kind = kind;
        npending++;
        int k = known_slot(pair.begin);
        if (k >= 0) known[k] = pair.begin;
        pair.state = PAIR_NONE;
        if (s->cond_mode) gpu_query_flush(cb, s);  /* a query under a condition: the next draw may read it */
        return;
    }
    pair_abandon(cb);
    *(uint64_t *)(uintptr_t)addr = 0;  /* the first value: the counter starts each pair at 0 */
    pair.begin = addr, pair.draws = 0, pair.lost = 0;
    if (!have_pool()) {
        pair.state = PAIR_CLOSED, pair.lost = 1;  /* answers "visible" */
        return;
    }
    if (!fresh) {  /* the next block of queries, reset: outside rendering */
        gpu_end_rendering(cb, s);
        if (next_query >= QUERIES) next_query = 0;
        vkCmdResetQueryPool(cb, pool, next_query, BLOCK);
        fresh = BLOCK;
    }
    pair.query = next_query++, fresh--;
    pair.state = PAIR_WAITING;
}

void gpu_query_render_enable(VkCommandBuffer cb, GpuState *s, int mode, uint64_t addr) {
    s->cond_mode = mode, s->cond_addr = addr;
    if (!mode) return;
    gpu_query_flush(cb, s);  /* its counts must be in memory before the draws that test them */
    if (log_on()) {
        tally.conds++;
        int k = known_slot(addr);
        if (tally.conds <= 40)
            fprintf(stderr, "yw4: counter: frame %" PRIu64 ": condition %s at %#" PRIx64 "%s; count in memory %" PRIu64
                    "\n", gpu_frames(), mode == 1 ? "IF_EQUAL" : "IF_NOT_EQUAL", addr,
                    k >= 0 && known[k] == addr ? "" : " (no pair wrote it)", count_in_memory(addr));
    }
}

void gpu_query_before_draw(VkCommandBuffer cb, GpuState *s) {
    if (pair.state == PAIR_WAITING) {
        vkCmdBeginQuery(cb, pool, pair.query, vk.occlusion_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
        pair.state = PAIR_COUNTING;
    } else if (pair.state == PAIR_CLOSED) {
        pair.lost = 1;
    }
    if (pair.state) pair.draws++;
    gpu_query_condition(cb, s->cond_mode, s->cond_addr);
}

void gpu_query_condition(VkCommandBuffer cb, int mode, uint64_t addr) {
    if (!mode) return;
    int k = known_slot(addr);
    int is_known = k >= 0 && known[k] == addr;
    if (log_on()) {
        tally.cond_draws++;
        if (!is_known) tally.cond_unknown++;
        else if (!count_in_memory(addr) == (mode == 2)) tally.cond_draws_zero++;
    }
    VkBuffer buf;
    VkDeviceSize off;
    if (!cull_on() || !is_known || !gpu_buffer_at(addr + 16, 4, &buf, &off) || off & 3) return;
    /* IF_NOT_EQUAL: draw when the second value (the count; the first is 0) is not 0 */
    VkConditionalRenderingBeginInfoEXT bi = {.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT,
                                             .buffer = buf, .offset = off,
                                             .flags = mode == 1 ? VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT : 0};
    vkCmdBeginConditionalRenderingEXT(cb, &bi);
    cond_open = 1;
}

void gpu_query_after_draw(VkCommandBuffer cb) {
    if (!cond_open) return;
    vkCmdEndConditionalRenderingEXT(cb);
    cond_open = 0;
}
