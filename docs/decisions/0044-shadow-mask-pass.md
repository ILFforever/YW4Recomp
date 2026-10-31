# 0044 — Our own shadow mask program

Status: accepted, on by default (2026-10-30); the settings panel's SHADOW MASK row (F1, DEVELOPER OPTIONS) or
`YW4_SHADOW_MASK=game` switches back.

## Context

The lighting stage is the next piece of the renderer to become ours (docs/frame.md "Lighting stage"). Its most
expensive pass is the shadow mask `f_7f3e90(device, renderer)`: one half-size draw (program `98c09b`/`06d03ad7`)
that works out per pixel how much sun reaches the surface, from the cascaded shadow maps, with our soft shadows
(decision 0025) inside. Its algorithm was decoded from the NV assembly first (frame.md, six steps).

## Decision

A readable fragment program, `runtime/shaders/shadow_mask.frag`, with the translated program's interface
(bindings, input, output), swapped in for `06d03ad7` in the engine's own draw (gpu_draw.c, like the G-buffer's
programs of decision 0026). The draw, its inputs and its target stay the engine's; only the program is ours. It
has its own switch (runtime/src/shadow_mask.c), not the G-buffer's. Its pipeline is built at start-up with the
game's (no first-use stall).

The same-frame A/B (gpu_ab.c) checks it: a hook on `f_7f3e90` records the markers around the engine's function
when `YW4_AB_PASS=shadowmask`, so in the frames of `YW4_AB_AT` the pass runs with ours and then with the game's
program from the same images. `YW4_AB_SHOW=<n>` lists the differing texels.

## Not bit-exact, accepted

Every operation is the game's, in its order (fma where it fuses, the signs made from two integer compares, an if
instead of an early return). Yet at the late-game save at 4K 21:9 a frame has 2-4 of 2.8 million texels that
differ by one bit of the 11-bit result with soft shadows off, and ~130 with them on (a hair's change tips the
blocker search, so a few pixels differ more: 0.38 against 0.21 lit at one). The control (the game's program both
times) gives 0, and the soft shadows' debug views show the compare depths themselves differ by an ulp at a few
pixels while the positions match. Splicing the game's translated code into ours section by section only moved
the differences around; both are compiled by the same glslang with the same options. So the driver optimises the
two equivalent programs a little differently.

Other options were: mark the arithmetic `precise` in both programs (likely exact, at some GPU cost, and changes the
translated game program), or keep reshaping ours to the driver's choices (no end in sight). The owner judged the
differences not visible (single scattered half-size pixels, like the soft shadows' own grain) and chose to accept
them, unlike the earlier passes, which are exact.

## Cost

The same as the game's: at the late-game save the mask took 2.54 ms per frame against the game's 2.62 (300-frame
averages of `YW4_GPU_PASSES`), with the frame's GPU time 12.4 ms either way.

## Speed-ups (2026-10-30)

Two switches in the panel's SPEED-UPS rows, off by default until the owner has compared the look in game:

- SOFT SHADOW FILTER (`YW4_MASK_FILTER=once`): with soft shadows on, the game's five lookups (points less than a
  texel apart) are each a 16-tap filter; one lookup at the centre covers the same ground.
- SOFT SHADOW SEARCH (`YW4_PCSS_SEARCH_TAPS=8`): the blocker search with every other tap of its disk.

The mask at the late-game save, 4K 21:9, soft shadows x2, 4096 maps (300-frame averages): the game's program
2.62 ms, ours as the game does it 2.54, filter once 1.48, filter once and search 8 1.23. Both depart from the
game's result, so the A/B no longer applies with them on.

Also, always on: pixels facing away from the sun (the result is 1 whatever the lookups find) and fully faded ones
(the result is 0) skip the lookups. The same result (the A/B against the game's program unchanged: 125-128
texels), ~0.1 ms less: ours 2.45 ms, with both speed-ups 1.09. Less than hoped: few pixels face away at the
late-game save, and a group of pixels the GPU runs together only saves when all of them skip.

Owner's verdict in game (2026-10-31): SOFT SHADOW FILTER FAST looks the same, so it is now on by default
(`YW4_MASK_FILTER=five` for the game's); SOFT SHADOW SEARCH 8 looked the same but put small shadow blobs on some
objects (a thin caster missed by the halved search gives a pixel a different softness), so it stays off.

## Next

Owning the program lets the shadow mask improve: contact shadows for small details the 1024-texel cascades miss,
and the mask at full size instead of half.
