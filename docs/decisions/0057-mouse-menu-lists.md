# 0057 — Mouse support in Yo-kai Pad pages and scrolling lists
<!-- decision
state: accepted
date: 2026-11-08
replaces: none
code: runtime/src/mousemenu.c, runtime/src/mousemenu.h, runtime/src/window.c, docs/debugging.md
-->

Status: accepted (2026-11-08). Extends the Yo-kai Pad mouse prototype in `runtime/src/mousemenu.c` to the pages opened
from the Pad (item lists and "Use on who?" Yo-kai selection list, quest lists, scrolling menus, Ukiukipedia categories and entry list,
Organize Members Watcher card, 3x2 party grid, and "What will you do?" character action popup, Watch skills character selection,
Treasure photos, Soulmate, and Settings left/right arrow clicks) and adds mouse wheel scrolling in lists and page cycling on the
Yo-kai Pad main screen.

## Context

Commits `abdbb2f` and `359c251` added mouse support to the Yo-kai Pad (`yw::CMainMenu`, vtable `main+0x14c0a88`, grid
control hash `0x6098c54d`): while the Pad is in front, hovering an icon steps the focus there with D-pad presses, a left
click goes there and presses A, and a right click presses B.

When the player opens a page from the Pad, the Pad's layers close and a child `yw::CMenu*` object opens on top. Most of
these pages are vertical lists or fixed-row menus that derive from `yw::CMenuCursor`:

- **Scrolling lists backed by `lives::CMenuListView` (`vt = main+0x14b8520`)**:
  - **Inventory** (`yw::CMenuStopWatchItem`, `vt = main+0x14c0838`, `lv_hash = 0x17001a6a`, item control `0x50937ef4`, 8 visible rows)
  - **Inventory "Use on who?" Yo-kai Selection List** (`yw::CMenuStopWatchItem`, shares `vt = main+0x14c0838`, `lv_hash = 0x802b98b6`, item control `0x4e925801`, 6 visible rows for party members to receive the item)
  - **Quests / Story / Cases** (`yw::CMenuQuestOrder`, shares `vt = main+0x14c0838`, `lv_hash = 0xaab2a496`, item control `0xb4eb3a55`, 8 visible rows)
  - **Info / Play Record** (`yw::CMenuRecord`, `vt = main+0x14c4558`, `lv_hash = 0x04f7643f`, item control `0xa17f69cc`, 8 visible rows)
  - **Settings** (`yw::CMenuSetting`, `vt = main+0x14c60f8`, `lv_hash = 0xc3a04661`, item control `0x857cd4fe`, 7 visible rows)
  - **Help** (`yw::CMenuHelp`, `vt = main+0x14c00f8`, `lv_hash = 0x9ee9efdf`, item control `0x0142cd83`, 8 visible rows)
  - **Yo-kai Trophies** (`yw::CMenuTrophyObj`, `vt = main+0x14c9658`, `lv_hash = 0xffb21636`, item control `0xb3f13b48`, 8 visible rows)
  - **Treasure Photos** (`yw::CMenuTreasure`, `vt = main+0x14c9488`, `lv_hash = 0xafdaecc2`, item control `0x14cc3aa3`, 4 visible rows)
  - **Soulmate** (`yw::CMenuKonkatsuList`, `vt = main+0x14c0498`, `lv_hash = 0x596edf43`, item control `0x933f4e58`, 8 visible rows)
  - **Ukiukipedia Entry List** (`yw::CMenuDictionaryTop`, shares `vt = main+0x14befd8`, `lv_hash = 0xb5ac8c55`, item control `0xca481af3`, 8 visible rows)
  - **Members — Full List** (`yw::CMemberMenu`, `vt = main+0x14c1948`, `lv_hash = 0x252cc057`, item control `0xceb4579b`, 8 visible rows)
  - On these screens, `f_9f3e90(menu, lv_hash, 0) + 0x118` holds the `lives::CMenuListView*` (`lv`). The item control's
    `+0x138` (`u16`) is the ring-buffer slot count (typically `rows + 2`, with `s32` at `lv + 0xb0 = 1` top buffer row)
    and `+0x13c` (`u16`) is the ring-buffer slot index (`0 .. slot_count - 1`), which wraps modulo `slot_count` as the
    list scrolls. The `CMenuListView` stores the absolute selected item index at `lv + 0x11c` (`s32`), the total item
    count at `lv + 0x128` (`s32`), and the scroll base at `lv + 0x114` (`s32`, initialized to `-ld32(lv + 0xb0)` at the
    top of the list, as seen in `f_9d31a0`, `f_9d4480`, `f_9d5530`, and `f_9d6550`). Thus the first visible item index
    is `top = ld32(lv + 0x114) + ld32(lv + 0xb0)` and the focused visible row on screen (`0 .. rows - 1`) is
    `ld32(lv + 0x11c) - top`.
- **Fixed-row menus and sub-grids (no `CMenuListView`, `lv_hash = 0`)**:
  - **Pad — Members sub-popup** (`yw::CMainMenu`, `vt = main+0x14c0a88`, hash `0x48a3835c`, 2x1 icons: *Main Members* / *Full List* on
    the Pad itself).
  - **Ukiukipedia Categories** (`yw::CMenuDictionaryTop`, `vt = main+0x14befd8`, hash `0x4c41ffe9`, 1x4 rows).
  - **Members — Watcher card** (`yw::CMemberMenu`, `vt = main+0x14c1948`, hash `0x180cb413`, 1x1 top card).
  - **Members — Main Members** (`yw::CMemberMenu`, `vt = main+0x14c1948`, hash `0x634fbf8d`, 3x2 party card grid).
  - **Members — "What will you do?" popup** (`yw::CMenuSubWindow`, `vt = main+0x14c7b78`, hash `0xb2869aec`, 1x6 action choices:
    Switch, Switch Moves, Set Leader, Remove, Change Name, Reset Name).
  - **Watch skills** (`yw::CMenuGeneralWindow`, `vt = main+0x14bfa78`, hash `0x1405a5f7`, 6x1 character card row).

## Decision

- Replace the single-screen Pad state in `runtime/src/mousemenu.c` with an 18-entry `screens[]` table describing each
  known screen's `vtable`, `hash`, optional `lv_hash`, grid/list dimensions (`cols`, `rows`), and measured
  1280 x 720 slot geometry (`x0`, `y0`, `dx`, `dy`, `hw`, `hh`).
- Support bidirectional navigation between sibling controls on the same menu:
  - In Organize Members (`yw::CMemberMenu`), the Watcher card (`members_watcher`, hash `0x180cb413`) and the 3x2 party
    grid (`members_grid`, hash `0x634fbf8d`) are separate child controls of the same menu. When the Watcher card is
    focused, hovering or clicking a grid card sends `BTN_DOWN` to transition focus to the grid; when a grid card is
    focused, hovering or clicking the Watcher card sends `BTN_UP` to return focus to the Watcher card.
  - During `CMemberMenu` initialization, both `members_watcher` and `members_grid` receive focus calls; `yw4_hook_menu_focus`
    ignores `members_grid` while `layers_open == 0` and the previous screen was `members_watcher`, preserving initial
    Watcher focus so the two sibling controls do not fight during menu open.
- Support value-change arrows on Settings (`yw::CMenuSetting`, `vt = main+0x14c60f8`):
  - When the user clicks the left arrow area (`x in [470, 530]` in 1280x720 menu space), send `BTN_LEFT`.
  - When the user clicks the right arrow area (`x in [730, 798.5]`), send `BTN_RIGHT`.
  - Avoid setting `step_from` on arrow clicks since row focus does not change.
- Prevent relative mouse capture during menu interaction:
  - In `runtime/src/window.c`, guard the SDL relative mouse capture on left click with `!mousemenu_open()` so clicking
    on menu items does not capture or hide the mouse cursor.
- Track the active screen (`active_idx`, `active_menu`) in `yw4_hook_menu_focus`, `yw4_hook_menu_layer_opened`, and
  `yw4_hook_menu_layer_closed`:
  - When a non-Pad screen receives focus, switch `active_idx` to that screen and reset pending D-pad/click state (preserving
    state across sibling transitions between `members_watcher` and `members_grid`).
  - When a child popup layer closes (e.g. `item_action` or `pad_members`), restore the underlying parent menu screen.
- For `CMenuListView`-backed screens (`lv_hash != 0`), read `top = ld32(lv + 0x114) + ld32(lv + 0xb0)` and
  `sel = ld32(lv + 0x11c)` so hover and click map visible rows `0 .. rows - 1` even after scrolling past the first page,
  and clamp hit-testing to `top + row < ld32(lv + 0x128)`.
- Forward `SDL_EVENT_MOUSE_WHEEL` in `runtime/src/window.c` to `mousemenu_wheel(dir)` before gameplay wheel bindings:
  when a list screen (`cols == 1`) is in front, queue one `BTN_UP` (`dir > 0`) or `BTN_DOWN` (`dir < 0`) step per wheel
  notch; when the Yo-kai Pad grid (`pad`, `cols > 1`) is in front, alternate `BTN_R` and `BTN_L` (`pad_page ^= 1`,
  reset when the Pad closes) so scrolling in either direction cycles between Page 1 and Page 2 (since `L1` and `R1`
  clamp at Page 1 and Page 2 in `yw::CMainMenu`); cancel any in-flight hover target.
- Extend `YW4_MOUSE_SCRIPT` to accept an optional 5th column `<wheel>` (`<ms> <x> <y> <buttons> [<wheel>]`) and add
  `YW4_MOUSE_LOG=1` for menu focus and state logging.

## Consequences

- The Yo-kai Pad and the main pages opened from it (Inventory, Quests, Info/Record, Settings, Help, Trophies,
  Ukiukipedia, and Organize Members) respond to mouse hover, left-click selection, right-click back, mouse wheel
  scrolling in lists, and mouse wheel page cycling on the Yo-kai Pad main screen.
- Gameplay mouse wheel bindings (`window.c`) remain untouched when no supported menu is in front.

### Open issues (owner playtest, 2026-11-08)
The rest of the Yo-kai Pad pages are assumed working; remaining issues to fix:
- **Watch skills**: still broken (character cards cannot be clicked).
- **Soulmate**: page is buggy.
- **Slimey marks (Trophies)**: page not working.
- **Treasure photos**: only Y axis working.
- **Organize Members → Switch moves**: not working.
