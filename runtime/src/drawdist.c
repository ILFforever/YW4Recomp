/* DRAW DISTANCE (decision 0085; owner, 2026-11-20: "notoriously bad draw distance and model pop-in without fading:
 * trees, buildings, people just pop in"): the map parts' far clip times 1, 2, 4 or 10, live. It brings in lamps, plants
 * and signs from farther away; trees, buildings and people are on another rule, not found yet.
 *
 * 1. The map is built in pieces around the player. Its parameters (registered in f_5fe120 with the property system):
 *      +0x278 m_radiusToBuild          float  pieces within this are built      (busy town and school: 10)
 *      +0x280 m_heightToBuild          float                                    (town 10, Yo-kai World 50)
 *      +0x288 m_radiusToDestroyOffset  float  destroyed past radius + this      (town 15, Yo-kai World 5)
 *    f_6082e0 is called on that map object every frame (from main+0x857f4c). A walk A/B (2026-11-20, radius 10 vs
 *    40, 30 fps, save_base_walk + walk.txt): 5-8% more draws at 40, the screenshots the same (the extra pieces are
 *    out of view).
 * 2. f_5f3ec0, per map part (from main+0x5e8604), goes over the part's objects (list +0x4b0, 64-byte entries,
 *    position at +0x30, count +0x4b8) and, with m_isEnableClip (+0xa9) on, skips each one whose squared distance
 *    from the camera (+ the float at +0x604) is past m_farClip (+0xa0) squared (or inside m_nearClip, +0x9c, when
 *    that is above 0). Town and school: far 100, near 0, on. Registered in f_5e86e0 with m_colMeshPath,
 *    m_naviMeshPath, m_noOcclusionCulling... A walk A/B at far 100 vs 300: the same draws (477 / 478 at frame 4200).
 *
 * YW4_DRAW_DISTANCE (settings.ini, the row): 1 the game's, 4 (default) or 10 times both. YW4_DRAWDIST_LOG=1: the
 * cull lists' sizes now and then. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yw4rt.h"

/* DRAW DISTANCE (Detail): DEFAULT, MEDIUM (x2, owner: "where is my 2x"), FAR (x4, the default, owner's pick
 * 2026-11-20: "x4 is a good mix, x10 is overkill"), VERY FAR (x10). */
static const float factors[] = {1, 2, 4, 10};
enum { NLEVELS = sizeof factors / sizeof *factors };
static int level = -1;

int drawdist_index(void) {
    /* guest threads read this, the window thread cycles it: compute in a local, store once */
    int l = __atomic_load_n(&level, __ATOMIC_RELAXED);
    if (l < 0) {
        const char *e = getenv("YW4_DRAW_DISTANCE");
        float f = e && *e ? (float)atof(e) : 4;
        l = 0;
        for (int i = 0; i < NLEVELS; i++)
            if (f >= factors[i]) l = i;
        int none = -1;  /* a cycle that got in first wins */
        if (!__atomic_compare_exchange_n(&level, &none, l, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) l = none;
    }
    return l;
}

float drawdist_factor(void) { return factors[drawdist_index()]; }

/* F6 steps it too, for tests only (owner: no F6 in a release) */
int drawdist_key(void) {
    const char *e = getenv("YW4_DRAW_DISTANCE_KEY");
    return e && *e == '1';
}

void drawdist_cycle(void) {
    int l = (drawdist_index() + 1) % NLEVELS;
    __atomic_store_n(&level, l, __ATOMIC_RELAXED);
    fprintf(stderr, "yw4: drawdist: DRAW DISTANCE x%g\n", factors[l]);
}

/* the game's own values per object, so the factor applies to them and not to what was written before */
typedef struct {
    uint64_t obj;
    float orig, set;
} Kept;
static Kept kept[128];

static float scaled(uint64_t obj, uint64_t off, float now) {
    float f = factors[drawdist_index()];
    for (int i = 0; i < 128; i++) {
        if (kept[i].obj == obj + off) {
            if (now != kept[i].set) kept[i].orig = now;  /* the game wrote its own value again (a new map) */
            kept[i].set = kept[i].orig * f;
            return kept[i].set;
        }
        if (!kept[i].obj) {
            kept[i] = (Kept){obj + off, now, now * f};
            return kept[i].set;
        }
    }
    return now * f;  /* table full: once (it would compound, but 128 map parts are not seen) */
}

int yw4_hook_map_update(CPUState *ctx) {
    static int logged;
    uint64_t map = ctx->x[0];
    if (!map) return 0;
    float r, h, d;
    memcpy(&r, (const void *)(uintptr_t)(map + 0x278), sizeof r);
    memcpy(&h, (const void *)(uintptr_t)(map + 0x280), sizeof h);
    memcpy(&d, (const void *)(uintptr_t)(map + 0x288), sizeof d);
    if (!logged) {
        fprintf(stderr, "yw4: drawdist: map %#llx: radius to build %g, height %g, destroy offset %g\n",
                (unsigned long long)map, r, h, d);
        logged = 1;
    }
    /* Not scaled for players: a bigger build radius built a map piece that doesn't belong there (owner, 2026-11-20:
     * a tall building in the middle of a Yo-kai World street at x2, "that building SHOULD NOT BE THERE"), and in the
     * walk test it showed nothing farther. YW4_DRAW_DISTANCE_RADIUS=1 scales it too (tests). */
    const char *er = getenv("YW4_DRAW_DISTANCE_RADIUS");
    if (!er || *er != '1') return 0;
    float want = scaled(map, 0x278, r);
    if (want != r) memcpy((void *)(uintptr_t)(map + 0x278), &want, sizeof want);
    return 0;
}

int yw4_hook_map_clip(CPUState *ctx) {
    static int logged;
    static unsigned calls;
    uint64_t part = ctx->x[0];
    if (!part) return 0;
    float fc, nc;
    uint8_t on;
    memcpy(&fc, (const void *)(uintptr_t)(part + 0xa0), sizeof fc);
    memcpy(&nc, (const void *)(uintptr_t)(part + 0x9c), sizeof nc);
    memcpy(&on, (const void *)(uintptr_t)(part + 0xa9), sizeof on);
    if (getenv("YW4_DRAWDIST_LOG") && ++calls % 600 == 0 && calls <= 600 * 12) {
        uint32_t count;
        memcpy(&count, (const void *)(uintptr_t)(part + 0x4b8), sizeof count);
        fprintf(stderr, "yw4: drawdist: call %u: map part %#llx has %u objects in its cull list (far %g, clip %s)\n",
                calls, (unsigned long long)part, count, fc, on ? "on" : "off");
    }
    if (logged < 4) {
        fprintf(stderr, "yw4: drawdist: map part %#llx: clip %s, near %g, far %g\n", (unsigned long long)part,
                on ? "on" : "off", nc, fc);
        logged++;
    }
    if (fc <= 0) return 0;  /* no far limit: nothing to scale */
    float want = scaled(part, 0xa0, fc);
    if (want != fc) memcpy((void *)(uintptr_t)(part + 0xa0), &want, sizeof want);
    return 0;
}
