# Yo-kai Pad mouse map

Map of every screen reached from the Yo-kai Pad (X menu), with its class vtable, control hash, layout and mouse
status. Positions are in the 1280 x 720 menu space. Work in progress (2026-11-16).

Method: `YW4_MOUSE_LOG=1` for the vtable and hash of each focus; `YW4_MOUSE_SCRIPT` for a scripted mouse;
F12 / script flag 256 for screenshots. Test saves are copies of `work/testsaves/save_base_lategame`.

## The Yo-kai Pad (yw::CMainMenu, vt main+0x14c0a88, grid hash 0x6098c54d)

How to open: X on the field (late-game save: clock popup first, A at about tick 4000, X at about 4300).
How to leave: B or a right click on the Pad closes it. The Pad opens on page 2 on this save.
Pages: a 4 x 3 grid, two pages. The first wheel notch goes to page 1; each later notch toggles (tested in a run).
Slot centres in 1280 x 720 space: x = 125 + 144 c, y = 215 + 143.5 r (screens[] "pad" row).
Mouse test set-up: replay `pad_live` (input ends at tick 4309, so the mouse is live). Important: while a replay still
runs, `window_input` (the mouse) is skipped, so a replay that runs on (e.g. to tick 9000) makes every mouse press
do nothing.

### Page 1 (5 icons, slots 0 to 4)

| Slot | Icon | Opens | Class vt (main-rel.) | OnChangeFocus hashes | Mouse today | Shot |
|---|---|---|---|---|---|---|
| 0 | Soulmates | 2 x 2 tile menu: Matching, Training, Custom Equip, Soul List (measured on work/mouse_map_proof/runJ_frame_004661.png: centres (440, 216), (840, 216), (440, 448), (840, 448); tiles about 328 x 200, so a 2 x 2 grid with dx 400, dy 232) | 0x14c0838 | 0x500ef853 (4 items) | not in screens[]: a click on a tile falls through to the Pad (run H); B closes it | work/mouse_map_proof/runC_frame_004631.png |
| 1 | Slimey Mark | "Slimey Marks" list (Found card on the right) | 0x14c2348, list view class 0x14c20d0 | 0xff59842f, then 0x795f8b82 (10 items) | not in screens[] (screens[] "trophies" is vt 0x14c9658, a different class); click opens it, B closes it | work/mouse_map_proof/runC_frame_004961.png |
| 2 | Ark Swap | message "The Ark Swap App isn't available right now." | 0x14bfa78 | 0x31092732, 0xf51efb06 | locked on this save; the message does not close on a right click (B) and no left click is taken | work/mouse_map_proof/runC_frame_005291.png |
| 3 | Ark Scan | run I: not checked (see the note on the locked boxes; its click came under the Diary message) | ? | ? | ? | ? |
| 4 | Treasure | "Treasure Photos" grid: 5 columns x 3 rows of photo cards on screen (Clue, Beginner Searcher, 30 cards in all, rows scroll) | 0x14c9488 | 0x14cc3aa3 (138 = 30), 0xafdaecc2 (lv 0x72299b9c80, vt 0x14c9210) | in screens[] as a 1 x 4 list: WRONG, it is a 5 x 3 grid; that is why only the Y axis works | work/mouse_map_proof/runH_frame_005350.png |

Soulmates' "Soul List" (under the tile menu) and the rest of the tile menu: not reached yet.

### Page 2 (12 icons, slots 0 to 11)

| Slot | Icon | Opens | Class vt | OnChangeFocus hashes | Mouse today | Shot |
|---|---|---|---|---|---|---|
| 0 | Members | popup "Main Members" / "Full List" (2 x 1) | 0x14c0a88 (Pad) | 0x48a3835c | in screens[] "pad_members" (row x0 268, y0 343.5, dx 145); both clicks work (runs D and H) | work/mouse_map_proof/runD_frame_004481.png |
| 0.1 | Members > Main Members | "Organize Members": Watcher card (Summer) on top, 3 x 2 grid of Yo-kai cards below | 0x14c1948 | 0x341fec29, 0x180cb413 (watcher), 0x634fbf8d (grid) | in screens[] "members_watcher" and "members_grid" | work/mouse_map_proof/runD_frame_004660.png |
| 0.2 | Members > Full List | "Organize Members" full list: Summer, Deadcool, Uribou, Damona, Komasan, Shuracon, Neco-XX, Hammy (8 rows, scrolls) | 0x14c1948 | 0x492db690, 0x252cc057 (lv 0x72299ba2b0), 0xceb4579b (10 items) | in screens[] "members_list" (8 rows); click opens it (run H) | work/mouse_map_proof/runH_frame_004675.png |
| 0.1.a.1 | Members > Main Members > Watcher card > Switch Moves | "Organize Members" moves page for the watcher: Equipment (Enormous Tree Spear, Jingeki Armor) and a Moves list (HP Beam II, Yo-kai Beam III, Battle Drone I, Ally Attack Up II, Party Morph Deva, ...); the right side shows the selected item's text | 0x14c1948 (same class as Organize Members) | 0x38295b2, 0xf56ee633 (lv class 0x14b8070, 7 rows), 0x7112e669 (138 = 7), 0x9f9d7687 | not in screens[]: the click on Switch Moves works (DOWN step, then A opens it, run K), but the page's list is not mouse-enabled | work/mouse_map_proof/runK_frame_006235.png |
| 0.1.a | Members > Main Members > Watcher card | "What will you do?" popup: Switch (520, 270), Switch Moves (520, 318), Set Leader (520, 366), Change Name (520, 414), each about 180 wide; Remove and Reset Name below (not on screen) | 0x14c7b78 | 0xb2869aec (4 visible) | in screens[] "members_action" (x0 519.6, y0 268, dy 48): the measured rows match it (checked on the grid, 2026-11-16) | work/mouse_map_proof/runD_frame_004825.png |
| 1 | Diary | message "The Diary App isn't available right now." (locked on this save) | 0x14bfa78 | 0x31092732, 0xf51efb06 | not a page; the right click does not close it (run I: it stayed up for every later click) | work/mouse_map_proof/runI_frame_004496.png |
| 2 | Inventory | list of items (Food: Plum Rice Ball...) with description card | 0x14c0838 | 0x50937ef4 (lv 0x17001a6a) | in screens[] "inventory" | work/mouse_map_proof/runD_frame_005725.png |
| 3 | Quests | "Quests" list with a case card (Cases) | 0x14c0838 | 0xb4eb3a55 (lv 0xaab2a496) | in screens[] "quests" | work/mouse_map_proof/runD_frame_006055.png |
| 4 | Help | Help list (RPG: Walk Around the City ...) | 0x14c00f8 | 0x0142cd83 (lv 0x9ee9efdf) | in screens[] "help" | work/mouse_map_proof/runD_frame_006385.png |
| 5 | Settings | Settings list (Control Settings: camera speed ...) | 0x14c60f8 | 0x857cd4fe (lv 0xc3a04661) | in screens[] "settings" (its left/right arrows at x 470-530 and 730-798.5) | work/mouse_map_proof/runD_frame_006715.png |
| 6 | News | "Update Info No. 04" notice (a big picture with the DLC news, a Notices button); B closes it | 0x14bfa78 | 0x1405a5f7 (same hash as the watch_skills row in screens[]: it is NOT Watch Skills) | not in screens[]; click opens it, B closes it | work/mouse_map_proof/runE_frame_004496.png |
| 7 | Play Record | a stats page (Total played time, No. of entries in Ukiukipedia, Most used Yo-kai, Most used humans); no list | 0x14c4558 | 0xa17f69cc (138 = 10), then 0x04f7643f (list view lv 0x722999de90, vt 0x14c42e0) | in screens[] "record" (wheel only, x0 365 y0 191.5); not a list, the wheel does nothing visible; B closes it | work/mouse_map_proof/runE_frame_004841.png |
| 8 | Lambert Post | message "The Lambert Mail App isn't available right now." (locked on this save) | 0x14bfa78 | 0x31092732, 0xf51efb06 | not a page; the right click does NOT close this box (it stayed up for every later click in run E) | work/mouse_map_proof/runE_frame_005186.png |
| 9 | Trophies | "Yo-kai Trophies" list (All Trophies 26%; rows with GET marks; a description card). An "Earned a new Trophy!" box can appear over it | 0x14c9658 | 0x77e65489, 0xffb21636 (lv), 0xb3f13b48 (the list rows, 10 items) | in screens[] "trophies" (hover and wheel only, no SCR_CLICK); B closes it | work/mouse_map_proof/runF_frame_004839.png |
| 10 | Watch Skills | "Watch Skills" page: six characters standing in a row (Cole, and five others), a hexagon with Yo-kai, Friend, Body, Technique, Soul, Love points | 0x14c6b78 (NOT 0x14bfa78) | 0x5b8ae332 | in screens[] "watch_skills", but with the wrong vtable (0x14bfa78 is News's box) and a wrong 6 x 1 layout (x spacing 188); characters at about x 144, 264, 384, 504, 640, 744, y about 420 (from the screenshot) | work/mouse_map_proof/runF_frame_004494.png |
| 11 | Ukiukipedia | category list: Yo-kai (83), Boss Yo-kai (7), Special Yo-kai (9), Humans (27) | 0x14befd8 | 0xf3b132d2, 0x4c41ffe9 (4 rows) | in screens[] "ukiukipedia" (4 rows, hover, click, wheel) | work/mouse_map_proof/runF_frame_005184.png |

Notes:
- Watcher card click: the first click (on the watcher card) opens "What will you do?". The popup's Switch Moves row:
  from the screenshot, at about (512, 368) in menu space (cell estimate, +-5); screens[] puts row 1 at y 316.
  Needs a precise check before the fix.
- Members popup: B closes it and returns to the Pad. While it is up, clicks on the Pad's icons go to the popup, so
  Diary was not reached.
- Each sub-page's B goes back one level (Members screen -> popup -> Pad).

## Watch Skills: why its characters cannot be clicked (run G, 2026-11-16)

The character clicks at about (144, 420), (264, 420), (384, 420), (504, 420) fell through to the Pad under the page:
the log shows the Pad's grid focus stepping left and right and the Pad's Help, Settings, News and Play Record
opening. The Watch Skills page (vt 0x14c6b78) is not in screens[], so the mouse still thinks the Pad is in front.
So the fix is a screens[] row for vt 0x14c6b78, hash 0x5b8ae332, with the six character slots. Check the slot
positions on a screenshot before writing them (grid: `work/mouse_map_proof/runF_watch_grid.png` style, 80-unit lines).

## Fixes made (2026-11-16, after the map; runtime/src/mousemenu.c)

- `watch_skills`: vt 0x14bfa78 -> 0x14c6b78, hash 0x1405a5f7 -> 0x5b8ae332, x0 144, y0 420, dx 120, hw 60, hh 220.
  Tested (run L/M): Cole's character slot opens her skill tree with a click and returns with B. The hover step to the
  second character sends RIGHT but the focus does not move, so slots 2 to 6 still cannot be reached by mouse (open).
- `soulmates_tiles` (new row): vt 0x14c0838, hash 0x500ef853, 2 x 2, x0 440, y0 216, dx 400, dy 232, hw 164, hh 100.
  Tested (run N): the Soul List tile opens the Soulmates list (vt 0x14c0498, "Hot Pot Soul" list) by hover and click.
- `slimey_marks` (new row): vt 0x14c2348, hash 0x795f8b82, lv 0xff59842f, 1 x 8, x0 396, y0 162, dy 58.3, hw 332, hh 28.
  Tested (run N): the Slimey Mark icon opens the list. The row click is sent (not yet proven by a sub-page).
- `treasure`: now 5 x 3 (cols 5, rows 3, x0 176, y0 180, dx 229, dy 128, hw 108, hh 60), no wheel.
  Test: run O (see the status below).
- `trophies`: SCR_CLICK added (geometry not re-measured; it is the Yo-kai Trophies list on Pad page 2).
- Focus hook (`yw4_hook_menu_focus`): when the active screen's own hash takes the focus again while a page is open on
  the same menu, the page counts as closed (Watch Skills' skill tree left its layers unbalanced, so the screen stayed
  "covered" and the mouse ignored it: `front=0` in the log, run M).

## What is still unknown (end of mapping, 2026-11-16)

- Soulmates' Soul List page: needs the tile menu in screens[] first (then the click can get there).
- Switch Moves page: which list the rows belong to (the equipment or the moves list), and which control takes the focus.
- Trophies (page 2) rows: whether a click opens a detail page or only focuses the row.
- Watch Skills characters: the focus index for each character (not measured yet; the characters fell through to the Pad).
- Ark Scan and Ukiukipedia's inner lists: not reached (Ark Scan is a locked box most likely).
- Play Record: a stats page, no list.

## Notes from runs H to J

- Locked boxes: "The Ark Swap / Lambert Mail / Diary App isn't available right now." Each one takes all clicks until it
  is closed by A, and a right click does not close it. None of them is in screens[]. So after one of these, the rest of a
  run's clicks do nothing. Visit them last. A possible fix: a screens[] row for the box (vt 0x14bfa78, hash 0x31092732,
  one slot on its button, click = A).
- Members popup: the right click on the Pad closes the Pad (not the popup), so do not send an extra B on the Pad.
- The Watch Skills, Soulmates' tile menu and Treasure grid are all clicked wrongly today (see the rows above).

## Watch Skills: the character focus (2026-11-16, runs Q, R, S)

- Key-only replay runs Q and R (Pad -> Watch Skills, then RIGHT presses on the character select): the highlight moves
  Cole -> Summer -> Hammy -> Katie -> Bruno -> Dean (proof: work/mouse_map_proof/watch_skills_keys_right_steps.png).
  The game sends NO OnChangeFocus for hash 0x5b8ae332 on those moves: its control keeps focus index 0. So the
  screens[] focus cannot read the character.
- Fix in mousemenu.c: the mouse keeps its own character count (`wsk_index`): 0 when the screen opens from the Pad,
  +1 per RIGHT and -1 per LEFT the mouse sends. Built; pytest 71 passed.
- Not proven in a mouse run: run S (scripted mouse) failed before the screen opened. The Pad's A press on Watch Skills
  (slot 10) was lost: the log shows the focus reach slot 10 at the same time as the A press, and no Watch Skills
  focus followed. The same click worked in run M. Needs a retry (add a short wait after the last Pad step).

## Status of the five open issues (2026-11-16)

| Issue (decision 0057) | Status | Note |
|---|---|---|
| 1. Watch skills (characters cannot be clicked) | partly | Character 1 (Cole) opens with a click. Characters 2 to 6: the game's highlight moves with RIGHT/LEFT but is not reported through OnChangeFocus; a mouse count (`wsk_index`) now drives the steps. Built, not yet proven in a mouse run (run S lost the Pad click). |
| 2. Soulmate (page buggy) | partly | The tile menu now works (Soul List opens by hover and click). The Soul List's own rows are the old "soulmate" row (not re-tested). |
| 3. Slimey marks (Trophies) | partly | The Slimey Mark list opens from its icon and has a row (new `slimey_marks` row). The row click is sent; not yet shown to open a sub-page. Trophies (Pad page 2) got SCR_CLICK, not re-measured. |
| 4. Treasure photos (only Y works) | fixed (run P) | Was a 1 x 4 list; it is a 5 x 3 grid. Hover moves across columns and rows, a click opens the photo, B goes back. |
| 5. Organize Members -> Switch moves | partly | The click on Switch Moves works (run K). The moves/equipment page it opens is not in screens[] yet (its list hashes are in the map). |

Also found: locked boxes ("Diary / Lambert Mail / Ark Swap App isn't available") block all clicks until A is pressed,
and a right click does not close them. Not fixed; a possible row for the box is in the notes above.

The Pad focus fix (page closed when the screen's own control gets focus back) is in `yw4_hook_menu_focus`.

## The map and Mirapo's list (2026-11-18)

Both are the map's class (vt `main+0x14c14f8`). The plain map focuses `0xb05d3ff0`, `0xc9818754`, `0xde413e27`
and `0xf56c6de4`; Mirapo's small screen over it focuses its rows as `0xb730fbe9` (visible row at `+0x13e`) and its
list view as `0xc037cb7f` (9 places, 8 shown). On the plain map moving the mouse pans and the wheel zooms; while
Mirapo's screen is up (`mousemenu_mirapo`) the mouse is a menu's instead: no panning (it moved the selection), the
wheel steps the rows, left click picks, right click goes back. Confirmed by the owner in game.
