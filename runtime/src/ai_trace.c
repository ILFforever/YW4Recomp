/* YW4_TRACE_AI=1: who in the game evaluates the battle AI's conditions (debugging only).
 *
 * The AI tables (gamedata/battle/ai/..._ai.cfg.bin: party_ai, enemy_ai, ...) hold their conditions as small encoded
 * expressions (base64 in the file): "00 00 00 00 <size> <count>", then e.g. 35 <u32 function hash> ... 32 <u32 constant>
 * ... 78 (a comparison). A background thread looks for one of party_ai's expressions in the game's memory every 3
 * seconds, decoded first, else as the file's base64 text, and puts a read watch (host.c) on the first copy found: the
 * reads are reported with the reader's guest call stack (kernel.c). That names the evaluator, the next step to reading
 * the AI's rules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "kernel.h"
#include "yw4load.h"
#include "yw4rt.h"

#ifdef _WIN32
#include <windows.h>

/* party_ai's condition 23 (an HP check, the hash 0x4e97c967 then 0x8996b8e1 and a constant 300): decoded and as base64 */
static const uint8_t EXPR[] = {0x00, 0x00, 0x00, 0x00, 0x1e, 0x0b, 0x35, 0x67, 0xc9, 0x97, 0x4e, 0x00, 0x01, 0x00, 0x32,
                               0x00, 0x00, 0x00, 0x00, 0x78, 0x35, 0xe1, 0xb8, 0x96, 0x89, 0x00, 0x01, 0x00, 0x32,
                               0x00, 0x00, 0x01, 0x2c, 0x71, 0x8f};
static const char EXPR64[] = "AAAAAB4LNWfJl04AAQAyAAAAAHg14biWiQABADIAAAEscY8=";

/* The first copy of `pat` in the game's committed memory, or 0. */
static uint64_t find(const void *pat, size_t n) {
    for (uint64_t a = YW4_GUEST_LO; a < YW4_GUEST_HI;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) break;
        uint64_t base = (uint64_t)(uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY)) && !(mbi.Protect & PAGE_GUARD)) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)base, *e = p + mbi.RegionSize;
            for (; p + n <= e; p++) {
                p = memchr(p, ((const uint8_t *)pat)[0], (size_t)(e - p - n + 1));
                if (!p) break;
                if (!memcmp(p, pat, n)) return (uint64_t)(uintptr_t)p;
            }
        }
        a = end > a ? end : a + 4096;
    }
    return 0;
}

static void search(void *arg) {
    (void)arg;
    for (int round = 0; round < 200; round++) {  /* 10 minutes */
        Sleep(3000);
        /* the expression's core from its first call on (the game may keep it without the 4-byte header and the size) */
        uint64_t at = find(EXPR + 6, 19);
        int decoded = at != 0;
        if (!at && round >= 20) at = find(EXPR64, sizeof EXPR64 - 1);  /* after a minute: the base64 text */
        if (!at) continue;
        fprintf(stderr, "yw4: ai: party_ai's condition 23 found %s at %#llx\n", decoded ? "decoded" : "as base64 text",
                (unsigned long long)at);
        yw4_read_watch_arm(at, decoded ? 19 : sizeof EXPR64 - 1, 40);
        return;
    }
    fprintf(stderr, "yw4: ai: party_ai's condition 23 not found in memory\n");
}

/* YW4_TRACE_AI=2: from the loader instead. The cfg reader's handler for BTL_AI_COND_DATA entries (main+0x3db1e0, the
 * table at main_000.c's schema registrations: hash ddd4ac39) fills a 0x30-byte record at [[[x0]+0x20]+0x30]: +0 the
 * condition's name hash, +4..+0x1c floats (-1 = unset), +0x20 flags, +0x28 the decoded expression (or 0).
 * Loads come in batches: battle_ai's 30 at start-up, the fight's own tables (party_ai, enemy_ai, ...) when a battle
 * begins. Once a batch is complete, a read watch goes on the page holding most of its expressions:
 * the reads name the code that evaluates them (reads from the loader's path, main+0x990888, are its linking). */
extern void (*yw4_entry_hook)(CPUState *ctx, uint64_t addr);  /* dispatch.c */
enum { MAX_RECS = 8192 };
static uint64_t cond_fn, recs[MAX_RECS];
static volatile long nrecs, batch, batch_start;
static volatile LONG64 last_load_ms;

static uint64_t peek(uint64_t p) {
    return p >= 0x7000000000ull && p < 0x7800000000ull && !(p & 7) ? *(const uint64_t *)(uintptr_t)p : 0;
}

static void dump_rec(const char *what, uint64_t r) {
    const uint32_t *w = (const uint32_t *)(uintptr_t)r;
    fprintf(stderr, "yw4: ai: %s record %#llx:", what, (unsigned long long)r);
    for (int i = 0; i < 12; i++) fprintf(stderr, " %08x", w[i]);
    uint64_t p = peek(r + 0x28);
    if (p) {
        fprintf(stderr, "  [+0x28]:");
        for (int i = 0; i < 24; i++) fprintf(stderr, " %02x", ((const uint8_t *)(uintptr_t)p)[i]);
    }
    fprintf(stderr, "\n");
}

/* The evaluator (main+0x97f770, found by the watch above, 2026-11-12) calls a query by its hash: a sorted table of
 * 0x18-byte entries (+8 the u32 hash, +0x10 the function) at [[x0+0x208]+8], [x0+0x208] starting with an s16 count.
 * The first evaluation writes the table out: which function answers which query. */
static uint64_t vm_fn;
static void dump_queries(CPUState *ctx) {
    uint64_t reg = peek(ctx->x[0] + 0x208);
    if (!reg) return;
    const uint8_t *h = (const uint8_t *)(uintptr_t)reg;
    int16_t n;
    memcpy(&n, h, 2);
    fprintf(stderr, "yw4: ai: query table at %#llx:", (unsigned long long)reg);
    for (int i = 0; i < 32; i++) fprintf(stderr, " %02x", h[i]);
    fprintf(stderr, "\nyw4: ai: %d queries\n", n);
    uint64_t e = peek(reg + 8);
    for (int i = 0; e && i < n && i < 400; i++, e += 0x18) {
        uint64_t q0 = *(const uint64_t *)(uintptr_t)e, fn = peek(e + 0x10);
        uint32_t hash = *(const uint32_t *)(uintptr_t)(e + 8);
        char a[48], name[64] = "";
        if (q0 >= 0x7000000000ull && q0 < 0x7800000000ull) {  /* a name? */
            const char *c = (const char *)(uintptr_t)q0;
            int k = 0;
            while (k < 63 && c[k] >= 0x20 && c[k] < 0x7f) name[k] = c[k], k++;
            name[k] = 0;
        }
        fprintf(stderr, "yw4: ai: query %08x -> %s  (+0 %#llx%s%s)\n", hash, yw4_describe(fn, a), (unsigned long long)q0,
                name[0] ? " = " : "", name);
    }
}

static void cond_hook(CPUState *ctx, uint64_t addr) {
    if (addr == vm_fn) {
        static int dumped;
        if (!dumped) dumped = 1, dump_queries(ctx);
        return;
    }
    if (addr != cond_fn) return;
    uint64_t rec = peek(peek(peek(ctx->x[0]) + 0x20) + 0x30);
    if (!rec || nrecs >= MAX_RECS) return;
    uint64_t now = GetTickCount64();
    if (!nrecs || now - (uint64_t)last_load_ms > 3000) {  /* a new batch */
        batch++, batch_start = nrecs;
        uint64_t st[10];
        int k = yw4_call_stack(ctx, st, 10);
        char a[48];
        fprintf(stderr, "yw4: ai: batch %ld of condition records starts at %#llx, loaded from:\n", batch,
                (unsigned long long)rec);
        for (int j = 0; j < k; j++) fprintf(stderr, "       %s\n", yw4_describe(st[j] - 4, a));
    }
    recs[nrecs++] = rec;
    last_load_ms = (LONG64)now;
}

static void watch_after_loads(void *arg) {
    (void)arg;
    long done = 0;
    for (;;) {
        Sleep(2000);
        if (batch == done || GetTickCount64() - (uint64_t)last_load_ms < 8000) continue;
        long b = batch, from = batch_start, to = nrecs;
        done = b;
        int with = 0;
        for (long i = from; i < to; i++) with += peek(recs[i] + 0x28) != 0;
        fprintf(stderr, "yw4: ai: batch %ld complete: %ld records, %d with an expression\n", b, to - from, with);
        for (long i = from; i < to && i < from + 6; i++) dump_rec("e.g.", recs[i]);
        /* the first batch (battle_ai, at start-up) is what a fight reads: no other batch came in one (2026-11-12) */
        /* the page with the most expressions of this batch, and their span in it */
        uint64_t best = 0;
        int best_n = 0;
        for (long i = from; i < to; i++) {
            uint64_t p = peek(recs[i] + 0x28) & ~4095ull;
            if (!p) continue;
            int n = 0;
            for (long j = from; j < to; j++) n += (peek(recs[j] + 0x28) & ~4095ull) == p;
            if (n > best_n) best_n = n, best = p;
        }
        if (!best) continue;
        uint64_t lo = best + 4096, hi = best;
        for (long i = from; i < to; i++) {
            uint64_t p = peek(recs[i] + 0x28);
            if ((p & ~4095ull) != best) continue;
            if (p < lo) lo = p;
            if (p + 0x40 > hi) hi = p + 0x40;
        }
        if (hi > best + 4096) hi = best + 4096;
        /* the records may follow the expressions on the same page: the linker reads those at every battle start, and
         * they used up the reports (2026-11-12); the watch stops short of the first record above the expressions */
        for (long i = 0; i < nrecs; i++)
            if (recs[i] >= lo && recs[i] < hi) hi = recs[i];
        fprintf(stderr, "yw4: ai: %d expressions of batch %ld on page %#llx: watching their reads\n", best_n, b,
                (unsigned long long)best);
        yw4_read_watch_arm(lo, hi - lo, 120);
        return;
    }
}

void ai_trace_start(void) {
    const char *e = getenv("YW4_TRACE_AI");
    if (e && *e == '2') {
        cond_fn = yw4_image_base + 0x3db1e0;
        vm_fn = yw4_image_base + 0x97f770;
        yw4_entry_hook = cond_hook;
        yw4_probe_any = 1;
        fprintf(stderr, "yw4: ai: logging the battle AI's condition records as they load (YW4_TRACE_AI=2)\n");
        host_thread_start(watch_after_loads, NULL, 1 << 16);
        return;
    }
    if (!e || *e != '1') return;
    fprintf(stderr, "yw4: ai: looking for the battle AI's conditions in memory (YW4_TRACE_AI)\n");
    host_thread_start(search, NULL, 1 << 20);
}
#else
void ai_trace_start(void) {}
#endif
