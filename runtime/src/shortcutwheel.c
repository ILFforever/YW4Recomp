/* The shortcut wheel: see shortcutwheel.h and decision 0075. */
#include "shortcutwheel.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "mousemenu.h"
#include "window.h"
#include "yw4rt.h"

/* the Switch's button bits (bindings.c's inputs[]) */
#define BIT_L (1ull << 6)
#define BIT_LEFT (1ull << 12)
#define BIT_UP (1ull << 13)
#define BIT_RIGHT (1ull << 14)
#define BIT_DOWN (1ull << 15)
#define DPAD (BIT_LEFT | BIT_UP | BIT_RIGHT | BIT_DOWN)

/* The slots round the ring, clockwise from the top (the angle / 45): the D-pad's four on the straight lines as on
 * the pad (1 up, 2 right, 3 down, 4 left), the hold-L set's on the diagonals after each. The game's own names
 * (the English patch's help and tutorial text) and the D-pad direction each presses. */
typedef struct {
    int slot;
    const char *name;
    uint64_t dir;
    int with_l;
} Slot;
static const Slot ring[8] = {
    {1, "Auto-Navi", BIT_UP, 0},       {5, "Change Watchers", BIT_UP, 1},
    {2, "Diary", BIT_RIGHT, 0},        {6, "Mirapo", BIT_RIGHT, 1},
    {3, "Bike", BIT_DOWN, 0},          {7, "Change Yo-kai Companion", BIT_DOWN, 1},
    {4, "Naviwoof Guide", BIT_LEFT, 0}, {8, "Change Moves and Equipment", BIT_LEFT, 1},
};

static int enabled = 1;
int swheel_enabled(void) { return __atomic_load_n(&enabled, __ATOMIC_ACQUIRE); }
void swheel_set_enabled(int on) { __atomic_store_n(&enabled, on ? 1 : 0, __ATOMIC_RELEASE); }

enum { IDLE, OPEN, PASS, FIRE };
static int state;     /* atomic: the drawing reads it */
static int picked;    /* atomic: the ring index pointed at, -1 the middle */
static float vx, vy;  /* the pointer, in mouse pixels from the middle (the window's thread) */
static int fire_ring;
static uint64_t fire_at;
/* for the animations (shortcutwheel.c's drawing): when it opened, when and how it closed (1 used, 2 any other
 * way), and the slot used */
static uint64_t open_ms, close_ms;
static int close_kind, close_ring;
static void closed(int kind, int ring_index) {
    __atomic_store_n(&close_ring, ring_index, __ATOMIC_RELEASE);
    __atomic_store_n(&close_ms, SDL_GetTicks(), __ATOMIC_RELEASE);
    __atomic_store_n(&close_kind, kind, __ATOMIC_RELEASE);
}

/* How far the mouse goes before it points at a slot, and how far the pointer can go (so turning back is quick). */
#define DEAD 100.0f  /* was 40: the pointer had to come closer to a slot before it picks (owner, 2026-11-16) */
#define REACH 140.0f

int swheel_open(void) { return __atomic_load_n(&state, __ATOMIC_ACQUIRE) == OPEN; }

/* The pointer for the drawing (1/16 mouse pixels from the middle): a dot that follows the mouse from the middle, so
 * it moves before the lens snaps to a slot (owner, 2026-11-16). */
static int ptr_x, ptr_y;  /* atomic */

void swheel_motion(float dx, float dy) {
    vx += dx, vy += dy;
    float len = sqrtf(vx * vx + vy * vy);
    if (len > REACH) vx *= REACH / len, vy *= REACH / len, len = REACH;
    __atomic_store_n(&ptr_x, (int)(vx * 16), __ATOMIC_RELEASE);
    __atomic_store_n(&ptr_y, (int)(vy * 16), __ATOMIC_RELEASE);
    int p = -1;
    if (len >= DEAD) {
        float a = atan2f(vx, -vy) * (180.0f / 3.14159265f);  /* 0 up, clockwise */
        if (a < 0) a += 360.0f;
        p = (int)floorf((a + 22.5f) / 45.0f) % 8;
    }
    __atomic_store_n(&picked, p, __ATOMIC_RELEASE);
}

/* The presses for a pick, by the game's input reads since it began: the hold-L set holds L first (as a player
 * would), then L and the direction; then all is let go for a few reads before the wheel can open again. The game
 * takes the hold-L four only once its HUD has turned to their page, about a second after L goes down (2026-11-15:
 * 3 and 12 reads did nothing, 60 opened Mirapo's list). L goes on to the game all the time the wheel is open
 * (2026-11-16: so the cross turns its page and 1 + an arrow key still works), so on letting go on one of them only
 * the rest of the second is waited; a D-pad one lets L go PAGE_BACK reads first, the cross back on its page. */
#define HOLD_L 60
#define PAGE_BACK 8
#define PRESS 6
#define GAP 3
static uint64_t l_since;  /* the read L went down (the wheel opened) */
static uint64_t fire_lead;

static void hud_dump(const char *when);  /* below, with the HUD's hook */
static unsigned slot_mask(int what);     /* 0 locked, 1 grey, 2 on */

/* YW4_WHEEL_LOG=1 (and YW4_WHEEL_TEST, window.c): what the wheel does, on stderr */
static void note(const char *what, uint64_t reads) {
    static int on = -1;
    if (on < 0) on = (getenv("YW4_WHEEL_LOG") && *getenv("YW4_WHEEL_LOG") != '0') || getenv("YW4_WHEEL_TEST");
    if (on) fprintf(stderr, "yw4: shortcut wheel: %s (input read %llu)\n", what, (unsigned long long)reads);
}

static uint64_t update(uint64_t b, int key, int ok, uint64_t reads);

/* The last input read the game got L (atomic): while it has L its cross shows the hold-L page, and its button reports
 * (yw4_hook_field_guide_button) then are the hold-L four's under the same ids, not Bike's or Auto-Navi's. */
static uint64_t l_read;

uint64_t swheel_update(uint64_t b, int key, int ok, uint64_t reads) {
    b = update(b, key, ok, reads);
    if (b & BIT_L) __atomic_store_n(&l_read, reads ? reads : 1, __ATOMIC_RELEASE);
    return b;
}

static uint64_t update(uint64_t b, int key, int ok, uint64_t reads) {
    static int key_was;
    int down = key && !key_was;
    key_was = key;
    if (!swheel_enabled()) {
        __atomic_store_n(&state, IDLE, __ATOMIC_RELEASE);
        return b;
    }
    int st = __atomic_load_n(&state, __ATOMIC_ACQUIRE);
    if (down && ok && st != IDLE && st != OPEN)  /* pressed while an earlier use hasn't finished: why it didn't open */
        fprintf(stderr, "yw4: shortcut wheel: key down, not opened: still %s (since input read %llu, now %llu)\n",
                st == FIRE ? "using the last shortcut" : "waiting for the key to be let go",
                (unsigned long long)fire_at, (unsigned long long)reads);
    switch (st) {
    case IDLE:
        if (down && ok) {
            vx = vy = 0;
            __atomic_store_n(&ptr_x, 0, __ATOMIC_RELEASE), __atomic_store_n(&ptr_y, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&picked, -1, __ATOMIC_RELEASE);
            __atomic_store_n(&open_ms, SDL_GetTicks(), __ATOMIC_RELEASE);
            __atomic_store_n(&close_kind, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&state, OPEN, __ATOMIC_RELEASE);
            l_since = reads ? reads : 1;  /* L goes on to the game from now: its cross turns to the hold-L page */
            note("open", reads);
            hud_dump("open");
        }
        return b;
    case OPEN:
        if (!ok || (b & DPAD)) {  /* the old way (L + an arrow key), or the field went away: plain L until let go */
            __atomic_store_n(&state, key ? PASS : IDLE, __ATOMIC_RELEASE);
            closed(2, -1);
            note(b & DPAD ? "closed: L + an arrow key, the old way" : "closed: not exploring any more", reads);
            return b;
        }
        if (key)  /* held: L goes on to the game as without the wheel (owner, 2026-11-16: with it held back, the
                   * game's cross never turned and 1 + an arrow key, the old way, did nothing) */
            return b;
        {
            int p = __atomic_load_n(&picked, __ATOMIC_ACQUIRE);
            if (p < 0 || (slot_mask(0) >> p & 1)) {  /* the middle, or a shortcut not unlocked yet: nothing */
                __atomic_store_n(&state, IDLE, __ATOMIC_RELEASE);
                closed(2, -1);
                note(p < 0 ? "let go in the middle: nothing used" : "let go on a locked shortcut: nothing used", reads);
                return b & ~BIT_L;
            }
            fire_ring = p, fire_at = reads;
            {
                static int hold_l = -1;  /* YW4_WHEEL_HOLD_L=<reads>: the hold-L time, for tests */
                if (hold_l < 0) hold_l = getenv("YW4_WHEEL_HOLD_L") ? atoi(getenv("YW4_WHEEL_HOLD_L")) : HOLD_L;
                uint64_t held = l_since ? reads - l_since : 0;
                /* a hold-L one: L stays held until the second is full; a D-pad one: L let go for PAGE_BACK reads first,
                 * so the cross is back on the D-pad's page before the arrow */
                fire_lead = ring[p].with_l ? (held < (uint64_t)hold_l ? (uint64_t)hold_l - held : 0) : PAGE_BACK;
                l_since = 0;
            }
            closed(1, p);
            __atomic_store_n(&state, FIRE, __ATOMIC_RELEASE);
            char t[96];
            snprintf(t, sizeof t, "use shortcut %d, %s (after %llu more reads of L)", ring[p].slot, ring[p].name,
                     (unsigned long long)fire_lead);
            note(t, reads);
        }
        /* fall through: the first press now */
    case FIRE: {
        const Slot *s = &ring[fire_ring];
        uint64_t n = reads - fire_at, lead = fire_lead;
        b &= ~(BIT_L | DPAD);
        if (n < lead) b |= s->with_l ? BIT_L : 0;  /* the hold-L one: L; a D-pad one: nothing, the page turns back */
        else if (n < lead + PRESS) b |= s->dir | (s->with_l ? BIT_L : 0);
        else if (n >= lead + PRESS + GAP) __atomic_store_n(&state, key ? PASS : IDLE, __ATOMIC_RELEASE);
        return b;
    }
    case PASS:
    default:
        if (!key) __atomic_store_n(&state, IDLE, __ATOMIC_RELEASE);
        return b;
    }
}

/* The field HUD's shortcut cross (main+0x755ea0, read with Ghidra 2026-11-15): called with the HUD object in x0
 * whenever the cross is updated. Its bytes: +0x61a the page shown (0 the D-pad's four, 1 the hold-L four);
 * +0x604 + page * 4 + i the shortcut i of that page "on" (its btn0N_on part: the green disc, the bike while riding);
 * +0x5ea..0x5ed (page 0) and +0x600..0x603 (page 1) four parts shown or not. Hooked (recomp/overrides.toml) to keep
 * the object's address; read when the wheel is drawn. */
static uint64_t hud_obj;  /* atomic */
int yw4_hook_field_guide(CPUState *ctx) {
    __atomic_store_n(&hud_obj, ctx->x[0], __ATOMIC_RELEASE);
    mousemenu_field_hud_seen();  /* the field is back: a map closed without telling (Mirapo's list) is gone */
    hud_dump("HUD update");
    return 0;
}

/* The cross's button state (main+0x756520, x1 the button 0xc-0x12, w2 and w3 two flags): hooked to keep the last
 * flags of each button (YW4_WHEEL_LOG=1 logs each change). */
static uint8_t button_flags[7];  /* atomic: bit 0 w2, bit 1 w3 */
int yw4_hook_field_guide_button(CPUState *ctx) {
    uint32_t id = (uint32_t)ctx->x[1];
    if (id < 0xc || id > 0x12) return 0;
    /* the reports are by the cross's place, and the hold-L four share them with the D-pad four: while the game has L
     * (the wheel open, a hold-L shortcut being used, L + an arrow key) and for PAGE_BACK reads after, until the cross
     * is back on the D-pad's page, they aren't Bike's or Auto-Navi's (owner, 2026-11-16: opening the wheel turned
     * them off, Change Watchers turned Auto-Navi "on", Change Yo-kai Companion Bike). Kept as they were. */
    uint64_t lr = __atomic_load_n(&l_read, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == OPEN || (lr && window_input_reads() <= lr + PAGE_BACK)) return 0;
    uint8_t f = (uint8_t)((ctx->x[2] & 1) | (ctx->x[3] & 1) << 1);
    uint8_t old = __atomic_exchange_n(&button_flags[id - 0xc], f, __ATOMIC_ACQ_REL);
    static int log = -1;
    if (log < 0) log = getenv("YW4_WHEEL_LOG") && *getenv("YW4_WHEEL_LOG") != '0';
    if (log && old != f) fprintf(stderr, "yw4: shortcut wheel: HUD button %#x: %u %u\n", id, f & 1, f >> 1);
    return 0;
}

/* The HUD's bytes +0x5e0..0x61f (YW4_WHEEL_LOG=1), to match them to the shortcuts. */
static void hud_dump(const char *when) {
    static int log = -1;
    if (log < 0) log = getenv("YW4_WHEEL_LOG") && *getenv("YW4_WHEEL_LOG") != '0';
    uint64_t o = __atomic_load_n(&hud_obj, __ATOMIC_ACQUIRE);
    if (!log || !o) return;
    uint8_t now[0x40];
    memcpy(now, (const void *)(uintptr_t)(o + 0x5e0), sizeof now);
    char t[0x40 * 3 + 1];
    for (int i = 0; i < 0x40; i++) snprintf(t + i * 3, 4, "%02x ", now[i]);
    fprintf(stderr, "yw4: shortcut wheel: %s, HUD %#llx +0x5e0: %s\n", when, (unsigned long long)o, t);
}

/* The state of each slot from the HUD (bit = ring index), as its cross shows it:
 * - locked: not unlocked yet in the story (the HUD shows an empty plate). The HUD's bytes +0x5ea, +0x5eb, +0x5ed,
 *   +0x5ec (D-pad up, right, down, left; the cross's update turns their icons on by them) and +0x600..0x603 (the
 *   hold-L page, when +0x5ff says the game has it; an early save: 0x5ff 0, the HUD shows no "Hold").
 * - grey: unlocked but not usable here (the HUD's grey-out parts): +0x604 + page * 4 + direction, written by the
 *   cross's check (main+0x758e20).
 * - on: the green disc, the bike being ridden: the last state the game gave the button (main+0x756520): 0xe up
 *   (Auto-Navi), 0x11 down (Bike), seen 2026-11-15 (0x11 went 1 on the bike, 0 off it).
 * No HUD seen yet: all unlocked, none grey or on. */
static int dir_index(uint64_t dir) { return dir == BIT_UP ? 0 : dir == BIT_RIGHT ? 1 : dir == BIT_DOWN ? 2 : 3; }
static unsigned slot_mask(int what) {  /* 0 locked, 1 grey, 2 on */
    uint64_t o = __atomic_load_n(&hud_obj, __ATOMIC_ACQUIRE);
    unsigned m = 0;
    for (int i = 0; i < 8; i++) {
        int d = dir_index(ring[i].dir), page = ring[i].with_l;
        int bit = 0;
        if (what == 2) {
            static const uint32_t ids[4] = {0xe, 0xf, 0x11, 0x10};
            bit = !page && (__atomic_load_n(&button_flags[ids[d] - 0xc], __ATOMIC_ACQUIRE) & 1) &&
                  (ids[d] == 0xe || ids[d] == 0x11);
        } else if (o) {
            static const uint16_t shown[4] = {0x5ea, 0x5eb, 0x5ed, 0x5ec};
            const uint8_t *hud = (const uint8_t *)(uintptr_t)o;
            /* the hold-L page only once the game has it (+0x5ff: the HUD's hold-L part and its handling, 0 early in
             * the story, where the cross shows no "Hold" at all) */
            bit = what == 0 ? (page ? !hud[0x5ff] || !hud[0x600 + d] : !hud[shown[d]]) : hud[0x604 + page * 4 + d] != 0;
        }
        m |= (unsigned)bit << i;
    }
    return m;
}
static unsigned active_mask(void) { return slot_mask(2); }

int swheel_hud_ready(void) { return __atomic_load_n(&hud_obj, __ATOMIC_ACQUIRE) != 0; }


/* ---- drawing: style A "Refined" (the owner's pick, 2026-11-15; work/mockups/shortcut-wheel/styles3.html): a cone of
 * light from the middle to the pick, a scope lens with an inner glow and four corner marks, the rim lit where it
 * points, faint plates under the icons; locked = a dashed outline and a small lock, on = a green ring and dot.
 * In the overlay's design units (a 1600 x 900 artboard); the mockup's 1080-high pixels * 5/6. */

#define CX 800.0f
#define CY 450.0f
#define DISC 245.0f   /* the disc's radius */
#define SLOT_R 167.0f /* the icons' circle */
#define ICON 57.0f
#define ICON_PICKED 84.0f
#define LENS 70.0f

#define DEEP 0x160e26u
#define DISC_FILL 0x1b1230u
#define PLATE 0x2a1b4cu
#define LILAC 0xd9cdeau
#define TEAL 0x5fd6e6u
#define PALE 0xbff4fbu
#define WHITE 0xffffffu
#define GREEN 0x9be83au

/* The animations (ms): opening grows from 85 % and fades in; the lens glides between slots; using one flashes the
 * lens and shrinks it onto the icon, then fades; cancelling (or closing any other way) shrinks to 90 % and fades. */
#define OPEN_MS 120
#define GLIDE_MS 80
#define USE_FLASH_MS 100
#define USE_FADE_MS 80
#define CANCEL_MS 100

void swheel_rect(float r[4]) {
    r[0] = CX - DISC - 30, r[1] = CY - DISC - 30, r[2] = r[3] = (DISC + 30) * 2;
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float ease_out(float t) { t = clamp01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }

int swheel_visible(void) {
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == OPEN) return 1;
    int kind = __atomic_load_n(&close_kind, __ATOMIC_ACQUIRE);
    uint64_t t = SDL_GetTicks() - __atomic_load_n(&close_ms, __ATOMIC_ACQUIRE);
    return kind && t < (uint64_t)(kind == 1 ? USE_FLASH_MS + USE_FADE_MS : CANCEL_MS);
}

/* The lens's place round the ring (degrees, 0 up, clockwise): it glides to a new pick (the drawing's thread only). */
static float glide_from, glide_to;
static uint64_t glide_at;
static int glide_pick = -1;
static float lens_angle(int p, uint64_t now) {
    if (p < 0) return glide_to;
    float to = (float)p * 45.0f;
    if (p != glide_pick) {
        float cur = glide_pick < 0 ? to : glide_from + (glide_to - glide_from) * ease_out((float)(now - glide_at) / GLIDE_MS);
        float d = fmodf(to - cur + 540.0f, 360.0f) - 180.0f;  /* the short way round */
        glide_from = cur, glide_to = cur + d, glide_at = now, glide_pick = p;
    }
    return glide_from + (glide_to - glide_from) * ease_out((float)(now - glide_at) / GLIDE_MS);
}

uint32_t swheel_look(void) {
    uint64_t now = SDL_GetTicks();
    int icons = yw4_wheel_icon(1, &(int){0}, &(int){0}) != NULL;
    int open = __atomic_load_n(&state, __ATOMIC_ACQUIRE) == OPEN;
    uint32_t look = (uint32_t)(__atomic_load_n(&picked, __ATOMIC_ACQUIRE) + 1) | (uint32_t)icons << 4 |
                    active_mask() << 5 | (uint32_t)open << 13 | slot_mask(0) << 14 | (slot_mask(1) & 0xff) << 22;
    /* the pointer's dot, in 2-pixel steps: a new picture when it moves */
    uint32_t q = (uint32_t)(__atomic_load_n(&ptr_x, __ATOMIC_ACQUIRE) >> 5 & 0xff) |
                 (uint32_t)(__atomic_load_n(&ptr_y, __ATOMIC_ACQUIRE) >> 5 & 0xff) << 8;
    look ^= q * 2246822519u;
    /* while something moves, a new picture every frame */
    int moving = !open || now - __atomic_load_n(&open_ms, __ATOMIC_ACQUIRE) < OPEN_MS + 20 || now - glide_at < GLIDE_MS + 20;
    return moving ? look ^ (uint32_t)(now * 2654435761u) : look;
}

static void at(float angle, float r, float *x, float *y) {
    float a = angle * (3.14159265f / 180.0f);
    *x = CX + r * sinf(a), *y = CY - r * cosf(a);
}
static void circle(const UiCanvas *c, float x, float y, float r, uint32_t rgb, float a) {
    ui_box_alpha(c, x - r, y - r, r * 2, r * 2, r, rgb, a);
}
static void ring_line(const UiCanvas *c, float x, float y, float r, float w, uint32_t rgb, float a) {
    ui_card(c, x - r, y - r, r * 2, r * 2, r, rgb, 0, w, rgb, a);
}
static void dots(const UiCanvas *c, float x, float y, float r, float gap, float size, uint32_t rgb, float a) {
    int n = (int)(2 * 3.14159265f * r / gap);
    for (int k = 0; k < n; k++) {
        float t = (float)k / (float)n * 2 * 3.14159265f;
        circle(c, x + r * sinf(t), y - r * cosf(t), size, rgb, a);
    }
}

/* One pixel of the (premultiplied) canvas: rgb at opacity a over what is there. */
static void put(const UiCanvas *c, int i, int j, uint32_t rgb, float a) {
    if (a <= 0.002f) return;
    if (a > 1) a = 1;
    uint8_t *p = c->px + ((size_t)j * (size_t)c->w + (size_t)i) * 4;
    float s[3] = {(float)(rgb >> 16 & 255), (float)(rgb >> 8 & 255), (float)(rgb & 255)};
    for (int k = 0; k < 3; k++) p[k] = (uint8_t)(s[k] * a + p[k] * (1 - a) + 0.5f);
    p[3] = (uint8_t)(255 * a + p[3] * (1 - a) + 0.5f);
}

/* The light: the cone from the middle to the lens, the rim's glow and its bright arc where it points, the lens's
 * inner glow and its soft halo; per pixel, only over the box they cover. */
static void light(const UiCanvas *c, float ang, float lx, float ly, float lens) {
    float xs[5], ys[5];
    at(ang - 22, DISC + 10, &xs[0], &ys[0]);
    at(ang + 22, DISC + 10, &xs[1], &ys[1]);
    at(ang, DISC + 10, &xs[2], &ys[2]);
    xs[3] = CX, ys[3] = CY, xs[4] = lx, ys[4] = ly;
    float x0 = lx - lens - 20, x1 = lx + lens + 20, y0 = ly - lens - 20, y1 = ly + lens + 20;
    for (int k = 0; k < 5; k++) {
        if (xs[k] < x0) x0 = xs[k];
        if (xs[k] > x1) x1 = xs[k];
        if (ys[k] < y0) y0 = ys[k];
        if (ys[k] > y1) y1 = ys[k];
    }
    int i0 = (int)floorf(c->ox + x0 * c->s), i1 = (int)ceilf(c->ox + x1 * c->s);
    int j0 = (int)floorf(c->oy + y0 * c->s), j1 = (int)ceilf(c->oy + y1 * c->s);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > c->w) i1 = c->w;
    if (j1 > c->h) j1 = c->h;
    const float cone_r0 = 14, cone_r1 = SLOT_R + LENS, rim = DISC - 1;
    for (int j = j0; j < j1; j++)
        for (int i = i0; i < i1; i++) {
            float x = ((float)i + 0.5f - c->ox) / c->s, y = ((float)j + 0.5f - c->oy) / c->s;
            float dx = x - CX, dy = y - CY, r = sqrtf(dx * dx + dy * dy);
            float a = atan2f(dx, -dy) * (180.0f / 3.14159265f);
            float da = fabsf(fmodf(a - ang + 540.0f, 360.0f) - 180.0f);
            if (r > cone_r0 && r < cone_r1 && r < DISC) {  /* the cone: brighter toward the lens, soft sides */
                float side = clamp01((17.0f - da) / 2.5f);
                put(c, i, j, TEAL, 0.30f * (r - cone_r0) / (cone_r1 - cone_r0) * side);
            }
            if (da < 24 && fabsf(r - rim) < 14) {  /* the rim: a soft glow and a bright arc */
                float side = clamp01((20.0f - da) / 3.0f), d = (r - rim) / 5.0f;
                put(c, i, j, TEAL, 0.6f * expf(-d * d) * side);
                put(c, i, j, PALE, clamp01(1.6f - fabsf(r - rim)) * side);
            }
            float lr = sqrtf((x - lx) * (x - lx) + (y - ly) * (y - ly)) / lens;
            if (lr < 1) put(c, i, j, TEAL, 0.05f + 0.33f * clamp01((lr - 0.55f) / 0.45f));  /* the lens's inner glow */
            float halo = (lr - 1) * lens / 6.0f;
            if (fabsf(halo) < 3) put(c, i, j, TEAL, 0.35f * expf(-halo * halo));
        }
}

/* A grey copy of a slot's icon, for one the game says can't be used here (made once, kept). */
static const uint8_t *grey_icon(int slot, int w, int h, const uint8_t *px) {
    static uint8_t *copies[9];
    if (slot < 1 || slot > 8) return px;
    if (!copies[slot] && (copies[slot] = malloc((size_t)w * (size_t)h * 4)))
        for (int k = 0; k < w * h; k++) {
            const uint8_t *s = px + k * 4;
            uint8_t g = (uint8_t)((s[0] * 77 + s[1] * 150 + s[2] * 29) >> 8);
            copies[slot][k * 4] = copies[slot][k * 4 + 1] = copies[slot][k * 4 + 2] = g, copies[slot][k * 4 + 3] = s[3];
        }
    return copies[slot] ? copies[slot] : px;
}

/* the small lock on a locked slot */
static void lock_mark(const UiCanvas *c, float x, float y) {
    ui_box_alpha(c, x - 7, y - 2, 14, 11, 2, LILAC, 0.5f);
    for (int k = 0; k <= 8; k++) {  /* the shackle, a half circle of short strokes */
        float t0 = 3.14159265f * (float)k / 9, t1 = 3.14159265f * (float)(k + 1) / 9;
        ui_line(c, x - 4 * cosf(t0), y - 2 - 4 * sinf(t0), x - 4 * cosf(t1), y - 2 - 4 * sinf(t1), 2, LILAC, 0.5f);
    }
}

void swheel_draw(const UiCanvas *c0) {
    uint64_t now = SDL_GetTicks();
    int open = __atomic_load_n(&state, __ATOMIC_ACQUIRE) == OPEN;
    int kind = open ? 0 : __atomic_load_n(&close_kind, __ATOMIC_ACQUIRE);
    float t_open = (float)(now - __atomic_load_n(&open_ms, __ATOMIC_ACQUIRE));
    float t_close = (float)(now - __atomic_load_n(&close_ms, __ATOMIC_ACQUIRE));
    int p = open ? __atomic_load_n(&picked, __ATOMIC_ACQUIRE) : kind == 1 ? __atomic_load_n(&close_ring, __ATOMIC_ACQUIRE) : -1;
    unsigned on = active_mask(), locked = slot_mask(0), grey = slot_mask(1);

    /* the whole wheel's size and opacity for the animations */
    float scale = 1, alpha = 1, flash = 0, shrink = 0;
    if (open) {
        float e = ease_out(t_open / OPEN_MS);
        scale = 0.85f + 0.15f * e, alpha = e;
    } else if (kind == 1) {
        float f = clamp01(t_close / USE_FLASH_MS);
        flash = 1 - f, shrink = ease_out(f);
        alpha = 1 - clamp01((t_close - USE_FLASH_MS) / USE_FADE_MS);
    } else if (kind == 2) {
        float e = ease_out(t_close / CANCEL_MS);
        scale = 1 - 0.1f * e, alpha = 1 - e;
    }
    UiCanvas cv = *c0;  /* grown or shrunk about the middle */
    cv.s = c0->s * scale, cv.ox = c0->ox + CX * c0->s * (1 - scale), cv.oy = c0->oy + CY * c0->s * (1 - scale);
    const UiCanvas *c = &cv;

    /* the disc: dark, a dotted ring inside the rim, the rim */
    circle(c, CX, CY, DISC, DISC_FILL, 0.9f);
    ring_line(c, CX, CY, DISC - 10, 14, DEEP, 0.35f);
    dots(c, CX, CY, DISC - 6, 9, 0.9f, TEAL, 0.25f);
    ring_line(c, CX, CY, DISC, 2, TEAL, 0.35f);
    for (int i = 0; i < 8; i++) {  /* faint plates under the icons */
        if (locked >> i & 1) continue;
        float x, y;
        at((float)i * 45, SLOT_R, &x, &y);
        circle(c, x, y, ICON * 0.62f, PLATE, 0.55f);
    }

    /* the light and the lens on the pick */
    float ang = lens_angle(p, now), lx = 0, ly = 0, lens = LENS * (1 - 0.35f * shrink);
    if (p >= 0) {
        at(ang, SLOT_R, &lx, &ly);
        light(c, ang, lx, ly, lens);
        ring_line(c, lx, ly, lens, 2.5f, PALE, 1);
        dots(c, lx, ly, lens - 9, 8, 0.8f, PALE, 0.35f);
        for (int k = 0; k < 4; k++) {
            float a = (ang + 45 + (float)k * 90) * (3.14159265f / 180.0f);
            ui_line(c, lx + (lens + 4) * sinf(a), ly - (lens + 4) * cosf(a), lx + (lens + 13) * sinf(a),
                    ly - (lens + 13) * cosf(a), 3, PALE, 1);
        }
    }
    /* the middle (where letting go uses nothing) is the pointer: its circle follows the mouse from the middle
     * (REACH -> PTR_R), so it moves before the lens snaps to a slot (owner, 2026-11-16: the circle itself, no line) */
    if (p < 0) {  /* hidden once a shortcut is picked: the lens shows it (owner, 2026-11-16) */
        const float PTR_R = 110.0f;  /* at the pick distance (DEAD) the circle meets the lens round the slot */
        float mx = CX, my = CY;
        if (open) {
            mx += (float)__atomic_load_n(&ptr_x, __ATOMIC_ACQUIRE) / 16.0f / REACH * PTR_R;
            my += (float)__atomic_load_n(&ptr_y, __ATOMIC_ACQUIRE) / 16.0f / REACH * PTR_R;
        }
        circle(c, mx, my, 20, DEEP, 1);  /* full size all along (owner: it shrank once a slot was picked) */
        ring_line(c, mx, my, 20, 2.5f, TEAL, 1);
        circle(c, mx, my, 3.5f, TEAL, 1);
    }

    /* the shortcuts: arriving one after another when it opens (15 ms apart) */
    for (int i = 0; i < 8; i++) {
        float x, y, sz = i == p ? ICON_PICKED : ICON;
        at((float)i * 45, SLOT_R, &x, &y);
        float arrive = open ? ease_out((t_open - 15.0f * (float)i) / 60.0f) : 1;
        sz *= 0.6f + 0.4f * arrive;
        if (locked >> i & 1) {
            dots(c, x, y, sz * 0.5f, 7, 1.2f, LILAC, 0.35f * arrive);
            lock_mark(c, x, y);
            continue;
        }
        if (on >> i & 1) {  /* on: a green ring with a soft glow, and a dot */
            ring_line(c, x, y, sz * 0.6f + 4, 12, GREEN, 0.3f);
            ring_line(c, x, y, sz * 0.6f, 4, GREEN, 1);
            circle(c, x + sz * 0.45f, y - sz * 0.45f, 9.5f, DEEP, 1);
            circle(c, x + sz * 0.45f, y - sz * 0.45f, 7, GREEN, 1);
        }
        int w = 0, h = 0;
        const uint8_t *px = yw4_wheel_icon(ring[i].slot, &w, &h);
        if (px) ui_image(c, grey >> i & 1 ? grey_icon(ring[i].slot, w, h, px) : px, w, h, x - sz / 2, y - sz / 2, sz, sz, 0);
        else {
            char el[160];
            float a = ring[i].dir == BIT_UP ? 0 : ring[i].dir == BIT_RIGHT ? 90 : ring[i].dir == BIT_DOWN ? 180 : 270;
            snprintf(el, sizeof el, "<path fill=\"#000\" transform=\"rotate(%g 12 12)\" d=\"M12 4 L20 17 L4 17 Z\"/>", a);
            ui_shape(c, el, x - sz * 0.3f, y - sz * 0.3f, sz * 0.6f, sz * 0.6f, 24, i == p ? PALE : LILAC);
        }
        if (grey >> i & 1) circle(c, x, y, sz * 0.62f, DEEP, 0.4f);  /* not usable here: grey, darker */
        /* the rest aren't dimmed while one is picked: the pick is already larger (owner, 2026-11-16) */
        if (arrive < 1) circle(c, x, y, sz * 0.62f, DISC_FILL, 0.8f * (1 - arrive));
    }
    if (p >= 0 && flash > 0) circle(c, lx, ly, lens, WHITE, 0.7f * flash);  /* used: the lens flashes */

    /* the pick's name under the middle, "on" under it in green */
    if (p >= 0) {
        int lk = locked >> p & 1;
        const char *name = lk ? "Locked" : ring[p].name;
        /* a long name on two lines, split at the space nearest its middle: on one it ran into the side slots' icons
         * (owner, 2026-11-16: "Change moves and Equipment", "Change Yo-kai Companion") */
        char lines[2][64] = {{0}, {0}};
        snprintf(lines[0], sizeof lines[0], "%s", name);
        if (ui_text_w(UI_ROUNDED_800, 22, name) > 230) {
            int len = (int)strlen(name), best = -1;
            for (int k = 0; k < len; k++)
                if (name[k] == ' ' && (best < 0 || abs(k - len / 2) < abs(best - len / 2))) best = k;
            if (best > 0) {
                snprintf(lines[0], sizeof lines[0], "%.*s", best, name);
                snprintf(lines[1], sizeof lines[1], "%s", name + best + 1);
            }
        }
        float ty = CY + 16;
        for (int k = 0; k < 2 && lines[k][0]; k++, ty += 28) {
            float w = ui_text_w(UI_ROUNDED_800, 22, lines[k]) + 8;
            ui_text_in(c, UI_ROUNDED_800, 22, CX - w / 2, ty + 2, w, 34, 1, lines[k], DEEP);
            ui_text_in(c, UI_ROUNDED_800, 22, CX - w / 2, ty, w, 34, 1, lines[k], lk ? LILAC : PALE);
        }
        ty += 2;  /* the status under the name's last line */
        if (!lk && (on >> p & 1)) ui_text_in(c, UI_ROUNDED_700, 15, CX - 40, ty, 80, 22, 1, "on", GREEN);
        else if (!lk && (grey >> p & 1)) ui_text_in(c, UI_ROUNDED_700, 15, CX - 80, ty, 160, 22, 1, "not usable here", LILAC);
    }

    /* the whole picture's opacity (it is premultiplied: every channel) */
    if (alpha < 1) {
        float r[4];
        swheel_rect(r);
        int i0 = (int)floorf(c0->ox + r[0] * c0->s), i1 = (int)ceilf(c0->ox + (r[0] + r[2]) * c0->s);
        int j0 = (int)floorf(c0->oy + r[1] * c0->s), j1 = (int)ceilf(c0->oy + (r[1] + r[3]) * c0->s);
        if (i0 < 0) i0 = 0;
        if (j0 < 0) j0 = 0;
        if (i1 > c0->w) i1 = c0->w;
        if (j1 > c0->h) j1 = c0->h;
        for (int j = j0; j < j1; j++) {
            uint8_t *row = c0->px + ((size_t)j * (size_t)c0->w + (size_t)i0) * 4;
            for (int k = 0; k < (i1 - i0) * 4; k++) row[k] = (uint8_t)((float)row[k] * alpha + 0.5f);
        }
    }
}
