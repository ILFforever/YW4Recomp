/* Merging several mods' copies of one game data file (decision 0068).
 *
 * Two mods that ship the same file can't both be laid over the game: the later one's copy replaces the earlier
 * one's. For Level-5's .cfg.bin data files (items, shops, NPCs, ...) most mods change a few entries of a big list,
 * and different mods change different entries, so a merge against the game's own copy keeps all of them, the way
 * a three-way merge in version control does: each mod's copy is compared with the game's entry by entry (a Myers
 * diff), and every changed run of entries is taken from the mod that changed it. Two mods changing the same entries
 * is a real clash; then nothing is merged and the later mod's copy is used, as before.
 *
 * The game's copy is usually inside one of its CRI .cpk archives, so this file also reads those (the format notes
 * are in tools/yw4/cpk.py, which this follows).
 */
#include "modmerge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMPTY 0xFFFFFFFFu

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "yw4: mods: out of memory\n");
        exit(3);
    }
    return p;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

/* ---- CRI .cpk archives: an @UTF table (big-endian) in the "CPK " header chunk gives TocOffset; the "TOC " chunk's
 * table lists DirName, FileName, FileSize (stored), ExtractSize, FileOffset (from the TOC chunk). */

typedef struct {
    const uint8_t *b;
    size_t n;
    uint32_t rows_off, str_off, nrows;
    uint16_t row_len;
    int ncols;
    struct {
        const char *name;
        uint8_t storage, type;
        size_t at;  /* the constant's place in b, or the value's place in a row */
    } cols[64];
} Utf;

static int utf_size(uint8_t type) {
    static const int sizes[12] = {1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 4, 8};
    return type < 12 ? sizes[type] : -1;
}

static int utf_open(Utf *u, const uint8_t *b, size_t n) {
    if (n < 0x20 || memcmp(b, "@UTF", 4)) return -1;
    u->b = b, u->n = n;
    u->rows_off = be16(b + 10) + 8u, u->str_off = be32(b + 12) + 8u;
    u->ncols = be16(b + 24), u->row_len = be16(b + 26), u->nrows = be32(b + 28);
    if (u->ncols > 64 || u->str_off >= n || u->rows_off + (uint64_t)u->row_len * u->nrows > n) return -1;
    size_t pos = 0x20, row_at = 0;
    for (int i = 0; i < u->ncols; i++) {
        if (pos + 5 > n) return -1;
        uint8_t flags = b[pos];
        uint32_t name = be32(b + pos + 1);
        pos += 5;
        if (u->str_off + name >= n || !memchr(b + u->str_off + name, 0, n - u->str_off - name)) return -1;
        u->cols[i].name = (const char *)b + u->str_off + name;
        u->cols[i].storage = flags & 0xF0, u->cols[i].type = flags & 0x0F;
        int sz = utf_size(u->cols[i].type);
        if (sz < 0) return -1;
        if (u->cols[i].storage == 0x30) {
            u->cols[i].at = pos;
            pos += (size_t)sz;
        } else if (u->cols[i].storage == 0x50) {
            u->cols[i].at = row_at;
            row_at += (size_t)sz;
        }
    }
    return row_at <= u->row_len ? 0 : -1;
}

/* A column's value in a row: *s for a string column, *v for a number; 0 if the column isn't there. */
static int utf_get(const Utf *u, uint32_t row, const char *name, uint64_t *v, const char **s) {
    for (int i = 0; i < u->ncols; i++) {
        if (strcmp(u->cols[i].name, name)) continue;
        size_t at;
        if (u->cols[i].storage == 0x30) at = u->cols[i].at;
        else if (u->cols[i].storage == 0x50) at = u->rows_off + (size_t)row * u->row_len + u->cols[i].at;
        else return 0;
        const uint8_t *p = u->b + at;
        switch (u->cols[i].type) {
        case 0: case 1: *v = p[0]; return 1;
        case 2: case 3: *v = be16(p); return 1;
        case 4: case 5: *v = be32(p); return 1;
        case 6: case 7: *v = be64(p); return 1;
        case 0xA: {
            uint32_t o = be32(p);
            if (u->str_off + o >= u->n || !memchr(u->b + u->str_off + o, 0, u->n - u->str_off - o)) return 0;
            *s = (const char *)u->b + u->str_off + o;
            return 1;
        }
        default: return 0;
        }
    }
    return 0;
}

static uint8_t *read_at(FILE *f, uint64_t at, size_t n) {
    uint8_t *b = xmalloc(n);
#ifdef _WIN32
    int bad = _fseeki64(f, (long long)at, SEEK_SET);
#else
    int bad = fseeko(f, (off_t)at, SEEK_SET);
#endif
    if (bad || fread(b, 1, n, f) != n) {
        free(b);
        return NULL;
    }
    return b;
}

/* A chunk ("CPK ", "TOC "): 16-byte head with the table's size at +8, the @UTF table after it. */
static uint8_t *read_chunk(FILE *f, uint64_t at, const char *magic, size_t *n) {
    uint8_t *head = read_at(f, at, 16);
    if (!head) return NULL;
    uint64_t size = (uint64_t)le32(head + 8) | (uint64_t)le32(head + 12) << 32;
    int ok = !memcmp(head, magic, 4) && size >= 0x20 && size < (1u << 30);
    free(head);
    if (!ok) return NULL;
    *n = (size_t)size;
    return read_at(f, at + 16, (size_t)size);
}

/* CRILAYLA: the first 0x100 output bytes stored raw; the rest an LZ stream read backwards, bits MSB first, output
 * written from the end; flag 0 = an 8-bit literal, 1 = a 13-bit distance (+3) and a length 3 + 2,3,5,8,8...-bit
 * fields while a field is all ones. */
static uint8_t *crilayla(const uint8_t *src, size_t n, size_t *out_n) {
    if (n < 16 || memcmp(src, "CRILAYLA", 8)) return NULL;
    uint32_t size = le32(src + 8), prefix = le32(src + 12);
    if ((uint64_t)prefix + 0x110 > n) return NULL;
    size_t total = (size_t)size + 0x100;
    uint8_t *out = xmalloc(total);
    memcpy(out, src + 0x10 + prefix, 0x100);
    int64_t pos = (int64_t)0x10 + prefix - 1;
    unsigned cur = 0, left = 0;
    int bad = 0;
#define BITS(nbits, v)                                                         \
    do {                                                                       \
        unsigned n_ = (nbits);                                                 \
        (v) = 0;                                                               \
        while (n_) {                                                           \
            if (!left) {                                                       \
                if (pos < 0x10) { bad = 1; break; }                            \
                cur = src[pos--], left = 8;                                    \
            }                                                                  \
            unsigned t_ = n_ < left ? n_ : left;                               \
            (v) = ((v) << t_) | ((cur >> (left - t_)) & ((1u << t_) - 1));     \
            left -= t_, n_ -= t_;                                              \
        }                                                                      \
    } while (0)
    int64_t w = (int64_t)total - 1;
    while (w >= 0x100 && !bad) {
        unsigned flag;
        BITS(1, flag);
        if (bad) break;
        if (flag) {
            unsigned back, v;
            BITS(13, back);
            back += 3;
            unsigned length = 3;
            static const unsigned widths[3] = {2, 3, 5};
            int more = 1;
            for (int i = 0; i < 3 && more; i++) {
                BITS(widths[i], v);
                length += v;
                more = v == (1u << widths[i]) - 1;
            }
            while (more && !bad) {
                BITS(8, v);
                length += v;
                more = v == 0xFF;
            }
            for (unsigned i = 0; i < length && w >= 0x100; i++, w--) {
                if ((uint64_t)w + back >= total) { bad = 1; break; }
                out[w] = out[w + back];
            }
        } else {
            unsigned v;
            BITS(8, v);
            out[w--] = (uint8_t)v;
        }
    }
#undef BITS
    if (bad) {
        free(out);
        return NULL;
    }
    *out_n = total;
    return out;
}

uint8_t *cpk_read_file(const char *cpk_path, const char *inner_path, size_t *n) {
    FILE *f = fopen(cpk_path, "rb");
    if (!f) return NULL;
    uint8_t *result = NULL, *hb = NULL, *tb = NULL;
    size_t hn = 0, tn = 0;
    Utf h, t;
    uint64_t toc = 0;
    const char *s = NULL;
    if (!(hb = read_chunk(f, 0, "CPK ", &hn)) || utf_open(&h, hb, hn) || !utf_get(&h, 0, "TocOffset", &toc, &s) || !toc)
        goto done;
    if (!(tb = read_chunk(f, toc, "TOC ", &tn)) || utf_open(&t, tb, tn)) goto done;
    for (uint32_t r = 0; r < t.nrows; r++) {
        const char *dir = "", *name = "";
        uint64_t stored = 0, extract = 0, off = 0, v = 0;
        utf_get(&t, r, "DirName", &v, &dir);
        if (!utf_get(&t, r, "FileName", &v, &name)) continue;
        size_t dl = strlen(dir);
        if (dl ? strncmp(inner_path, dir, dl) || inner_path[dl] != '/' || strcmp(inner_path + dl + 1, name)
               : strcmp(inner_path, name))
            continue;
        utf_get(&t, r, "FileSize", &stored, &s);
        utf_get(&t, r, "ExtractSize", &extract, &s);
        utf_get(&t, r, "FileOffset", &off, &s);
        if (!stored || stored > (1u << 30) || extract > (1u << 30)) break;
        uint8_t *data = read_at(f, toc + off, (size_t)stored);
        if (!data) break;
        if (extract && extract != stored) {
            size_t got = 0;
            uint8_t *raw = crilayla(data, (size_t)stored, &got);
            free(data);
            if (!raw || got != extract) {
                free(raw);
                break;
            }
            data = raw, stored = extract;
        }
        result = data, *n = (size_t)stored;
        break;
    }
done:
    free(hb);
    free(tb);
    fclose(f);
    return result;
}

/* ---- Level-5 .cfg.bin: u32 entry count, string table offset, its size, its string count; the entries (u32 name
 * crc, u8 value count, 2-bit value types four to a byte (0 string, 1 int, 2 float), 0xFF to a 4-byte boundary, u32
 * values; a string is an offset into the table, 0xFFFFFFFF for none); 0xFF to 16; the strings, each once, in the
 * order the entries first use them; 0xFF to 16; then the entry names' table (kept as it is). Measured on the
 * game's 2.2.0 files: a file written back this way is byte-for-byte the game's (cfgbin_rewrite, the check). */

typedef struct {
    uint32_t crc;
    uint8_t k;
    const uint8_t *types, *vals;  /* in the file it came from */
    const char *st;               /* that file's string table */
    uint64_t h;
} Ent;

typedef struct {
    Ent *e;
    uint32_t count;
    const uint8_t *tail;
    size_t ntail;
} Cfg;

static unsigned vtype(const Ent *e, unsigned i) { return (e->types[i / 4] >> (2 * (i % 4))) & 3; }

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n--) h = (h ^ *b++) * 1099511628211ull;
    return h;
}

static int cfg_parse(const uint8_t *b, size_t n, Cfg *c) {
    memset(c, 0, sizeof *c);
    if (n < 16) return -1;
    uint32_t count = le32(b), st_off = le32(b + 4), st_len = le32(b + 8);
    if (st_off > n || st_len > n - st_off || count > n / 8) return -1;
    const char *st = (const char *)b + st_off;
    c->e = xmalloc(sizeof *c->e * (count ? count : 1));
    size_t p = 16;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 5 > st_off) goto bad;
        Ent *e = &c->e[i];
        e->crc = le32(b + p), e->k = b[p + 4], e->st = st;
        p += 5;
        size_t tl = (e->k + 3u) / 4;
        if (p + tl > st_off) goto bad;
        e->types = b + p;
        p = (p + tl + 3) & ~(size_t)3;
        if (p + 4u * e->k > st_off) goto bad;
        e->vals = b + p;
        p += 4u * e->k;
        uint64_t h = fnv(1469598103934665603ull, &e->crc, 4);
        h = fnv(h, &e->k, 1);
        for (unsigned j = 0; j < e->k; j++) {
            unsigned t = vtype(e, j);
            uint32_t v = le32(e->vals + 4 * j);
            h = fnv(h, &t, sizeof t);
            if (t == 0 && v != EMPTY) {
                if (v >= st_len || !memchr(st + v, 0, st_len - v)) goto bad;
                h = fnv(h, st + v, strlen(st + v) + 1);
            } else {
                h = fnv(h, &v, 4);
            }
        }
        e->h = h;
    }
    size_t tail = ((size_t)st_off + st_len + 15) & ~(size_t)15;
    c->count = count;
    c->tail = b + (tail < n ? tail : n), c->ntail = tail < n ? n - tail : 0;
    return 0;
bad:
    free(c->e);
    c->e = NULL;
    return -1;
}

static int ent_eq(const Ent *a, const Ent *b) {
    if (a->h != b->h || a->crc != b->crc || a->k != b->k) return 0;
    for (unsigned j = 0; j < a->k; j++) {
        unsigned t = vtype(a, j);
        if (t != vtype(b, j)) return 0;
        uint32_t x = le32(a->vals + 4 * j), y = le32(b->vals + 4 * j);
        if (t == 0 && x != EMPTY && y != EMPTY) {
            if (strcmp(a->st + x, b->st + y)) return 0;
        } else if (x != y) {
            return 0;
        }
    }
    return 1;
}

typedef struct {
    uint8_t *b;
    size_t n, cap;
} Buf;

static void put(Buf *o, const void *p, size_t n) {
    if (o->n + n > o->cap) {
        while (o->n + n > o->cap) o->cap = o->cap ? 2 * o->cap : 4096;
        uint8_t *nb = realloc(o->b, o->cap);
        if (!nb) {
            fprintf(stderr, "yw4: mods: out of memory\n");
            exit(3);
        }
        o->b = nb;
    }
    memcpy(o->b + o->n, p, n);
    o->n += n;
}
static void put32(Buf *o, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(o, b, 4);
}
static void pad(Buf *o, size_t to) {
    static const uint8_t ff = 0xFF;
    while (o->n % to) put(o, &ff, 1);
}

typedef struct {
    const char *str;
    uint32_t off;
} Str;

/* Writes entries (from any of the parsed files) as one file, with the strings laid out anew. */
static uint8_t *cfg_write(const Ent *e, size_t count, const uint8_t *tail, size_t ntail, size_t *out_n) {
    Buf o = {0}, s = {0};
    uint8_t zero[16] = {0};
    put(&o, zero, 16);
    size_t cap = 64;
    while (cap < 4 * count) cap <<= 1;
    Str *tab = calloc(cap, sizeof *tab);
    if (!tab) exit(3);
    uint32_t nstr = 0;
    for (size_t i = 0; i < count; i++) {
        const Ent *x = &e[i];
        put32(&o, x->crc);
        put(&o, &x->k, 1);
        put(&o, x->types, (x->k + 3u) / 4);
        pad(&o, 4);
        for (unsigned j = 0; j < x->k; j++) {
            uint32_t v = le32(x->vals + 4 * j);
            if (vtype(x, j) == 0 && v != EMPTY) {
                const char *str = x->st + v;
                size_t h = (size_t)fnv(1469598103934665603ull, str, strlen(str)) & (cap - 1);
                while (tab[h].str && strcmp(tab[h].str, str)) h = (h + 1) & (cap - 1);
                if (!tab[h].str) {
                    if (nstr + 1 > cap / 2) {  /* keep the table at most half full: grow it */
                        size_t ncap = cap * 2;
                        Str *nt = calloc(ncap, sizeof *tab);
                        if (!nt) exit(3);
                        for (size_t q = 0; q < cap; q++) {
                            if (!tab[q].str) continue;
                            size_t g = (size_t)fnv(1469598103934665603ull, tab[q].str, strlen(tab[q].str)) & (ncap - 1);
                            while (nt[g].str) g = (g + 1) & (ncap - 1);
                            nt[g] = tab[q];
                        }
                        free(tab);
                        tab = nt, cap = ncap;
                        h = (size_t)fnv(1469598103934665603ull, str, strlen(str)) & (cap - 1);
                        while (tab[h].str) h = (h + 1) & (cap - 1);
                    }
                    tab[h].str = str, tab[h].off = (uint32_t)s.n;
                    put(&s, str, strlen(str) + 1);
                    nstr++;
                }
                v = tab[h].off;
            }
            put32(&o, v);
        }
    }
    pad(&o, 16);
    uint32_t st_off = (uint32_t)o.n, st_len = (uint32_t)s.n;
    if (s.n) put(&o, s.b, s.n);
    if (ntail) {
        pad(&o, 16);
        put(&o, tail, ntail);
    }
    uint32_t head[4] = {(uint32_t)count, st_off, st_len, nstr};
    for (int i = 0; i < 4; i++) {
        uint8_t *p = o.b + 4 * i;
        p[0] = (uint8_t)head[i], p[1] = (uint8_t)(head[i] >> 8), p[2] = (uint8_t)(head[i] >> 16), p[3] = (uint8_t)(head[i] >> 24);
    }
    free(tab);
    free(s.b);
    *out_n = o.n;
    return o.b;
}

uint8_t *cfgbin_rewrite(const uint8_t *b, size_t n, size_t *out_n) {
    Cfg c;
    if (cfg_parse(b, n, &c)) return NULL;
    uint8_t *r = cfg_write(c.e, c.count, c.tail, c.ntail, out_n);
    free(c.e);
    return r;
}

/* ---- the diff: runs of base entries [i1, i2) a mod replaced with its entries [j1, j2) (an insert when i1 == i2) */

typedef struct {
    uint32_t i1, i2, j1, j2;
    int mod;
} Hunk;

typedef struct {
    Hunk *h;
    size_t n, cap;
} Hunks;

static void add_hunk(Hunks *hs, Hunk h) {
    if (hs->n == hs->cap) {
        hs->cap = hs->cap ? 2 * hs->cap : 64;
        Hunk *nh = realloc(hs->h, sizeof *nh * hs->cap);
        if (!nh) exit(3);
        hs->h = nh;
    }
    hs->h[hs->n++] = h;
}

enum { MAX_EDITS = 6000 };  /* more changes than this in one file: not merged (keeps the diff's memory small) */

/* Myers' O((N+M)D) diff of a[0..n) against b[0..m), after the common start and end are taken off. */
static int diff(const Ent *a, uint32_t n, const Ent *b, uint32_t m, int mod, Hunks *out) {
    uint32_t p = 0;
    while (p < n && p < m && ent_eq(&a[p], &b[p])) p++;
    uint32_t s = 0;
    while (s < n - p && s < m - p && ent_eq(&a[n - 1 - s], &b[m - 1 - s])) s++;
    const Ent *A = a + p, *B = b + p;
    int N = (int)(n - p - s), M = (int)(m - p - s);
    if (!N && !M) return 0;
    int max = N + M < MAX_EDITS ? N + M : MAX_EDITS;
    int *v = xmalloc(sizeof *v * (size_t)(2 * max + 3));
    int **trace = xmalloc(sizeof *trace * (size_t)(max + 1));
    int off = max + 1, D = -1;
    v[off + 1] = 0;
    for (int d = 0; d <= max && D < 0; d++) {
        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && v[off + k - 1] < v[off + k + 1])) ? v[off + k + 1] : v[off + k - 1] + 1;
            int y = x - k;
            while (x < N && y < M && ent_eq(&A[x], &B[y])) x++, y++;
            v[off + k] = x;
            if (x >= N && y >= M) D = d;
        }
        trace[d] = xmalloc(sizeof(int) * (size_t)(2 * d + 1));
        for (int k = -d; k <= d; k++) trace[d][k + d] = v[off + k];
        if (D >= 0) break;
    }
    int ok = D >= 0;
    if (ok) {
        /* walk back from the end: each step is one deleted entry of A or one inserted entry of B */
        typedef struct { int x, y, del; } Op;
        Op *ops = xmalloc(sizeof *ops * (size_t)(D + 1));
        int nops = 0, x = N, y = M;
        for (int d = D; d > 0; d--) {
            int k = x - y;
            const int *pv = trace[d - 1];
#define PV(kk) pv[(kk) + (d - 1)]
            int down = k == -d || (k != d && PV(k - 1) < PV(k + 1));
            int pk = down ? k + 1 : k - 1;
            int px = PV(pk), py = px - pk;
#undef PV
            int mx = down ? px : px + 1, my = down ? py + 1 : py;  /* the point right after this step's edit */
            while (x > mx && y > my) x--, y--;                      /* the equal run after it */
            ops[nops++] = down ? (Op){px, py, 0} : (Op){px, py, 1};
            x = px, y = py;
        }
        Hunk cur = {0, 0, 0, 0, mod};
        int open = 0;
        for (int i = nops - 1; i >= 0; i--) {
            Op o = ops[i];
            if (open && (uint32_t)o.x == cur.i2 && (uint32_t)o.y == cur.j2) {
                if (o.del) cur.i2++; else cur.j2++;
                continue;
            }
            if (open) add_hunk(out, (Hunk){cur.i1 + p, cur.i2 + p, cur.j1 + p, cur.j2 + p, mod});
            cur = (Hunk){(uint32_t)o.x, (uint32_t)o.x + (o.del ? 1u : 0u), (uint32_t)o.y, (uint32_t)o.y + (o.del ? 0u : 1u), mod};
            open = 1;
        }
        if (open) add_hunk(out, (Hunk){cur.i1 + p, cur.i2 + p, cur.j1 + p, cur.j2 + p, mod});
        free(ops);
    }
    for (int d = 0; d <= (D >= 0 ? D : max); d++) free(trace[d]);
    free(trace);
    free(v);
    return ok ? 0 : -1;
}

/* Two mods' changes collide: they share base entries, or one inserts inside the other's run (an insert at the
 * edge of a run is fine: it goes before the run). Two inserts at one place collide too (which goes first?). */
static int collide(const Hunk *a, const Hunk *b) {
    int ea = a->i1 == a->i2, eb = b->i1 == b->i2;
    if (!ea && !eb) return (a->i1 > b->i1 ? a->i1 : b->i1) < (a->i2 < b->i2 ? a->i2 : b->i2);
    if (ea && eb) return a->i1 == b->i1;
    const Hunk *ins = ea ? a : b, *run = ea ? b : a;
    return run->i1 < ins->i1 && ins->i1 < run->i2;
}

static int by_place(const void *x, const void *y) {
    const Hunk *a = x, *b = y;
    if (a->i1 != b->i1) return a->i1 < b->i1 ? -1 : 1;
    if (a->i2 != b->i2) return a->i2 < b->i2 ? -1 : 1;  /* an insert before the run that starts there */
    return a->mod - b->mod;
}

uint8_t *cfgbin_merge(const uint8_t *base, size_t nbase, const uint8_t *const *mods, const size_t *nmod, int nmods,
                      size_t *out_n, int *clash_a, int *clash_b) {
    *clash_a = *clash_b = -1;
    if (nmods < 1) return NULL;
    Cfg bc, *mc = xmalloc(sizeof *mc * (size_t)nmods);
    int parsed = 0, ok = !cfg_parse(base, nbase, &bc);
    Hunks hs = {0};
    uint8_t *result = NULL;
    for (int i = 0; ok && i < nmods; i++) {
        ok = !cfg_parse(mods[i], nmod[i], &mc[i]);
        if (!ok) break;
        parsed++;
        /* the entry names' table: a mod that added new kinds of entries can't be merged into the game's */
        ok = mc[i].ntail == bc.ntail && !memcmp(mc[i].tail, bc.tail, bc.ntail) && !diff(bc.e, bc.count, mc[i].e, mc[i].count, i, &hs);
    }
    if (ok) {
        qsort(hs.h, hs.n, sizeof *hs.h, by_place);
        for (size_t i = 0; i < hs.n && ok; i++)
            for (size_t j = i + 1; j < hs.n && hs.h[j].i1 <= hs.h[i].i2 && ok; j++)
                if (hs.h[i].mod != hs.h[j].mod && collide(&hs.h[i], &hs.h[j])) {
                    ok = 0;
                    *clash_a = hs.h[i].mod < hs.h[j].mod ? hs.h[i].mod : hs.h[j].mod;
                    *clash_b = hs.h[i].mod < hs.h[j].mod ? hs.h[j].mod : hs.h[i].mod;
                }
        if (ok) {
            size_t total = bc.count;
            for (size_t i = 0; i < hs.n; i++) total += (hs.h[i].j2 - hs.h[i].j1), total -= (hs.h[i].i2 - hs.h[i].i1);
            Ent *e = xmalloc(sizeof *e * (total ? total : 1));
            size_t w = 0;
            uint32_t pos = 0;
            for (size_t i = 0; i < hs.n; i++) {
                const Hunk *h = &hs.h[i];
                while (pos < h->i1) e[w++] = bc.e[pos++];
                for (uint32_t j = h->j1; j < h->j2; j++) e[w++] = mc[h->mod].e[j];
                pos = h->i2;
            }
            while (pos < bc.count) e[w++] = bc.e[pos++];
            result = cfg_write(e, w, bc.tail, bc.ntail, out_n);
            free(e);
        }
    }
    for (int i = 0; i < parsed; i++) free(mc[i].e);
    free(mc);
    free(bc.e);
    free(hs.h);
    return result;
}
