/* The mouse in the game's menus: the Yo-kai Pad (the X menu's 4 x 3 icon grid) and the pages it opens.
 *
 * The Switch game has no touch or mouse input, but every menu (yw::CMenuCursor and the 68 yw::CMenu* classes
 * built on it) still has the PC build's virtual methods, and menu events reach them through one queue
 * (f_9ed590 -> the menu's handler f_6dc700). Hooks on five of them (overrides.toml; this = the menu) tell us
 * what the Pad (yw::CMainMenu) and the pages it opens do:
 *   - OnChangeFocus (slot 40, f_6c68f0; x1 -> u32 hash of the control whose focus moved): a menu opened or its
 *     focus moved. f_9f3e90(menu, hash, 0) finds the control; for non-scrolling grids its u16 at +0x13c is the
 *     focused slot. For scrolling lists (lives::CMenuListView, pointed to by the list-view container control's
 *     +0x118), +0x11c is the selected row in the full list, (+0x114) + (+0xb0) is the top visible row on screen
 *     (+0x114 is the ring buffer's top slot and +0xb0 is its top buffer row count), and +0x128 is the total item
 *     count, so the focused visible row on screen is (+0x11c) - ((+0x114) + (+0xb0)).
 *   - OnBack (slot 27, f_6c59f0): B closed the whole Pad (or a sub-page that handles its own B).
 *   - OnDecideFocus (slot 42, f_6c6ba0): an item or icon was chosen; a page or sub-dialog usually opens over the
 *     current screen.
 *   - OnOpenEndLayer / OnCloseEndLayer (slots 31 / 32, f_6c5fa0 / f_6c6040): one of the menu's layers finished
 *     opening / closing. They balance (69 each for the whole Pad, 4 each for the Members popup, 12-42 each for
 *     sub-pages), so a screen is in front again when, after a choice, the count of open layers has gone up and
 *     come back to what it was then (deeper pages keep it up).
 * The hooks only set state; the window's thread shows the cursor while the Pad is open and turns the mouse into
 * the menu's own inputs: while a known screen is in front, hovering an item/icon steps the focus there with D-pad
 * presses (one at a time, each checked against the focus the screen then has), a left click moves there and
 * presses A, the scroll wheel steps one row up or down per notch in lists (or L/R on the Pad grid to cycle between
 * Pages 1 and 2), and a right click presses B anywhere in the Pad. So the game runs its normal paths: sounds,
 * animations, page changes. A press lasts until the game has read the input twice (window_input_reads), so a
 * stall can't swallow it.
 *
 * Positions are measured in the 1280 x 720 menu space (the 16:9 middle of the picture, the menus stay 16:9 at
 * any aspect ratio). YW4_MOUSE_MENUS=0 turns it off; YW4_MOUSE_LOG=1 logs menu focus, decision and layer hooks. */
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

enum { BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_L = 1 << 6, BTN_R = 1 << 7, BTN_LEFT = 1 << 12, BTN_UP = 1 << 13, BTN_RIGHT = 1 << 14, BTN_DOWN = 1 << 15 };
enum { PAGE_NONE, PAGE_OPENING, PAGE_OPEN };  /* a page over the active screen: none, chosen but not open yet, open */
enum { SCR_HOVER = 1 << 0, SCR_CLICK = 1 << 1, SCR_WHEEL = 1 << 2 };

typedef struct {
    const char *name;
    uint32_t vtable;       /* main-relative vtable address point of the menu class */
    uint32_t hash;         /* control hash passed to OnChangeFocus when selection moves */
    uint32_t lv_hash;      /* list-view container control hash (ctrl+0x118 -> lives::CMenuListView), or 0 */
    int cols, rows;        /* visible columns and rows on screen */
    float x0, y0, dx, dy;  /* first slot's centre (x0, y0) and step (dx, dy) in 1280 x 720 menu space */
    float hw, hh;          /* half-width and half-height of each slot's hit box in 1280 x 720 menu space */
    unsigned flags;        /* SCR_* */
} Screen;

/* Measured in the 1280 x 720 menu space from screenshots and OnChangeFocus logs (decision 0057). */
static const Screen screens[] = {
    /* Yo-kai Pad 4 x 3 icon grid (Pages 1 & 2, yw::CMainMenu) */
    {"pad",              0x14c0a88u, 0x6098c54du, 0,           4, 3, 125.0f, 215.0f, 144.0f, 143.5f,  66.0f,  66.0f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Yo-kai Pad "Members" 2-icon popup: Main Members / Full List (yw::CMainMenu) */
    {"pad_members",      0x14c0a88u, 0x48a3835cu, 0,           2, 1, 268.0f, 343.5f, 145.0f,   0.0f,  52.0f,  46.0f, SCR_HOVER | SCR_CLICK},
    /* Inventory 8-row scrolling list (yw::CMenuStopWatchItem) */
    {"inventory",        0x14c0838u, 0x50937ef4u, 0x17001a6au, 1, 8, 288.5f, 191.5f,   0.0f, 58.65f, 222.0f,  27.5f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Inventory "Use on who?" 6-row Yo-kai selection list (yw::CMenuStopWatchItem) */
    {"item_action",      0x14c0838u, 0x4e925801u, 0x802b98b6u, 1, 6, 288.5f, 191.5f,   0.0f, 58.65f, 222.0f,  27.5f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Quests 8-row scrolling list (yw::CMenuQuestOrder, shares yw::CMenuStopWatchItem's vtable) */
    {"quests",           0x14c0838u, 0xb4eb3a55u, 0xaab2a496u, 1, 8, 288.5f, 191.5f,   0.0f, 58.65f, 222.0f,  27.5f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Play Record 8-row scroll-only list (yw::CMenuRecord) */
    {"record",           0x14c4558u, 0xa17f69ccu, 0x04f7643fu, 1, 8, 365.0f, 191.5f,   0.0f, 58.65f, 315.0f,  27.5f, SCR_WHEEL},
    /* Settings 7-row scrolling list (yw::CMenuSetting; values change with Left/Right) */
    {"settings",         0x14c60f8u, 0x857cd4feu, 0xc3a04661u, 1, 7, 433.5f, 191.5f,   0.0f, 58.65f, 365.0f,  27.5f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Help 8-row scrolling list (yw::CMenuHelp) */
    {"help",             0x14c00f8u, 0x0142cd83u, 0x9ee9efdfu, 1, 8, 449.5f, 191.5f,   0.0f, 58.65f, 352.0f,  27.5f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Yo-kai Trophies 8-row scrolling list (yw::CMenuTrophyObj) */
    {"trophies",         0x14c9658u, 0xb3f13b48u, 0xffb21636u, 1, 8, 284.5f, 191.5f,   0.0f, 58.65f, 218.0f,  27.5f, SCR_HOVER | SCR_WHEEL},
    /* Ukiukipedia 4-row category list (yw::CMenuDictionaryTop) */
    {"ukiukipedia",      0x14befd8u, 0x4c41ffe9u, 0,           1, 4, 273.0f, 181.0f,   0.0f,  70.0f, 268.0f,  33.0f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Ukiukipedia 8-row inner entry list (yw::CMenuDictionaryTop) */
    {"ukiukipedia_list", 0x14befd8u, 0xca481af3u, 0xb5ac8c55u, 1, 8, 254.0f, 191.5f,   0.0f, 58.65f, 198.0f,  27.5f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Organize Members - Full List 8-row scrolling list (yw::CMemberMenu) */
    {"members_list",     0x14c1948u, 0xceb4579bu, 0x252cc057u, 1, 8, 268.0f, 191.5f,   0.0f, 58.65f, 203.0f,  27.5f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Organize Members - Watcher card (yw::CMemberMenu) */
    {"members_watcher",  0x14c1948u, 0x180cb413u, 0,           1, 1, 288.0f, 160.0f,   0.0f,   0.0f, 215.0f,  60.0f, SCR_HOVER | SCR_CLICK},
    /* Organize Members - Main Members 3 x 2 Yo-kai card grid (yw::CMemberMenu) */
    {"members_grid",     0x14c1948u, 0x634fbf8du, 0,           3, 2, 145.5f, 356.0f, 142.5f, 184.0f,  67.5f,  68.0f, SCR_HOVER | SCR_CLICK},
    /* Treasure photos 4-row scrolling list (yw::CMenuTreasure) */
    {"treasure",         0x14c9488u, 0x14cc3aa3u, 0xafdaecc2u, 1, 4, 285.0f, 185.0f,   0.0f, 126.5f, 220.0f,  60.0f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Soulmate 8-row scrolling list (yw::CMenuKonkatsuList) */
    {"soulmate",         0x14c0498u, 0x933f4e58u, 0x596edf43u, 1, 8, 288.5f, 191.5f,   0.0f, 58.65f, 222.0f,  27.5f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Watch skills 6-column character select (yw::CMenuGeneralWindow) */
    {"watch_skills",     0x14bfa78u, 0x1405a5f7u, 0,           6, 1, 169.0f, 354.0f, 188.4f,   0.0f,  87.0f, 150.0f, SCR_HOVER | SCR_CLICK},
    /* Organize Members "What will you do?" 6-row action popup (yw::CMenuSubWindow) */
    {"members_action",   0x14c7b78u, 0xb2869aecu, 0,           1, 6, 519.6f, 268.0f,   0.0f,  48.0f, 103.6f,  20.0f, SCR_HOVER | SCR_CLICK},
};
enum { NSCREENS = sizeof screens / sizeof *screens };

/* Written by the hooks (the game's threads), read by the window's thread. */
static int pad_open;          /* the Pad (or a page it opened) is on screen */
static int active_idx = -1;   /* index in screens[] currently active, or -1 */
static uint64_t active_menu;  /* menu object pointer of the active screen */
static uint64_t active_ctrl;  /* control pointer for active_screen->hash */
static uint64_t active_lv;    /* lives::CMenuListView pointer for active_screen->lv_hash, or 0 */
static int layers_open;       /* open layers on active_menu */
static int page;              /* PAGE_* */
static int page_base;         /* layers_open when an item/icon was chosen on active_menu */
static uint64_t page_since;   /* SDL ticks of the choice */
static int wheel_steps;       /* queued wheel notches (+ up, - down) from the window thread */

static int enabled(void) {
    static int e = -1;
    if (e < 0) {
        const char *v = getenv("YW4_MOUSE_MENUS");
        e = !(v && *v == '0');
    }
    return e;
}

static int script_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("YW4_MOUSE_SCRIPT") && *getenv("YW4_MOUSE_SCRIPT");
    return on;
}

static int menu_log_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("YW4_MOUSE_LOG");
        on = (v && *v != '0') || script_on();
    }
    return on;
}

int mousemenu_open(void) {
    return enabled() && __atomic_load_n(&pad_open, __ATOMIC_ACQUIRE);
}

int mousemenu_log_on(void) {
    return menu_log_on();
}

static int find_screen(uint64_t vt, uint32_t hash) {
    for (int i = 0; i < NSCREENS; i++)
        if (screens[i].vtable == vt && screens[i].hash == hash) return i;
    return -1;
}

static int is_members_screen(int idx) {
    return idx >= 0 && idx < NSCREENS && screens[idx].vtable == 0x14c1948u &&
           (screens[idx].hash == 0x180cb413u || screens[idx].hash == 0x634fbf8du);
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

/* OnChangeFocus (main+0x6c68f0), before the game's own: a menu is open or its focus moved. */
int yw4_hook_menu_focus(CPUState *ctx) {
    if (!enabled() || !ctx->x[0] || !ctx->x[1]) return 0;
    uint64_t menu = ctx->x[0];
    uint64_t vt = ld64(menu) - yw4_image_base;
    uint32_t hash = ld32(ctx->x[1]);
    if (menu_log_on()) {
        uint64_t c = find_control(ctx, menu, hash);
        if (c) {
            uint64_t lv = ld64(c + 0x118);
            uint64_t ic = lv ? find_control(ctx, menu, ld32(lv + 0xd0)) : 0;
            fprintf(stderr, "yw4: menu_focus: menu=%#llx vt=%#llx hash=%#x ctrl=%#llx "
                    "138=%d 13a=%d 13c=%u 13e=%d lv=%#llx",
                    (unsigned long long)menu, (unsigned long long)vt, hash, (unsigned long long)c,
                    (int16_t)ld16(c + 0x138), (int16_t)ld16(c + 0x13a), ld16(c + 0x13c),
                    (int16_t)ld16(c + 0x13e), (unsigned long long)lv);
            if (lv) {
                fprintf(stderr, " (vt=%#llx b0=%d b4=%u b8=%u c4=%u d0=%#x 114=%d 11c=%d 120=%d 128=%u ic13c=%d)",
                        (unsigned long long)(ld64(lv) - yw4_image_base),
                        (int32_t)ld32(lv + 0xb0), ld32(lv + 0xb4), ld32(lv + 0xb8), ld32(lv + 0xc4), ld32(lv + 0xd0),
                        (int32_t)ld32(lv + 0x114), (int32_t)ld32(lv + 0x11c), (int32_t)ld32(lv + 0x120),
                        ld32(lv + 0x128), ic ? (int)ld16(ic + 0x13c) : -1);
            }
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "yw4: menu_focus: menu=%#llx vt=%#llx hash=%#x ctrl=0\n",
                    (unsigned long long)menu, (unsigned long long)vt, hash);
        }
    }
    if (vt == PAD_VTABLE && hash == PAD_GRID_HASH)
        __atomic_store_n(&pad_open, 1, __ATOMIC_RELEASE);
    if (!__atomic_load_n(&pad_open, __ATOMIC_ACQUIRE)) return 0;

    int idx = find_screen(vt, hash);
    if (idx >= 0) {
        uint64_t prev_menu = __atomic_load_n(&active_menu, __ATOMIC_ACQUIRE);
        int prev_idx = __atomic_load_n(&active_idx, __ATOMIC_ACQUIRE);
        /* CMemberMenu initializes both members_watcher and members_grid during open,
         * but visual focus starts on members_watcher (0x180cb413). Don't let grid setup
         * clobber initial watcher focus while opening. */
        if (vt == 0x14c1948u && hash == 0x634fbf8du &&
            prev_idx == find_screen(0x14c1948u, 0x180cb413u) &&
            __atomic_load_n(&layers_open, __ATOMIC_ACQUIRE) == 0) {
            return 0;
        }
        uint64_t ctrl = find_control(ctx, menu, hash);
        uint64_t lv = 0;
        if (screens[idx].lv_hash) {
            uint64_t lv_ctrl = find_control(ctx, menu, screens[idx].lv_hash);
            if (lv_ctrl) lv = ld64(lv_ctrl + 0x118);
        }
        __atomic_store_n(&active_ctrl, ctrl, __ATOMIC_RELEASE);
        __atomic_store_n(&active_lv, lv, __ATOMIC_RELEASE);
        __atomic_store_n(&active_menu, menu, __ATOMIC_RELEASE);
        __atomic_store_n(&active_idx, idx, __ATOMIC_RELEASE);
        if (menu != prev_menu || idx != prev_idx) {
            if (menu != prev_menu) __atomic_store_n(&layers_open, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&page, PAGE_NONE, __ATOMIC_RELEASE);
            __atomic_store_n(&wheel_steps, 0, __ATOMIC_RELEASE);
        }
    }
    return 0;
}

/* OnBack (main+0x6c59f0): B closed the whole Pad (or a sub-page). */
int yw4_hook_menu_back(CPUState *ctx) {
    if (!enabled() || !ctx->x[0]) return 0;
    uint64_t menu = ctx->x[0];
    uint64_t vt = ld64(menu) - yw4_image_base;
    if (menu_log_on())
        fprintf(stderr, "yw4: menu_back: menu=%#llx vt=%#llx\n", (unsigned long long)menu, (unsigned long long)vt);
    if (vt == PAD_VTABLE) {
        __atomic_store_n(&pad_open, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&active_idx, -1, __ATOMIC_RELEASE);
        __atomic_store_n(&active_menu, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&active_ctrl, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&active_lv, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&wheel_steps, 0, __ATOMIC_RELEASE);
    } else if (menu == __atomic_load_n(&active_menu, __ATOMIC_ACQUIRE) &&
               __atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_NONE) {
        __atomic_store_n(&active_idx, -1, __ATOMIC_RELEASE);
        __atomic_store_n(&wheel_steps, 0, __ATOMIC_RELEASE);
    }
    return 0;
}

/* OnDecideFocus (main+0x6c6ba0): an item or icon was chosen; a page or dialog may open over the screen. */
int yw4_hook_menu_decide(CPUState *ctx) {
    if (!enabled() || !ctx->x[0]) return 0;
    uint64_t menu = ctx->x[0];
    uint64_t vt = ld64(menu) - yw4_image_base;
    if (menu_log_on())
        fprintf(stderr, "yw4: menu_decide: menu=%#llx vt=%#llx\n", (unsigned long long)menu, (unsigned long long)vt);
    if (menu == __atomic_load_n(&active_menu, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_NONE) {
        __atomic_store_n(&page_base, __atomic_load_n(&layers_open, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        __atomic_store_n(&page_since, SDL_GetTicks(), __ATOMIC_RELEASE);
        __atomic_store_n(&page, PAGE_OPENING, __ATOMIC_RELEASE);
    }
    return 0;
}

/* OnOpenEndLayer (main+0x6c5fa0) / OnCloseEndLayer (main+0x6c6040): a layer finished opening / closing. */
int yw4_hook_menu_layer_opened(CPUState *ctx) {
    if (!enabled() || !ctx->x[0]) return 0;
    uint64_t menu = ctx->x[0];
    uint64_t vt = ld64(menu) - yw4_image_base;
    if (menu_log_on())
        fprintf(stderr, "yw4: menu_layer_opened: menu=%#llx vt=%#llx\n", (unsigned long long)menu, (unsigned long long)vt);
    if (menu == __atomic_load_n(&active_menu, __ATOMIC_ACQUIRE)) {
        int n = __atomic_add_fetch(&layers_open, 1, __ATOMIC_ACQ_REL);
        if (__atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_OPENING && n > __atomic_load_n(&page_base, __ATOMIC_ACQUIRE))
            __atomic_store_n(&page, PAGE_OPEN, __ATOMIC_RELEASE);
    } else if (__atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_OPENING) {
        /* A sub-dialog with its own menu object (e.g. a Help article popup) opened over the active screen. */
        __atomic_add_fetch(&layers_open, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&page, PAGE_OPEN, __ATOMIC_RELEASE);
    }
    return 0;
}

int yw4_hook_menu_layer_closed(CPUState *ctx) {
    if (!enabled() || !ctx->x[0]) return 0;
    uint64_t menu = ctx->x[0];
    uint64_t vt = ld64(menu) - yw4_image_base;
    if (menu_log_on())
        fprintf(stderr, "yw4: menu_layer_closed: menu=%#llx vt=%#llx\n", (unsigned long long)menu, (unsigned long long)vt);
    if (menu == __atomic_load_n(&active_menu, __ATOMIC_ACQUIRE)) {
        int n = __atomic_sub_fetch(&layers_open, 1, __ATOMIC_ACQ_REL);
        if (__atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_OPEN && n <= __atomic_load_n(&page_base, __ATOMIC_ACQUIRE))
            __atomic_store_n(&page, PAGE_NONE, __ATOMIC_RELEASE);
        /* The Members 2-button popup (hash 0x48a3835c) lives on the Pad's own menu object and closes 4 layers on B
         * without firing OnChangeFocus on the 4x3 grid; restore the 4x3 grid when its layers close on B. */
        int cur_idx = __atomic_load_n(&active_idx, __ATOMIC_ACQUIRE);
        if (cur_idx >= 0 && screens[cur_idx].hash == 0x48a3835cu &&
            __atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_NONE) {
            uint64_t grid = find_control(ctx, menu, PAD_GRID_HASH);
            int pad_idx = find_screen(PAD_VTABLE, PAD_GRID_HASH);
            if (grid && pad_idx >= 0) {
                __atomic_store_n(&active_ctrl, grid, __ATOMIC_RELEASE);
                __atomic_store_n(&active_lv, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&active_idx, pad_idx, __ATOMIC_RELEASE);
            }
        }
    } else if (__atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_OPEN && vt != PAD_VTABLE) {
        int n = __atomic_sub_fetch(&layers_open, 1, __ATOMIC_ACQ_REL);
        if (n <= __atomic_load_n(&page_base, __ATOMIC_ACQUIRE))
            __atomic_store_n(&page, PAGE_NONE, __ATOMIC_RELEASE);
    }
    return 0;
}

/* ---- the window's thread */

int mousemenu_wheel(int dir) {
    if (!enabled() || !__atomic_load_n(&pad_open, __ATOMIC_ACQUIRE)) return 0;
    int idx = __atomic_load_n(&active_idx, __ATOMIC_ACQUIRE);
    if (idx >= 0 && (screens[idx].flags & SCR_WHEEL) && __atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_NONE) {
        int cur = __atomic_load_n(&wheel_steps, __ATOMIC_ACQUIRE);
        int next = cur + (dir > 0 ? 1 : -1);
        if (next > 8) next = 8;
        if (next < -8) next = -8;
        __atomic_store_n(&wheel_steps, next, __ATOMIC_RELEASE);
    }
    return 1;  /* swallow wheel while the Pad is open so field bindings don't fire */
}

/* The currently focused visible slot (0 .. cols*rows - 1) on screen s, or -1. */
static int screen_focus(const Screen *s, uint64_t ctrl, uint64_t lv) {
    if (s->lv_hash) {
        if (!lv) return -1;
        int32_t top = (int32_t)ld32(lv + 0x114) + (int32_t)ld32(lv + 0xb0);
        int32_t sel = (int32_t)ld32(lv + 0x11c);
        if (top < 0) top = 0;
        int vis = sel - top;
        return vis >= 0 && vis < s->cols * s->rows ? vis : -1;
    }
    if (!ctrl) return -1;
    int f = (int)ld16(ctrl + 0x13c);
    return f >= 0 && f < s->cols * s->rows ? f : -1;
}

/* The slot under the mouse (0 .. cols*rows - 1) on screen s, or -1. */
static int slot_at(const Screen *s, uint64_t lv, float mx, float my, int w, int h) {
    if (!s || !(s->flags & (SCR_HOVER | SCR_CLICK)) || w <= 0 || h <= 0) return -1;
    float uw = (float)w, uh = (float)h;  /* the menus' 16:9 area: the largest one in the middle of the picture */
    if (uw * 9.0f > uh * 16.0f) uw = uh * 16.0f / 9.0f;
    else uh = uw * 9.0f / 16.0f;
    float x = (mx - (w - uw) / 2) * 1280.0f / uw, y = (my - (h - uh) / 2) * 720.0f / uh;
    int c = s->cols > 1 ? (int)floorf((x - (s->x0 - s->dx * 0.5f)) / s->dx) : 0;
    int r = s->rows > 1 ? (int)floorf((y - (s->y0 - s->dy * 0.5f)) / s->dy) : 0;
    if (c < 0 || c >= s->cols || r < 0 || r >= s->rows) return -1;
    float cx = s->x0 + s->dx * c, cy = s->y0 + s->dy * r;
    if (fabsf(x - cx) > s->hw || fabsf(y - cy) > s->hh) return -1;
    if (s->lv_hash && lv) {
        int32_t top = (int32_t)ld32(lv + 0x114) + (int32_t)ld32(lv + 0xb0);
        uint32_t total = ld32(lv + 0x128);
        if (top < 0) top = 0;
        if (total > 0 && top + r * s->cols + c >= (int32_t)total) return -1;
    }
    return r * s->cols + c;
}

/* Tests (YW4_MOUSE_SCRIPT=<file>): a scripted mouse instead of the real one, so tests never move the desktop's
 * cursor. Lines "<ms> <x> <y> <buttons> [wheel]": after waiting <ms> (from the Pad opening, then from the line
 * before) the mouse is at (x, y) in the 1280 x 720 menu space with these buttons (1 left, 4 right; SDL's masks;
 * 256 also takes a screenshot) and optional wheel notches (+N up, -N down). */
static int script_step(float *mx, float *my, int w, int h, uint32_t *buttons, uint64_t now) {
    static int loaded, n, at;
    static struct { int ms; float x, y; uint32_t b; int wheel; } lines[256];
    static uint64_t since;
    if (!loaded) {
        loaded = 1;
        const char *p = getenv("YW4_MOUSE_SCRIPT");
        FILE *f = p && *p ? fopen(p, "r") : NULL;
        char l[128];
        while (f && n < 256 && fgets(l, sizeof l, f)) {
            int w_notch = 0;
            int got = sscanf(l, "%d %f %f %u %d", &lines[n].ms, &lines[n].x, &lines[n].y, &lines[n].b, &w_notch);
            if (got >= 4) lines[n].wheel = got >= 5 ? w_notch : 0, n++;
        }
        if (f) fclose(f), fprintf(stderr, "yw4: mouse menus: %d scripted mouse steps from %s\n", n, p);
    }
    if (!n) return 0;
    if (!since) {
        since = now;
        fprintf(stderr, "yw4: mouse menus: script step 0: (%g, %g) buttons %u wheel %d\n",
                lines[0].x, lines[0].y, lines[0].b, lines[0].wheel);
        if (lines[0].b & 256) window_request_screenshot();
        if (lines[0].wheel) {
            int w_dir = lines[0].wheel > 0 ? 1 : -1;
            for (int k = 0; k < abs(lines[0].wheel); k++) mousemenu_wheel(w_dir);
        }
    }
    while (at < n - 1 && now - since >= (uint64_t)lines[at + 1].ms) {
        since += (uint64_t)lines[at + 1].ms, at++;
        fprintf(stderr, "yw4: mouse menus: script step %d: (%g, %g) buttons %u wheel %d\n",
                at, lines[at].x, lines[at].y, lines[at].b, lines[at].wheel);
        if (lines[at].b & 256) window_request_screenshot();
        if (lines[at].wheel) {
            int w_dir = lines[at].wheel > 0 ? 1 : -1;
            for (int k = 0; k < abs(lines[at].wheel); k++) mousemenu_wheel(w_dir);
        }
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
    static int step_from = -1;  /* a D-pad step's start focus, until the screen's focus changes */
    static uint64_t step_at;
    static int click_target = -1, hover_target = -1, last_hover = -1, back_wanted, moved, unanswered, pad_page;
    static int last_idx = -1;
    static uint64_t last_menu;
    static uint32_t last_buttons;
    static float last_x = -1, last_y = -1;
    static int target_is_sib;
    *want_cursor = 0;
    if (!enabled()) return 0;
    uint64_t now = SDL_GetTicks();
    int open = __atomic_load_n(&pad_open, __ATOMIC_ACQUIRE);
    if (open) script_step(&mx, &my, w, h, &mouse_buttons, now);
    uint32_t down = mouse_buttons & ~last_buttons;
    last_buttons = mouse_buttons;
    if (!open) {
        press = 0, click_target = hover_target = last_hover = -1, step_from = -1, back_wanted = moved = pad_page = 0;
        target_is_sib = 0;
        last_idx = -1, last_menu = 0;
        last_x = mx, last_y = my;
        return 0;
    }
    *want_cursor = 1;
    int idx = __atomic_load_n(&active_idx, __ATOMIC_ACQUIRE);
    uint64_t menu = __atomic_load_n(&active_menu, __ATOMIC_ACQUIRE);
    const Screen *s = idx >= 0 && idx < NSCREENS ? &screens[idx] : NULL;
    if (idx != last_idx || menu != last_menu) {
        int was_members = is_members_screen(last_idx) && is_members_screen(idx) && menu == last_menu;
        last_idx = idx, last_menu = menu;
        if (!was_members) {
            click_target = hover_target = last_hover = -1, step_from = -1, moved = 0, unanswered = 0;
            target_is_sib = 0;
        } else {
            target_is_sib = 0;
        }
    }
    /* a choice that opened no page (nothing happened in 600 ms on a list or 2 s on the Pad): the screen is still in front */
    uint64_t timeout = s && s->lv_hash ? 600 : 2000;
    if (__atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_OPENING && now - __atomic_load_n(&page_since, __ATOMIC_ACQUIRE) > timeout)
        __atomic_store_n(&page, PAGE_NONE, __ATOMIC_RELEASE);
    int front = s && __atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_NONE;
    uint64_t ctrl = __atomic_load_n(&active_ctrl, __ATOMIC_ACQUIRE);
    uint64_t lv = __atomic_load_n(&active_lv, __ATOMIC_ACQUIRE);
    int focus = front ? screen_focus(s, ctrl, lv) : -1;
    if (mx != last_x || my != last_y) moved = 1, last_x = mx, last_y = my;
    /* what the mouse asks for, every update (also while a press is under way): a right click is B; on an active
     * screen, a left click on a slot goes there and presses A; moving onto a slot goes there and stays wanted
     * until the focus arrives (a still mouse then leaves the keys and the gamepad alone until it moves onto
     * another slot). A cursor resting on a slot when a screen comes to the front wants nothing until it moves. */
    int under = front ? slot_at(s, lv, mx, my, w, h) : -1;
    int under_sib = -1;
    if (front && is_members_screen(idx)) {
        int idx_watcher = find_screen(0x14c1948u, 0x180cb413u);
        int idx_grid = find_screen(0x14c1948u, 0x634fbf8du);
        int sib = (idx == idx_watcher) ? idx_grid : idx_watcher;
        if (under < 0 && sib >= 0) {
            under_sib = slot_at(&screens[sib], 0, mx, my, w, h);
            if (under_sib >= 0) {
                if (down & SDL_BUTTON_LMASK) {
                    click_target = under_sib;
                    target_is_sib = 1;
                }
                if (under_sib != last_hover) {
                    last_hover = under_sib;
                    if (moved) {
                        hover_target = under_sib;
                        target_is_sib = 1;
                    }
                }
            }
        } else if (under >= 0) {
            target_is_sib = 0;
        }
    }
    if (!front) click_target = hover_target = -1, moved = 0, target_is_sib = 0;
    if (down & SDL_BUTTON_RMASK) {
        back_wanted = 1, click_target = hover_target = -1, target_is_sib = 0;
        __atomic_store_n(&wheel_steps, 0, __ATOMIC_RELEASE);
    } else if ((down & SDL_BUTTON_LMASK) && under >= 0 && (s->flags & SCR_CLICK)) {
        click_target = under;
        target_is_sib = 0;
    }
    if (under >= 0) {
        if (under != last_hover) {
            last_hover = under;
            if (moved && (s->flags & SCR_HOVER)) {
                hover_target = under;
                target_is_sib = 0;
            }
        }
    } else if (under_sib < 0) {
        last_hover = -1;
    }
    if (hover_target >= 0 && hover_target == focus && !target_is_sib) hover_target = -1;
    /* a press lasts until the game has read the input twice (at least 40 ms), then it is up for as long before the
     * next: by reads, not time, so a stall (a screenshot, a loading hitch) can't swallow a press */
    uint64_t reads = window_input_reads();
    if (menu_log_on()) {
        static uint64_t next_dump;
        if (now >= next_dump) {
            next_dump = now + 500;
            fprintf(stderr, "yw4: mouse menus: state t=%llu screen=%s focus=%d under=%d hover=%d click=%d wheel=%d front=%d\n",
                    (unsigned long long)now, s ? s->name : "none", focus, under, hover_target, click_target,
                    __atomic_load_n(&wheel_steps, __ATOMIC_ACQUIRE), front);
        }
    }
    if (press) {
        if (now < press_until || reads < press_reads) return press;
        press = 0, gap_until = now + 40, gap_reads = reads + 2;
    }
    if (now < gap_until || reads < gap_reads) return 0;
    /* a step's answer: the screen's focus changes. The Pad drops a press in a new direction right after a move
     * (its cursor is still animating), so an unanswered step is sent again after 400 ms; after two unanswered
     * steps in a row the mouse stops asking for that slot (one the game won't move to). */
    if (step_from >= 0) {
        if (focus == step_from && now < step_at + 400) return 0;
        if (focus == step_from && ++unanswered >= 2) unanswered = 0, hover_target = click_target = -1;
        if (focus != step_from) unanswered = 0;
        step_from = -1;
    }
    if (back_wanted) back_wanted = 0, press = BTN_B;
    int wheel = __atomic_load_n(&wheel_steps, __ATOMIC_ACQUIRE);
    if (!press && front && wheel != 0 && (s->flags & SCR_WHEEL)) {
        press = s->cols > 1 ? ((pad_page ^= 1) ? BTN_R : BTN_L) : (wheel > 0 ? BTN_UP : BTN_DOWN);
        __atomic_store_n(&wheel_steps, wheel > 0 ? wheel - 1 : wheel + 1, __ATOMIC_RELEASE);
        hover_target = click_target = -1;
    }
    int target = click_target >= 0 ? click_target : hover_target;
    if (menu_log_on()) {
        static int last_under = -2, last_target = -2, last_front = -1, logged;
        if ((under != last_under || target != last_target || front != last_front) && logged++ < 2000)
            fprintf(stderr, "yw4: mouse menus: screen %s %s, under %d, target %d, focus %d\n",
                    s ? s->name : "none", front ? "in front" : "covered", under, target, focus);
        last_under = under, last_target = target, last_front = front;
    }
    if (!press && front && target_is_sib && is_members_screen(idx)) {
        press = (screens[idx].hash == 0x180cb413u) ? BTN_DOWN : BTN_UP;
        step_from = focus, step_at = now;
    } else if (!press && front && target >= 0 && focus >= 0) {
        int cols = s->cols;
        int dr = target / cols - focus / cols, dc = target % cols - focus % cols;
        if (dr) press = dr > 0 ? BTN_DOWN : BTN_UP;
        else if (dc) press = dc > 0 ? BTN_RIGHT : BTN_LEFT;
        else if (click_target >= 0) {
            if (s->vtable == 0x14c60f8u) {
                float uw = (float)w, uh = (float)h;
                if (uw * 9.0f > uh * 16.0f) uw = uh * 16.0f / 9.0f;
                else uh = uw * 9.0f / 16.0f;
                float x = (mx - (w - uw) / 2) * 1280.0f / uw;
                if (x >= 470.0f && x <= 530.0f) press = BTN_LEFT;
                else if (x >= 730.0f && x <= 798.5f) press = BTN_RIGHT;
            } else {
                press = BTN_A;
            }
            click_target = -1;
        }
        if (press && (dr || dc)) step_from = focus, step_at = now;
    }
    if (press) press_until = now + 40, press_reads = reads + 2;
    return press;
}
