/* YW4_MEMSCAN=<value>@<frame>,<frame>,...: a memory search for finding a game value, as a cheat finder does (tests;
 * first use: the Crank-a-kai's spins left today, 2026-11-14). YW4_MEMSCAN=<value>@live: the samples are taken when
 * Insert or F8 is pressed instead (a player at the game: replays drift where the world's timing differs), and the list is
 * written after every press from the second on.
 *
 * At the first frame, every 2-byte and 4-byte number in the game's writable memory (heap, the modules' data) that
 * equals <value> is noted (aligned; 1-byte numbers are left out: the heap also holds textures and models, where a
 * byte of 30 is everywhere). At each later frame their values are read again. At the last frame the log lists the
 * ones that changed, most changes first, with each one's values over the frames:
 *
 *     yw4: memscan: 0x7012345678 u32: 30 30 29 29 28 27 ...
 *
 * Run it with a replay of the change (tools.dev.run --replay): the replay repeats the same frames. The game's threads
 * keep running while it reads, so a value read mid-write is possible (and harmless here). */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "yw4load.h"


enum { MAX_FRAMES = 64, MAX_CANDIDATES = 8 << 20, SHOW = 60 };

typedef struct {
    uint64_t addr;
    uint8_t wide;  /* 1: u32, 0: u16 */
} Cand;

static int parsed, nframes, live, pending;
static uint64_t frames[MAX_FRAMES];
static int64_t value;
static Cand *cand;
static uint32_t *seen;  /* [candidate * MAX_FRAMES + frame index] */
static size_t ncand;

static void parse(void) {
    parsed = 1;
    const char *e = getenv("YW4_MEMSCAN");
    if (!e || !*e) return;
    char *end;
    value = strtoll(e, &end, 10);
    if (*end != '@') {
        fprintf(stderr, "yw4: memscan: YW4_MEMSCAN=<value>@<frame>,<frame>,... expected\n");
        return;
    }
    if (!strcmp(end + 1, "live")) {
        live = 1;
        fprintf(stderr, "yw4: memscan: press F8 (or Insert) to take a sample (the first finds the places holding %" PRId64 ")\n",
                value);
        return;
    }
    for (const char *p = end + 1; *p && nframes < MAX_FRAMES;) {
        unsigned long long f = strtoull(p, &end, 10);
        if (end == p) break;
        frames[nframes++] = f;
        p = *end == ',' ? end + 1 : end;
    }
}

static uint32_t read_at(const Cand *c) {
    const void *p = (const void *)(uintptr_t)c->addr;
    if (c->wide) { uint32_t v; memcpy(&v, p, 4); return v; }
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static void first_scan(void) {
    uint64_t addr[64], size[64];
    int n = yw4_memory_rw_regions(addr, size, 64);
    cand = malloc(sizeof *cand * MAX_CANDIDATES);
    if (!cand) return;
    uint64_t scanned = 0;
    for (int r = 0; r < n; r++) {
        const uint8_t *b = (const uint8_t *)(uintptr_t)addr[r];
        for (uint64_t o = 0; o + 4 <= size[r] && ncand < MAX_CANDIDATES; o += 2) {
            uint16_t h;
            memcpy(&h, b + o, 2);
            if (o % 4 == 0) {
                uint32_t w;
                memcpy(&w, b + o, 4);
                if ((int64_t)w == value) { cand[ncand++] = (Cand){addr[r] + o, 1}; continue; }
            }
            if ((int64_t)h == value && ncand < MAX_CANDIDATES) cand[ncand++] = (Cand){addr[r] + o, 0};
        }
        scanned += size[r];
    }
    seen = calloc(ncand ? ncand : 1, sizeof *seen * MAX_FRAMES);
    fprintf(stderr, "yw4: memscan: %zu places hold %" PRId64 " in %d regions (%" PRIu64 " MiB)%s\n", ncand, value, n,
            scanned >> 20, ncand == MAX_CANDIDATES ? ": the list is full, later places left out" : "");
}

static int changes(size_t i) {
    int c = 0;
    for (int k = 1; k < nframes; k++) c += seen[i * MAX_FRAMES + k] != seen[i * MAX_FRAMES + k - 1];
    return c;
}

/* A counter: it changes, and only ever steps one way and stays near the value searched for (within 15), as a count of
 * things left or used does. Busy scratch memory changes far more often and jumps about. */
static int steady(size_t i) {
    const uint32_t *v = &seen[i * MAX_FRAMES];
    int down = 1, up = 1;
    for (int k = 0; k < nframes; k++) {
        if ((int64_t)v[k] > value + 15 || (int64_t)v[k] < value - 15) return 0;
        if (k && v[k] > v[k - 1]) down = 0;
        if (k && v[k] < v[k - 1]) up = 0;
    }
    return down || up;
}

static int by_steady_then_changes(const void *a, const void *b) {
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    if (steady(x) != steady(y)) return steady(y) - steady(x);
    return changes(y) - changes(x);
}

/* Live mode, a press after each change: the value never went up and every change was a step of exactly one (a crank),
 * at least two of them, staying within 15 of the value searched for. A press where it stayed the same doesn't spoil it:
 * the first live run (2026-11-15) lost the two places reading 30 29 28 at its fourth press, where neither went down. */
static int down_at_every_press(size_t i) {
    const uint32_t *v = &seen[i * MAX_FRAMES];
    int steps = 0;
    for (int k = 1; k < nframes; k++) {
        if (v[k] > v[k - 1] || (int64_t)v[k] < value - 15) return 0;
        if (v[k] < v[k - 1]) {
            if (v[k - 1] - v[k] != 1) return 0;
            steps++;
        }
    }
    return steps >= 2;
}

/* The places that read one step down at each of the first presses (from the third): their values at every later press,
 * whatever they do then, so a counter that stops changing (moved, or a press with no crank) still shows in the log. */
enum { MAX_SHORT = 16 };
static size_t shortlist[MAX_SHORT], nshort;

static void shortlist_report(void) {
    if (!nshort && nframes == 3)
        for (size_t i = 0; i < ncand && nshort < MAX_SHORT; i++) {
            const uint32_t *v = &seen[i * MAX_FRAMES];
            if (v[0] == (uint32_t)value && v[1] + 1 == v[0] && v[2] + 1 == v[1]) shortlist[nshort++] = i;
        }
    for (size_t j = 0; j < nshort; j++) {
        size_t i = shortlist[j];
        fprintf(stderr, "yw4: memscan: shortlist %#" PRIx64 " %s:", cand[i].addr, cand[i].wide ? "u32" : "u16");
        for (int k = 0; k < nframes; k++) fprintf(stderr, " %u", seen[i * MAX_FRAMES + k]);
        fputc('\n', stderr);
    }
}

/* From the third press on: if exactly one place went down at every press, a write watch on it (kernel.c, host.c page
 * protection) logs every write with the guest function and its callers, with no limit: who changes it (leaving the
 * menu). Not the read watch: its 60 reports could all go to a reader every frame before the menu is left, and when
 * they run out it opens the page, which would end a write watch on the same page too. Every such place is listed. */
static void arm_when_found(void) {
    static int armed;
    if (nframes >= 3) shortlist_report();
    if (armed || nframes < 3) return;
    size_t found = 0, at = 0;
    for (size_t i = 0; i < ncand; i++)
        if (down_at_every_press(i)) {
            found++, at = i;
            fprintf(stderr, "yw4: memscan: down at every press: %#" PRIx64 " %s:", cand[i].addr, cand[i].wide ? "u32" : "u16");
            for (int k = 0; k < nframes; k++) fprintf(stderr, " %u", seen[i * MAX_FRAMES + k]);
            fputc('\n', stderr);
        }
    fprintf(stderr, "yw4: memscan: %zu place(s) went down one at a time, never up\n", found);
    /* two is common (the first run: the menu's count and the saved one, it seems): one write watch at a time, so the
     * first of them; the other is in the list above */
    if (found < 1 || found > 2) return;
    if (found == 2)
        for (size_t i = 0; i < ncand; i++)
            if (down_at_every_press(i)) { at = i; break; }
    armed = 1;
    fprintf(stderr, "yw4: memscan: the value is at %#" PRIx64 ": watching who writes it\n", cand[at].addr);
    yw4_write_watch_arm(cand[at].addr, cand[at].wide ? 4 : 2);
}

static void report(void) {
    size_t *idx = malloc(sizeof *idx * (ncand ? ncand : 1)), m = 0, nsteady = 0;
    if (!idx) return;
    for (size_t i = 0; i < ncand; i++)
        if (changes(i)) idx[m++] = i, nsteady += steady(i) != 0;
    qsort(idx, m, sizeof *idx, by_steady_then_changes);
    fprintf(stderr, "yw4: memscan: %zu of them count steadily one way within 15 of %" PRId64 " (listed first)\n", nsteady,
            value);
    fprintf(stderr, "yw4: memscan: %zu of %zu places changed over %d frames", m, ncand, nframes);
    for (int k = 0; k < nframes; k++) fprintf(stderr, "%s%" PRIu64, k ? "," : " (", frames[k]);
    fprintf(stderr, ")\n");
    for (size_t j = 0; j < m && j < SHOW; j++) {
        size_t i = idx[j];
        fprintf(stderr, "yw4: memscan: %#" PRIx64 " %s:", cand[i].addr, cand[i].wide ? "u32" : "u16");
        for (int k = 0; k < nframes; k++) fprintf(stderr, " %u", seen[i * MAX_FRAMES + k]);
        fputc('\n', stderr);
    }
    free(idx);
}

/* "main+0x1234" for an address inside a module, else the address */
static void where(uint64_t a, char *out, size_t n) {
    for (size_t m = 0; m < yw4_nmodules; m++) {
        uint64_t b = *yw4_modules[m].base;
        if (a - b < yw4_modules[m].mem_size) {
            snprintf(out, n, "%s+%#" PRIx64, yw4_modules[m].name, a - b);
            return;
        }
    }
    snprintf(out, n, "%#" PRIx64, a);
}

/* YW4_PEEK=<addr>[,<addr>...]: each address's u32 in the log every 300 frames (is a value at the same place every
 * run?). YW4_PTRSCAN=<addr>@<frame>: at that frame, every 8-byte value in writable memory that points into the
 * 64 KiB below <addr> (the start of the object holding it), modules' data first: a fixed place in main's data that
 * leads to the value is how a cheat finds it in every run. */
static void peek_and_ptrscan(uint64_t frame) {
    static int read_env;
    static uint64_t peek[8], ptr_target, ptr_frame;
    static int npeek;
    if (!read_env) {
        read_env = 1;
        for (const char *p = getenv("YW4_PEEK"); p && *p && npeek < 8;) {
            char *end;
            peek[npeek] = strtoull(p, &end, 16);
            if (end == p) break;
            npeek++;
            p = *end == ',' ? end + 1 : end;
        }
        const char *e = getenv("YW4_PTRSCAN");
        if (e && *e) {
            char *end;
            ptr_target = strtoull(e, &end, 16);
            ptr_frame = *end == '@' ? strtoull(end + 1, NULL, 10) : 0;
        }
    }
    if (npeek && frame % 300 == 0)
        for (int i = 0; i < npeek; i++) {
            uint32_t v;
            memcpy(&v, (const void *)(uintptr_t)peek[i], 4);
            fprintf(stderr, "yw4: peek: frame %" PRIu64 ": %#" PRIx64 " = %u\n", frame, peek[i], v);
        }
    if (ptr_target && frame == ptr_frame) {
        uint64_t addr[64], size[64];
        int n = yw4_memory_rw_regions(addr, size, 64), shown = 0;
        uint64_t lo = ptr_target - 0x10000, hi = ptr_target;
        for (int pass = 0; pass < 2; pass++)  /* modules' data first, then the heap */
            for (int r = 0; r < n; r++) {
                char w[48];
                where(addr[r], w, sizeof w);
                int in_module = strchr(w, '+') != NULL;
                if (in_module != (pass == 0)) continue;
                const uint8_t *b = (const uint8_t *)(uintptr_t)addr[r];
                for (uint64_t o = 0; o + 8 <= size[r] && shown < 80; o += 8) {
                    uint64_t v;
                    memcpy(&v, b + o, 8);
                    if (v < lo || v > hi) continue;
                    char at[48];
                    where(addr[r] + o, at, sizeof at);
                    fprintf(stderr, "yw4: ptrscan: %s holds %#" PRIx64 " (target - %#" PRIx64 ")\n", at, v, ptr_target - v);
                    shown++;
                }
            }
        fprintf(stderr, "yw4: ptrscan: %d pointer(s) into the 64 KiB below %#" PRIx64 "\n", shown, ptr_target);
    }
}

/* YW4_FINDBYTES=<hex bytes, ?? for any>[@<n>]: at an F8 / Insert press, every place in writable memory holding that
 * byte pattern is listed, and a write watch (every write, with the guest function and its callers) is put on the first
 * byte of match n (default 0). One watch per run: host.c's write watch can't be moved once armed. First use: the
 * Crank-a-kai's count, one byte in the save (2026-11-15): "?? 1e ?? 00 00 00 00 00 00 09" = count, the day's limit
 * (30), a byte that read 8, then the save's next bytes. */
static int find_pending;

static void find_bytes(void) {
    static int done;
    const char *e = getenv("YW4_FINDBYTES");
    if (done || !e || !*e) return;
    done = 1;
    uint8_t pat[64], any[64];
    int n = 0, pick = 0;
    for (const char *p = e; *p && *p != '@' && n < 64;) {
        if (*p == ' ' || *p == ',') { p++; continue; }
        if (p[0] == '?' && p[1] == '?') { any[n] = 1, pat[n++] = 0, p += 2; continue; }
        char hex[3] = {p[0], p[1] ? p[1] : 0, 0};
        any[n] = 0, pat[n++] = (uint8_t)strtoul(hex, NULL, 16);
        p += p[1] ? 2 : 1;
    }
    const char *at = strchr(e, '@');
    if (at) pick = atoi(at + 1);
    uint64_t addr[64], size[64], found[32];
    int nr = yw4_memory_rw_regions(addr, size, 64), nf = 0;
    for (int r = 0; r < nr; r++) {
        const uint8_t *b = (const uint8_t *)(uintptr_t)addr[r];
        for (uint64_t o = 0; o + (uint64_t)n <= size[r]; o++) {
            int k = 0;
            while (k < n && (any[k] || b[o + k] == pat[k])) k++;
            if (k < n) continue;
            if (nf < 32) found[nf] = addr[r] + o;
            nf++;
            char w[48];
            where(addr[r] + o, w, sizeof w);
            fprintf(stderr, "yw4: findbytes: match %d at %s:", nf - 1, w);
            for (int j = -4; j < n + 4; j++)  /* 4 bytes either side, inside the region */
                if ((int64_t)o + j >= 0 && o + j < size[r]) fprintf(stderr, " %02x", b[o + j]);
            fputc('\n', stderr);
        }
    }
    fprintf(stderr, "yw4: findbytes: %d match(es) for \"%s\"\n", nf, e);
    if (pick < nf && pick < 32) {
        fprintf(stderr, "yw4: findbytes: watching writes to match %d, %#" PRIx64 "\n", pick, found[pick]);
        yw4_write_watch_arm(found[pick], (uint64_t)n);  /* the whole pattern: the value may sit after a context prefix */
    }
}

void yw4_memscan_key(void) {
    if (!parsed) parse();
    if (live) __atomic_store_n(&pending, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&find_pending, 1, __ATOMIC_RELEASE);
}

void yw4_memscan_frame(uint64_t frame) {
    if (__atomic_exchange_n(&find_pending, 0, __ATOMIC_ACQ_REL)) find_bytes();
    peek_and_ptrscan(frame);
    if (!parsed) parse();
    if (live) {
        if (!__atomic_exchange_n(&pending, 0, __ATOMIC_ACQ_REL) || nframes == MAX_FRAMES) return;
        int k = nframes++;
        frames[k] = frame;
        if (k == 0) first_scan();
        for (size_t i = 0; i < ncand; i++) seen[i * MAX_FRAMES + k] = read_at(&cand[i]);
        fprintf(stderr, "yw4: memscan: sample %d at frame %" PRIu64 "\n", k + 1, frame);
        if (k) report(), arm_when_found();
        return;
    }
    for (int k = 0; k < nframes; k++) {
        if (frames[k] != frame) continue;
        if (k == 0) first_scan();
        for (size_t i = 0; i < ncand; i++) seen[i * MAX_FRAMES + k] = read_at(&cand[i]);
        if (k == nframes - 1) report();
    }
}
