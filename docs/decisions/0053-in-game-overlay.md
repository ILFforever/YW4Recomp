# 0053 — The in-game overlay: the game's own pause, a side panel drawn on the CPU from its design

Status: **accepted** (2026-11-05; the owner chose the buttons, the fonts and that it must match the design 1:1).

## Context

The owner wanted a Steam-like overlay inside the game: a side panel with Resume, Settings, Controls, Performance,
Take screenshot and Exit game, usable with a controller, in the launcher's purple and gold, with the game paused
behind it. Its look was settled first as a design (the overlay canvas, `Main.dc.html`, 1600 × 900). Three questions:
how to pause the game, how to draw a see-through, rounded, font-rich panel when the window only copies overlays
onto the picture, and which buttons open it.

## Decision

**Pausing: the game's own pause, its screen hidden** (`runtime/src/pause.c`). The game's main loop is a state
machine (`f_846d20`); Plus in the field switches it to `yw::MenuState`, whose Enter (`f_866540`) opens a
`SceneMenu` with the menus `gameover_menu` + `pause_menu` (the "Paused" screen), and whose Exit (`f_866cc0`) runs
once Continue has closed it. The overlay presses Plus for the player, keeps the picture of that moment and shows
it while paused, so the pause screen underneath never shows; resuming presses A on Continue until the Exit hook
says the game has left its pause. Rejected:

- *Losing focus* (FocusStateChanged, as the Home button): the game asks not to be suspended and ignores it.
- *MenuState without the pause screen* (the scene-open hook handing back "no scene"): the player and camera stop,
  but people and Yo-kai on the map keep moving and Whisper comes out; the pause scene is what freezes the world.
- *Guessing from the picture* when the screen opened or closed (the first version): failed at 30 frames/s.

**Drawing: on the CPU, in the design's units** (`runtime/src/ui.c`, `gamemenu.c`). The window copies overlays with
`vkCmdBlitImage`, without blending. While paused the picture doesn't change, so the kept picture is read back once,
and the overlay is put together on the CPU: the picture scaled to the window, the design's tint, then the panel
drawn over it, uploaded as one image only when what it shows changes. The design's numbers (CSS pixels) are used
as they are, scaled by the picture's height / 900, so it matches the design at any window size. Rounded boxes are
drawn by coverage (a signed distance per pixel), text with stb_truetype, the design's line icons with nanosvg.
A GPU pipeline with blending would also work, but needs shaders and a render pass for something that changes only
on a key press.

The design's fonts (M PLUS Rounded 1c, Zen Kaku Gothic New; SIL OFL 1.1) are built into the exe with `#embed`,
cut down to Latin (about 190 KB in all, `third_party/fonts`); nanosvg (zlib) draws the icons (`third_party/nanosvg`).
The header's logo is the game's own icon, read back from the exe's icon resource (`gamelogo.c`): it is game data,
put in by the build from the player's files (decision 0048), never in the repo.

**Pages use the real data**: Settings shows the player's settings by section (`settings_player_rows`,
`settings_change`), Controls the bindings with their own selection and key capture (`bindings.h`), Performance the
HUD's form (off, compact, detailed), corner and parts, saved with the settings and previewed with the HUD itself.

**Buttons** (the owner's choice): Escape on the keyboard (no longer the keyboard's second B); the touchpad on
PlayStation pads, the button SDL calls MISC1 on the others (Share on Xbox Series, Capture on Switch Pro), and Back +
Start on any pad (Xbox One and older have no Share). On a DualSense MISC1 is the mic button, which toggles the HUD.
Home / PS is left alone: Steam opens Big Picture on it. In the overlay W/S choose, D enters a page, A leaves it,
Q/E or LB/RB jump between sections, Left/Right change a value.

## Consequences

- Once drawn, the overlay costs one image copy per frame; a change (a key press) redraws it on the CPU (not timed
  yet).
- Where the game can't pause (cutscenes), the overlay still opens, over the picture of that moment, while the game
  runs on behind it.
- If a setting changes the picture's size while paused, the overlay keeps the earlier picture behind it until the
  game resumes.
- The F1/F2 panel stays as it was (its developer page is only there); keyboard players can use either.
