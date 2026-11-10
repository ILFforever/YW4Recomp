# 0033 — Widescreen: a wider camera, wider screen images, the UI kept 16:9 in the middle

<!-- decision
state: accepted
date: 2026-10-27
replaces: none
code: runtime/src/gpu.c, runtime/src/gpu_draw.c
-->

Status: accepted, first version (2026-10-27). The owner asked for widescreen/ultrawide next ("go wide").

## What we found

**The camera.** The main view's projection in the render state (`device+0x280`, device `main+0x56d8060`) has a
vertical field of view of 45° and 1.358 = 2.41421 / (16/9) across. Write watches led to the engine's camera objects:

| Camera offset | Value in town | Meaning |
|---|---|---|
| `+0x1a4`, `+0x1a8`, `+0x1ac`, `+0x1b0` | 0.785398, 1.77778, 0.1, 4000 | working field of view, aspect, near, far |
| `+0x1c0` | 1.358, …, 2.41421, … | the projection matrix (16 floats) |
| `+0x2e4`, `+0x2e8`, `+0x2ec`, `+0x2f0` | the same | the settings |

Every frame `f_97f510` walks the engine's camera list (`[main+0x5865720]`, count `main+0x5865728`) and copies each
camera's settings into its working fields. The projection is built from the **setting** `+0x2e8`: writing only the
working `+0x1a8` changed nothing, writing `+0x2e8` changed the projection (1.358 → 1.0347 for 21:9) and the picture
widened. The game's culling follows: objects at the new edges are drawn. A float 16/9 at `main+0x11fdf28` is only a
default that `f_53b3a0` (on-screen markers, it reads camera `+0x2e8`) uses when there is no camera; writing it at
start changed nothing.

**The images.** The screen-sized targets (docs/frame.md: 1600×900 and 1920×1080 with their smaller copies) carry
everything up to the composite; the UI is drawn after the bloom composite into a 1600×900 image; the game's last
draw copies that into one of **two** window images (1920×1080) that it presents in turn.

## Decision

1. **Camera:** a hook on `f_97f510` sets `+0x2e8` of every camera whose setting is 16:9 (or one of our shapes, so
   going back restores them) to the chosen aspect before the copy. The vertical view stays; the sides grow.
2. **Images:** every screen-sized target's width is also multiplied by the shape's factor against 16:9 (`ImageKey`
   `wide_num / wide_den`: 16:10 = 9/10, 21:9 = 4/3, 32:9 = 2). Viewports and scissors across follow it, as do the
   camera block's row 32 tap offsets (x only) and the present's crop.
3. **UI stays 16:9 in the middle:** a draw is UI when, in the same command list, it comes after the bloom composite,
   does not draw into one of the recently presented images (the window copies), and does not read a screen-sized
   image (the copy of the finished 3D picture into the UI image reads one). UI draws keep the game's width and are
   moved to the middle. TAA no longer shifts anything after the composite either (the rule it used, "after this
   frame's TAA by the frame counter", broke when render threads overlap).
4. **Setting:** ASPECT RATIO in the panel: 16:9 (the game's), 16:10, 21:9, 32:9 (`YW4_ASPECT`). The window already
   fits any aspect ratio with black bars.
5. **Full-screen layers are stretched** (added 2026-10-28): a UI draw that covers the game's whole screen is drawn
   across the full width instead of in the middle, so the dimming behind a dialog and the loading screen's black
   reach the sides. The runtime finds them without running the vertex program: the shader translator writes where
   a vertex program's position comes from into its interface (`m <buffer> <rows of x y z w>` when it is one constant
   buffer's rows dotted with input 0, as all four UI programs seen do; `m in` when it is input 0 as it is, the
   full-screen pass program), and `covers_screen` (`gpu_draw.c`) computes the corners of draws with 3 to 6
   vertices. Corners at the edges of clip space (±0.99 or beyond): the whole screen. Then, by the textures it
   samples:
   - a texture of exactly 16:9 shape from the game's files, a picture made for the screen (the copyright notice
     1920×1080, movies: 1920×1080 and two 960×540 planes, the dungeon map's backdrop 960×540): kept 16:9 in the
     middle, and the sides cleared to black first (`vkCmdClearAttachments`). Stretching made the copyright
     notice's silhouettes a third wider (owner: keep pictures 16:9); without the bars the 3D world the game still
     draws behind the bamboo forest's map showed at the sides (owner: black bars);
   - a texture of 16:9 shape the game renders into (a GPU-only memory pool: the map's UI, drawn into a
     1920×1080 image first): kept 16:9 like the rest of the UI, no bars;
   - anything else is stretched; a full-screen pass program only with no texture at all (it also copies images).

   The dimming samples a part of a 2048×2304 sheet and is stretched. The town's fast-travel map needs none of
   this: its tiles are drawn by a 3D model program (local-matrix block) with its own camera, which point 1
   widens, and they come out across the full width, showing more of the town (owner checked, 2026-10-28). The vertex and index reads are checked
   against the memory pools (`gpu_buffer_at`): checking them with `host_readable` (`VirtualQuery`) stopped the
   game at the title screen in every test run, though only reads were added. The cause was not found.

   **Widened 2026-10-31 (the diary):** the save diary's background stayed 16:9 with black sides while other
   menus stretched: its base layer is four quads in one draw (24 vertices) and its bars at the top and bottom span
   the width only. `covers_screen` now takes draws of up to 24 vertices and tells the whole screen (2) from the
   whole width (1); a draw across the whole width is stretched like one over the whole screen, unless it samples a
   16:9 picture (which still gets black bars only when it fills the screen; a band of one stays 16:9 in the middle).
   Checked at 21:9: the diary fills the width with its card and text undistorted; the copyright notice, title
   screen and loading screen unchanged. In a frame whose 16:9 picture got black bars (the map's backdrop), later
   bands across the width stay 16:9 too: the map's bottom bar stuck out into the right bar otherwise. Checked: the
   in-game save diary (D-pad right in town) fills the width, the map keeps its bars with the bar inside them.

6. **The battle's zoom** (added 2026-10-28): at a battle's start the game grows the 3D view in steps of 32×18
   from 1600×900 (1632×918 … 1888×1062, with copies at ½, ⅓, ¼ and ⅛). Those sizes were not screen-sized, so
   they stayed 16:9 and the swirl before the battle stayed at the sides for a few seconds. `zoom_chain` (`gpu.c`)
   makes them screen-sized: widened, but kept at the game's resolution. Making them 4K too ran the GPU out of
   memory within a few battles (`vkAllocateMemory` -2, frames up to 450 ms before it): every step is a new size,
   so a new set of images.

## How it was checked

Schoolyard, standing still and walking: 21:9 is 2133×900 at the 900 setting with the UI centred and the right size;
32:9 3200×900; with TAA at 21:9; the settings panel at 16:9 unchanged. The owner played at 21:9 / 4K (2026-10-28): exploring, battles
and cutscenes work; screenshots in the README.

Full-screen layers (point 5), 21:9 from the Yo-kai World save, screenshots from boot to after the "time changed"
dialog: the loading screen's black and the dialog's dimming reach the sides; the copyright notice, the LEVEL-5
movie, the autosave notice and the diary stay 16:9 with black sides, as before. At 16:10 and 32:9 the loading
screen and the copyright notice behave the same (the dialog had closed by then in those runs). 16:9 is unchanged
(nothing is checked there).

**16:10, the UI fitted (2026-11-11).** 16:10 is the only shape narrower than 16:9 (factor 9/10), so a UI kept at
the game's width stuck out of the image and lost both sides (the HUD and the Yo-kai Pad cut left and right).
Now, when the factor is below 1, `gpu_dynamic_state` (gpu_draw.c) fits the UI to the image's width: across it
takes the image's factor like the 3D, down it is shrunk by the same factor and moved to the middle, with a
45 px strip left above and below at 1600×900. Scissors follow. A whole-screen layer is fitted like the rest (so
its bands stay where the content on them is) and drawn twice more into the strips, showing only its outermost
rows (the layer 8 times as tall with its edge at the image's edge): a plain colour (dimming, loading black or
white) covers the whole image. A 16:9 picture (the copyright notice) gets its black bars above and below instead
of at the sides. The wider shapes take the old branch unchanged. Checked by the owner from screenshots: the
copyright and autosave notices, loading, the town HUD and the Yo-kai Pad.

**16:10, the in-game overlay's frame (2026-11-11).** The overlay (decision 0053) fits its 1600 x 900 design to
the picture's width at 16:10 and centres it, so a strip above and below it showed the game undimmed, with the
side panel and the key hints ending short (owner). Its frame now reaches the picture's edges (`gamemenu_draw`,
gamemenu.c): the tint and the side panel cover the strips, the header and the side buttons sit at the top, Exit
game, the version and the key hints (and Preview's bar) at the bottom; the pages and popups stay centred. The
mouse's click areas move with what is drawn. Checked by the owner at 16:10; 16:9 and wider have no strips and
are unchanged.

**World markers take the full width (2026-11-11).** The owner saw the icons that float over things (the "!"
above a yo-kai, the icon on a pole) drift away from their objects at 21:9, more the further from the middle.
A traced frame showed why: they are not UI. The game draws them after the composite as small quads placed with
the camera's (wide) projection, into the scene's image with the scene's depth attached and the depth test on, so
that walls hide them. Our UI rule kept them at the game's width in the middle like the HUD, which put each one
a quarter of its distance nearer the centre at 21:9 and made the icon as much narrower. Now a draw after the
composite that tests against a bound depth target is left out of the UI rule and gets the image's full width,
like the 3D (`gpu_dynamic_state`, gpu_draw.c). The HUD, the compass and the quest text use no depth and stay
in the middle. Checked by the owner at 21:9: markers on a pole and on a yo-kai sit on their objects. Not
checked: battles, 16:10 and 32:9, and other late draws with depth (a 3D model in a menu would now take the
full width too). The trace key (Shift+F11) records 12000 draws instead of 450: a frame in the forest is over
4000 and the markers come last.

## Not done yet

- 16:10: dialog dimming and battles not checked yet; bars at a menu's edge (the Yo-kai Pad's bottom bar) end
  45 px above the image's bottom, and the strip below shows the layer's edge colour.

- Full-screen layers seen so far: the ones above; the owner checked the Yo-kai Pad and both maps at 21:9 / 4K.
  Other menus (Yo-kai Pad apps, battle results) not checked yet: one of one colour stretches, a 16:9 picture
  gets black bars.
- A 16:9 picture fading in: the bars are black at once while the middle fades.
- At 16:10 the sides are cut, not the top: the vertical field of view is kept for every shape.
