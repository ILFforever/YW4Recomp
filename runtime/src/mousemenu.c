/* The mouse in the game's menus. Prototype: the Yo-kai Pad (the X menu's icon grid, 4 x 3 per page).
 *
 * The Switch game has no touch or mouse input, but every menu (yw::CMenuCursor and the 68 yw::CMenu* classes
 * built on it) still has the PC build's virtual methods, and menu events reach them through one queue
 * (f_9ed590 -> the menu's handler f_6dc700). Hooks on five of them (overrides.toml; this = the menu) tell us
 * what the Pad (yw::CMainMenu) does:
 *   - OnChangeFocus (slot 40, f_6c68f0; x1 -> u32 hash of the control whose focus moved): the Pad opened. The
 *     control with hash 0x6098c54d is the icon grid; f_9f3e90(menu, hash, 0) finds it (as f_6bb580 does for
 *     lists) and its u16 at +0x13c is the focused icon, which the window's thread then reads directly.
 *   - OnBack (slot 27, f_6c59f0): B closed the whole Pad (going back from one of its pages doesn't call it).
 *   - OnDecideFocus (slot 42, f_6c6ba0): an icon was chosen; its page usually opens over the grid.
 *   - OnOpenEndLayer / OnCloseEndLayer (slots 31 / 32, f_6c5fa0 / f_6c6040): one of the Pad's layers finished
 *     opening / closing. They balance (69 each for the whole Pad, 4 each for the Members page), so the grid is in
 *     front again when, after a choice, the count of open layers has gone up and come back to what it was then
 *     (deeper pages keep it up). The grid control itself doesn't change while a page covers it.
 * The hooks only set flags; the window's thread shows the cursor while the Pad is open and turns the mouse into the Pad's own inputs:
 * while the grid is in front, hovering an icon steps the focus there with D-pad presses (one at a time, each
 * checked against the focus the grid then has), a left click on an icon moves there and presses A; a right
 * click presses B anywhere in the Pad. So the game runs its normal paths: sounds, animations, page changes. A
 * press lasts until the game has read the input twice (window_input_reads), so a stall can't swallow it.
 *
 * The icons' places are measured in the 1280 x 720 menu space (the 16:9 middle of the picture, the menus stay
 * 16:9 at any aspect ratio): centres x 125 + 144 c, y 215 + 143.5 r. YW4_MOUSE_MENUS=0 turns it off. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "mousemenu.h"
#include "window.h"
#include "yw4rt.h"

#define PAD_GRID_HASH 0x6098c54du
#define PAD_VTABLE 0x14c0a88u  /* yw::CMainMenu's vtable (address point), main-relative */
#define COLS 4
#define ROWS 3

enum { BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_LEFT = 1 << 12, BTN_UP = 1 << 13, BTN_RIGHT = 1 << 14, BTN_DOWN = 1 << 15 };
enum { PAGE_NONE, PAGE_OPENING, PAGE_OPEN };  /* a page over the grid: none, chosen but not open yet, open */

/* Written by the hooks (the game's threads), read by the window's thread. */
static int pad_open;       /* the Pad is on screen */
static uint64_t pad_grid;  /* its icon grid control, once seen */
static int layers_open;    /* the Pad's layers opened and not closed again */
static int page;           /* PAGE_* */
static int page_base;      /* layers_open when the icon was chosen */
static uint64_t page_since; /* SDL ticks of the choice */

static int enabled(void) {
    static int e = -1;
    if (e < 0) {
        const char *v = getenv("YW4_MOUSE_MENUS");
        e = !(v && *v == '0');
    }
    return e;
}

static int is_pad(uint64_t menu) { return enabled() && menu && ld64(menu) == yw4_image_base + PAD_VTABLE; }

static int script_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("YW4_MOUSE_SCRIPT") && *getenv("YW4_MOUSE_SCRIPT");
    return on;
}

/* f_9f3e90(menu, hash, 0): the menu's control with that name hash, or 0. Called from a hook at a function's
 * entry, so the caller's registers are put back afterwards. */
static uint64_t find_control(CPUState *ctx, uint64_t menu, uint32_t hash) {
    static GuestFn fn;
    if (!fn) fn = yw4_lookup(yw4_image_base + 0x9f3e90);
    if (!fn) return 0;
    uint64_t x[31];
    memcpy(x, ctx->x, sizeof x);
    uint64_t sp = ctx->sp;
    ctx->x[0] = menu, ctx->x[1] = hash, ctx->x[2] = 0;
    fn(ctx);
    uint64_t control = ctx->x[0];
    memcpy(ctx->x, x, sizeof x);
    ctx->sp = sp;
    return control;
}

/* OnChangeFocus (main+0x6c68f0), before the game's own: the Pad is open; its grid's focus moved. */
int yw4_hook_menu_focus(CPUState *ctx) {
    if (!is_pad(ctx->x[0]) || !ctx->x[1]) return 0;
    if (!__atomic_exchange_n(&pad_open, 1, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&page, PAGE_NONE, __ATOMIC_RELEASE);
    }
    if (ld32(ctx->x[1]) == PAD_GRID_HASH && !__atomic_load_n(&pad_grid, __ATOMIC_ACQUIRE)) {
        uint64_t grid = find_control(ctx, ctx->x[0], PAD_GRID_HASH);
        if (grid) __atomic_store_n(&pad_grid, grid, __ATOMIC_RELEASE);
    }
    return 0;
}

/* OnBack (main+0x6c59f0): B closed the whole Pad. */
int yw4_hook_menu_back(CPUState *ctx) {
    if (is_pad(ctx->x[0])) {
        __atomic_store_n(&pad_open, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&pad_grid, 0, __ATOMIC_RELEASE);  /* found again next time: the Pad may be made anew */    }
    return 0;
}

/* OnDecideFocus (main+0x6c6ba0): an icon was chosen; its page opens over the grid. */
int yw4_hook_menu_decide(CPUState *ctx) {
    if (is_pad(ctx->x[0]) && __atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_NONE) {
        __atomic_store_n(&page_base, __atomic_load_n(&layers_open, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        __atomic_store_n(&page_since, SDL_GetTicks(), __ATOMIC_RELEASE);
        __atomic_store_n(&page, PAGE_OPENING, __ATOMIC_RELEASE);
    }
    return 0;
}

/* OnOpenEndLayer (main+0x6c5fa0) / OnCloseEndLayer (main+0x6c6040): a layer of the Pad finished opening / closing. */
int yw4_hook_menu_layer_opened(CPUState *ctx) {
    if (!is_pad(ctx->x[0])) return 0;
    int n = __atomic_add_fetch(&layers_open, 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_OPENING && n > __atomic_load_n(&page_base, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&page, PAGE_OPEN, __ATOMIC_RELEASE);    }
    return 0;
}

int yw4_hook_menu_layer_closed(CPUState *ctx) {
    if (!is_pad(ctx->x[0])) return 0;
    int n = __atomic_sub_fetch(&layers_open, 1, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_OPEN && n <= __atomic_load_n(&page_base, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&page, PAGE_NONE, __ATOMIC_RELEASE);    }
    return 0;
}

/* ---- the window's thread */

/* The icon under the mouse (0..11), or -1. */
static int icon_at(float mx, float my, int w, int h) {
    if (w <= 0 || h <= 0) return -1;
    float uw = (float)w, uh = (float)h;  /* the menus' 16:9 area: the largest one in the middle of the picture */
    if (uw * 9.0f > uh * 16.0f) uw = uh * 16.0f / 9.0f;
    else uh = uw * 9.0f / 16.0f;
    float x = (mx - (w - uw) / 2) * 1280.0f / uw, y = (my - (h - uh) / 2) * 720.0f / uh;
    int c = (int)floorf((x - (125.0f - 72.0f)) / 144.0f), r = (int)floorf((y - (215.0f - 72.0f)) / 143.5f);
    if (c < 0 || c >= COLS || r < 0 || r >= ROWS) return -1;
    float cx = 125.0f + 144.0f * c, cy = 215.0f + 143.5f * r;  /* the icon and its name, not the gaps */
    if (fabsf(x - cx) > 66.0f || fabsf(y - cy) > 66.0f) return -1;
    return r * COLS + c;
}

/* Tests (YW4_MOUSE_SCRIPT=<file>): a scripted mouse instead of the real one, so tests never move the desktop's
 * cursor. Lines "<ms> <x> <y> <buttons>": after waiting <ms> (from the Pad opening, then from the line before)
 * the mouse is at (x, y) in the 1280 x 720 menu space with these buttons (1 left, 4 right; SDL's masks; 256 also
 * takes a screenshot). */
static int script_step(float *mx, float *my, int w, int h, uint32_t *buttons, uint64_t now) {
    static int loaded, n, at;
    static struct { int ms; float x, y; uint32_t b; } lines[256];
    static uint64_t since;
    if (!loaded) {
        loaded = 1;
        const char *p = getenv("YW4_MOUSE_SCRIPT");
        FILE *f = p && *p ? fopen(p, "r") : NULL;
        char l[128];
        while (f && n < 256 && fgets(l, sizeof l, f))
            if (sscanf(l, "%d %f %f %u", &lines[n].ms, &lines[n].x, &lines[n].y, &lines[n].b) == 4) n++;
        if (f) fclose(f), fprintf(stderr, "yw4: mouse menus: %d scripted mouse steps from %s\n", n, p);
    }
    if (!n) return 0;
    if (!since) since = now;
    while (at < n - 1 && now - since >= (uint64_t)lines[at + 1].ms) {
        since += (uint64_t)lines[at + 1].ms, at++;
        fprintf(stderr, "yw4: mouse menus: script step %d: (%g, %g) buttons %u\n", at, lines[at].x, lines[at].y, lines[at].b);
        if (lines[at].b & 256) window_request_screenshot();
    }
    float uw = (float)w, uh = (float)h;
    if (uw * 9.0f > uh * 16.0f) uw = uh * 16.0f / 9.0f;
    else uh = uw * 9.0f / 16.0f;
    *mx = (w - uw) / 2 + lines[at].x * uw / 1280.0f;
    *my = (h - uh) / 2 + lines[at].y * uh / 720.0f;
    *buttons = lines[at].b & 0xff;
    return 1;
}

uint64_t mousemenu_update(float mx, float my, int w, int h, uint32_t mouse_buttons, int *want_cursor) {
    static uint64_t press, press_until, gap_until, press_reads, gap_reads;  /* the button being pressed now */
    static int step_from = -1;  /* a D-pad step's start focus, until the grid's focus changes */
    static uint64_t step_at;
    static int click_target = -1, hover_target = -1, last_hover = -1, back_wanted, moved, unanswered;
    static uint32_t last_buttons;
    static float last_x = -1, last_y = -1;
    *want_cursor = 0;
    if (!enabled()) return 0;
    uint64_t now = SDL_GetTicks();
    int open = __atomic_load_n(&pad_open, __ATOMIC_ACQUIRE);
    if (open) script_step(&mx, &my, w, h, &mouse_buttons, now);
    uint32_t down = mouse_buttons & ~last_buttons;
    last_buttons = mouse_buttons;
    if (!open) {
        press = 0, click_target = hover_target = -1, step_from = -1, back_wanted = moved = 0;
        last_x = mx, last_y = my;
        return 0;
    }
    *want_cursor = 1;
    /* a choice that opened no page (nothing happened in two seconds): the grid is still in front */
    if (__atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_OPENING && now - __atomic_load_n(&page_since, __ATOMIC_ACQUIRE) > 2000)
        __atomic_store_n(&page, PAGE_NONE, __ATOMIC_RELEASE);
    int grid_front = __atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_NONE;
    uint64_t grid = __atomic_load_n(&pad_grid, __ATOMIC_ACQUIRE);
    int focus = grid ? (int)ld16(grid + 0x13c) : -1;
    if (focus >= COLS * ROWS) focus = -1;
    if (mx != last_x || my != last_y) moved = 1, last_x = mx, last_y = my;
    /* what the mouse asks for, every update (also while a press is under way): a right click is B; on the grid, a
     * left click on an icon goes there and presses A; moving onto an icon goes there and stays wanted until the
     * focus arrives (a still mouse then leaves the keys and the gamepad alone until it moves onto another icon). A
     * cursor resting on an icon when the grid comes to the front wants nothing until it moves. */
    int under = grid_front ? icon_at(mx, my, w, h) : -1;
    if (!grid_front) click_target = hover_target = -1, moved = 0;
    if (down & SDL_BUTTON_RMASK) back_wanted = 1, click_target = hover_target = -1;
    else if ((down & SDL_BUTTON_LMASK) && under >= 0) click_target = under;
    if (under != last_hover) {
        last_hover = under;
        if (under >= 0 && moved) hover_target = under;
    }
    if (hover_target >= 0 && hover_target == focus) hover_target = -1;
    /* a press lasts until the game has read the input twice (at least 40 ms), then it is up for as long before the
     * next: by reads, not time, so a stall (a screenshot, a loading hitch) can't swallow a press */
    uint64_t reads = window_input_reads();
    if (script_on()) {
        static uint64_t next_dump;
        if (now >= next_dump) {
            next_dump = now + 500;
            fprintf(stderr, "yw4: mouse menus: state t=%llu reads=%llu press=%#llx press_reads=%llu gap_reads=%llu step_from=%d "
                    "focus=%d hover=%d click=%d front=%d hid=%llu live=%llu\n", (unsigned long long)now, (unsigned long long)reads,
                    (unsigned long long)press, (unsigned long long)press_reads, (unsigned long long)gap_reads, step_from,
                    focus, hover_target, click_target, grid_front);
        }
    }
    if (press) {
        if (now < press_until || reads < press_reads) return press;
        press = 0, gap_until = now + 40, gap_reads = reads + 2;
    }
    if (now < gap_until || reads < gap_reads) return 0;
    /* a step's answer: the grid's focus changes. The Pad drops a press in a new direction right after a move (its
     * cursor is still animating), so an unanswered step is sent again after 400 ms; after two unanswered steps in a
     * row the mouse stops asking for that icon (one the game won't move to). */
    if (step_from >= 0) {
        if (focus == step_from && now < step_at + 400) return 0;
        if (focus == step_from && ++unanswered >= 2) unanswered = 0, hover_target = click_target = -1;
        if (focus != step_from) unanswered = 0;
        step_from = -1;
    }
    if (back_wanted) back_wanted = 0, press = BTN_B;
    int target = click_target >= 0 ? click_target : hover_target;
    if (script_on()) {
        static int last_under = -2, last_target = -2, last_front = -1, logged;
        if ((under != last_under || target != last_target || grid_front != last_front) && logged++ < 200)
            fprintf(stderr, "yw4: mouse menus: grid %s, under %d, target %d, focus %d\n",
                    grid_front ? "in front" : "covered", under, target, focus);
        last_under = under, last_target = target, last_front = grid_front;
    }
    if (!press && grid_front && target >= 0 && focus >= 0) {
        int dr = target / COLS - focus / COLS, dc = target % COLS - focus % COLS;
        if (dr) press = dr > 0 ? BTN_DOWN : BTN_UP;
        else if (dc) press = dc > 0 ? BTN_RIGHT : BTN_LEFT;
        else if (click_target >= 0) press = BTN_A, click_target = -1;
        if (press && press != BTN_A) step_from = focus, step_at = now;
    }
    if (press) press_until = now + 40, press_reads = reads + 2;
    return press;
}
