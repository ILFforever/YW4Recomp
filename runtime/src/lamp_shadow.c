/* Lamp shadows (decision 0081; lamp_shadow.h has the outline). This file: the lamps, which of them get shadows, each
 * one's six views, the characters' draws copied into them, and the table the lamp program reads. */
#include "yw4rt.h"
#include "lamp_shadow.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"
#include "gpu_internal.h"
#include "host.h"

uint32_t lamp_shadow_size(void) {
    static uint32_t size;
    if (!size) {
        const char *e = getenv("YW4_LAMP_SHADOW_SIZE");
        uint32_t v = e && *e ? (uint32_t)atoi(e) : 512;
        size = v < 256 ? 256 : v > 2048 ? 2048 : v;
    }
    return size;
}

/* ---- the lamps of the last lamp draw (executor), read by the render thread */

enum { MAX_LAMPS = 256 };
typedef struct {
    float pos[3], radius, colour[3];
    int still;  /* at the same place in the lamp draw before: cars carry lamps too, and a moving one never matches */
} Lamp;

static HostLock lamps_lock = HOST_LOCK_INIT;
static Lamp lamps[MAX_LAMPS];
static int nlamps;

/* Every vertex of a lamp's volume carries its world position and radius (vertex program 198f41, attribute 2) and
 * its colour (attribute 1), as floats; one draw holds all of a frame's lamps (busy street: ~500 vertices, ~25 lamps). */
void lamp_shadow_collect(const uint8_t *vertices, uint32_t count, uint32_t stride) {
    Lamp found[MAX_LAMPS];
    int n = 0;
    for (uint32_t v = 0; v < count && n < MAX_LAMPS; v++) {
        const float *c = (const float *)(const void *)(vertices + (uint64_t)v * stride + 16);
        const float *p = (const float *)(const void *)(vertices + (uint64_t)v * stride + 32);
        if (!(p[3] > 0.0f) || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) continue;
        int k = 0;
        while (k < n && !(found[k].pos[0] == p[0] && found[k].pos[1] == p[1] && found[k].pos[2] == p[2] &&
                          found[k].radius == p[3]))
            k++;
        if (k < n) continue;
        found[n] = (Lamp){{p[0], p[1], p[2]}, p[3], {c[0], c[1], c[2]}, 0};
        n++;
    }
    host_lock(&lamps_lock);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < nlamps && !found[i].still; j++)
            found[i].still = found[i].pos[0] == lamps[j].pos[0] && found[i].pos[1] == lamps[j].pos[1] &&
                             found[i].pos[2] == lamps[j].pos[2];
    memcpy(lamps, found, sizeof *found * (size_t)n);
    nlamps = n;
    host_unlock(&lamps_lock);
}

/* ---- the chosen lamps' views (render thread) */

/* A view of the cube: looking along `fwd` from the lamp, `up` above; 90 degrees and a little more, so that the soft
 * filter's taps near a view's edge stay inside it (the program picks the view by the largest axis, so a point is
 * never more than 45 degrees off its view's middle). Vulkan clip space: depth 0 at `near`, 1 at the lamp's reach. */
static void face_matrix(const float l[3], float radius, int face, float out[16]) {
    static const float fwd[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const float upv[6][3] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    const float *f = fwd[face], *u = upv[face];
    float r[3] = {u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]};
    const float near = 0.05f, far = radius > 0.2f ? radius : 0.2f, t = 1.05f;  /* tan(half angle): ~92.7 degrees */
    float a = far / (far - near), b = -far * near / (far - near);
    const float *rows[3] = {r, u, f};
    float v[3][4];
    for (int i = 0; i < 3; i++)
        v[i][0] = rows[i][0], v[i][1] = rows[i][1], v[i][2] = rows[i][2],
        v[i][3] = -(rows[i][0] * l[0] + rows[i][1] * l[1] + rows[i][2] * l[2]);
    for (int j = 0; j < 4; j++) {
        out[0 * 4 + j] = v[0][j] / t;
        out[1 * 4 + j] = v[1][j] / t;
        out[2 * 4 + j] = a * v[2][j] + (j == 3 ? b : 0.0f);
        out[3 * 4 + j] = v[2][j];
    }
}

static void mul4(const float *a, const float *b, float *r) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
}

/* The box (local space) through mvp is entirely on the far side of one clip plane (as shadow.c's `outside`). A box with
 * min > max is never left out. */
static int outside(const float lo[3], const float hi[3], const float *mvp) {
    if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) return 0;
    unsigned all = 0x3F;
    for (int c = 0; c < 8; c++) {
        float v[3] = {c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2]}, p[4];
        for (int i = 0; i < 4; i++) p[i] = mvp[i * 4] * v[0] + mvp[i * 4 + 1] * v[1] + mvp[i * 4 + 2] * v[2] + mvp[i * 4 + 3];
        unsigned out = (p[0] < -p[3]) | (p[0] > p[3]) << 1 | (p[1] < -p[3]) << 2 | (p[1] > p[3]) << 3 | (p[3] <= 0) << 4 |
                       (p[2] > p[3]) << 5;
        all &= out;
        if (!all) return 0;
    }
    return 1;
}

enum { RING = 16 };  /* passes in flight: reused 16 frames later, long after the GPU ran them */
static LampShadowPass *building;
/* Debugging (blinks): for the pass being built, per lamp, the characters (by begin's list) within its reach, the
 * submeshes given that reached it, and those every view left out; submeshes the shadow pass couldn't copy; the objects
 * given; and characters met only in a farther cascade's layer since (seen in the next pass's log). */
static struct {
    int chars_near[LAMP_SHADOW_LAMPS], given_near[LAMP_SHADOW_LAMPS], culled[LAMP_SHADOW_LAMPS];
    int given, skipped[2], nchars;
    uint64_t objects[64];
    int nobjects, far_new;
    float lamp[LAMP_SHADOW_LAMPS][3];
    uint32_t draws[LAMP_SHADOW_LAMPS];
    int nlamps;
} dbg, dbg_last;

void lamp_shadow_note_skip(int why) {
    if (building && why >= 0 && why < 2) dbg.skipped[why]++;
}

void lamp_shadow_note_far(uint64_t object) {
    for (int i = 0; i < dbg_last.nobjects; i++)
        if (dbg_last.objects[i] == object) return;
    dbg_last.far_new++;
}
static long stat_passes, stat_lamps, stat_draws, stat_dropped, stat_changes, stat_min_draws = -1, stat_max_draws;
int lamp_shadow_open(void) { return building != NULL; }

/* (object, submesh) pairs given to the pass being built: a character drawn in more than one
 * cascade or layer is copied once */
enum { GIVEN = 1024 };
static uint64_t given[GIVEN][2];
static int ngiven;

/* The lamps with shadows (render thread), each with how far its shadows have faded in (0..1). Shadows fade in over
 * FADE passes when a lamp is chosen and out before it is dropped: with ~100 lamps along a street and a few chosen,
 * walking changes which ones, and a lamp's shadows that came and went at once looked like blinking (owner,
 * 2026-11-19: "it only blinks while moving"). */
typedef struct {
    Lamp l;
    float fade;
    int leaving;  /* fading out to make room for a nearer lamp, even while it still qualifies */
    int absent;   /* passes it has been missing from the game's lamp draw */
} Chosen;
static Chosen chosen[LAMP_SHADOW_LAMPS];
static int nchosen;
enum { FADE = 8, ABSENT = 30 };

static int same_pos(const float a[3], const float b[3]) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

/* How near a lamp is to the nearest character, relative to its reach (below 1: it lights one); 1e30 if it can't have
 * shadows (off, as every lamp by day, or moving). */
static float lamp_score(const Lamp *l, const float *characters, int ncharacters) {
    if (l->colour[0] + l->colour[1] + l->colour[2] <= 1e-4f || !l->still) return 1e30f;
    float best = 1e30f;
    for (int c = 0; c < ncharacters; c++) {
        const float *p = characters + 3 * c;
        float dx = p[0] - l->pos[0], dy = p[1] + 0.6f - l->pos[1], dz = p[2] - l->pos[2];  /* the body's middle */
        float d = sqrtf(dx * dx + dy * dy + dz * dz) / l->radius;
        if (d < best) best = d;
    }
    return best;
}

static void log_leave(const Chosen *c, const char *why) {
    static int logged;
    if (logged++ < 20)
        fprintf(stderr, "yw4: lamp shadows: lamp (%.1f %.1f %.1f) dropped at GPU frame %" PRIu64 ": %s%s\n", c->l.pos[0],
                c->l.pos[1], c->l.pos[2], gpu_frames(), why, logged == 20 ? " (the last one logged)" : "");
}

/* Objects present in the layer that began the pass being built: a character missing from it (below, carry) is only
 * carried over while its object is still there. */
enum { PRESENT = 512 };
static const LampShadowPass *last_pass;  /* the pass before (NULL: the last frame had none: nothing to carry over) */
static uint64_t present_obj[PRESENT];
static int npresent_obj;

int lamp_shadow_begin(const float *characters, int ncharacters, const uint64_t *present, int npresent) {
    building = NULL;
    npresent_obj = npresent < PRESENT ? npresent : PRESENT;
    memcpy(present_obj, present, sizeof *present * (size_t)npresent_obj);
    if (!gpu_lamp_shadows()) {
        nchosen = 0, last_pass = NULL;
        return 0;
    }
    Lamp all[MAX_LAMPS];
    host_lock(&lamps_lock);
    int n = nlamps;
    memcpy(all, lamps, sizeof *all * (size_t)n);
    host_unlock(&lamps_lock);
    float score[MAX_LAMPS];
    for (int i = 0; i < n; i++) score[i] = lamp_score(&all[i], characters, ncharacters);
    /* the chosen ones fade toward 1 while they still light a character (and aren't leaving), else toward 0, and go at
     * 0; at once if the lamp draw no longer has them (gone, moved) or they are off */
    int changed = 0, kept = 0;
    float worst = -1;  /* the farthest chosen one that stays */
    int worst_k = -1;
    for (int k = 0; k < nchosen; k++) {
        Chosen c = chosen[k];
        int i = 0;
        while (i < n && !same_pos(all[i].pos, c.l.pos)) i++;
        /* missing from the lamp draw: the game draws only the lamps whose volume is in view, and a lamp whose volume
         * slips out of view for a frame came back faded out (a blink). Kept as it is for ABSENT passes: it lights
         * nothing on screen meanwhile, and the program finds no lamp of its own at that place. */
        if (i == n && ++c.absent <= ABSENT) {
            chosen[kept++] = c;
            continue;
        }
        if (i == n || score[i] >= 1e29f) {
            log_leave(&c, i == n ? "missing from the game's lamp draw for a while" : "turned off or moving");
            changed = 1;
            continue;
        }
        c.l = all[i], c.absent = 0;
        int stays = score[i] < 1.0f && !c.leaving;
        c.fade += stays ? 1.0f / FADE : -1.0f / FADE;
        if (c.fade > 1.0f) c.fade = 1.0f;
        if (c.fade <= 0.0f) {
            log_leave(&c, c.leaving ? "faded out for a nearer lamp" : "faded out: no character in its reach");
            changed = 1;
            continue;
        }
        if (stays && score[i] > worst) worst = score[i], worst_k = kept;
        chosen[kept++] = c;
    }
    nchosen = kept;
    /* newcomers, nearest first: into a free place; with none free, the farthest staying one starts leaving if the
     * newcomer is clearly nearer (by a third), and the newcomer comes once it has gone */
    for (;;) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (score[i] >= 1.0f) continue;
            int have = 0;
            for (int k = 0; k < nchosen && !have; k++) have = same_pos(chosen[k].l.pos, all[i].pos);
            if (!have && (best < 0 || score[i] < score[best])) best = i;
        }
        if (best < 0) break;
        if (nchosen < LAMP_SHADOW_LAMPS) {
            chosen[nchosen++] = (Chosen){all[best], 1.0f / FADE, 0, 0};
            changed = 1;
            score[best] = 1e30f;  /* taken: on to the next */
            continue;
        }
        if (worst_k >= 0 && score[best] < worst * 0.67f) chosen[worst_k].leaving = 1;
        break;
    }
    stat_changes += changed;
    if (!nchosen) {
        last_pass = NULL;
        return 0;
    }
    static LampShadowPass *ring[RING];
    static unsigned ring_next;
    LampShadowPass **p = &ring[ring_next++ % RING];
    if (!*p && !(*p = malloc(sizeof **p))) return 0;
    LampShadowPass *s = *p;
    s->lamps = (uint32_t)nchosen, s->n = 0;
    for (int k = 0; k < nchosen; k++) {
        const Lamp *l = &chosen[k].l;
        s->lamp[k][0] = l->pos[0], s->lamp[k][1] = l->pos[1], s->lamp[k][2] = l->pos[2], s->lamp[k][3] = l->radius;
        s->fade[k] = chosen[k].fade;
        for (int f = 0; f < LAMP_SHADOW_FACES; f++) face_matrix(l->pos, l->radius, f, s->face_vp[k * LAMP_SHADOW_FACES + f]);
    }
    building = s;
    stat_passes++, stat_lamps += nchosen;
    ngiven = 0;
    memset(&dbg, 0, sizeof dbg);
    dbg.nchars = ncharacters, dbg.nlamps = nchosen;
    for (int k = 0; k < nchosen; k++) {
        memcpy(dbg.lamp[k], s->lamp[k], sizeof dbg.lamp[k]);
        for (int c = 0; c < ncharacters; c++) {
            const float *q = characters + 3 * c;
            float dx = q[0] - s->lamp[k][0], dy = q[1] - s->lamp[k][1], dz = q[2] - s->lamp[k][2], r = s->lamp[k][3] + 2.0f;
            dbg.chars_near[k] += dx * dx + dy * dy + dz * dz <= r * r;
        }
    }
    return 1;
}

void lamp_shadow_add(const ShadowDraw *d, const float world[16], const float box_lo[3], const float box_hi[3],
                     uint64_t object, uint64_t submesh) {
    LampShadowPass *s = building;
    if (!s) return;
    for (int i = 0; i < ngiven; i++)
        if (given[i][0] == object && given[i][1] == submesh) return;
    if (ngiven < GIVEN) given[ngiven][0] = object, given[ngiven][1] = submesh, ngiven++;
    dbg.given++;
    int known = 0;
    for (int i = 0; i < dbg.nobjects && !known; i++) known = dbg.objects[i] == object;
    if (!known && dbg.nobjects < 64) dbg.objects[dbg.nobjects++] = object;
    const float at[3] = {world[3], world[7], world[11]};
    for (uint32_t k = 0; k < s->lamps; k++) {
        float dx = at[0] - s->lamp[k][0], dy = at[1] - s->lamp[k][1], dz = at[2] - s->lamp[k][2];
        float reach = s->lamp[k][3] + 2.0f;  /* the object's origin is at its feet: its body reaches about 2 m further */
        if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
        dbg.given_near[k]++;
        int seen_by = 0;
        for (int f = 0; f < LAMP_SHADOW_FACES; f++) {
            float mvp[16];
            mul4(s->face_vp[k * LAMP_SHADOW_FACES + f], world, mvp);
            if (outside(box_lo, box_hi, mvp)) continue;
            seen_by++;
            if (s->n == LAMP_SHADOW_DRAWS) {
                stat_dropped++;
                return;
            }
            LampShadowDraw *o = &s->d[s->n++];
            o->d = *d;
            memcpy(o->d.mvp, mvp, sizeof o->d.mvp);
            o->d.cond_mode = 0, o->d.cond_addr = 0;  /* the sun view's occlusion test says nothing about the lamp's */
            o->layer = k * LAMP_SHADOW_FACES + (uint32_t)f;
            o->carried = 0, o->object = object;
            stat_draws++;
            dbg.draws[k]++;
        }
        dbg.culled[k] += !seen_by;
    }
}

static long stale_fills, fills, blips, queued_late;  /* lamp draws with lamp shadows on: with an older pass, in all; blips (executor) */

/* A lamp that had characters drawn into its views last pass and none now, while still chosen: why (the first 30). */
static void report_empty(void) {
    static int logged;
    for (int k = 0; k < dbg.nlamps && logged < 30; k++) {
        if (dbg.draws[k]) continue;
        int j = 0;
        while (j < dbg_last.nlamps && memcmp(dbg_last.lamp[j], dbg.lamp[k], sizeof dbg.lamp[k])) j++;
        if (j == dbg_last.nlamps || !dbg_last.draws[j]) continue;
        logged++;
        fprintf(stderr, "yw4: lamp shadows: EMPTY at GPU frame %" PRIu64 ": lamp (%.1f %.1f %.1f) had %u draws last pass, "
                "none now: characters in its reach %d (last pass %d), their submeshes given %d (last %d), left out by "
                "every view %d; all submeshes given %d (last %d), not copied %d+%d; characters %d (last %d); characters "
                "met only in farther cascades since the last pass %d\n", gpu_frames(), dbg.lamp[k][0], dbg.lamp[k][1],
                dbg.lamp[k][2], dbg_last.draws[j], dbg.chars_near[k], dbg_last.chars_near[j], dbg.given_near[k],
                dbg_last.given_near[j], dbg.culled[k], dbg.given, dbg_last.given, dbg.skipped[0], dbg.skipped[1], dbg.nchars,
                dbg_last.nchars, dbg_last.far_new);
    }
}

void lamp_shadow_note_late(void) { queued_late++; }

/* A character in the last pass, still in the game's list, but given no draws this time: its draws from the last pass
 * again, for up to CARRY passes (for each lamp still chosen; the same lamp's views have the same matrices). Now and
 * then our shadow pass doesn't count a character as its own for a frame (its check against the game's draw, one frame
 * in 32 per object, can fail mid-animation; the game draws it then), and its lamp shadows went for that frame: a
 * blink (2026-11-19, the EMPTY log lines: "characters 1 (last 2)"). A frame-old pose in a soft shadow doesn't show. */
enum { CARRY = 3 };
static long stat_carried;

static void carry(LampShadowPass *s) {
    const LampShadowPass *p = last_pass;
    if (!p) return;
    for (uint32_t i = 0; i < p->n && s->n < LAMP_SHADOW_DRAWS; i++) {
        const LampShadowDraw *o = &p->d[i];
        if (o->carried >= CARRY) continue;
        int there = 0, drawn = 0;
        for (int j = 0; j < npresent_obj && !there; j++) there = present_obj[j] == o->object;
        for (int j = 0; j < ngiven && !drawn; j++) drawn = given[j][0] == o->object;
        if (!there || drawn) continue;
        const float *pos = p->lamp[o->layer / LAMP_SHADOW_FACES];
        uint32_t k = 0;
        while (k < s->lamps && memcmp(s->lamp[k], pos, 3 * sizeof(float))) k++;
        if (k == s->lamps) continue;
        LampShadowDraw *c = &s->d[s->n++];
        *c = *o;
        c->layer = k * LAMP_SHADOW_FACES + o->layer % LAMP_SHADOW_FACES;
        c->carried = o->carried + 1;
        dbg.draws[k]++;
        stat_carried++;
    }
}

LampShadowPass *lamp_shadow_end(void) {
    LampShadowPass *s = building;
    building = NULL;
    if (s) {
        carry(s);
        last_pass = s;
        report_empty();
        dbg_last = dbg;
    }
    if (s) {
        if (stat_min_draws < 0 || (long)s->n < stat_min_draws) stat_min_draws = (long)s->n;
        if ((long)s->n > stat_max_draws) stat_max_draws = (long)s->n;
    }
    static long reported;
    if (stat_passes >= reported + 600) {  /* the log, every 600 passes */
        reported = stat_passes;
        fprintf(stderr, "yw4: lamp shadows: %ld passes: %.2f lamps and %.1f draws each on average (%ld-%ld), the lamps "
                "changed %ld times; lamp draws with an older pass %ld of %ld, blips %ld; passes queued late %ld; draws carried over %ld%s\n",
                stat_passes,
                (double)stat_lamps / (double)stat_passes, (double)stat_draws / (double)stat_passes, stat_min_draws,
                stat_max_draws, stat_changes, __atomic_load_n(&stale_fills, __ATOMIC_RELAXED),
                __atomic_load_n(&fills, __ATOMIC_RELAXED), __atomic_load_n(&blips, __ATOMIC_RELAXED), queued_late, stat_carried, stat_dropped ? " (some draws dropped: the pass was full)" : "");
        stat_min_draws = -1, stat_max_draws = 0;
    }
    return s;
}

/* ---- the table for the lamp program (executor) */

static LampShadowPass shown;  /* lamps and views of the pass drawn last (only those fields) */
static uint64_t shown_frame = UINT64_MAX;

static uint32_t shown_draws[LAMP_SHADOW_LAMPS];  /* draws into each lamp's views in the pass drawn last */

void lamp_shadow_drawn(const LampShadowPass *p) {
    memset(shown_draws, 0, sizeof shown_draws);
    for (uint32_t i = 0; i < p->n; i++) shown_draws[p->d[i].layer / LAMP_SHADOW_FACES]++;
    shown.lamps = p->lamps;
    memcpy(shown.lamp, p->lamp, sizeof shown.lamp);
    memcpy(shown.fade, p->fade, sizeof shown.fade);
    memcpy(shown.face_vp, p->face_vp, sizeof shown.face_vp);
    shown_frame = gpu_frames();
}

/* ---- blips (owner, 2026-11-19: "if a shadow was being rendered at 100% then it blips to 0 then back it's probably a
 * bug"): per GPU frame, at its first lamp draw, which lamps had shadows (in the pass the program read, with characters
 * drawn into their views). A lamp with them in frames n-2 and n but not in n-1 is a blip, logged with why (the first
 * 20; all counted in the summary line). YW4_LAMP_SHADOW_LOG=1: every frame's state too. */
typedef struct {
    uint64_t frame;
    int live, n;
    float pos[LAMP_SHADOW_LAMPS][3];
    uint32_t draws[LAMP_SHADOW_LAMPS];
    float fade[LAMP_SHADOW_LAMPS];
    uint64_t pass_frame;
} Seen;
static Seen seen[3];  /* the last three frames, newest first */

static int seen_lamp(const Seen *f, const float pos[3]) {  /* its index there, -1 if not there */
    for (int k = 0; k < f->n; k++)
        if (f->pos[k][0] == pos[0] && f->pos[k][1] == pos[1] && f->pos[k][2] == pos[2]) return k;
    return -1;
}
static int shadowed(const Seen *f, const float pos[3]) {
    int k = seen_lamp(f, pos);
    return f->live && k >= 0 && f->draws[k] > 0 && f->fade[k] > 0.0f;
}

static void watch_frame(uint64_t now, int live) {
    if (seen[0].frame == now && seen[0].n) return;  /* this frame's first lamp draw only */
    seen[2] = seen[1], seen[1] = seen[0];
    Seen *f = &seen[0];
    memset(f, 0, sizeof *f);
    f->frame = now, f->live = live, f->pass_frame = shown_frame, f->n = (int)shown.lamps;
    for (int k = 0; k < f->n; k++)
        memcpy(f->pos[k], shown.lamp[k], sizeof f->pos[k]), f->draws[k] = shown_draws[k], f->fade[k] = shown.fade[k];
    static int every = -1;
    if (every < 0) every = getenv("YW4_LAMP_SHADOW_LOG") && atoi(getenv("YW4_LAMP_SHADOW_LOG")) > 0;
    if (every) {
        fprintf(stderr, "yw4: lamp shadows: frame %" PRIu64 ": %s (pass of frame %" PRIu64 "), lamps:", now,
                live ? "shadows" : "none", shown_frame);
        for (int k = 0; k < f->n; k++)
            fprintf(stderr, " (%.1f %.1f %.1f) %u draws fade %.2f", f->pos[k][0], f->pos[k][1], f->pos[k][2], f->draws[k],
                    f->fade[k]);
        fprintf(stderr, "\n");
    }
    const Seen *a = &seen[2], *b = &seen[1];
    if (!a->frame || b->frame != a->frame + 1 || f->frame != b->frame + 1) return;  /* three frames in a row */
    for (int k = 0; k < a->n; k++) {
        if (!shadowed(a, a->pos[k]) || !shadowed(f, a->pos[k]) || shadowed(b, a->pos[k])) continue;
        long n = __atomic_add_fetch(&blips, 1, __ATOMIC_RELAXED);
        if (n > 20) continue;
        int kb = seen_lamp(b, a->pos[k]);
        const char *why = !b->live ? "no recent pass reached the lamp draw"
                          : kb < 0 ? "the lamp was not chosen in that pass"
                                   : "no character was drawn into its views";
        fprintf(stderr, "yw4: lamp shadows: BLIP at GPU frame %" PRIu64 ": lamp (%.1f %.1f %.1f) had shadows in frames %"
                PRIu64 " and %" PRIu64 " (%u, %u draws) but not in between: %s (that frame read the pass of frame %"
                PRIu64 ")%s\n", b->frame, a->pos[k][0], a->pos[k][1], a->pos[k][2], a->frame, f->frame, a->draws[k],
                f->draws[seen_lamp(f, a->pos[k])], why, b->pass_frame, n == 20 ? " (the last one logged)" : "");
    }
}

/* The inverse of the camera's view (3 rows, world to view, then 0 0 0 1): a rotation and a move, so the rotation's
 * transpose and the move turned back (row-major). */
static void view_inverse(const float *v, float out[16]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) out[i * 4 + j] = v[j * 4 + i];
    for (int i = 0; i < 3; i++)
        out[i * 4 + 3] = -(out[i * 4 + 0] * v[3] + out[i * 4 + 1] * v[7] + out[i * 4 + 2] * v[11]);
    out[12] = out[13] = out[14] = 0, out[15] = 1;
}

void lamp_shadow_fill(LampShadowTable *t, const float *camera_rows) {
    memset(t, 0, sizeof *t);
    /* the pass drawn last: the images hold what it drew, so its table is the one to read. Not one older than a few
     * frames: none has been drawn since (lamp shadows off, no character near a lit lamp). (Only this frame's, at first:
     * the pass and the lamp draw are not always in the same GPU frame, and the shadows went for a frame: blinking.) */
    uint64_t now = gpu_frames();
    int live = gpu_lamp_shadows() && shown_frame != UINT64_MAX && now - shown_frame <= 4;
    if (gpu_lamp_shadows()) {
        __atomic_add_fetch(&fills, 1, __ATOMIC_RELAXED);
        if (now != shown_frame) __atomic_add_fetch(&stale_fills, 1, __ATOMIC_RELAXED);
        watch_frame(now, live);
    }
    t->info[0] = live ? (float)shown.lamps : 0.0f;
    t->info[1] = 1.0f / (float)lamp_shadow_size();
    /* the fade with distance from the caster (lamp_light.frag): YW4_LAMP_SHADOW_FADE=<start>,<end> in metres */
    static float fade[2] = {-1, -1};
    if (fade[0] < 0) {
        fade[0] = 1.0f, fade[1] = 3.0f;  /* owner: 0.3-2 m made them too light */
        const char *e = getenv("YW4_LAMP_SHADOW_FADE");
        if (e) sscanf(e, "%f,%f", &fade[0], &fade[1]);
        if (fade[1] <= fade[0]) fade[1] = fade[0] + 0.01f;
    }
    t->info[2] = fade[0], t->info[3] = fade[1];
    if (camera_rows) view_inverse(camera_rows + 4 * 4, t->view_to_world);
    if (!live) return;
    memcpy(t->lamp, shown.lamp, sizeof t->lamp);
    memcpy(t->fade, shown.fade, sizeof shown.fade);
    memcpy(t->face_vp, shown.face_vp, sizeof t->face_vp);
}
