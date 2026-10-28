# 0038 — Our own distance blur pass

Status: accepted, on by default (2026-10-28); the settings panel's DISTANCE BLUR PASS row (F1) or
`YW4_DOF_PASS=game` switches back.

## Context

The second post-processing pass after bloom (0037). The engine's distance blur `f_8007d0(device, renderer)`
(docs/frame.md "Distance blur") shrinks the scene into a half-size image, blurs it in rounds (across, then down,
`renderer +0x868` times: 1 in town, about 10 in a battle the per-pass timer caught, ~4 ms at 4K 21:9) and blends it over the scene
by depth. Our runtime already changed it from the outside: the panel's DISTANCE BLUR (half or off) and the effects
resolution (its images keep the game's size ratio).

## Decision

The same structure as bloom. A hook on `f_8007d0` (runtime/src/dof.c) reads from the renderer object its image
records 0 (scene), 9 and 10 (the half-size pair) and 42 (the G-buffer's depth), the rounds, the blur radius and
spread, and the camera's near/far and focus settings, and computes the constants as the engine does:

- the Gaussian weights of `f_7f3620`: `expf(-k²/2r²)` for k = 0..7 over twice their sum, in float, summed left to
  right, with the same `expf` (the sdk's is musl 1.1.19's, which the runtime already uses for the game: decision
  0013), so the weights are bit-identical;
- the composite's 8 fragment constants from near, far and the focus settings, with its divide-by-zero guards.

The executor (runtime/src/gpu_post.c, shared with bloom) draws the shrink, the rounds and the composite with our
shaders (runtime/shaders/dof*), written from the game's programs' operations in their order: the 16-tap blur with
its pairwise sums, and the composite's depth to distance to amount, the 4-neighbour edge rule, and the discard. The
quad, viewport, samplers (colour: min 3 / mag 1; depth: min 2 / mag 0; both through `gpu_sampler`) and blend
(`source alpha`, `one minus source alpha`; alpha written) are the game's. The composite reads the camera's far
distance from the camera block the game has bound, as its program does. The panel's DISTANCE BLUR setting is
applied by our pass (half: the amount constant halved; off: no composite), and its images still keep the game's
size ratio.

Left to the game's function: the variant with `renderer +0x2c70` set (a depth-aware shrink with a lookup texture
and a different composite; not seen yet, logged when it is), and a depth record without its descriptor.

## How it is checked

`YW4_DOF_PASS=check`, like bloom's: ours into copies of the two half-size images and the scene, compared texel by
texel with the game's after its composite. `YW4_DOF_CHECK_AT=<frames>` picks the frames. All nine test saves at 4K 21:9 with the half setting, two frames
each in the scene: 0 of ~29 million texels differ per save (bloom's check, run alongside, still 0 too); the
school save at the game's full setting: 0. The +0x2c70 variant did not come up.

## Open

- The +0x2c70 variant, when it shows up.
- Then the improvements this makes possible: fewer, wider rounds in battles at 4K (the 10 passes are the cost),
  as an option.
