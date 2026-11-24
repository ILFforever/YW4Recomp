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

#include "kernel.h"
#include "mousemenu.h"
#include "relay.h"
#include "window.h"
#include "yw4rt.h"

#define PAD_GRID_HASH 0x6098c54du
#define PAD_VTABLE 0x14c0a88u  /* yw::CMainMenu's vtable (address point), main-relative */

enum { BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_L = 1 << 6, BTN_R = 1 << 7, BTN_LEFT = 1 << 12, BTN_UP = 1 << 13, BTN_RIGHT = 1 << 14, BTN_DOWN = 1 << 15 };
enum { PAGE_NONE, PAGE_OPENING, PAGE_OPEN };  /* a page over the active screen: none, chosen but not open yet, open */
/* SCR_COUNT: slots past the focus control's own item count (+0x138) are not slots (a message box with one button
 * on the Yes / No row, a room list with fewer rooms than rows) */
/* SCR_WHEEL_LR: the wheel steps left / right along a row (a message box's Yes / No), not up / down or the Pad's pages */
/* SCR_CLICK_ANY: a left click off the slots picks the highlighted one (A), as on a box (owner, 2026-11-20: the room
 * list) */
enum { SCR_HOVER = 1 << 0, SCR_CLICK = 1 << 1, SCR_WHEEL = 1 << 2, SCR_COUNT = 1 << 3, SCR_WHEEL_LR = 1 << 4,
       SCR_CLICK_ANY = 1 << 5 };

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
    /* Yo-kai Trophies 8-row scrolling list (yw::CMenuTrophyObj; Pad page 2 "Trophies") */
    {"trophies",         0x14c9658u, 0xb3f13b48u, 0xffb21636u, 1, 8, 284.5f, 191.5f,   0.0f, 58.65f, 218.0f,  27.5f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Slimey Marks 8-row list (yw::CMenuSlimeyMark-style list, vt main+0x14c2348; Pad page 1 "Slimey Mark"). Rows measured
     * on a screenshot 2026-11-16: first row centre y 162, step 58.3, one column about 664 wide (x 64 .. 728). */
    {"slimey_marks",     0x14c2348u, 0x795f8b82u, 0xff59842fu, 1, 8, 396.0f, 162.0f,   0.0f, 58.3f, 332.0f,  28.0f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Pad page 1 "Soulmates": the 2 x 2 tile menu (Matching, Training, Custom Equip., Soul List; vt main+0x14c0838).
     * Tile centres measured 2026-11-16: (440, 216), (840, 216), (440, 448), (840, 448); tiles about 328 x 200. */
    {"soulmates_tiles",  0x14c0838u, 0x500ef853u, 0,           2, 2, 440.0f, 216.0f, 400.0f, 232.0f, 164.0f, 100.0f, SCR_HOVER | SCR_CLICK},
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
    /* Treasure photos: a grid of 5 columns x 3 rows of photo cards (yw::CMenuTreasure; measured 2026-11-16 from a
     * screenshot: column centres 176, 405, 634, 863, 1092; row centres 180, 308, 436). No wheel: the wheel would page
     * the grid L/R (cols > 1). */
    {"treasure",         0x14c9488u, 0x14cc3aa3u, 0,           5, 3, 176.0f, 180.0f, 229.0f, 128.0f, 108.0f,  60.0f, SCR_HOVER | SCR_CLICK},
    /* Soulmate 8-row scrolling list (yw::CMenuKonkatsuList) */
    {"soulmate",         0x14c0498u, 0x933f4e58u, 0x596edf43u, 1, 8, 288.5f, 191.5f,   0.0f, 58.65f, 222.0f,  27.5f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* Watch Skills 6-character row (its own class, vt main+0x14c6b78, focus hash 0x5b8ae332; the old row had the
     * News notice's class and hash). Characters measured 2026-11-16 at x about 144, 264, 384, 504, 640, 744, y 200 .. 640. */
    {"watch_skills",     0x14c6b78u, 0x5b8ae332u, 0,           6, 1, 144.0f, 420.0f, 120.0f,   0.0f,  60.0f, 220.0f, SCR_HOVER | SCR_CLICK},
    /* Organize Members "What will you do?" 6-row action popup (yw::CMenuSubWindow) */
    {"members_action",   0x14c7b78u, 0xb2869aecu, 0,           1, 6, 519.6f, 268.0f,   0.0f,  48.0f, 103.6f,  20.0f, SCR_HOVER | SCR_CLICK},
    /* Ark Swap (the trade menu, vt main+0x14c8de8; Online and Local are the same screens, decision 0082). Measured
     * 2026-11-19 on the owner's F12 shots of a trade (work/screenshots frame_003228 .. 010759, 1280 x 720):
     * the start: Swap Locally (270, 290), Swap Online (270, 376), rows about 536 x 64 */
    {"trade_start",      0x14c8de8u, 0xb000c468u, 0,           1, 2, 270.0f, 290.0f,   0.0f,  86.0f, 268.0f,  32.0f, SCR_HOVER | SCR_CLICK | SCR_WHEEL | SCR_COUNT},
    /* the room list: Recruit by myself (270, 192), the first host (270, 310); more hosts not measured yet (one only).
     * Two controls, one list here: Recruit is its own control (TRADE_RECRUIT_HASH), the hosts are this one (its
     * +0x13c counts from the first host, +0x13a the hosts); see rooms_recruit. Owner's F12 2026-11-20: Player2's bar
     * x 59 .. 444, y 282 .. 337 */
    {"trade_rooms",      0x14c8de8u, 0x2649a77au, 0,           1, 2, 270.0f, 192.0f,   0.0f, 118.0f, 200.0f,  30.0f,
     SCR_HOVER | SCR_CLICK | SCR_WHEEL | SCR_COUNT | SCR_CLICK_ANY},
    /* which Yo-kai to offer: 8 rows from (290, 192), step 58.65, about 450 wide (its list view: 0xd8deebe3) */
    {"trade_yokai",      0x14c8de8u, 0xceb4579bu, 0xd8deebe3u, 1, 8, 290.0f, 192.0f,   0.0f, 58.65f, 225.0f,  27.5f, SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* the swap screen's Swap button (640, 385), about 240 x 52 (its focus hash: the last one-item control the screen
     * focused before Swap was pressed; to confirm in a run) */
    {"trade_swap",       0x14c8de8u, 0x887ea52eu, 0,           1, 1, 640.0f, 385.0f,   0.0f,   0.0f, 120.0f,  26.0f, SCR_HOVER | SCR_CLICK},
    /* the trade's result: the Yo-kai that came, talking (vt main+0x14c8598, one item; owner's F12 2026-11-20): a
     * click anywhere goes on, as A */
    {"trade_result",     0x14c8598u, 0x5015be9du, 0,           1, 1, 640.0f, 360.0f,   0.0f,   0.0f, 640.0f, 360.0f, SCR_CLICK},
    /* Blasters' mission R menu (yw::CBustersSettingRetireMenu, opened without the Pad: see standalone): Retire
     * Mission, Field Camera (< Far >: its arrows clicked step it), Return to Battle. Measured on the owner's F12s
     * (2026-11-20, 1600 x 900): rows at y 592 / 662 / 735, x 296 .. 1240 */
    {"busters_r_menu",   0x14b5bc8u, 0x9f07b5d8u, 0,           1, 3, 616.0f, 474.0f,   0.0f,  57.5f, 380.0f,  26.0f,
     SCR_HOVER | SCR_CLICK | SCR_WHEEL},
    /* the game's message box (vt main+0x14bfa78): Yes (490, 450), No (790, 450), about 240 x 52, the same in every
     * box of the trade; a box with one button takes the Yes spot (SCR_COUNT) */
    {"message_yes_no",   0x14bfa78u, 0xf51efb06u, 0,           2, 1, 490.0f, 450.0f, 300.0f,   0.0f, 120.0f,  26.0f,
     SCR_HOVER | SCR_CLICK | SCR_WHEEL | SCR_WHEEL_LR | SCR_COUNT},
    /* the same box focused on its other one-item controls (seen in the trade's logs): the whole box goes on */
    {"message_box_a",    0x14bfa78u, 0x31092732u, 0,           1, 1, 640.0f, 360.0f,   0.0f,   0.0f, 450.0f, 210.0f, SCR_HOVER | SCR_CLICK},
    {"message_box_b",    0x14bfa78u, 0xbc012ac7u, 0,           1, 1, 640.0f, 360.0f,   0.0f,   0.0f, 450.0f, 210.0f, SCR_HOVER | SCR_CLICK},
    {"message_box_c",    0x14bfa78u, 0x3fa820a4u, 0,           1, 1, 640.0f, 360.0f,   0.0f,   0.0f, 450.0f, 210.0f, SCR_HOVER | SCR_CLICK},
};
enum { NSCREENS = sizeof screens / sizeof *screens };

/* Watch Skills (vt main+0x14c6b78): LEFT and RIGHT move the character highlight, but the game does not report that move
 * through OnChangeFocus (control 0x5b8ae332 keeps focus index 0; replay runs Q and R on 2026-11-16: screenshots show the
 * highlight move Cole -> Summer -> Hammy -> ...). So the mouse keeps its own count: 0 when the screen opens from the Pad,
 * +1 for each RIGHT and -1 for each LEFT the mouse sends. Presses by the player's keys are not counted. */
/* Ark Swap's room list is two controls: Recruit by myself and the hosts' list. The mouse takes them as one list,
 * slot 0 Recruit and slot 1 + n the host n, so moving between them is a D-pad step like any other (owner, 2026-11-20:
 * the mouse read Player2's focus as Recruit's, so it pressed down on Player2 and did nothing on Recruit). */
#define TRADE_VTABLE 0x14c8de8u
#define TRADE_ROOMS_HASH 0x2649a77au
#define TRADE_RECRUIT_HASH 0xd36583bbu
static int rooms_recruit;     /* the room list's focus is on Recruit by myself */
/* The Ark Swap screen a message box opened over: a pick in the box closes it and the screen is back, but the game
 * focuses nothing then (its focus never moved), so the mouse kept working the gone box (owner's F12, 2026-11-20:
 * the room list after a Yes / No: the wheel stepped left / right, clicks missed). Put back on the box's pick. */
static int under_idx = -1, under_recruit;

/* A game menu that opens without the Pad (whose focus otherwise turns the mouse on: pad_open): Blasters' mission R
 * menu. The mouse is its from its focus; another menu's focus, but the Yes / No box over it, hands the mouse back to
 * the battle at once (the battle's HUD takes the focus as it closes: owner's run, 2026-11-20). */
#define RETIRE_MENU_VTABLE 0x14b5bc8u
static int standalone;
static void standalone_close(const char *why);  /* below, after the state it resets */
static uint64_t under_menu, under_ctrl, under_lv;
static int is_trade_rooms(const Screen *s) { return s && s->vtable == TRADE_VTABLE && s->hash == TRADE_ROOMS_HASH; }

#define WATCH_SKILLS_VTABLE 0x14c6b78u
static int wsk_index;        /* the character highlighted on Watch Skills, by the mouse's count */

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

/* Conversations (owner, 2026-11-15): the talk window is one menu class for the whole conversation (its text pages
 * one-item controls, a choice list a control of N items, hash 0xac75395 for Eyepo's three); the field's HUD menu
 * focuses and reopens its layers when the conversation ends (seen with YW4_MOUSE_LOG at a save point). */
#define TALK_VTABLE 0x14c8598u
#define FIELD_HUD_VTABLE 0x14c5698u
static int talking;
int mousemenu_talking(void) { return enabled() && __atomic_load_n(&talking, __ATOMIC_ACQUIRE); }

/* The map (its menu class, vt main+0x14c14f8, opened as SceneMenu ee2d9d06; seen 2026-11-15): open from its first
 * focus until the field's HUD comes back, as a conversation. The left stick pans it, the right stick zooms. */
#define MAP_VTABLE 0x14c14f8u
static int map_shown;
static uint64_t map_event_ms;  /* the map's last focus, back or layer (SDL ticks) */
int mousemenu_map(void) { return enabled() && __atomic_load_n(&map_shown, __ATOMIC_ACQUIRE); }

/* Mirapo's small screen over the map (same class): its rows focus as 0xb730fbe9 (the visible row at +0x13e); its
 * list view 0xc037cb7f takes the focus as it closes (YW4_MOUSE_LOG, 2026-11-19). The plain map's controls: 0xb05d3ff0,
 * 0xc9818754, 0xde413e27, 0xf56c6de4 (owner's run, 2026-11-18). Up from a row's focus until another of the map's
 * controls focuses, it is backed out of, or the map is gone. While up the mouse is a menu's: moving it no longer pans (it moved the selection), the wheel steps the
 * rows (owner, 2026-11-18). */
#define MIRAPO_ROWS_HASH 0xb730fbe9u
#define MIRAPO_LIST_HASH 0xc037cb7fu
static int mirapo;
int mousemenu_mirapo(void) { return mousemenu_map() && __atomic_load_n(&mirapo, __ATOMIC_ACQUIRE); }

/* The map's Map Pin row (A on the map: five pins side by side, its control 0x79c3b11f; owner, 2026-11-19): up from its
 * focus until another of the map's controls focuses, it is backed out of, or the map is gone. While up the mouse is a
 * menu's, sideways: moving it no longer pans (its pushes moved the selection), the wheel steps the pins left and
 * right, left places the pin, right goes back (bindings.c). */
#define MAP_PIN_HASH 0x79c3b11fu
static int map_pin;
int mousemenu_map_pin(void) { return mousemenu_map() && __atomic_load_n(&map_pin, __ATOMIC_ACQUIRE); }

/* The field HUD's shortcut cross was updated (shortcutwheel.c's hook): the field is back. Mirapo's list is this map
 * class too, and backed out of (B twice) the field's HUD menu doesn't focus again, so the map stayed "shown" and the
 * shortcut wheel never opened again (owner, 2026-11-16). The cross isn't updated while the map is up; half a second
 * after the map's last event, so an update just as it opens doesn't count. */
/* Naviwoof Guide (vt main+0x14c2df8, seen 2026-11-16): a row of pins, left and right; the row is one control, so
 * moving along it focuses nothing. Up from its first focus until it is backed out of or the field is back; while up
 * the mouse's wheel steps left and right, its clicks confirm and go back (bindings.c; owner, 2026-11-16). */
#define NAVIWOOF_VTABLE 0x14c2df8u
static int naviwoof;
static uint64_t naviwoof_ms;
int mousemenu_naviwoof(void) { return enabled() && __atomic_load_n(&naviwoof, __ATOMIC_ACQUIRE); }

void mousemenu_field_hud_seen(void) {
    uint64_t now = SDL_GetTicks();
    if (__atomic_load_n(&naviwoof, __ATOMIC_ACQUIRE) && now - __atomic_load_n(&naviwoof_ms, __ATOMIC_ACQUIRE) >= 500) {
        __atomic_store_n(&naviwoof, 0, __ATOMIC_RELEASE);
        if (menu_log_on()) fprintf(stderr, "yw4: menu: Naviwoof Guide gone (the field's HUD is updated again)\n");
    }
    if (!__atomic_load_n(&map_shown, __ATOMIC_ACQUIRE)) return;
    if (now - __atomic_load_n(&map_event_ms, __ATOMIC_ACQUIRE) < 500) return;
    __atomic_store_n(&map_shown, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&mirapo, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&map_pin, 0, __ATOMIC_RELEASE);
    if (menu_log_on()) fprintf(stderr, "yw4: menu: map gone (the field's HUD is updated again)\n");
}

/* The battle's Stopwatching menu (Pin / Items / Switch Members, vt main+0x14c6e58) and the pages it opens (Switch
 * Members, vt main+0x14c7028, a grid six wide; Items, vt main+0x14c73c8): up while the last menu focus was one of
 * them (or a Yes / No box over them); the fight's own menus focus as soon as it goes on. Seen 2026-11-15. Two rules
 * failed first (the owner: after an item, the clicks stayed the menu's): "until the menu closes a layer while in
 * front" (other menus focus first) and "until the fight's HUD focuses" (the menu's list focuses again after it); its
 * layer counts don't balance either. While up the clicks confirm and go back (bindings.c); the wheel already moves
 * it. */
#define STOPWATCH_VTABLE 0x14c6e58u
#define STOPWATCH_MEMBERS_VTABLE 0x14c7028u
#define STOPWATCH_ITEMS_VTABLE 0x14c73c8u
#define STOPWATCH_LIST_HASH 0xbd8d7086u  /* Pin / Items / Switch Members */
#define BATTLE_START_HASH 0xa4f85585u    /* the battle's start: Start Battle / Stopwatching (same class) */
static int stopwatch;
static uint64_t stopwatch_off_at;  /* SDL ms: when a focus outside it turns it off, unless it focuses again first */
int mousemenu_stopwatch(void) {
    if (!enabled()) return 0;
    uint64_t at = __atomic_load_n(&stopwatch_off_at, __ATOMIC_ACQUIRE);
    if (at && SDL_GetTicks() >= at) {
        __atomic_store_n(&stopwatch, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&stopwatch_off_at, 0, __ATOMIC_RELEASE);
    }
    return __atomic_load_n(&stopwatch, __ATOMIC_ACQUIRE);
}

static void talk_note(uint64_t vt) {
    if (vt == TALK_VTABLE) __atomic_store_n(&talking, 1, __ATOMIC_RELEASE);
    else if (vt == MAP_VTABLE) {
        __atomic_store_n(&map_event_ms, SDL_GetTicks(), __ATOMIC_RELEASE);
        __atomic_store_n(&map_shown, 1, __ATOMIC_RELEASE);
    } else if (vt == NAVIWOOF_VTABLE) {
        __atomic_store_n(&naviwoof_ms, SDL_GetTicks(), __ATOMIC_RELEASE);
        __atomic_store_n(&naviwoof, 1, __ATOMIC_RELEASE);
    }
    else if (vt == FIELD_HUD_VTABLE) {
        __atomic_store_n(&talking, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&map_shown, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&naviwoof, 0, __ATOMIC_RELEASE);
    }
}

/* The Yes / No box (yw::CMenuGeneralWindow, its two buttons side by side, hash 0xf51efb06: the shop's "Is that
 * correct?", seen 2026-11-15) has the focus: the wheel steps left and right there. Cleared by any other focus. */
#define GENERAL_WINDOW_VTABLE 0x14bfa78u
#define YES_NO_HASH 0xf51efb06u
static int sideways;
int mousemenu_sideways(void) { return enabled() && __atomic_load_n(&sideways, __ATOMIC_ACQUIRE); }

/* Blasters' chat (yw::CChatMenu, X in the team room and in missions; its message list 0x7c7e7b58, 6 rows) moves on
 * the D-pad only: while it is open the keyboard's left stick keys press the D-pad instead and the field set's keys are
 * used (owner, 2026-11-20: W / S didn't move it, 1 / 3 didn't switch its tabs, W A S D walked the player behind it;
 * bindings.c). Open from its focus until another menu's focus or a back out of it. Sending a message (a pick) focuses
 * yw::CChatIconMenu (the message's bubble): that doesn't close it. With the mouse off too.
 * Not the mission's R menu (yw::CBustersSettingRetireMenu, vtable 0x14b5bc8: Retire Mission, Field Camera, Return to
 * Battle): half the screen, the game goes on behind it, so W A S D stay the player's (owner, 2026-11-20). */
#define CHAT_VTABLE 0x14b6308u
#define CHAT_ICON_VTABLE 0x14b6138u
static int dpad_menu_open;
int mousemenu_dpad_menu(void) { return __atomic_load_n(&dpad_menu_open, __ATOMIC_ACQUIRE); }

/* Blasters' R menu with its Field Camera row highlighted (row 1 of list 0x9f07b5d8): A does nothing there, so the
 * keys' A (the owner's E) steps it right instead (bindings.c; owner, 2026-11-20). Mouse on or off: the menu's
 * focus hook notes it (r_menu_ctrl), the row is read live. */
static uint64_t r_menu_ctrl;
int mousemenu_r_menu_camera_row(void) {
    uint64_t c = __atomic_load_n(&r_menu_ctrl, __ATOMIC_ACQUIRE);
    return c && ld16(c + 0x13c) == 1;
}
static void dpad_menu_note(CPUState *ctx, int focus) {
    if (!ctx->x[0]) return;
    uint64_t vt = ld64(ctx->x[0]) - yw4_image_base;
    if (vt == CHAT_ICON_VTABLE) return;
    int is_menu = vt == CHAT_VTABLE;
    int now = focus ? is_menu : (is_menu ? 0 : __atomic_load_n(&dpad_menu_open, __ATOMIC_ACQUIRE));
    if (now != __atomic_exchange_n(&dpad_menu_open, now, __ATOMIC_ACQ_REL) && menu_log_on())
        fprintf(stderr, "yw4: menu: Blasters' chat %s\n", now ? "open: the left stick keys press the D-pad" : "closed");
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
    dpad_menu_note(ctx, 1);
    if (ctx->x[0] && ctx->x[1]) {  /* Blasters' R menu's list (mousemenu_r_menu_camera_row), mouse on or off */
        uint64_t v = ld64(ctx->x[0]) - yw4_image_base;
        if (v == 0x14b5bc8u && ld32(ctx->x[1]) == 0x9f07b5d8u)
            __atomic_store_n(&r_menu_ctrl, find_control(ctx, ctx->x[0], 0x9f07b5d8u), __ATOMIC_RELEASE);
        else if (v != 0x14b5bc8u && v != GENERAL_WINDOW_VTABLE)
            __atomic_store_n(&r_menu_ctrl, 0, __ATOMIC_RELEASE);
    }
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
    talk_note(vt);
    if (vt == MAP_VTABLE) {
        /* only its rows: the list view takes the focus as Mirapo's list closes, back on the map (owner, 2026-11-19:
         * after closing it the mouse didn't pan the map until the pin page had opened and closed) */
        int m = hash == MIRAPO_ROWS_HASH;
        if (m != __atomic_exchange_n(&mirapo, m, __ATOMIC_ACQ_REL) && menu_log_on())
            fprintf(stderr, "yw4: menu: Mirapo's list %s\n", m ? "up" : "gone (the map's own controls)");
        int pin = hash == MAP_PIN_HASH;
        if (pin != __atomic_exchange_n(&map_pin, pin, __ATOMIC_ACQ_REL) && menu_log_on())
            fprintf(stderr, "yw4: menu: Map Pin row %s\n", pin ? "up" : "gone");
    } else if (vt == FIELD_HUD_VTABLE) {
        __atomic_store_n(&mirapo, 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&sideways, vt == GENERAL_WINDOW_VTABLE && hash == YES_NO_HASH, __ATOMIC_RELEASE);
    /* the last focus decides: the Stopwatching menu's choice list or the battle's start choice, its pages, or a Yes /
     * No box over them: on. The class's info-card controls (0x81f8dde3, 0xcf634eb0: the ally or enemy picked with L or
     * R in the menu, and after the fight goes on) change nothing. Anything else (the fight's HUD menus): off, 0.4 s
     * later, unless the menu focuses or picks again first: Switch Members focuses the fight's HUD for a moment between
     * its picks (owner, 2026-11-15: after Tab the hints went back; the members' second pick). */
    /* a Yes / No box only while the menu is already up: Mirapo's "warp here?" turned it on in the field, where
     * nothing turns it off, and the shortcut wheel stopped opening (owner, 2026-11-16) */
    if ((vt == STOPWATCH_VTABLE && (hash == STOPWATCH_LIST_HASH || hash == BATTLE_START_HASH)) ||
        vt == STOPWATCH_MEMBERS_VTABLE || vt == STOPWATCH_ITEMS_VTABLE ||
        (vt == GENERAL_WINDOW_VTABLE && __atomic_load_n(&stopwatch, __ATOMIC_ACQUIRE))) {
        __atomic_store_n(&stopwatch, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&stopwatch_off_at, 0, __ATOMIC_RELEASE);
    } else if (!(vt == STOPWATCH_VTABLE && (hash == 0x81f8dde3u || hash == 0xcf634eb0u)) &&
               __atomic_load_n(&stopwatch, __ATOMIC_ACQUIRE) && !__atomic_load_n(&stopwatch_off_at, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&stopwatch_off_at, SDL_GetTicks() + 400, __ATOMIC_RELEASE);
    }
    if (vt == PAD_VTABLE && hash == PAD_GRID_HASH)
        __atomic_store_n(&pad_open, 1, __ATOMIC_RELEASE);
    if (vt == RETIRE_MENU_VTABLE) {  /* Blasters' R menu: the mouse is its, without the Pad (standalone) */
        if (!__atomic_load_n(&pad_open, __ATOMIC_ACQUIRE)) {
            standalone = 1;
            __atomic_store_n(&pad_open, 1, __ATOMIC_RELEASE);
            if (menu_log_on()) fprintf(stderr, "yw4: menu: Blasters' R menu: the mouse is the menu's\n");
        }
    } else if (standalone && vt != GENERAL_WINDOW_VTABLE) {  /* it closed: the mouse is the battle's again */
        standalone_close("another menu's focus");
        return 0;
    }
    if (!__atomic_load_n(&pad_open, __ATOMIC_ACQUIRE)) return 0;

    /* the room list's Recruit row: the hosts' screen, its control (for the hosts' count), and slot 0 */
    const int on_recruit = vt == TRADE_VTABLE && hash == TRADE_RECRUIT_HASH;
    if (on_recruit) hash = TRADE_ROOMS_HASH;
    int idx = find_screen(vt, hash);
    if (idx >= 0) {
        if (screens[idx].vtable == GENERAL_WINDOW_VTABLE) {  /* a box over an Ark Swap screen: remember that screen */
            int prev = __atomic_load_n(&active_idx, __ATOMIC_ACQUIRE);
            if (prev >= 0 && (screens[prev].vtable == TRADE_VTABLE || screens[prev].vtable == RETIRE_MENU_VTABLE)) {
                under_recruit = __atomic_load_n(&rooms_recruit, __ATOMIC_ACQUIRE);
                under_menu = __atomic_load_n(&active_menu, __ATOMIC_ACQUIRE);
                under_ctrl = __atomic_load_n(&active_ctrl, __ATOMIC_ACQUIRE);
                under_lv = __atomic_load_n(&active_lv, __ATOMIC_ACQUIRE);
                __atomic_store_n(&under_idx, prev, __ATOMIC_RELEASE);  /* last: the other thread checks it first */
            }
        } else {
            __atomic_store_n(&under_idx, -1, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&rooms_recruit, on_recruit, __ATOMIC_RELEASE);
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
            if (menu != prev_menu && vt == WATCH_SKILLS_VTABLE) __atomic_store_n(&wsk_index, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&page, PAGE_NONE, __ATOMIC_RELEASE);
            __atomic_store_n(&wheel_steps, 0, __ATOMIC_RELEASE);
        } else if (hash == screens[idx].hash && __atomic_load_n(&page, __ATOMIC_ACQUIRE) == PAGE_OPEN) {
            /* the screen's own control took the focus again while a page opened over it: that page is closed (Watch
             * Skills: a character's skill tree closed with its layer count not back at the start, so the screen stayed
             * "covered" and ignored the mouse; 2026-11-16) */
            __atomic_store_n(&page, PAGE_NONE, __ATOMIC_RELEASE);
            __atomic_store_n(&wheel_steps, 0, __ATOMIC_RELEASE);
        }
    }
    return 0;
}

/* OnBack (main+0x6c59f0): B closed the whole Pad (or a sub-page). */
int yw4_hook_menu_back(CPUState *ctx) {
    dpad_menu_note(ctx, 0);
    if (!enabled() || !ctx->x[0]) return 0;
    uint64_t menu = ctx->x[0];
    uint64_t vt = ld64(menu) - yw4_image_base;
    if (menu_log_on())
        fprintf(stderr, "yw4: menu_back: menu=%#llx vt=%#llx\n", (unsigned long long)menu, (unsigned long long)vt);
    /* backing out of the map closes it: the map is gone now. The field HUD's update (mousemenu_field_hud_seen) only
     * comes when the shortcut cross changes, and after closing the map in a place where nothing changed it never
     * came: the mouse kept panning a map that wasn't there and never went back to the camera (owner, 2026-11-19;
     * YW4_MOUSE_LOG: the map's menu_back, then no field HUD focus or update for 1000 frames). Backing out of Mirapo's
     * list (the same class) goes back to the map: the map stays, its next back closes it. */
    if (vt == MAP_VTABLE) {
        __atomic_store_n(&map_event_ms, 0, __ATOMIC_RELEASE);  /* and the next field HUD update ends it at once */
        __atomic_store_n(&map_pin, 0, __ATOMIC_RELEASE);       /* the Map Pin row backed out of, if it was up */
        if (__atomic_exchange_n(&mirapo, 0, __ATOMIC_ACQ_REL)) {
            if (menu_log_on()) fprintf(stderr, "yw4: menu: Mirapo's list backed out of: the map again\n");
        } else if (__atomic_exchange_n(&map_shown, 0, __ATOMIC_ACQ_REL) && menu_log_on()) {
            fprintf(stderr, "yw4: menu: map gone (backed out of)\n");
        }
    }
    if (vt == NAVIWOOF_VTABLE) __atomic_store_n(&naviwoof, 0, __ATOMIC_RELEASE);  /* backed out of: gone */
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

/* The box over an Ark Swap screen is gone: that screen is the mouse's again */
static void box_gone(const char *why) {
    /* take the index once and clear it: the game's thread and the window's both end up here, and only one may act */
    int u = __atomic_exchange_n(&under_idx, -1, __ATOMIC_ACQ_REL);
    if (u < 0 || u >= NSCREENS) return;
    __atomic_store_n(&rooms_recruit, under_recruit, __ATOMIC_RELEASE);
    __atomic_store_n(&active_ctrl, under_ctrl, __ATOMIC_RELEASE);
    __atomic_store_n(&active_lv, under_lv, __ATOMIC_RELEASE);
    __atomic_store_n(&active_menu, under_menu, __ATOMIC_RELEASE);
    __atomic_store_n(&active_idx, u, __ATOMIC_RELEASE);
    __atomic_store_n(&page, PAGE_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&wheel_steps, 0, __ATOMIC_RELEASE);
    if (menu_log_on()) fprintf(stderr, "yw4: mouse menus: %s: %s again\n", why, screens[u].name);
}

/* OnDecideFocus (main+0x6c6ba0): an item or icon was chosen; a page or dialog may open over the screen. */
int yw4_hook_menu_decide(CPUState *ctx) {
    ldn_menu_decided();  /* with the mouse off too: Blasters' Online row (ldn.c yw4_hook_menu_row) */
    if (!enabled() || !ctx->x[0]) return 0;
    uint64_t menu = ctx->x[0];
    uint64_t vt = ld64(menu) - yw4_image_base;
    if (menu_log_on())
        fprintf(stderr, "yw4: menu_decide: menu=%#llx vt=%#llx\n", (unsigned long long)menu, (unsigned long long)vt);
    if (vt == STOPWATCH_VTABLE || vt == STOPWATCH_MEMBERS_VTABLE || vt == STOPWATCH_ITEMS_VTABLE)
        __atomic_store_n(&stopwatch_off_at, 0, __ATOMIC_RELEASE);  /* a pick in it: still open */
    if (vt == GENERAL_WINDOW_VTABLE && __atomic_load_n(&under_idx, __ATOMIC_ACQUIRE) >= 0 && menu == __atomic_load_n(&active_menu, __ATOMIC_ACQUIRE)) {
        /* a pick in a box over an Ark Swap screen: the box closes, the screen under it is back (a box or page the
         * pick opens focuses itself, and takes over) */
        box_gone("a pick in the box");
        return 0;
    }
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
    talk_note(vt);
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

/* Blasters' R menu closed (standalone): the mouse is the battle's again */
static void standalone_close(const char *why) {
    standalone = 0;
    __atomic_store_n(&under_idx, -1, __ATOMIC_RELEASE);
    __atomic_store_n(&pad_open, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&active_idx, -1, __ATOMIC_RELEASE);
    __atomic_store_n(&active_menu, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&active_ctrl, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&active_lv, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&wheel_steps, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&page, PAGE_NONE, __ATOMIC_RELEASE);
    if (menu_log_on()) fprintf(stderr, "yw4: menu: Blasters' R menu closed (%s): the mouse is the battle's\n", why);
}

int yw4_hook_menu_layer_closed(CPUState *ctx) {
    if (ctx->x[0] && ld64(ctx->x[0]) - yw4_image_base == 0x14b5bc8u)  /* the R menu going away */
        __atomic_store_n(&r_menu_ctrl, 0, __ATOMIC_RELEASE);
    if (!enabled() || !ctx->x[0]) return 0;
    uint64_t menu = ctx->x[0];
    uint64_t vt = ld64(menu) - yw4_image_base;
    if (menu_log_on())
        fprintf(stderr, "yw4: menu_layer_closed: menu=%#llx vt=%#llx\n", (unsigned long long)menu, (unsigned long long)vt);
    /* the R menu's own layer closing is it going away; the battle's HUD doesn't always take the focus after (owner,
     * 2026-11-20: the mouse stayed the menu's) */
    if (standalone && vt == RETIRE_MENU_VTABLE) {
        standalone_close("its layer closed");
        return 0;
    }
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
    if (s->vtable == WATCH_SKILLS_VTABLE) return __atomic_load_n(&wsk_index, __ATOMIC_ACQUIRE);
    if (s->lv_hash) {
        if (!lv) return -1;
        int32_t top = (int32_t)ld32(lv + 0x114) + (int32_t)ld32(lv + 0xb0);
        int32_t sel = (int32_t)ld32(lv + 0x11c);
        if (top < 0) top = 0;
        int vis = sel - top;
        return vis >= 0 && vis < s->cols * s->rows ? vis : -1;
    }
    if (is_trade_rooms(s)) {  /* Recruit is slot 0, the host n slot 1 + n */
        if (__atomic_load_n(&rooms_recruit, __ATOMIC_ACQUIRE)) return 0;
        if (!ctrl) return -1;
        int f = 1 + (int)ld16(ctrl + 0x13c);
        return f < s->cols * s->rows ? f : -1;
    }
    if (!ctrl) return -1;
    int f = (int)ld16(ctrl + 0x13c);
    return f >= 0 && f < s->cols * s->rows ? f : -1;
}

typedef struct { float x, y, w, h; } MenuRect;

/* The menus' 16:9 area: the largest 16:9 rectangle centred inside a w x h window. */
static MenuRect menu_16x9_bounds(int w, int h) {
    float uw = (float)w, uh = (float)h;
    if (uw * 9.0f > uh * 16.0f) uw = uh * 16.0f / 9.0f;
    else uh = uw * 9.0f / 16.0f;
    return (MenuRect){((float)w - uw) / 2, ((float)h - uh) / 2, uw, uh};
}

/* The slot under the mouse (0 .. cols*rows - 1) on screen s, or -1. */
static int slot_at(const Screen *s, uint64_t lv, float mx, float my, int w, int h) {
    if (!s || !(s->flags & (SCR_HOVER | SCR_CLICK)) || w <= 0 || h <= 0) return -1;
    MenuRect b = menu_16x9_bounds(w, h);
    float x = (mx - b.x) * 1280.0f / b.w, y = (my - b.y) * 720.0f / b.h;
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
    MenuRect b = menu_16x9_bounds(w, h);
    *mx = b.x + lines[at].x * b.w / 1280.0f;
    *my = b.y + lines[at].y * b.h / 720.0f;
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
    /* a box over an Ark Swap screen that closed with no pick (the room list's after its scan; owner's runs 2026-11-20:
     * the wheel worked there only after opening and backing out of Recruit's box): the game destroyed it, so its
     * object is no longer a box (the destructors set its type back to a base class's). A box waiting for its click
     * (Please select a Yo-kai to swap) stays one until then. The screen's own layer events can't tell the two apart. */
    if (__atomic_load_n(&under_idx, __ATOMIC_ACQUIRE) >= 0) {
        int cur = __atomic_load_n(&active_idx, __ATOMIC_ACQUIRE);
        uint64_t box = __atomic_load_n(&active_menu, __ATOMIC_ACQUIRE);
        if (cur >= 0 && cur < NSCREENS && screens[cur].vtable == GENERAL_WINDOW_VTABLE && box &&
            ld64(box) - yw4_image_base != GENERAL_WINDOW_VTABLE)
            box_gone("the box was destroyed");
    }
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
    /* the room list: Up from the first host to Recruit focuses nothing in the game (its focus comes only on the way
     * down; nothing in either control or the menu changes either, 2026-11-20 probe), so an Up the game reads there,
     * from any device or the mouse, is the move to Recruit. A host's focus takes it back (yw4_hook_menu_focus). */
    if (is_trade_rooms(s)) {
        static uint64_t was;
        const uint64_t b = hid_current_buttons(), up = (1ull << 13) | (1ull << 17);  /* D-pad Up, left stick up */
        if ((b & up) && !(was & up) && front && ctrl && ld16(ctrl + 0x13c) == 0)
            __atomic_store_n(&rooms_recruit, 1, __ATOMIC_RELEASE);
        was = b;
    }
    int focus = front ? screen_focus(s, ctrl, lv) : -1;
    if (mx != last_x || my != last_y) moved = 1, last_x = mx, last_y = my;
    /* what the mouse asks for, every update (also while a press is under way): a right click is B; on an active
     * screen, a left click on a slot goes there and presses A; moving onto a slot goes there and stays wanted
     * until the focus arrives (a still mouse then leaves the keys and the gamepad alone until it moves onto
     * another slot). A cursor resting on a slot when a screen comes to the front wants nothing until it moves. */
    int under = front ? slot_at(s, lv, mx, my, w, h) : -1;
    if (under >= 0 && (s->flags & SCR_COUNT) && ctrl) {  /* fewer items than slots: the rest are not slots */
        int16_t items = (int16_t)ld16(ctrl + 0x138);
        /* the room list: Recruit and the hosts (+0x13a: 1 with one host, where +0x138 was 8; none not seen yet) */
        if (is_trade_rooms(s)) items = (int16_t)(1 + ((int16_t)ld16(ctrl + 0x13a) > 0 ? ld16(ctrl + 0x13a) : 0));
        if (items > 0 && under >= items) under = -1;
    }
    /* the game's message box with one item ("Connection will now begin", "Finished saving": no button drawn, or one
     * OK): the whole box is it, so a click anywhere on the box goes on, as A (owner's F12, 2026-11-19). The box is
     * about x 190 .. 1090, y 150 .. 570 in 1280 x 720 (measured on that shot). */
    if (front && s && s->vtable == 0x14bfa78u && ctrl && (int16_t)ld16(ctrl + 0x138) == 1) {
        static const Screen whole = {"message_box", 0x14bfa78u, 0, 0, 1, 1, 640.0f, 360.0f, 0.0f, 0.0f, 450.0f, 210.0f,
                                     SCR_HOVER | SCR_CLICK};
        under = slot_at(&whole, 0, mx, my, w, h);
    }
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
    } else if ((down & SDL_BUTTON_LMASK) && under < 0 && front && focus >= 0 && (s->flags & SCR_CLICK_ANY)) {
        click_target = focus;  /* off the rows: the highlighted one, as A */
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
        press = (s->flags & SCR_WHEEL_LR) ? (wheel > 0 ? BTN_LEFT : BTN_RIGHT)
              : s->cols > 1 ? ((pad_page ^= 1) ? BTN_R : BTN_L) : (wheel > 0 ? BTN_UP : BTN_DOWN);
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
        else if (dc) {
            press = dc > 0 ? BTN_RIGHT : BTN_LEFT;
            if (s->vtable == WATCH_SKILLS_VTABLE) {  /* the press moves the highlight: count it (see wsk_index) */
                int n = __atomic_load_n(&wsk_index, __ATOMIC_ACQUIRE) + (dc > 0 ? 1 : -1);
                __atomic_store_n(&wsk_index, n < 0 ? 0 : n > 5 ? 5 : n, __ATOMIC_RELEASE);
            }
        } else if (click_target >= 0) {
            if (s->vtable == 0x14c60f8u) {
                MenuRect b = menu_16x9_bounds(w, h);
                float x = (mx - b.x) * 1280.0f / b.w;
                if (x >= 470.0f && x <= 530.0f) press = BTN_LEFT;
                else if (x >= 730.0f && x <= 798.5f) press = BTN_RIGHT;
            } else if (s->vtable == RETIRE_MENU_VTABLE && focus == 1) {
                /* Field Camera changes on Left / Right only (A does nothing there): its < arrow (x 675: owner's F12,
                 * 2026-11-20) steps left, the rest of the row right, cycling (owner: "make E on it cycle right") */
                MenuRect b = menu_16x9_bounds(w, h);
                float x = (mx - b.x) * 1280.0f / b.w;
                press = x >= 650.0f && x <= 705.0f ? BTN_LEFT : BTN_RIGHT;
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
