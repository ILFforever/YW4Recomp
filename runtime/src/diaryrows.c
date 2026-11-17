/* The Diary's stacked rows: the game's data changed in memory (see diaryrows.h).
 *
 * The Diary menu is data (docs/research/diary-save-slots.md, "Stacked rows"): its setting file gives the row layer
 * (diary_load_slot) 2 copies, and the locator object puts copy 0 and copy 1 on two bones of the anchor model
 * save00_06. Here: four bones more (six new ones, so the game's slide-in animation, which moves the old two, doesn't
 * pull rows back), a locator line for each copy, and 6 copies. The formats were checked by writing every menu model
 * of the game back byte for byte (a Python copy of this code, 2026-11-17). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diaryrows.h"

#define SLOT_LAYER 0xd14f93d6u  /* crc32("diary_load_slot") */
#define ROW_SAVES 5             /* rows 0..4 the saves, row 5 the Autosave */
/* Where the rows go (1920 x 1080 units, y down is negative): the game's were 192 apart from -440. Owner, 2026-11-17:
 * the saves closer together, the gap above the Autosave kept. */
/* The block centred on the card. A row's "Latest!" badge stands above it, over the row above: savefs.c draws each
 * row over the one above it (their draw priorities). */
static const float START_Y = -250.0f, STEP = 100.0f, AUTO_GAP = 128.0f;

int diaryrows_slots_enabled(void) {
    const char *b = getenv("YW4_DIARY_SLOTS");
    return !(b && !strcmp(b, "0"));
}

int diaryrows_enabled(void) {
    const char *a = getenv("YW4_DIARY_ROWS");
    return !(a && !strcmp(a, "0")) && diaryrows_slots_enabled();
}

static uint32_t crc32_str(const char *s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t get32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint16_t get16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static size_t al(size_t x, size_t a) { return (x + a - 1) / a * a; }

/* A growing buffer. */
typedef struct {
    uint8_t *b;
    size_t n, cap;
    int bad;
} Buf;
static void put(Buf *o, const void *p, size_t k) {
    if (o->bad) return;
    if (o->n + k > o->cap) {
        size_t c = o->cap ? o->cap : 4096;
        while (c < o->n + k) c *= 2;
        uint8_t *nb = realloc(o->b, c);
        if (!nb) {
            o->bad = 1;
            return;
        }
        o->b = nb, o->cap = c;
    }
    if (p) memcpy(o->b + o->n, p, k);
    else memset(o->b + o->n, 0, k);
    o->n += k;
}
static void pad(Buf *o, size_t a) { put(o, NULL, al(o->n, a) - o->n); }

static void bone_name(char out[32], int i) { snprintf(out, 32, "_pos_load_bar_locate%d", 10 + i); }

/* ---- the anchor model: a G4PK package, its first part the G4SK skeleton (version 104)
 *
 * G4PK: 0x40 header (+0xc: size - 0x40, +0x20: u32 part count), at 0x40 the parts' u32 offsets (/4, from 0x40) and
 * then their u32 sizes; parts 0x20-aligned, the file padded to 0x20.
 * G4SK: 0x40 header (+0xc: size - 0x40, +0x20: u16 bone count, +0x24: 11 u16 section offsets /4 from 0x40): world
 * 3x4 matrices (0x30 each, from 0x40), inverse matrices, local scale / rotation / position (0x30 each; the three
 * 0x20-aligned), three more sections (empty here), name crc32s (u32), parents (u16; the root's is the count), the
 * indices sorted by crc (u16), flags (u8), a u32 each, the names' offsets (u16, from the table, padded to 4) and the
 * names; after them a few bytes kept as they are. */

#define MAXB 64
static uint32_t sort_hash[MAXB];
static int by_hash(const void *a, const void *b) {
    uint32_t x = sort_hash[*(const uint16_t *)a], y = sort_hash[*(const uint16_t *)b];
    return x < y ? -1 : x > y;
}

static uint8_t *skeleton(const uint8_t *s, size_t n, size_t *out_n) {
    if (n < 0x40 || memcmp(s, "G4SK", 4) || get16(s + 6) != 104) return NULL;
    unsigned nb = get16(s + 0x20);
    size_t f[11];
    for (int i = 0; i < 11; i++) f[i] = 0x40 + 4 * (size_t)get16(s + 0x24 + 2 * i);
    if (nb == 0 || nb + ROW_SAVES + 1 > MAXB || f[8] != f[2] || f[7] + 2 * (size_t)nb > n) return NULL;
    size_t size = get32(s + 0xc);
    if (0x40 + size > n) return NULL;
    const char *name[MAXB];
    size_t end = 0;
    int bar = -1;
    for (unsigned i = 0; i < nb; i++) {
        size_t o = f[7] + get16(s + f[7] + 2 * i);
        const uint8_t *z = o < n ? memchr(s + o, 0, n - o) : NULL;
        if (!z) return NULL;
        name[i] = (const char *)s + o;
        if ((size_t)(z - s) + 1 > end) end = (size_t)(z - s) + 1;
        if (!strcmp(name[i], "_pos_load_bar00")) bar = (int)i;
        char b0[32];
        bone_name(b0, 0);
        if (!strcmp(name[i], b0)) return NULL;  /* already changed */
    }
    if (bar < 0 || end > 0x40 + size) return NULL;
    size_t slack = 0x40 + size - end, pad_end = n - (0x40 + size);
    /* the bar's world position: its matrix's translation column */
    float bx, by;
    memcpy(&bx, s + 0x40 + (size_t)bar * 0x30 + 12, 4);
    memcpy(&by, s + 0x40 + (size_t)bar * 0x30 + 28, 4);

    unsigned nn = nb + ROW_SAVES + 1;
    uint32_t hash[MAXB];
    uint16_t parent[MAXB], order[MAXB];
    uint8_t flag[MAXB];
    uint32_t each[MAXB];
    char extra[ROW_SAVES + 1][32];
    float y[ROW_SAVES + 1];
    for (unsigned i = 0; i < nb; i++) {
        hash[i] = get32(s + f[2] + 4 * i), parent[i] = get16(s + f[3] + 2 * i);
        flag[i] = s[f[5] + i], each[i] = get32(s + f[6] + 4 * i);
    }
    for (int k = 0; k <= ROW_SAVES; k++) {
        unsigned i = nb + (unsigned)k;
        bone_name(extra[k], k);
        name[i] = extra[k];
        hash[i] = crc32_str(extra[k]), parent[i] = (uint16_t)bar, flag[i] = 1, each[i] = 0;
        y[k] = k < ROW_SAVES ? START_Y - (float)k * STEP : START_Y - (float)(ROW_SAVES - 1) * STEP - AUTO_GAP;
    }
    for (unsigned i = 0; i < nn; i++) order[i] = (uint16_t)i, sort_hash[i] = hash[i];
    qsort(order, nn, sizeof *order, by_hash);

    Buf o = {0};
    put(&o, s, 0x40);
    size_t fi[11];
#define MARK() (o.n)
    /* world matrices, then the inverses, then the local transforms: the game's, then ours */
    for (int sec = 0; sec < 3; sec++) {
        if (sec) fi[sec - 1] = MARK();
        size_t from = sec == 0 ? 0x40 : f[sec - 1];
        put(&o, s + from, (size_t)nb * 0x30);
        for (int k = 0; k <= ROW_SAVES; k++) {
            float m[12] = {0};
            if (sec == 2) {  /* scale 1 1 1 0, rotation 0 0 0 1, position x y 0 1 (under the bar) */
                m[0] = m[1] = m[2] = 1, m[7] = 1, m[8] = 0, m[9] = y[k] - by, m[11] = 1;
            } else {         /* a 3x4 translation: world at (bx, y), its inverse at (-bx, -y) */
                float sg = sec == 0 ? 1.0f : -1.0f;
                m[0] = m[5] = m[10] = 1, m[3] = sg * bx, m[7] = sg * y[k];
            }
            put(&o, m, sizeof m);
        }
        pad(&o, 0x20);
    }
    fi[8] = fi[9] = fi[10] = MARK();
    fi[2] = MARK();
    put(&o, hash, 4 * (size_t)nn), pad(&o, 4);
    fi[3] = MARK();
    put(&o, parent, 2 * (size_t)nn), pad(&o, 4);
    fi[4] = MARK();
    put(&o, order, 2 * (size_t)nn), pad(&o, 4);
    fi[5] = MARK();
    put(&o, flag, nn), pad(&o, 4);
    fi[6] = MARK();
    put(&o, each, 4 * (size_t)nn), pad(&o, 4);
    fi[7] = MARK();
    size_t so = al(2 * (size_t)nn, 4), at = so;
    for (unsigned i = 0; i < nn; i++) {
        uint16_t v = (uint16_t)at;
        put(&o, &v, 2);
        at += strlen(name[i]) + 1;
    }
    put(&o, NULL, so - 2 * (size_t)nn);
    for (unsigned i = 0; i < nn; i++) put(&o, name[i], strlen(name[i]) + 1);
    put(&o, NULL, slack);
    size_t new_size = o.n - 0x40;
    put(&o, NULL, pad_end);
#undef MARK
    if (o.bad || at > 0xFFFF) {
        free(o.b);
        return NULL;
    }
    put32(o.b + 0xc, (uint32_t)new_size);
    put16(o.b + 0x20, (uint16_t)nn);
    for (int i = 0; i < 11; i++) put16(o.b + 0x24 + 2 * i, (uint16_t)((fi[i] - 0x40) / 4));
    *out_n = o.n;
    return o.b;
}

uint8_t *diaryrows_model(const uint8_t *b, size_t n, size_t *out_n) {
    if (n < 0x48 || memcmp(b, "G4PK", 4)) return NULL;
    uint32_t parts = get32(b + 0x20);
    if (parts == 0 || parts > 16 || 0x40 + 8 * (size_t)parts > n) return NULL;
    size_t off[16], len[16];
    for (uint32_t i = 0; i < parts; i++) {
        off[i] = 0x40 + 4 * (size_t)get32(b + 0x40 + 4 * i), len[i] = get32(b + 0x40 + 4 * (parts + i));
        if (off[i] > n || len[i] > n - off[i]) return NULL;
    }
    size_t sk_n = 0;
    uint8_t *sk = skeleton(b + off[0], len[0], &sk_n);
    if (!sk) return NULL;
    Buf o = {0};
    put(&o, b, off[0]);  /* the header and its tables (rewritten below) */
    for (uint32_t i = 0; i < parts; i++) {
        if (i) pad(&o, 0x20);
        off[i] = o.n, len[i] = i ? len[i] : sk_n;
        put(&o, i ? b + 0x40 + 4 * (size_t)get32(b + 0x40 + 4 * i) : sk, len[i]);
    }
    pad(&o, 0x20);
    free(sk);
    if (o.bad) {
        free(o.b);
        return NULL;
    }
    for (uint32_t i = 0; i < parts; i++) {
        put32(o.b + 0x40 + 4 * i, (uint32_t)((off[i] - 0x40) / 4));
        put32(o.b + 0x40 + 4 * (parts + i), (uint32_t)len[i]);
    }
    put32(o.b + 0xc, (uint32_t)(o.n - 0x40));
    *out_n = o.n;
    return o.b;
}

/* ---- Level-5 cfg.bin (the locator object and the menu setting): u32 entry count, string table offset, its size,
 * its string count; entries (u32 name crc, u8 value count, 2-bit value types four to a byte padded to 4, u32
 * values); 0xFF up to 16; the strings; the names' table. Only int values change here, and entries are copied, so the
 * strings stay as they are. */

typedef struct {
    size_t at, end, vals;  /* the entry's start, end, and its first value */
    uint32_t crc;
    unsigned k;
} Ent;

static int entries(const uint8_t *b, size_t n, Ent *e, int max) {
    if (n < 16) return -1;
    uint32_t count = get32(b), st = get32(b + 4);
    if (st > n || (int)count > max) return -1;
    size_t p = 16;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 5 > st) return -1;
        e[i].at = p, e[i].crc = get32(b + p), e[i].k = b[p + 4];
        p = al(p + 5 + (e[i].k + 3) / 4, 4);
        e[i].vals = p;
        p += 4 * (size_t)e[i].k;
        if (p > st) return -1;
        e[i].end = p;
    }
    return (int)count;
}

uint8_t *diaryrows_locator(const uint8_t *b, size_t n, size_t *out_n) {
    static Ent e[256];
    int ne = entries(b, n, e, 256);
    if (ne <= 0) return NULL;
    /* PROP_PARAM 'GlobalLayerName', id, bone, child layer, child copy, child's root bone */
    uint32_t pp = crc32_str("PROP_PARAM");
    int line[2], nl = 0;
    for (int i = 0; i < ne; i++)
        if (e[i].crc == pp && e[i].k == 6 && get32(b + e[i].vals + 12) == SLOT_LAYER) {
            if (nl == 2) return NULL;
            line[nl++] = i;
        }
    if (nl != 2 || get32(b + e[line[0]].vals + 16) != 0 || get32(b + e[line[1]].vals + 16) != 1) return NULL;
    const Ent *t = &e[line[1]];
    size_t tl = t->end - t->at, add = (size_t)(DIARY_ROW_COPIES - 2) * tl;
    uint32_t st = get32(b + 4);
    size_t ents_end = e[ne - 1].end, new_end = ents_end + add, padn = al(new_end, 16) - new_end;
    Buf o = {0};
    put(&o, b, t->end);
    for (int c = 2; c < DIARY_ROW_COPIES; c++) put(&o, b + t->at, tl);
    put(&o, b + t->end, ents_end - t->end);
    for (size_t i = 0; i < padn; i++) put(&o, "\xff", 1);
    put(&o, b + st, n - st);
    if (o.bad) {
        free(o.b);
        return NULL;
    }
    char nm[32];
    for (int c = 0; c < DIARY_ROW_COPIES; c++) {
        size_t v = c < 2 ? e[line[c]].vals : t->end + (size_t)(c - 2) * tl + (t->vals - t->at);
        bone_name(nm, c);
        put32(o.b + v + 8, crc32_str(nm));
        if (c >= 2) {
            char id[32];
            snprintf(id, sizeof id, "diary_rows_attach%02d", c);
            put32(o.b + v + 4, crc32_str(id));
            put32(o.b + v + 16, (uint32_t)c);
        }
    }
    put32(o.b, (uint32_t)ne + DIARY_ROW_COPIES - 2);
    put32(o.b + 4, (uint32_t)(new_end + padn));
    *out_n = o.n;
    return o.b;
}

uint8_t *diaryrows_setting(const uint8_t *b, size_t n, size_t *out_n) {
    static Ent e[256];
    int ne = entries(b, n, e, 256);
    if (ne <= 0) return NULL;
    /* MENU_LAYER_INFO layer crc, name, object, copies, ... */
    uint32_t li = crc32_str("MENU_LAYER_INFO");
    for (int i = 0; i < ne; i++) {
        if (e[i].crc != li || e[i].k < 4 || get32(b + e[i].vals) != SLOT_LAYER) continue;
        if (get32(b + e[i].vals + 12) != 2) return NULL;
        uint8_t *o = malloc(n);
        if (!o) return NULL;
        memcpy(o, b, n);
        put32(o + e[i].vals + 12, DIARY_ROW_COPIES);
        *out_n = n;
        return o;
    }
    return NULL;
}

/* ---- the in-game Save's hint: its system text line (crc 0xcdcfb802, the diary_save script's guide) gets the D-pad's
 * left / right icon (the game's help text: "<CMD_CHANGE_BACK1>, <CMD_CHANGE_NEXT1>, or [$gaiji_key_all_s100] to
 * switch screens") and "Change Slot" before it. TEXT_INFO id, variant, text. The new text goes at the end of the
 * string table (0xFF to 16 after it, then the names' table, as before). */

#define SAVE_HINT_ID 0xcdcfb802u
static const char HINT_EN[] = "<CMD_ENTER|5>[CY2]Save[C]   <CMD_BACK>Back";
/* D-pad left and right as two icons, not the pair's one (gaiji_key_all_s100): on a keyboard the runtime shows each
 * one's key, and the pair (a group) would keep the Switch icon (bindings.c gaiji_swap) */
static const char HINT_NEW[] = "[$gaiji_key_lt_s000][$gaiji_key_rt_s000]Change Slot   <CMD_ENTER|5>[CY2]Save[C]   <CMD_BACK>Back";

/* ---- the title's Continue (the diary_load script, its only user of text 0xbdaf66c1): "Select / Back" gets X's
 * Delete before it (owner, 2026-11-17), the X button's icon (gaiji_bt01: the north button, Switch X; on a keyboard
 * the runtime shows its key, bindings.c gaiji_swap). X deletes there only (savefs.c). */
#define LOAD_HINT_ID 0xbdaf66c1u
static const char LOAD_HINT_EN[] = "<CMD_ENTER>Select   <CMD_BACK>Back";
static const char LOAD_HINT_NEW[] = "[$gaiji_bt01_s000]Delete   <CMD_ENTER>Select   <CMD_BACK>Back";

/* One text line (TEXT_INFO id, its English text) given new text, put at the end of the string table; NULL when the
 * line isn't there as expected (another language: left as it is). */
static uint8_t *hint_replace(const uint8_t *b, size_t n, size_t *out_n, uint32_t id, const char *en, const char *text) {
    static Ent e[4096];
    int ne = entries(b, n, e, 4096);
    if (ne <= 0) return NULL;
    uint32_t st = get32(b + 4), sl = get32(b + 8), ns = get32(b + 12);
    if ((uint64_t)st + sl > n) return NULL;
    uint32_t ti = crc32_str("TEXT_INFO");
    for (int i = 0; i < ne; i++) {
        if (e[i].crc != ti || e[i].k < 3 || get32(b + e[i].vals) != id) continue;
        uint32_t so = get32(b + e[i].vals + 8);
        if (so >= sl || !memchr(b + st + so, 0, sl - so) || strcmp((const char *)b + st + so, en)) return NULL;
        size_t names = al(st + (size_t)sl, 16), add = strlen(text) + 1;
        if (names > n) return NULL;
        Buf o = {0};
        put(&o, b, st + (size_t)sl);
        put(&o, text, add);
        while (o.n % 16) put(&o, "\xff", 1);
        put(&o, b + names, n - names);
        if (o.bad) {
            free(o.b);
            return NULL;
        }
        put32(o.b + e[i].vals + 8, sl);
        put32(o.b + 8, sl + (uint32_t)add);
        put32(o.b + 12, ns + 1);
        *out_n = o.n;
        return o.b;
    }
    return NULL;
}

/* The in-game Save's hint and the title's Continue hint, each where it is the English line; NULL when neither is. */
uint8_t *diaryrows_save_hint(const uint8_t *b, size_t n, size_t *out_n) {
    size_t n1 = 0, n2 = 0;
    uint8_t *one = hint_replace(b, n, &n1, SAVE_HINT_ID, HINT_EN, HINT_NEW);
    uint8_t *two = hint_replace(one ? one : b, one ? n1 : n, &n2, LOAD_HINT_ID, LOAD_HINT_EN, LOAD_HINT_NEW);
    if (two) {
        free(one);
        *out_n = n2;
        return two;
    }
    if (one) *out_n = n1;
    return one;
}
