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
            else if (w[i].inside && !(off & 7) && guest_ptr(v) && v < w[i].value && w[i].value - v < (w[i].inside == 2 ? 0x10000u : 16u << 20))
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
    /* YW4_TRACE_DRAWLIST_COLOURS=<n>: only draws into n colour targets (3: the G-buffer) */
    static int colours = -2;
    if (colours == -2) colours = getenv("YW4_TRACE_DRAWLIST_COLOURS") ? atoi(getenv("YW4_TRACE_DRAWLIST_COLOURS")) : -1;
    if (colours >= 0 && cb->target_colors != (uint32_t)colours) return;
    if (!in_frame() || __atomic_fetch_add(&printed, 1, __ATOMIC_RELAXED) >= max_printed) return;
    static _Thread_local char storage[1 << 16];  /* one draw's report, written out in one go */
    Out o = {storage, 0, sizeof storage};
    const NvnProgram *pr = cb->program;
    out(&o, "yw4: drawlist: (submesh draws so far %ld, from the model renderer %ld)\n", calls, arms);
    out(&o, "yw4: drawlist: draw vs %016llx fs %016llx target %ux%u: node %#" PRIx64 " object %#" PRIx64
        " geometry %#" PRIx64 " primitive %#" PRIx64 "\n",
        pr ? (unsigned long long)pr->stages[0].key : 0ull, pr ? (unsigned long long)pr->stages[1].key : 0ull,
        cb->target_w, cb->target_h, cur.node, cur.obj, cur.geom, cur.prim);
    Want w[3 + 2 * 16 + 2 + TRACE_TEXTURES + 20];
    char names[2 * 16 + 3][24], tnames[TRACE_TEXTURES][24], snames[20][24];
    int nw = 0;
    /* the engine's texture slots (f_974cd0 binds from [device+0x10d0]+0x780 + slot*16: the texture
     * object, whose +8 is the NVN texture id): print them, and look for where the objects come from */
    uint64_t state = guest_ptr(cur.dev) && readable(cur.dev + 0x10d0, 8) ? *(const uint64_t *)(uintptr_t)(cur.dev + 0x10d0) : 0;
    if (guest_ptr(state) && readable(state + 0x780, 20 * 16)) {
        out(&o, "yw4: drawlist:   engine texture slots at %#" PRIx64 " (object, id):", state + 0x780);
        for (int i = 0; i < 20; i++) {
            uint64_t t = *(const uint64_t *)(uintptr_t)(state + 0x780 + 16ull * i);
            if (!guest_ptr(t) || !readable(t + 8, 4)) continue;
            out(&o, " %d:%#" PRIx64 "/%#x", i, t, *(const uint32_t *)(uintptr_t)(t + 8));
            snprintf(snames[i], sizeof snames[0], "texture slot %d object", i);
            w[nw++] = (Want){snames[i], t, {0}, 0, 2, 0, 0};  /* also array bases up to 64 KiB below it */
        }
        out(&o, "\n");
    }
    /* the object's texture table (f_a45890's material binding): 16-byte entries {texture
     * resource, index}; a resource keeps 0x50-byte records at [+0x30], counted from the
     * header's (+0x18) byte +0x20 */
    /* ... of the model record (object+0x140, 0x240 bytes per geometry) of the geometry drawn */
    uint64_t mdl = 0;
    if (guest_ptr(cur.obj) && readable(cur.obj + 0x120, 0x40)) {
        uint64_t geos = *(const uint64_t *)(uintptr_t)(cur.obj + 0x128), models = *(const uint64_t *)(uintptr_t)(cur.obj + 0x140);
        unsigned ngeo = *(const uint8_t *)(uintptr_t)(cur.obj + 0x1c8);
        for (unsigned g = 0; g < ngeo && g < 16 && guest_ptr(geos) && readable(geos + 8ull * g, 8); g++)
            if (*(const uint64_t *)(uintptr_t)(geos + 8ull * g) == cur.geom) mdl = models + 0x240ull * g;
    }
    out(&o, "yw4: drawlist:   model record %#" PRIx64 "\n", mdl);
    if (guest_ptr(state) && readable(state + 0x790, 8)) {  /* texture slot 1's object, as 64-bit words */
        uint64_t t1 = *(const uint64_t *)(uintptr_t)(state + 0x790);
        if (guest_ptr(t1) && readable(t1, 0x50)) {
            out(&o, "yw4: drawlist:   slot 1 object words:");
            for (int i = 0; i < 10; i++) out(&o, " %" PRIx64, *(const uint64_t *)(uintptr_t)(t1 + 8ull * i));
            out(&o, "\n");
        }
    }
    /* object+0x3f0 -> +0x60: a table of 0x18-byte entries pointing at texture objects: which
     * entries the bound slots are */
    if (guest_ptr(cur.obj) && readable(cur.obj + 0x3f0, 8) && guest_ptr(state) && readable(state + 0x780, 20 * 16)) {
        uint64_t a = *(const uint64_t *)(uintptr_t)(cur.obj + 0x3f0);
        uint64_t t = guest_ptr(a) && readable(a + 0x60, 8) ? *(const uint64_t *)(uintptr_t)(a + 0x60) : 0;
        out(&o, "yw4: drawlist:   object+0x3f0 = %#" PRIx64 ", its +0x60 table %#" PRIx64 ":", a, t);
        for (int sl = 0; sl < 20 && guest_ptr(t); sl++) {
            uint64_t want = *(const uint64_t *)(uintptr_t)(state + 0x780 + 16ull * sl);
            if (!guest_ptr(want)) continue;
            for (int k = 0; k < 512 && readable(t + 0x18ull * k, 0x18); k++)
                if (*(const uint64_t *)(uintptr_t)(t + 0x18ull * k) == want) {
                    out(&o, " slot %d = entry %d", sl, k);
                    break;
                }
        }
        out(&o, "\n");
    }
    for (int which = 0; which < 2; which++) {
        uint64_t tt = guest_ptr(mdl) && readable(mdl + 0xf0, 16) ? *(const uint64_t *)(uintptr_t)(mdl + (which ? 0xf0 : 0xf8)) : 0;
        if (!guest_ptr(tt) || !readable(tt, 8 * 16)) continue;
        out(&o, "yw4: drawlist:   model+%#x texture table:", which ? 0xf0 : 0xf8);
        for (int i = 0; i < 8; i++) {
            uint64_t res = *(const uint64_t *)(uintptr_t)(tt + 16ull * i);
            uint16_t idx = *(const uint16_t *)(uintptr_t)(tt + 16ull * i + 8);
            if (!guest_ptr(res) || !readable(res + 0x18, 0x20)) continue;
            uint64_t hdr = *(const uint64_t *)(uintptr_t)(res + 0x18), arr = *(const uint64_t *)(uintptr_t)(res + 0x30);
            unsigned base = guest_ptr(hdr) && readable(hdr + 0x20, 4) ? *(const uint8_t *)(uintptr_t)(hdr + 0x20) : 0;
            uint64_t recd = guest_ptr(arr) && idx >= base ? arr + 0x50ull * (idx - base) : 0;
            uint64_t p20 = guest_ptr(recd) && readable(recd + 0x20, 8) ? *(const uint64_t *)(uintptr_t)(recd + 0x20) : 0;
            out(&o, " [%d] res %#" PRIx64 " idx %u base %u -> record %#" PRIx64 " (+0x20 %#" PRIx64 ")", i, res, idx,
                base, recd, p20);
        }
        out(&o, "\n");
    }
    /* the fragment textures' handles (id | sampler << 20): the engine's texture object keeps its
     * handle at +0xf8, so a hit at "...+0xf8" is the path to the texture object */
    for (int i = 0; i < TRACE_TEXTURES; i++)
        if (cb->trace_tex[1][i] && (uint32_t)cb->trace_tex[1][i]) {
            snprintf(tnames[i], sizeof tnames[0], "fs texture %d", i);
            w[nw++] = (Want){tnames[i], 0, {0}, 0, 0, 0, (uint32_t)cb->trace_tex[1][i]};
        }
    w[nw++] = (Want){"vertex buffer 0", cb->trace_vb0, {0}, 0, 1, 0, 0};
    w[nw++] = (Want){"program", cb->program_guest, {0}, 0, 0, 0, 0};
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
        for (int i = 0; i < TRACE_TEXTURES; i++)
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
    if (guest_ptr(mdl) && readable(mdl + 0xf8, 8)) {  /* the model's first texture resource */
        uint64_t tt = *(const uint64_t *)(uintptr_t)(mdl + 0xf8);
        if (guest_ptr(tt) && readable(tt, 8)) search(&o, "texture resource", *(const uint64_t *)(uintptr_t)tt, 3, SPAN, path, 0, w, nw);
        search(&o, "model", mdl, 2, 0x240, path, 0, w, nw);
    }
    search(&o, "renderer", main_base + 0x56ab1b0, 0, SPAN, path, 0, w, nw);  /* the static renderer object */
    search(&o, "device", cur.dev, 0, SPAN, path, 0, w, nw);
    fwrite(o.buf, 1, o.len, stderr);
}
