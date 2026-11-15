# 0075 — The shortcut wheel: hold L's key, point with the mouse

<!-- decision
state: accepted
date: 2026-11-15
replaces: none
code: runtime/src/shortcutwheel.c, runtime/src/shortcutwheel.h, runtime/src/window.c, runtime/src/bindings.c, runtime/src/gamemenu.c, recomp/overrides.toml, tools/yw4/wheel_icons.py, tools/yw4/bc7.py, recomp/build.py, recomp/ci.py, tools/yw4/build_install.py
-->

Status: accepted. Built and tried in scripted runs (`YW4_WHEEL_TEST`, 2026-11-15): shortcuts 1-4 work (Auto-Navi,
Diary, Bike on and off, Naviwoof Guide), the hold-L four through Mirapo (its list opens), with the green "on"
state, locked and "not usable here" slots. Not yet played by the owner with a real mouse.

## Context

The game has eight field shortcuts on the D-pad: four on their own (Auto-Navi, Diary, Bike, Naviwoof Guide) and four
while L is held (Change Watchers, Mirapo, Change Yo-kai Companion, Change moves and Equipment). With the starting
keys the D-pad is the arrow keys and L is 1: a long reach from WASD. The owner wanted the mouse to pick them instead.
Ideas tried on paper first (work/mockups/shortcut-wheel): WASD as the D-pad while 1 is held, blind mouse flicks,
two hold keys, then a wheel with all eight. The owner chose a wheel, then Steam's radial menu as the model, then the
"Searchlight" look out of six styles (Watch Mode's teal lens finds the pick, the rest dims, the name in the middle).

## What it does

- **Exploring only.** It opens when a key bound to L in the exploring set is pressed (1 to start) and the game has
  the mouse, outside battles, the game's menus (the Yo-kai Pad, conversations, the map, the Stopwatching menu), the
  overlay and its pause. Elsewhere the key is plain L, as before.
- **While it is open** the game still gets L (2026-11-16, the owner's hand test: with L held back the cross never
  turned its page and 1 + an arrow key did nothing), and the mouse's movement goes to the wheel instead of the
  camera. A pointer collects the movement (at most 140 mouse pixels from the middle); past 100 (first 40: the owner
  wanted to come closer to a slot) it points at one of eight sectors of 45 degrees.
- **Letting go of the key** uses the slot pointed at: the D-pad direction for 6 reads of the game's input (slots 1-4),
  or L and the direction (slots 5-8). The game takes the hold-L four only once its HUD has turned to their page,
  about a second after L goes down (3 and 12 reads of L did nothing, 60 opened Mirapo's list). L has been held since
  the wheel opened, so on letting go only the rest of the 60 reads is waited. A D-pad slot lets L go 8 reads first
  (the cross back on its page), then presses the direction. Let go in the middle: nothing.
- **The old way still works:** an arrow key (any D-pad press) while the key is held closes the wheel and lets L
  through until the key is let go (owner's answer, 2026-11-15).
- **When it doesn't open** the log says why ("key down, not opened: map"). Found that way in the owner's test: after
  Mirapo's list (the map's menu class) was backed out of, the field's HUD menu never focused again, so the map
  counted as open for good; the cross's next update now ends it (mousemenu_field_hud_seen). Mirapo's "warp here?"
  Yes / No box turned the Stopwatching menu's flag on outside a battle (a Yes / No box now counts only over that
  menu). And after a game menu the mouse wasn't always taken back: the wheel opens if the window has the keyboard,
  and takes the mouse itself.
- **On / Off** on the Controls page (Mouse shortcut wheel), saved as `YW4_SHORTCUT_WHEEL` in settings.ini (owner's
  requirement). Off: nothing of it runs.

Why the ring order: the four D-pad shortcuts on the straight lines where the D-pad points, the hold-L set on the
diagonals. A player who knows the pad finds the first four where they expect them.

## How it is drawn

As F2's quick look is (decision 0053's overlay canvas, `menu_draw` in window.c): drawn on the CPU with ui.c into a
see-through canvas and blended over the running game, only its square, in the overlay's 1600 x 900 design units.
The look is style A "Refined" of the second styling round (work/mockups/shortcut-wheel/styles3.html, the owner's
pick): a cone of light from the middle to the pick, a scope lens with an inner glow and corner marks, the rim lit
where it points, faint plates under the icons. The soft light is drawn per pixel, only over the box it covers.
Animations: opening grows from 85 % and fades in (120 ms) with the icons arriving 15 ms apart; the lens glides to a
new pick (80 ms); using one flashes the lens and fades (100 + 80 ms); cancelling shrinks to 90 % and fades
(100 ms). The picture is redrawn every frame only while something moves, else only when the pick changes.
After the owner's hand test (2026-11-16): the middle circle is the pointer (it follows the mouse, full size, and
hides once a slot is picked); the other icons aren't dimmed (the pick is already larger); a long name goes on two
lines (it ran into the side slots).

## What the wheel reads from the game

The field HUD's shortcut cross (read with Ghidra, `work/ghidra/hud_guide*.c`, game code never in the repo):
- **Its object**, kept by a hook at the cross's update (main+0x755ea0, x0). The wheel opens only once it exists (so
  not before the first load). A first try, "the field HUD had the last menu focus", blocked it in the late-game save.
- **Locked** (not unlocked in the story): D-pad up / right / down / left = object bytes +0x5ea / +0x5eb / +0x5ed /
  +0x5ec; the hold-L four +0x600..0x603, only when +0x5ff says the game has the hold-L page at all (an early save
  has none: its cross shows no "Hold"). Drawn as a dashed outline and a lock; letting go there uses nothing.
- **Not usable here** (the cross's grey-out, main+0x758e20): +0x604 + page * 4 + direction. Set for the D-pad four
  inside Mystic Grove, where the game blocks them. Drawn in grey, with "not usable here" under the name.
- **On** (the HUD's green disc): the state the game gives each button through main+0x756520 (hooked: x1 the button,
  w2 on): 0xe Auto-Navi, 0x11 Bike (0xf Diary and 0x10 Naviwoof are buttons too, never "on"). The ids are the
  cross's places, shared by the hold-L four: while the game has L (and 8 reads after) its reports are theirs
  (Change Watchers set Auto-Navi "on", opening the wheel turned both off), so they are ignored then.

## The game's icons

The icons are the field HUD's own (its "guide" sheet, `rpg00_01.g4tx` in menu.cpk, a BC7 texture). They are game
data, so they follow decision 0048's icon: `tools.yw4.wheel_icons` takes them from the player's RomFS at build time
into `<work_dir>/wheel/icons.bin`, and recomp.build writes them into a generated `wheel_icons.c`. The launcher's
build runs it before compiling. Without the file the wheel draws D-pad arrows.

Players' builds have no Pillow, so `tools/yw4/bc7.py` decodes BC7 in plain Python, only the blocks the icons cover
(tests/test_bc7.py checks it against Pillow on random blocks of every mode; the real icons match byte for byte).

## Not done yet

- All eight used by hand in the late-game save (2026-11-16); not with a controller (the wheel is the mouse's).
- The names are the English patch's words in the code; a Japanese game shows them in English.
- The HUD object could be stale for a moment after an area change (a new HUD is built; the hook then updates it).

## Test hooks

`YW4_WHEEL_TEST=<read>:<slot>[,...]` holds the key for 40 input reads from each read and points at the slot (0: the
middle), and takes a screenshot of the open wheel (or at `YW4_WHEEL_SHOTS=<reads>,...` after each start: past 40
the use or cancel animation); `YW4_WHEEL_LOG=1` logs what it does, the HUD's bytes and its button states. Input
reads come about twice per drawn frame, so `--shots` frames can't aim at it. Replays (`s` lines) give buttons in
hex. In save_base_busy rain starts a few seconds in and takes the player off the bike.
