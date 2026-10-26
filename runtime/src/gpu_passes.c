/* Per-pass GPU timing (F5, or YW4_GPU_PASSES=1 at start): where the GPU's time goes in a frame.
 *
 * While it is on, every submission writes a GPU timestamp at each change of render targets (the
 * start of a pass), around each compute dispatch and around each pass of our own renderer
 * (CMD_NATIVE), plus one at its start and end. The time between two marks belongs to the pass
 * the first one started. Passes are grouped by what they draw into (the colour targets' and the
 * depth target's sizes and NVN formats) and their first program, summed over 300 frames, and the
 * most expensive groups go to the log in milliseconds per frame. A timestamp waits for the work
 * before it, so a pass's time includes any wait for the one before (barriers). gpu_lock held. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_internal.h"

enum { SLOTS = 16, PER_SLOT = 512, GROUPS = 512, REPORT_FRAMES = 300, SHOWN = 24 };

typedef struct {
    char label[80];
    uint64_t vs, fs;  /* the first program drawn or dispatched after the mark */
    uint32_t draws, restarts, uploads;  /* rendering ended inside the pass; textures uploaded */
    uint64_t vertices;  /* drawn: indices (or vertices) x instances */
} Mark;

static VkQueryPool pool;
static double period_ns;
static int on = -1;
static Mark marks[SLOTS][PER_SLOT];
static uint32_t nmarks[SLOTS];
static int timed[SLOTS];  /* the slot's submission writes marks */
static int cur = -1;      /* the slot being recorded */

typedef struct {
    char label[80];
    uint64_t vs, fs;
    double ms;
    long draws, passes, restarts, uploads;
    uint64_t vertices;
} Group;
static Group groups[GROUPS];
static int ngroups;
static uint64_t report_from;

static int enabled(void) {
    if (on < 0) on = getenv("YW4_GPU_PASSES") && atoi(getenv("YW4_GPU_PASSES"));
    return on;
}

void gpu_passes_toggle(void) {
    on = !enabled();
    ngroups = 0, report_from = gpu_frames();
    fprintf(stderr, "yw4: gpu passes: timing %s (F5)\n", on ? "on: the most expensive passes every 300 frames" : "off");
}

static void mark(VkCommandBuffer cb, const char *label) {
    if (cur < 0 || !timed[cur] || nmarks[cur] >= PER_SLOT - 1) return;
    Mark *m = &marks[cur][nmarks[cur]];
    snprintf(m->label, sizeof m->label, "%s", label);
    m->vs = m->fs = 0, m->draws = m->restarts = m->uploads = 0, m->vertices = 0;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, (uint32_t)cur * PER_SLOT + nmarks[cur]++);
}

/* gpu.c, begin(): the slot's submission starts. */
void gpu_passes_begin(VkCommandBuffer cb, int slot) {
    cur = slot;
    timed[slot] = 0;
    if (!enabled() || slot >= SLOTS) return;
    if (!pool) {
        if (!vk.props.limits.timestampComputeAndGraphics || vk.props.limits.timestampPeriod <= 0) return;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SLOTS * PER_SLOT};
        if (vkCreateQueryPool(vk.device, &qi, NULL, &pool) != VK_SUCCESS) return;
        period_ns = vk.props.limits.timestampPeriod;
    }
    vkCmdResetQueryPool(cb, pool, (uint32_t)slot * PER_SLOT, PER_SLOT);
    timed[slot] = 1, nmarks[slot] = 0;
    mark(cb, "(start of a submission)");
}

/* gpu.c, submit(): the last mark closes the last pass. */
void gpu_passes_end(VkCommandBuffer cb) {
    mark(cb, "");
    cur = -1;
}

static void describe_targets(const GpuState *s, char *out, size_t size) {
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < s->ncolor && i < 8 && n < size; i++) {
        const NvnTexture *t = s->rt.color[i];
        if (t) n += (size_t)snprintf(out + n, size - n, "%s%ux%u f%u", n ? " " : "", t->width, t->height, t->format);
    }
    const NvnTexture *d = s->rt.depth;
    if (d && n < size) n += (size_t)snprintf(out + n, size - n, "%sdepth %ux%u f%u", n ? " + " : "", d->width, d->height, d->format);
    if (!n) snprintf(out, size, "(no targets)");
}

/* gpu.c: render targets set (after the previous rendering ended), or a dispatch or a native pass
 * done (what follows draws into the targets again). */
void gpu_passes_targets(VkCommandBuffer cb, const GpuState *s) {
    if (cur < 0 || !timed[cur]) return;
    char label[80];
    describe_targets(s, label, sizeof label);
    mark(cb, label);
}

/* gpu.c: a compute dispatch or a pass of our own is about to run. */
void gpu_passes_other(VkCommandBuffer cb, const char *what, uint64_t key) {
    if (cur < 0 || !timed[cur]) return;
    char label[80];
    snprintf(label, sizeof label, "%s", what);
    mark(cb, label);
    if (key && nmarks[cur]) marks[cur][nmarks[cur] - 1].vs = key;
}

/* gpu.c: a draw, with the program bound. */
void gpu_passes_draw(const NvnProgram *p, uint64_t vertices) {
    if (cur < 0 || !timed[cur] || !nmarks[cur]) return;
    Mark *m = &marks[cur][nmarks[cur] - 1];
    if (!m->draws && p) m->vs = p->stages[0].key, m->fs = p->stages[1].key;
    m->draws++;
    m->vertices += vertices;
}

/* gpu.c / gpu_draw.c: rendering ended (1) or a texture uploaded (2) inside the current pass. */
void gpu_passes_note(int what) {
    if (cur < 0 || !timed[cur] || !nmarks[cur]) return;
    Mark *m = &marks[cur][nmarks[cur] - 1];
    if (what == 1) m->restarts++;
    else m->uploads++;
}

static void report(void) {
    uint64_t frames = gpu_frames() - report_from;
    if (!frames) return;
    double total = 0;
    for (int i = 0; i < ngroups; i++) total += groups[i].ms;
    /* most expensive first */
    for (int i = 1; i < ngroups; i++)
        for (int j = i; j > 0 && groups[j].ms > groups[j - 1].ms; j--) {
            Group g = groups[j];
            groups[j] = groups[j - 1], groups[j - 1] = g;
        }
    fprintf(stderr, "yw4: gpu passes: %.2f ms of GPU time per frame over %" PRIu64 " frames; most expensive:\n",
            total / (double)frames, frames);
    for (int i = 0; i < ngroups && i < SHOWN; i++)
        fprintf(stderr, "yw4: gpu passes: %6.2f ms %5.1f passes %6.1f draws %6.1f restarts %6.1f uploads %7.0fk vertices  %-44s vs %016"
                PRIx64 " fs %016" PRIx64 "\n", groups[i].ms / (double)frames, (double)groups[i].passes / (double)frames,
                (double)groups[i].draws / (double)frames, (double)groups[i].restarts / (double)frames,
                (double)groups[i].uploads / (double)frames, (double)groups[i].vertices / (double)frames / 1000,
                groups[i].label, groups[i].vs, groups[i].fs);
    ngroups = 0;
    report_from = gpu_frames();
}

/* gpu.c: the slot's submission has completed (before its queries are reset). */
void gpu_passes_collect(int slot) {
    if (slot < 0 || slot >= SLOTS || !timed[slot]) return;
    timed[slot] = 0;
    uint32_t n = nmarks[slot];
    static uint64_t t[PER_SLOT];
    if (n < 2 || vkGetQueryPoolResults(vk.device, pool, (uint32_t)slot * PER_SLOT, n, sizeof t, t, sizeof t[0],
                                       VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    for (uint32_t i = 0; i + 1 < n; i++) {
        const Mark *m = &marks[slot][i];
        double ms = t[i + 1] >= t[i] ? (double)(t[i + 1] - t[i]) * period_ns / 1e6 : 0;
        int g = 0;
        while (g < ngroups && !(groups[g].vs == m->vs && groups[g].fs == m->fs && !strcmp(groups[g].label, m->label))) g++;
        if (g == ngroups) {
            if (ngroups == GROUPS) g = GROUPS - 1;  /* full: the last group takes the rest */
            else {
                ngroups++;
                memset(&groups[g], 0, sizeof groups[g]);
                memcpy(groups[g].label, m->label, sizeof groups[g].label);
                groups[g].vs = m->vs, groups[g].fs = m->fs;
            }
        }
        groups[g].ms += ms, groups[g].draws += m->draws, groups[g].passes++;
        groups[g].restarts += m->restarts, groups[g].uploads += m->uploads, groups[g].vertices += m->vertices;
    }
    if (gpu_frames() >= report_from + REPORT_FRAMES) report();
}
