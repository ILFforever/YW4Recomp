# 0045 — Contact shadows

Status: accepted, on by default (2026-11-01); the settings panel's SHADOWS > CONTACT SHADOWS row switches
them while playing, `YW4_CONTACT=0` at start.

## Context

The cascaded shadow maps (4096 texels, decision 0025's PCSS) can't resolve small, close detail: a tap over a sink,
the gap under a wall's cap or a balcony, the leaves inside a hedge shading each other. Owning the shadow mask
program (decision 0044) makes room for a screen-space fix in the same draw.

## Decision

In `runtime/shaders/shadow_mask.frag` (`contact_shadow`, `contact_smoothed`), for each half-size pixel that can use
it (in a cascade, facing the sun), march 12 steps over 0.3 view units (about 30 cm) from the surface toward the sun
(cb12 row 0) across the half-size depth. The march projects with the inverse of cb8 rows 20-23, which carry the
anti-aliasing's shift (`cb8_scaled_copy`), so it lines up with the shifted depth. A step that lands behind the
picture by more than a hair and less than the thickness is a hit: the mask (how much is in shadow) becomes
max(shadow map's result, 1 - t²) for the hit at ray fraction t.

Settings go to the program in the 4th vec4 of the YW4PCSS block (binding 95, now 64 bytes):
steps, length, thickness, debug.

## What it took (each found in game by the owner, then fixed)

- **Dots** (a random start per pixel): the four pixels of a 2x2 block march a quarter step apart and share their
  results through quad swaps (`subgroupQuadSwap*`), averaged with depth-aware weights: four times the steps for the
  price of one, no blur across outlines. Pixels that don't use the result (sky, past the last cascade, facing away)
  skip the march and are left out of the average: the first version marched every pixel and cost 0.77 ms.
- **A band on the ground beside posts:** filtered depth reads blended the post and the ground into a depth nothing
  has; the march reads texels (`texelFetch`). A hairline stayed: rays passing behind a post's side, whose screen
  depth says nothing about its thickness. Hits where the blocker is seen almost exactly edge-on (|n·v| < 0.15) don't
  count.
- **The line under a wall's cap faded as the view turned:** two earlier fixes for the post (skip hits beside the
  pixel; drop a hit when the ray comes out in front again) also removed the cap's line, which is a texel or two tall
  from afar and which rays leave over the cap's top. Both are gone. The thickness is 0.07 but widened by 1/|n·v|
  (at most 4x) for slanted faces, whose depth spreads over more of the view.

## Limits

Screen space only: nothing off screen or hidden casts. It is not ambient occlusion: darkness under benches and in
corners (light from the sky blocked, not the sun) needs SSAO, next on the list.

## Cost

The shadow mask pass at 4K 21:9: +0.04 ms at the late-game save (much in shade), +0.25 ms in the sunny schoolyard
(1.08 → 1.34 ms, owner's run with F5).

Measured again 2026-11-02 with full-size effects (the mask at 2560×1080, the owner's settings) in the bamboo forest:
about 0.85 ms (`YW4_MASK_BREAKDOWN=1`: the mask 3.60 ms as set, 2.76 without contact shadows), well over the
figures above, which were at half-size effects or in shade.

## How it was checked

The owner played with F7 at the lamp post, a wall's cap from several angles, hedges and houses (screenshots in the
README). Comparisons within one run (`YW4_CONTACT_ALTERNATE=1`, frames N and N+1), since separate runs of a replay
don't line up even with the clock held (`YW4_CLOCK`).
