# 0087 — Vertex positions marked invariant

<!-- decision
state: accepted
date: 2026-11-21
replaces: none
code: recomp/shader.py, runtime/shaders/gbuffer_main.vert, runtime/shaders/material_main.vert
-->

Status: accepted. The owner confirmed the fix on the RX 580 by eye (2026-11-21).

## Problem

On the owner's Linux test PC (AMD RX 580, Mesa RADV), leaves flickered grey in the opening schoolyard cutscene
(Nate, Katie, Jibanyan and Whisper by the hedge), on the PC's own screen and over VNC. Never on the RTX 4070.
Ruled out one by one, the grey staying each time: every pass of ours switched to the game's (shadows, shadow mask,
G-buffer, screen passes, bloom, distance blur), anti-aliasing, PCSS, texture filtering, vertex data read from the
game's memory instead of copies (`YW4_VRAM_GEOMETRY=0`), every texture uploaded again each frame and de-tiled on the
CPU (`YW4_GPU_REUPLOAD=1`, `YW4_GPU_DETILE=0`), texture reads before the discard (c9c58df) and KIL as demote.

Still screenshots didn't show it: the hedge's mean saturation was the same on both GPUs (111.3). Measured frame
to frame instead (55 shots 0.1 s apart while the scene holds on a dialogue line, per pixel, RX 580 against the
4070 with the same save, replay and settings), the RX 580 had extra change in one place: a thin band along the top
of the hedge, where leaves overlap other leaves and the building behind.

## Cause

The game draws its scene twice (frame.md: a light pre-pass renderer): the G-buffer pass writes depth, then the lit
material pass draws the same objects again, only where the depth test is EQUAL. That needs both passes to compute
each vertex's position to the last bit. They run different programs (and different pipelines of the same
program), and nothing told the compiler the position must come out the same: none of the translated vertex
shaders, and none of ours, marked `gl_Position` invariant. Vulkan lets a driver contract `a * b + c` into a fused
multiply-add in one pipeline and not in another. RADV did; NVIDIA in practice doesn't. A pixel whose depth missed by
a bit failed EQUAL, kept the G-buffer pass's colour instead of its material, and showed grey; as the leaves sway,
which pixels miss changes every frame: a flicker.

## Decision

`gl_Position` is `invariant` in every translated vertex shader and in the output of every translated geometry
shader (glslang refuses it on a geometry shader's input block; invariance needn't match across stages), and in our
own scene vertex shaders: the G-buffer, material and shadow-caster ones. All 1349 game shaders translate and
compile.

It forbids only the reorderings that could change the position: nothing visible changes where the passes already
matched, and the cost is at most a few instructions per vertex.

## Measured

The owner watched the cutscene live on the RX 580 after the change: "seems to be good". By the numbers (the same
save, replay and settings: 55 shots 0.1 s apart while the scene holds; mean change of a pixel from shot to shot,
of 255):

| | Whole picture | Top of the hedge | Pixels changing 12+ more than on the 4070 |
|---|---|---|---|
| RX 580 before | 0.98 | 1.27 | 106, along the top of the hedge |
| RX 580 with invariant positions | 0.97 | 1.08 | 85, all on the edge of Whisper's floating tail |
| RTX 4070 | 0.74 | 0.72 | |

The 4070's run reaches the cutscene at another moment (its loading is slower), so the characters' idle animations
don't line up with it: what is left is them, not the hedge.

How it was found, for the next one like it: a still screenshot can't show a flicker, but the change per pixel
between consecutive shots, against another GPU's, can (a few lines of Python over two shot folders).
