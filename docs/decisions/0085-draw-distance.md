# 0085 — Draw distance: the map's far clip, four times as far

<!-- decision
state: accepted
date: 2026-11-20
replaces: none
code: runtime/src/drawdist.c, runtime/src/settings.c, recomp/overrides.toml
-->

Status: accepted for the props it covers. Trees, buildings and people popping in are not covered (below).

## Problem

The game pops things in close by with no fade (owner, 2026-11-20: "notoriously bad draw distance and model pop-in
without fading: trees, buildings, people just pop in"). Model detail levels were ruled out earlier (frame.md, "Model
detail at a distance": these maps never switch models).

## What the game does

Found by property names (each registered with its offset; scan for the code that loads the name, then its
`mov x10, #offset`) and read live with YW4_PROBE / YW4_PROBE_MEM:

- **A far clip per map part.** `f_5f3ec0` runs per map part (from `main+0x5e8604`) over the part's object list (+0x4b0,
  64-byte entries, position at +0x30, count +0x4b8). With `m_isEnableClip` (+0xa9) on it skips every object whose
  squared distance from the camera (plus the float at +0x604) is past `m_farClip` (+0xa0) squared, or inside
  `m_nearClip` (+0x9c) when that is above 0. The town, the school and Yo-kai World: far 100, near 0, on. The lists
  hold few objects (0-8 per part in the town walk): lamps, plants, signs, not the buildings.
- **A build radius for the map's pieces.** The map object (`f_6082e0` every frame, from `main+0x857f4c`):
  `m_radiusToBuild` +0x278 (10), `m_heightToBuild` +0x280 (10; Yo-kai World 50), `m_radiusToDestroyOffset` +0x288 (15;
  Yo-kai World 5). Radius 40 in the town walk: 5-8% more draws, the screenshots the same (the extra pieces were out
  of view).

Ruled out on the way: `m_fDrawableAreaRange` (map icons), `popupFarDist` (UI name pop-ups),
`m_cameraFar/NearFadeIn/OutDistance` (a component that fades, `f_1e8050`).

## Decision

DRAW DISTANCE (Detail): DEFAULT (the game's), MEDIUM (x2), FAR (x4, the default, owner's pick: "x4 is a good mix, x10 is
overkill"), VERY FAR (x10). drawdist.c's hooks multiply both limits, remembering each object's own value so a change
applies to it and a new map's values are taken as they come; live. `YW4_DRAW_DISTANCE` = 1 / 2 / 4 / 10.

At FAR, in the owner's Yo-kai World street, a potted plant on the temple steps, two street lamps and small details at
the end of the street show that the game drew only from close by (owner's F12 shots, 2026-11-20).

## Cost

RTX 4070 laptop, owner's settings (4K, DLSS anti-aliasing, model K, frame generation), uncapped:
- standing on that street, one run, each step 1200 frames: 62.4 / 63.2 / 63.3 / 62.4 fps (default, x2, x4, x10),
  GPU 14.2-14.6 ms;
- the owner's own walk (his recording after a save, replayed 4 times; 115 s each): 52.7 / 52.2 / 51.7 / 52.9 fps, no
  frames over 50 ms in any.

Within noise: free.

## Not covered

Trees, buildings and people that pop in come from another rule, not found yet. Next: a replay walking straight at one
and the draw list at the frame it appears (does the game hand it over late, or not at all).

F6 steps the setting with `YW4_DRAW_DISTANCE_KEY=1` (tests; no key for players).
