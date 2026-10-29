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
- ~~Fewer, wider rounds in battles~~: done, see below.

## Addendum 2026-10-29: the battle blur, and BATTLE BLUR fast (on by default)

The blur that costs GPU time in battles is not this pass and not the shared rounds `f_7f3620` (an earlier
commit assumed so: probes in a battle showed `f_7f1bf0`, `f_7f3620` and `f_7f3900` never run there). It is a
render job of its own, `main+0x897d90(device, job)`, queued by `main+0x89ac00` from `main+0x311f40`, with its own
loop: a copy into target `job+0`, then n rounds (across into target `job+2` attachment 4, down into its attachment
2, the same Gaussian weights, radius `job+0x10`), then back. Job: u16 `+0` / `+2` targets, f32 `+4` strength,
`+8` / `+0xc` spread across / down, `+0x10` radius, i32 `+0x14` rounds at most; n = (uint)(strength x (max - 1) + 1),
each step spread x strength / the image's size. It plays as a battle starts: strength ramps from 0.06 to ~1.5 in
about a second (1 to 17 rounds, radius 2, max 12), then the pass stops.

BATTLE BLUR fast (`YW4_BLUR_FAST`, panel PERFORMANCE): the hook hands the game's job a copy with m = ceil(n / 4)
rounds, strength 1 and the spreads times strength x sqrt(n / m). Blurs add up by their widths squared, so the
total width stays the same for a quarter of the passes; the game still draws it with its own shaders. Measured
in battles: ~0.16 ms per pass either way, so at 12 rounds 24 passes (~3.8 ms) become 6 (~1.0 ms). Not identical
texel for texel (the 8-tap kernel's ends fall differently), so it was judged by eye: the owner saw no difference
at battle start, and it is on by default (`YW4_BLUR_FAST=0` for the game's rounds). The same option also gives
`f_7f3620`'s rounds (our executor, `YW4_BLUR_PASS`) a quarter of the passes when they have more than two.
