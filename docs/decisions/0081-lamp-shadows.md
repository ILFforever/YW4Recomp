# 0081 — Lamp shadows (experimental): shadow maps of the characters for the nearest lamps

<!-- decision
state: accepted
date: 2026-11-19
replaces: none
code: runtime/src/lamp_shadow.c, runtime/src/lamp_shadow.h, runtime/src/gpu_shadow.c, runtime/src/shadow.c, runtime/shaders/lamp_light.frag, runtime/src/gpu_draw.c, runtime/src/settings.c
-->

Status: accepted as experimental (owner, 2026-11-19: "good enough, mark as experimental"; played it on the night saves;
strength, filter, image size and fade chosen by them). The settings panel's SHADOWS > POINT LIGHT SHADOWS row (the
standard PC name, and not "lamp": the game's lamp lights are also porch lights and lit windows; its help text begins
EXPERIMENTAL): OFF / LOW / MEDIUM / HIGH / MAX, OFF by default for
everyone (MEDIUM recommended). F4 switched it during testing; F4 now does nothing.

## Context

Only the sun casts shadows in the game. At night the street lamps, porch lights and windows are point lights (the
lamp program `42759b`, ours since decision 0046: volumes drawn at a third of the screen size and added up), and
people and Yo-kai standing under them throw none.

## What was tried first: a march across the screen

The owner first chose short screen-space shadows (the contact shadows' method, decision 0045, aimed at each lamp): a
12-step march from each lit pixel toward the lamp across the depth. Short rays (0.5 m) only gave a blob under the feet
that lifted with them, as only the feet are that close to the ground; long rays (2.5 m) gave a body's shape, but it
floated off the feet and came out grainy: the depth buffer holds only the front of a character, so near the feet the
march can't tell whether a ray passed through the legs or behind them. Owner: "lamp shadows is just a blob unlike the
original game". Replaced by real shadow maps; the march is gone.

## Decision

For the few lamps nearest the characters, draw the characters into a cube of six depth images per lamp (90-degree
views, 512 × 512, D32), as point lights' shadows usually are, and look each lamp-lit pixel up in its lamp's cube.

- **The lamps** come from the lamp draw itself (`lamp_shadow_collect`, at the draw on the GPU executor): every vertex
  of a lamp's volume carries its world position and radius (vertex program `198f41`, attribute 2) and its colour
  (attribute 1), as floats, 48 bytes a vertex; one draw holds all the frame's lamps (busy street: about 100). World
  space, Y up, the same as the characters' world matrices (checked by printing both).
- **The choice** (`lamp_shadow_begin`, on the render thread as the sun's nearest cascade starts): lit lamps that stand
  still, by how near the nearest character is relative to their reach; up to 6. Lamps are compared at the precision
  of their positions, so a moving one (cars carry lamp lights too) is never "still" and gets no shadows: the pass is
  drawn before the frame's lamp draw, from the last frame's positions, and a moving lamp's never matched.
- **The characters' draws** are those of our sun shadow pass (decision 0024): every skinned caster it has learned,
  with its bone palette as the model renderer bound it, copied with each lamp view's matrix (projection × view × the
  object's world) into the views whose frustum holds the object's box (`lamp_shadow_add`). One pass a frame holds the
  characters of all the frame's shadow layers, each submesh once, and is queued into the shadow command buffer when
  the farthest cascade ends (`lamp_queue`).
- **Drawing** (`gpu_lamp_shadow_pass`, gpu_shadow.c): the sun pass's pipelines and palette ring, into our own 2D array
  image, no culling, our own dynamic state (then `gpu_dynamic_forget`), barriers before and after.
- **The lookup** (`lamp_light.frag lamp_lit`): the pixel's and the lamp's view-space positions to world space (the
  inverse of camera rows 4-6, in a table at binding 94), the lamp found in the table by position, the point moved off
  its surface along the normal (1.5 texels' width there, plus 0.03), the view by the largest axis, 3 × 3 compares 0.75
  texels apart through the hardware's bilinear depth compare (binding 93), with no derivatives (after early returns:
  the AMD rule, issues #8 and #19). The lamp's light there is multiplied by `mix(1, lit, strength × fade × the fade
  with distance from the caster)` (below).

## What it took (each found in game by the owner, or by the logs below)

- **Too dark:** a shadow first took all of the lamp's light. Now a strength: the panel has 0.25, 0.4 (recommended),
  0.6 and 1.
- **Blinking.** Found with a per-frame detector (a lamp with shadows in frames n-1 and n+1 but not n, logged with why)
  and a log of each pass whose lamp got no character draws while it had some the pass before. Each cause, in order:
  1. The program read only the pass of its own GPU frame, but in a quarter of frames the pass and the lamp draw fall
     in different GPU frames: it now reads the pass drawn last (its images hold what it drew), unless older than 4.
  2. One pass per presented frame, but the render thread can run a frame ahead of that count: a frame got none.
  3. With 4 lamps chosen from ~100 along a street, walking changed the choice and a lamp's shadows came and went at
     once: 6 lamps, a lamp chosen before counts a third nearer, and shadows fade in and out over 8 passes.
  4. The game can draw a cascade in two layers; a pass per layer let the second one (no characters) replace the first.
  5. A character near a cascade's edge is drawn in the next cascade in some frames: all cascades now add to the pass.
  6. Our sun pass doesn't count a character as its own when its check against the game's draw (one frame in 32 per
     object) fails, usually a bone palette caught mid-update, until the next check: for lamp shadows a learned
     character counts anyway; and a character still in the game's list but given no draws gets its last pass's draws
     again, for up to 3 passes.
  7. The game draws only lamps whose volume is in view; a chosen lamp missing from the draw for a frame came back faded
     out: it keeps its place for 30 passes.
  Owner's 24,000-frame session after these: 0 blips.
- **Soft edges:** the filter's reach was 1.5 texels; same-frame crops of a shadow on a wall at 1.5, 0.75, 0.5 and 0
  (`work/ab_kept/lamp_shadow_maps/lamp_wall_filters.png`): the owner chose 0.75 and kept 512 × 512 images over 1024.
  The other limit is the game's: lamp light is drawn at a third of the screen size.
- **Long shadows** next to the game's short moon shadow drew the eye (a lamp a little above a character's head throws
  one metres long; right for a point of light). A real lamp has a size, and a shadow gets lighter the farther it
  falls from what casts it: the program finds the caster's distance (the raw depths around, a second sampler of the
  same images, binding 92) and fades the shadow from 1 to 3 m past it (`YW4_LAMP_SHADOW_FADE`; 0.3 to 2 m made them all
  too light). A filter widening with that distance too, as an area light's penumbra does, turned the shapes to blobs
  on top of the third-size picture: left out.
- **Who casts them:** every character that casts the moon shadow, so the player and partner, and the NPCs that cast
  one; NPCs without a moon shadow get none (their draws come only from the main view's pass, after the shadow passes:
  a later piece of work, one frame behind, with the main view's skinning). Leaving out only the player and partner
  would need the player's object found. The owner kept it as it is.

## Cost

Busy street, 4K, the owner's settings (full-size effects), measured with the first version (4 lamps, the nearest
cascade's characters): the whole frame's GPU time 13.98 → about 14.15 ms (about 45 character draws into the lamps'
views; the lookups 0.05 ms of it, in the third-size pass). Not measured again since 6 lamps and all cascades. Video memory: 36 images
of 512 × 512 × 4 bytes, 36 MiB. By day nothing is drawn or looked up: every lamp's colour is 0.

## Not done

- Only characters cast lamp shadows (not walls, posts or benches; caching static casters, as engines do, would be the
  way to add them).
- Lamp shadows need our sun shadow pass (SHADOW PASS = CUSTOM) and our screen passes (SCREEN PASSES = CUSTOM).
- Bigger images for the nearest lamps only (a shadow atlas with sizes per lamp, as most engines do) if 512 is ever
  too coarse; `YW4_LAMP_SHADOW_SIZE` tries other sizes for all of them.

## Debugging

`YW4_LAMP_SHADOWS=<strength>,<softness>,<bias>,1` paints what each lamp reaches green and its shadows red, in place of
its light. The log has a summary every 600 passes (lamps and draws per pass, choice changes, passes read late, blips,
carried draws), and lines for blips, for passes a lamp got no character draws in (EMPTY, with counts at each step:
characters in reach, submeshes given, left out by every view), and for lamps that left the choice and why.
`YW4_LAMP_SHADOW_LOG=1`: every frame's lamps and draws. The test scenes `lamp` (the owner's save in front of a lamp)
and `busy_walk` (the walk's input on the busy save: moving under lamps; not frame-exact) are in tools/dev/scenes.toml.
