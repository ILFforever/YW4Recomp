/* YW4_TRACE_DRAWLIST=<frame>: how the engine's draw list reaches the draws (docs/frame.md).
 *
 * From presented frame <frame> on, each entry of the submesh draw main+0xa75e90
 * (device, geometry, primitive) called by the model renderer main+0xa45890 notes the object
 * being drawn. The next draw recorded on that thread is
 * printed with what was bound for it (program, vertex buffer 0, index buffer, uniform
 * buffers), and each bound address and the first 16 bytes of each bound uniform buffer are
 * searched for in the object, the geometry and the primitive records, following pointers
 * two levels: the paths found are where the engine keeps that draw's data.
 * YW4_TRACE_DRAWLIST_MAX (default 24) limits the draws printed.
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nvn.h"
#include "yw4rt.h"

extern void (*yw4_entry_hook)(CPUState *ctx, uint64_t addr);  /* dispatch.c */
extern uint64_t yw4_frames_presented;

static int from = -2, printed, max_printed = 24;
static uint64_t main_base;
static _Thread_local struct { uint64_t node, obj, geom, prim, dev; int armed; } cur;

static long calls, arms;  /* entries of the submesh draw in the traced frames: all, from the model renderer */

/* From frame <from> until enough draws are printed (the searches are slow: the frame moves on) */
static int in_frame(void) {
    uint64_t f = __atomic_load_n(&yw4_frames_presented, __ATOMIC_RELAXED);
    return from >= 0 && f >= (uint64_t)from && __atomic_load_n(&printed, __ATOMIC_RELAXED) < max_printed;
}

static int guest_ptr(uint64_t p) { return p >= 0x7000000000ull && p < 0x7800000000ull && !(p & 7); }

/* host_readable is a system call: remember the answer per 64 KiB block (blocks the game
 * maps or unmaps meanwhile may be answered wrongly; good enough for a trace) */
static int readable(uint64_t addr, uint64_t size) {
    enum { SLOTS = 1 << 14 };
    static uint64_t key[SLOTS];
    static uint8_t val[SLOTS];
    for (uint64_t b = addr >> 16; b <= (addr + size - 1) >> 16; b++) {
        uint32_t h = (uint32_t)(b * 0x9E3779B97F4A7C15ull >> 50);
        while (key[h] && key[h] != b) h = (h + 1) & (SLOTS - 1);
        if (!key[h]) val[h] = (uint8_t)(host_readable(b << 16, 1 << 16) ? 1 : 2), key[h] = b;
        if (val[h] != 1) return 0;
    }
    return 1;
}

static void entry(CPUState *ctx, uint64_t addr) {
    if (addr != main_base + 0xa75e90 || !in_frame()) return;
    __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    uint64_t ret = ctx->x[30] - main_base;
    if (ret != 0xa4bf94 && ret != 0xa4c2d0) return;  /* only the model renderer's two call sites */
    __atomic_add_fetch(&arms, 1, __ATOMIC_RELAXED);
    /* the model renderer draws in a second phase, from sorted 0x48-byte draw items; there the
     * object being drawn is in its stack slot sp+0x390 (sp is still the caller's at entry) */
    cur.node = 0;
    cur.obj = *(const uint64_t *)(uintptr_t)(ctx->sp + 0x390);
    cur.geom = ctx->x[1], cur.prim = ctx->x[2], cur.dev = ctx->x[0], cur.armed = 1;
}

int trace_drawlist_on(void) {
    if (from == -2) {
        const char *s = getenv("YW4_TRACE_DRAWLIST");
        from = s && *s ? atoi(s) : -1;
        if (getenv("YW4_TRACE_DRAWLIST_MAX")) max_printed = atoi(getenv("YW4_TRACE_DRAWLIST_MAX"));
        if (from >= 0) {
            for (size_t i = 0; i < yw4_ncode; i++)
                if (!strcmp(yw4_code[i].name, "main")) main_base = *yw4_code[i].base;
            yw4_entry_hook = entry;
        }
    }
    return from >= 0;
}


typedef struct {
    const char *what;
    uint64_t value;      /* an address to find (0: none) */
    uint8_t bytes[16];   /* or 16 bytes of content to find */
    int has_bytes, inside;  /* inside: also report pointers up to 16 MiB below the address */
    int hits;
    uint32_t v32;        /* or a 32-bit value to find (0: none) */
} Want;

enum { SPAN = 0x400, MAX_HITS = 6 };  /* bytes searched in each structure (default); reports per wanted value */

typedef struct { char *buf; size_t len, cap; } Out;

static void out(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(Out *o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n > 0) o->len = o->len + (size_t)n < o->cap ? o->len + (size_t)n : o->cap - 1;
}

/* 16 bytes worth searching for: not all zero, not one 32-bit value repeated */
static int distinctive(const uint8_t b[16]) {
    uint32_t w[4];
    memcpy(w, b, 16);
    return !(w[0] == w[1] && w[1] == w[2] && w[2] == w[3]);
}

static void search(Out *o, const char *root, uint64_t at, int depth, int span, char *path, size_t plen, Want *w, int nw) {
    if (!guest_ptr(at) || !readable(at, span)) return;
    const uint8_t *p = (const uint8_t *)(uintptr_t)at;
    for (int off = 0; off < span; off += 4) {
        for (int i = 0; i < nw; i++) {
            if (w[i].hits >= MAX_HITS) continue;
            if (w[i].v32 && *(const uint32_t *)(p + off) == w[i].v32) {
                w[i].hits++, out(o, "yw4: drawlist:     %s = %s%s+%#x\n", w[i].what, root, path, off);
                continue;
            }
            if (off + 8 > span) continue;
            uint64_t v = *(const uint64_t *)(p + off);
            if (w[i].value && !(off & 7) && v == w[i].value)
                w[i].hits++, out(o, "yw4: drawlist:     %s = %s%s+%#x\n", w[i].what, root, path, off);
            else if (w[i].inside && !(off & 7) && guest_ptr(v) && v < w[i].value && w[i].value - v < (16u << 20))
                w[i].hits++, out(o, "yw4: drawlist:     %s = %s%s+%#x + %#" PRIx64 "\n", w[i].what, root, path, off,
                                 w[i].value - v);
            if (w[i].has_bytes && off + 16 <= span && !memcmp(p + off, w[i].bytes, 16))
                w[i].hits++, out(o, "yw4: drawlist:     %s contents = %s%s+%#x\n", w[i].what, root, path, off);
        }
    }
    if (!depth) return;
    for (int off = 0; off < span; off += 8) {
        uint64_t q = *(const uint64_t *)(p + off);
        if (!guest_ptr(q) || q == at) continue;
        int n = snprintf(path + plen, 96 - plen, "+%#x*", off);
        if (n > 0 && plen + (size_t)n < 96) search(o, root, q, depth - 1, span, path, plen + (size_t)n, w, nw);
        path[plen] = 0;
    }
}

void trace_drawlist_draw(const NvnCmdBuf *cb, uint64_t index_addr) {
    if (!cur.armed) return;
    cur.armed = 0;
    if (!in_frame() || __atomic_fetch_add(&printed, 1, __ATOMIC_RELAXED) >= max_printed) return;
    static _Thread_local char storage[1 << 16];  /* one draw's report, written out in one go */
    Out o = {storage, 0, sizeof storage};
    const NvnProgram *pr = cb->program;
    out(&o, "yw4: drawlist: (submesh draws so far %ld, from the model renderer %ld)\n", calls, arms);
    out(&o, "yw4: drawlist: draw vs %016llx fs %016llx target %ux%u: node %#" PRIx64 " object %#" PRIx64
        " geometry %#" PRIx64 " primitive %#" PRIx64 "\n",
        pr ? (unsigned long long)pr->stages[0].key : 0ull, pr ? (unsigned long long)pr->stages[1].key : 0ull,
        cb->target_w, cb->target_h, cur.node, cur.obj, cur.geom, cur.prim);
    Want w[2 + 2 * 16 + 2];
    char names[2 * 16 + 3][24];
    int nw = 0;
    w[nw++] = (Want){"vertex buffer 0", cb->trace_vb0, {0}, 0, 1, 0, 0};
    if (index_addr) w[nw++] = (Want){"index buffer", index_addr, {0}, 0, 1, 0, 0};
    out(&o, "yw4: drawlist:   vertex buffer 0 at %#" PRIx64 ", index buffer at %#" PRIx64 "\n", cb->trace_vb0,
        index_addr);
    out(&o, "yw4: drawlist:   attributes (format/stream/offset):");
    for (uint32_t i = 0; i < cb->trace_nattribs; i++) {
        uint32_t a = cb->trace_attribs[i];
        if (a & 0xFF) out(&o, " %u:%#x/%u/%u", i, a & 0xFF, a >> 8 & 0x1F, a >> 13);
    }
    out(&o, "\n");
    for (int s = 0; s < 2; s++)  /* texture handles: id | sampler << 20 */
        for (int i = 0; i < 4; i++)
            if (cb->trace_tex[s][i])
                out(&o, "yw4: drawlist:   %s texture %d handle %#" PRIx64 "\n", s ? "fs" : "vs", i, cb->trace_tex[s][i]);
    if (guest_ptr(cur.prim) && readable(cur.prim, 0x50)) {  /* the primitive (submesh) record, as 32-bit words */
        const uint32_t *pw = (const uint32_t *)(uintptr_t)cur.prim;
        out(&o, "yw4: drawlist:   primitive words:");
        for (int i = 0; i < 0x50 / 4; i++) out(&o, " %x", pw[i]);
        out(&o, "\n");
    }
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 16; i++) {
            uint64_t a = cb->trace_ub[s][i];
            if (!a) continue;
            snprintf(names[s * 16 + i], sizeof names[0], "%s cbuf %d", s ? "fs" : "vs", i);
            Want x = {names[s * 16 + i], a, {0}, 0, 0, 0, 0};
            if (readable(a, 16)) {
                memcpy(x.bytes, (const void *)(uintptr_t)a, 16);
                x.has_bytes = distinctive(x.bytes);
            }
            w[nw++] = x;
            out(&o, "yw4: drawlist:   %s at %#" PRIx64 "\n", x.what, a);
            if (s == 0 && i == 1 && readable(a + 128, 64)) {  /* the view's matrix, as numbers */
                const float *m = (const float *)(uintptr_t)(a + 128);
                out(&o, "yw4: drawlist:   vs cbuf 1 @128:");
                for (int k = 0; k < 16; k++) out(&o, " %.4g", m[k]);
                out(&o, "\n");
            }
            if (s == 0 && (i == 1 || i == 2) && readable(a + 128, 16)) {  /* the shaders' matrices at byte 128 */
                snprintf(names[32 + i], sizeof names[0], "vs cbuf %d @128", i);
                Want y = {names[32 + i], 0, {0}, 0, 0, 0, 0};
                memcpy(y.bytes, (const void *)(uintptr_t)(a + 128), 16);
                y.has_bytes = distinctive(y.bytes);
                w[nw++] = y;
            }
        }
    char path[96] = "";
    search(&o, "object", cur.obj, 2, SPAN, path, 0, w, nw);
    search(&o, "geometry", cur.geom, 2, SPAN, path, 0, w, nw);
    search(&o, "primitive", cur.prim, 1, SPAN, path, 0, w, nw);
    if (guest_ptr(cur.obj) && readable(cur.obj + 0x120, 8))  /* the material set, deeper but narrower */
        search(&o, "material", *(const uint64_t *)(uintptr_t)(cur.obj + 0x120), 3, 0x100, path, 0, w, nw);
    search(&o, "renderer", main_base + 0x56ab1b0, 0, SPAN, path, 0, w, nw);  /* the static renderer object */
    search(&o, "device", cur.dev, 0, SPAN, path, 0, w, nw);
    fwrite(o.buf, 1, o.len, stderr);
}
