/* Minimap watch (YW4_MINIMAP_WATCH=1; python -m tools.dev.minimap): finds the frames in which the minimap goes
 * blank, and what was different about them.
 *
 * The game draws the minimap into small images of its own (340x304 and 516x460, docs/frame.md) and then puts
 * them on the screen with a UI draw that samples them. So a blank minimap is one of three things, and the watch
 * looks at each, every frame:
 *
 *   1. nothing (or less) was drawn into the minimap's image    -> the count of draws into it, and of clears
 *   2. it was drawn, but from textures that weren't there      -> its draws' textures: missing, or uploaded
 *                                                                 in this very frame
 *   3. the image was fine but never reached the screen         -> the count of draws elsewhere that sample it
 *
 * and at the image itself, read back two frames later without stalling (as the flash detector does, window.c):
 * how much of it is lit and how much is opaque. That is the ground truth for "blank"; the counts are the why.
 *
 * Written to YW4_MINIMAP_DIR (default work/minimap):
 *   minimap_draws.csv    one row per frame and image: the counts
 *   minimap_pixels.csv   one row per frame and image: lit and opaque texels (per mille), mean colour
 *   minimap_<frame>_<w>x<h>_{a_before,b_blank,c_after}.bmp   the image around a drop in lit or opaque texels
 *   minimap_<frame>_draws.txt   every draw of the frame before and of the frame a count dropped in
 * and an event per drop and recovery in the run's events file (events.h, kind "minimap").
 *
 * All of it runs on the thread that holds gpu_lock (gpu_execute, gpu_present). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MAKE_DIR(p) mkdir(p, 0777)
#endif

#include "events.h"
#include "gpu_internal.h"

enum { TARGETS = 4, SLOTS = 4, MAX_DRAWS = 256, DRAW_TEX = 6, MAX_SAVED = 40, RING = 8 };

typedef struct {
    uint64_t addr, handle;  /* the texels' guest address; the handle the draw named it by (pool ids) */
    uint32_t w, h, format;
    uint8_t slot, missing, fresh;
} WTex;

typedef struct {
    uint64_t vs, fs;
    uint32_t verts;
    int8_t target;   /* the minimap image drawn into, or -1: a draw elsewhere that samples one */
    int8_t samples;  /* ... which one */
    uint8_t ntex;
    WTex tex[DRAW_TEX];
    /* the draw's own data as the game's memory held it when the draw was translated: where, a fingerprint, and
     * the start of it as floats (the GPU reads the same memory, but later) */
    uint64_t vb_addr, vb_hash;
    uint32_t first, stride;
    uint8_t stale, nvf, ncb;
    float vf[16];
    struct { uint8_t stage, slot, nf; uint64_t addr, size, hash; float f[8]; } cb[4];
} WDraw;

typedef struct {
    int draws, clears, shown, missing, fresh;
    uint64_t verts;
} WCount;

typedef struct {
    uint64_t frame;
    WCount c[TARGETS];
    int ndraws;
    WDraw d[MAX_DRAWS];
} WFrame;

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *map;
    uint64_t seq, frame;  /* seq 0: nothing to read */
} WSlot;

typedef struct {
    NvnTexture tex;  /* a copy: the image is found by it at the end of the frame */
    uint64_t last_used;
    WSlot slot[SLOTS];
    uint32_t pw, ph;  /* the size the readback buffers were made for */
    /* the picture: the last one read, and a drop in progress */
    uint8_t *prev, *work;
    uint64_t prev_frame;
    int prev_lit, prev_opaque;
    int blank, base_lit, base_opaque;
    uint64_t blank_from;
    /* the counts: a drop in progress */
    int low, base_draws, base_shown;
    uint64_t low_from;
} WTarget;

static int on = -1;
static char dir[400] = "work/minimap";
static WTarget tg[TARGETS];
static int ntg;
static WFrame fr[RING];  /* the frame being drawn (cur) and the ones before: a picture is judged two frames late */
static int cur;
static FILE *draws_csv, *pixels_csv;
static int saved;

int gpu_watch_on(void) {
    if (on < 0) {
        const char *e = getenv("YW4_MINIMAP_WATCH");
        on = e && atoi(e) > 0;
        if (on) {
            const char *d = getenv("YW4_MINIMAP_DIR");
            if (d && *d) snprintf(dir, sizeof dir, "%s", d);
            MAKE_DIR(dir);
            char path[512];
            snprintf(path, sizeof path, "%s/minimap_draws.csv", dir);
            draws_csv = fopen(path, "w");
            snprintf(path, sizeof path, "%s/minimap_pixels.csv", dir);
            pixels_csv = fopen(path, "w");
            if (!draws_csv || !pixels_csv) {
                fprintf(stderr, "yw4: minimap: can't write in %s: watch off\n", dir);
                on = 0;
                return 0;
            }
            fprintf(draws_csv, "frame,target,draws,clears,shown,missing,fresh,verts\n");
            fprintf(pixels_csv, "frame,target,lit_pm,opaque_pm,r,g,b,a\n");
            fprintf(stderr, "yw4: minimap: watch on, files in %s\n", dir);
        }
    }
    return on;
}

static int minimap_size(const NvnTexture *t) {
    return (t->width == 340 && t->height == 304) || (t->width == 516 && t->height == 460);
}

static int target_of(const NvnTexture *t, int add) {
    if (!t || !t->addr) return -1;
    for (int i = 0; i < ntg; i++)
        if (tg[i].tex.addr == t->addr && tg[i].tex.width == t->width && tg[i].tex.height == t->height) return i;
    if (!add || !minimap_size(t)) return -1;
    int i = ntg;
    if (ntg < TARGETS) {
        ntg++;
    } else {  /* full: the one unused longest goes (the game made its images anew) */
        i = 0;
        for (int k = 1; k < TARGETS; k++)
            if (tg[k].last_used < tg[i].last_used) i = k;
        for (int k = 0; k < SLOTS; k++) tg[i].slot[k].seq = 0;
        free(tg[i].prev);
        uint8_t *work = tg[i].work;
        WSlot keep[SLOTS];
        uint32_t pw = tg[i].pw, ph = tg[i].ph;
        memcpy(keep, tg[i].slot, sizeof keep);
        memset(&tg[i], 0, sizeof tg[i]);
        memcpy(tg[i].slot, keep, sizeof keep), tg[i].pw = pw, tg[i].ph = ph, tg[i].work = work;
    }
    tg[i].tex = *t;
    tg[i].tex.found = NULL, tg[i].tex.found_epoch = 0;
    fprintf(stderr, "yw4: minimap: frame %" PRIu64 ": watching image %#" PRIx64 " (%ux%u)\n", gpu_frames() + 1, t->addr,
            t->width, t->height);
    return i;
}

void gpu_watch_clear(const NvnTexture *t) {
    int i = target_of(t, 1);
    if (i >= 0) fr[cur].c[i].clears++, tg[i].last_used = gpu_frames() + 1;
}

int gpu_watch_wants(const GpuState *s, const GpuWatchTex *tex, int ntex) {
    if (s->ncolor && target_of(s->rt.color[0], 1) >= 0) return 1;
    for (int i = 0; i < ntex; i++)
        if (target_of(tex[i].tex, 0) >= 0) return 1;
    return 0;
}

/* FNV-1a of up to `max` bytes at addr, and its first floats; 0 when the memory can't be read. */
static uint64_t fingerprint(uint64_t addr, uint64_t size, uint64_t max, float *f, int maxf, uint8_t *nf) {
    *nf = 0;
    if (size > max) size = max;
    /* in a memory pool of the game's (host_readable asks Windows about the region: 0.8 ms a call here, 2026-11-13) */
    VkBuffer buf;
    VkDeviceSize at;
    if (!addr || !size || !gpu_buffer_at(addr, size, &buf, &at)) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < size; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    *nf = (uint8_t)(size / 4 < (uint64_t)maxf ? size / 4 : (uint64_t)maxf);
    memcpy(f, p, (size_t)*nf * 4);
    return h;
}

void gpu_watch_draw(const GpuState *s, uint64_t vs, uint64_t fs, uint64_t verts, const GpuWatchTex *tex, int ntex,
                    const GpuWatchData *data) {
    WFrame *f = &fr[cur];
    int into = s->ncolor ? target_of(s->rt.color[0], 1) : -1, samples = -1;
    if (into < 0) {
        for (int i = 0; i < ntex && samples < 0; i++) samples = target_of(tex[i].tex, 0);
        if (samples < 0) return;
        f->c[samples].shown++;
    } else {
        WCount *c = &f->c[into];
        c->draws++, c->verts += verts;
        for (int i = 0; i < ntex; i++) c->missing += !tex[i].tex, c->fresh += tex[i].fresh;
        tg[into].last_used = gpu_frames() + 1;
    }
    if (f->ndraws == MAX_DRAWS) return;
    WDraw *d = &f->d[f->ndraws++];
    memset(d, 0, sizeof *d);
    d->vs = vs, d->fs = fs, d->verts = (uint32_t)verts, d->target = (int8_t)into, d->samples = (int8_t)samples;
    for (int i = 0; i < ntex && d->ntex < DRAW_TEX; i++) {
        WTex *x = &d->tex[d->ntex++];
        x->slot = (uint8_t)tex[i].slot, x->missing = !tex[i].tex, x->fresh = (uint8_t)tex[i].fresh, x->handle = tex[i].handle;
        if (tex[i].tex)
            x->addr = tex[i].tex->addr, x->w = tex[i].tex->width, x->h = tex[i].tex->height, x->format = tex[i].tex->format;
    }
    uint64_t off = (uint64_t)data->first * data->stride, len = verts * data->stride;
    d->vb_addr = data->vb_addr + off, d->first = data->first, d->stride = data->stride, d->stale = (uint8_t)data->stale;
    if (data->vb_addr && off + len <= data->vb_size) d->vb_hash = fingerprint(d->vb_addr, len, 1024, d->vf, 16, &d->nvf);
    for (int i = 0; i < data->ncb; i++) {
        const GpuWatchBuf *b = &data->cb[i];
        d->cb[i].stage = b->stage, d->cb[i].slot = b->slot, d->cb[i].addr = b->addr, d->cb[i].size = b->size;
        d->cb[i].hash = fingerprint(b->addr, b->size, 1024, d->cb[i].f, 8, &d->cb[i].nf);
    }
    d->ncb = (uint8_t)data->ncb;
}

/* ---- the picture */

static void save_bmp(const WTarget *t, const uint8_t *px, uint64_t frame, uint64_t named, const char *tag) {
    if (!px) return;
    char path[512];
    snprintf(path, sizeof path, "%s/minimap_%06" PRIu64 "_%ux%u_%s.bmp", dir, named, t->tex.width, t->tex.height, tag);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t w = t->pw, h = t->ph, row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    uint8_t hd[54] = {'B', 'M'};
    int32_t top_down = -(int32_t)h;
    memcpy(hd + 2, &size, 4), memcpy(hd + 10, &(uint32_t){54}, 4), memcpy(hd + 14, &(uint32_t){40}, 4);
    memcpy(hd + 18, &w, 4), memcpy(hd + 22, &top_down, 4), memcpy(hd + 26, &(uint16_t){1}, 2);
    memcpy(hd + 28, &(uint16_t){24}, 2);
    fwrite(hd, 1, 54, f);
    uint8_t *line = calloc(1, row);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = px + ((size_t)y * w + x) * 4;
            line[x * 3] = p[2], line[x * 3 + 1] = p[1], line[x * 3 + 2] = p[0];
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "yw4: minimap: frame %" PRIu64 " saved as %s\n", frame, path);
}

static void write_draws(FILE *f, const WFrame *w);

/* The draws of frames first..last that the ring still has, to minimap_<named>_draws.txt. */
static void dump_draws(uint64_t named, uint64_t first, uint64_t last) {
    char path[512];
    snprintf(path, sizeof path, "%s/minimap_%06" PRIu64 "_draws.txt", dir, named);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (uint64_t n = first; n <= last; n++)
        for (int i = 0; i < RING; i++)
            if (i != cur && fr[i].frame == n) write_draws(f, &fr[i]);
    fclose(f);
}

/* One picture read back: its numbers, and whether it went blank against the one before. */
static void picture(int ti, const uint8_t *px, uint64_t frame) {
    WTarget *t = &tg[ti];
    size_t n = (size_t)t->pw * t->ph, lit = 0, opaque = 0;
    uint64_t sum[4] = {0};
    for (size_t i = 0; i < n; i++) {
        const uint8_t *p = px + i * 4;
        lit += p[0] > 24 || p[1] > 24 || p[2] > 24;
        opaque += p[3] > 16;
        sum[0] += p[0], sum[1] += p[1], sum[2] += p[2], sum[3] += p[3];
    }
    int lit_pm = (int)(lit * 1000 / n), opaque_pm = (int)(opaque * 1000 / n);
    fprintf(pixels_csv, "%" PRIu64 ",%ux%u,%d,%d,%d,%d,%d,%d\n", frame, t->tex.width, t->tex.height, lit_pm, opaque_pm,
            (int)(sum[0] / n), (int)(sum[1] / n), (int)(sum[2] / n), (int)(sum[3] / n));
    if (!t->blank) {
        int lit_drop = t->prev && t->prev_lit >= 50 && lit_pm * 2 < t->prev_lit;
        int opaque_drop = t->prev && t->prev_opaque >= 50 && opaque_pm * 2 < t->prev_opaque;
        if (lit_drop || opaque_drop) {
            t->blank = 1, t->blank_from = frame, t->base_lit = t->prev_lit, t->base_opaque = t->prev_opaque;
            fprintf(stderr, "yw4: minimap: frame %" PRIu64 ": image %ux%u went blank: lit %d -> %d, opaque %d -> %d per mille"
                            " (frame %" PRIu64 " before)\n", frame, t->tex.width, t->tex.height, t->prev_lit, lit_pm,
                    t->prev_opaque, opaque_pm, t->prev_frame);
            yw4_event("minimap", frame, "\"what\":\"pixels\",\"target\":\"%ux%u\",\"lit\":[%d,%d],\"opaque\":[%d,%d]",
                      t->tex.width, t->tex.height, t->prev_lit, lit_pm, t->prev_opaque, opaque_pm);
            if (saved++ < MAX_SAVED) {
                save_bmp(t, t->prev, t->prev_frame, frame, "a_before");
                save_bmp(t, px, frame, frame, "b_blank");
                dump_draws(frame, frame - 1, frame + 1);
            }
        }
    } else if ((lit_pm * 4 >= t->base_lit * 3 && opaque_pm * 4 >= t->base_opaque * 3) || frame - t->blank_from > 600) {
        int back = frame - t->blank_from <= 600;
        fprintf(stderr, "yw4: minimap: frame %" PRIu64 ": image %ux%u %s after %" PRIu64 " frames (lit %d, opaque %d)\n",
                frame, t->tex.width, t->tex.height, back ? "is back" : "stays as it is: taken as the new normal",
                frame - t->blank_from, lit_pm, opaque_pm);
        yw4_event("minimap", frame, "\"what\":\"%s\",\"of\":\"pixels\",\"target\":\"%ux%u\",\"since\":%" PRIu64
                  ",\"frames\":%" PRIu64, back ? "back" : "stays", t->tex.width, t->tex.height, t->blank_from,
                  frame - t->blank_from);
        if (back && saved <= MAX_SAVED) save_bmp(t, px, frame, t->blank_from, "c_after");
        t->blank = 0;
    }
    if (!t->prev) t->prev = malloc(n * 4);
    memcpy(t->prev, px, n * 4);
    t->prev_frame = frame, t->prev_lit = lit_pm, t->prev_opaque = opaque_pm;
}

static void slots_free(WTarget *t) {
    for (int k = 0; k < SLOTS; k++) {
        if (t->slot[k].buf) vkDestroyBuffer(vk.device, t->slot[k].buf, NULL), vkFreeMemory(vk.device, t->slot[k].mem, NULL);
        memset(&t->slot[k], 0, sizeof t->slot[k]);
    }
    free(t->prev), free(t->work);
    t->prev = t->work = NULL, t->blank = 0;
}

static void slot_make(WTarget *t, WSlot *sl) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)t->pw * t->ph * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateBuffer(vk.device, &bci, NULL, &sl->buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(vk.device, sl->buf, &mr);
    /* cached memory: reading the plain host-visible kind is uncached and took 157 ms a frame (2026-11-13) */
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                                         VK_MEMORY_PROPERTY_HOST_CACHED_BIT)};
    if (mai.memoryTypeIndex == UINT32_MAX)
        mai.memoryTypeIndex = vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(vk.device, &mai, NULL, &sl->mem));
    CHECK(vkBindBufferMemory(vk.device, sl->buf, sl->mem, 0));
    void *p;
    CHECK(vkMapMemory(vk.device, sl->mem, 0, VK_WHOLE_SIZE, 0, &p));
    sl->map = p;
}

/* ---- the counts */

static void write_draws(FILE *f, const WFrame *w) {
    fprintf(f, "frame %" PRIu64 ": %d draws recorded%s\n", w->frame, w->ndraws, w->ndraws == MAX_DRAWS ? " (more not kept)" : "");
    for (int i = 0; i < w->ndraws; i++) {
        const WDraw *d = &w->d[i];
        if (d->target >= 0)
            fprintf(f, "  into %ux%u ", tg[d->target].tex.width, tg[d->target].tex.height);
        else
            fprintf(f, "  ON SCREEN from %ux%u ", tg[d->samples].tex.width, tg[d->samples].tex.height);
        fprintf(f, "vs %016" PRIx64 " fs %016" PRIx64 " verts %u, textures:", d->vs, d->fs, d->verts);
        for (int k = 0; k < d->ntex; k++) {
            const WTex *x = &d->tex[k];
            if (x->missing) fprintf(f, " [%u: MISSING]", x->slot);
            else fprintf(f, " [%u: handle %#" PRIx64 " texels %#" PRIx64 " %ux%u fmt %u%s]", x->slot, x->handle, x->addr, x->w,
                         x->h, x->format, x->fresh ? " UPLOADED NOW" : "");
        }
        fprintf(f, "\n      vertices at %#" PRIx64 " first %u stride %u%s print %016" PRIx64 ":", d->vb_addr, d->first, d->stride,
                d->stale ? " COPY STALE" : "", d->vb_hash);
        for (int k = 0; k < d->nvf; k++) fprintf(f, " %g", d->vf[k]);
        for (int k = 0; k < d->ncb; k++) {
            fprintf(f, "\n      %s cbuf %u at %#" PRIx64 " size %" PRIu64 " print %016" PRIx64 ":", d->cb[k].stage ? "fs" : "vs",
                    d->cb[k].slot, d->cb[k].addr, d->cb[k].size, d->cb[k].hash);
            for (int j = 0; j < d->cb[k].nf; j++) fprintf(f, " %g", d->cb[k].f[j]);
        }
        fputc('\n', f);
    }
}

static void counts(uint64_t frame) {
    WFrame *now = &fr[cur], *before = &fr[(cur + RING - 1) % RING];
    now->frame = frame;
    int dump = 0;
    for (int i = 0; i < ntg; i++) {
        WTarget *t = &tg[i];
        const WCount *c = &now->c[i], *p = &before->c[i];
        fprintf(draws_csv, "%" PRIu64 ",%ux%u,%d,%d,%d,%d,%d,%" PRIu64 "\n", frame, t->tex.width, t->tex.height, c->draws,
                c->clears, c->shown, c->missing, c->fresh, c->verts);
        if (before->frame + 1 != frame) continue;
        if (!t->low) {
            int fewer = p->draws >= 2 && c->draws * 2 < p->draws, hidden = p->shown > 0 && !c->shown;
            int lost = c->missing > p->missing;
            if (!fewer && !hidden && !lost) continue;
            if (fewer || hidden) t->low = 1, t->low_from = frame, t->base_draws = p->draws, t->base_shown = p->shown;
            fprintf(stderr, "yw4: minimap: frame %" PRIu64 ": image %ux%u: draws into it %d -> %d, draws putting it on screen"
                            " %d -> %d, missing textures %d -> %d, uploaded now %d\n", frame, t->tex.width, t->tex.height,
                    p->draws, c->draws, p->shown, c->shown, p->missing, c->missing, c->fresh);
            yw4_event("minimap", frame, "\"what\":\"%s\",\"target\":\"%ux%u\",\"draws\":[%d,%d],\"shown\":[%d,%d],"
                      "\"missing\":[%d,%d],\"fresh\":%d", fewer ? "draws" : hidden ? "shown" : "missing", t->tex.width,
                      t->tex.height, p->draws, c->draws, p->shown, c->shown, p->missing, c->missing, c->fresh);
            dump = 1;
        } else if ((c->draws * 4 >= t->base_draws * 3 && (c->shown || !t->base_shown)) || frame - t->low_from > 600) {
            int back = frame - t->low_from <= 600;
            fprintf(stderr, "yw4: minimap: frame %" PRIu64 ": image %ux%u: draws %s after %" PRIu64 " frames (%d into it, %d"
                            " on screen)\n", frame, t->tex.width, t->tex.height, back ? "are back" : "stay as they are",
                    frame - t->low_from, c->draws, c->shown);
            yw4_event("minimap", frame, "\"what\":\"%s\",\"of\":\"draws\",\"target\":\"%ux%u\",\"since\":%" PRIu64
                      ",\"frames\":%" PRIu64, back ? "back" : "stays", t->tex.width, t->tex.height, t->low_from,
                      frame - t->low_from);
            t->low = 0;
        }
    }
    fflush(draws_csv);
    cur = (cur + 1) % RING;
    if (dump && saved++ < MAX_SAVED) dump_draws(frame, frame - 1, frame);
    memset(&fr[cur], 0, sizeof fr[cur]);
}

/* gpu_present: the frame's counts, the pictures that have arrived, and this frame's pictures on their way
 * (copies recorded in cb, which the caller submits and then names with gpu_watch_submitted). */
static WSlot *pending[TARGETS];

void gpu_watch_frame(VkCommandBuffer cb, uint64_t frame) {
    counts(frame);
    for (int i = 0; i < ntg; i++) {
        WTarget *t = &tg[i];
        pending[i] = NULL;
        /* arrived: oldest first */
        for (;;) {
            WSlot *old = NULL;
            for (int k = 0; k < SLOTS; k++)
                if (t->slot[k].seq && (!old || t->slot[k].frame < old->frame)) old = &t->slot[k];
            if (!old || gpu_wait(old->seq, 0) != 0) break;
            if (!t->work) t->work = malloc((size_t)t->pw * t->ph * 4);
            memcpy(t->work, old->map, (size_t)t->pw * t->ph * 4);  /* one pass over the GPU's memory, then ours */
            picture(i, t->work, old->frame);
            old->seq = 0;
        }
        if (frame - t->last_used > 120) continue;  /* not drawn lately: nothing new to see */
        GpuImage *g = image_find(&t->tex);
        if (!g) continue;
        if (g->format != VK_FORMAT_R8G8B8A8_UNORM && g->format != VK_FORMAT_R8G8B8A8_SRGB) {
            static int logged;
            if (!logged++) fprintf(stderr, "yw4: minimap: image format %d: its picture isn't read, counts only\n", (int)g->format);
            continue;
        }
        if (t->pw != g->width || t->ph != g->height) {
            int busy = 0;
            for (int k = 0; k < SLOTS; k++) busy |= t->slot[k].seq != 0;
            if (busy) continue;  /* the old size's pictures first */
            slots_free(t);
            t->pw = g->width, t->ph = g->height;
        }
        WSlot *sl = NULL;
        for (int k = 0; k < SLOTS && !sl; k++)
            if (!t->slot[k].seq) sl = &t->slot[k];
        if (!sl) continue;  /* the GPU is four frames behind: skip this one */
        if (!sl->buf) slot_make(t, sl);
        full_barrier(cb);
        VkBufferImageCopy cp = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {g->width, g->height, 1}};
        vkCmdCopyImageToBuffer(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, sl->buf, 1, &cp);
        sl->frame = frame;
        pending[i] = sl;
    }
    fflush(pixels_csv);
}

void gpu_watch_submitted(uint64_t seq) {
    for (int i = 0; i < TARGETS; i++)
        if (pending[i]) pending[i]->seq = seq, pending[i] = NULL;
}
