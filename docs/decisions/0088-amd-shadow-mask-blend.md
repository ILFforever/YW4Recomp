# 0088 — AMD: no cascade blend where the cascades don't overlap

<!-- decision
state: accepted
date: 2026-11-21
replaces: none
code: runtime/shaders/shadow_mask.frag
-->

Status: accepted. Fixes the long dark bars on the ground on AMD graphics under Windows (issues #8, #19), reproduced
and checked on the owner's laptop's Radeon 780M (AMD driver 26.8.1).

## Problem

On AMD cards under Windows, our shadow mask (decision 0044) drew long dark bars with thin lit gaps across the ground,
most visibly on the bamboo forest's paths; with the game's mask program (Advanced › Shadow mask: Game) they were gone.
Never on NVIDIA, nor on the RX 580 under Linux (Mesa).

## How it was found

With the Graphics card setting (Display › Graphics card, the laptop's 780M) the bug showed on a PC we control, so
the mask could be captured on both cards at the same still moment (`YW4_CAPTURE_FS=06d03ad7`, debug values in
the mask's place through `YW4_PCSS_DEBUG`), step by step:

- the shadow maps themselves (F10): the same on both cards, characters included;
- the pixel's distance and the cascades' ranges: the same;
- the cascade chosen, the next one, the fade, facing away, N·L: the same;
- the blend weight toward the next cascade: 0.86 on the 780M, 0.015 on the 4070.

## Cause

The blend is `clamp((distance - next_start) / (far_end - next_start), 0, 1)`, written in the game's order as
`(-z + -next_start) / (-next_start + far_end)`. The game sets the next cascade's start to this one's far end
(19.92 and 19.92 in the bamboo forest): no overlap, so the denominator is zero. On NVIDIA and the Switch it is +0, the
numerator is negative, -x / +0 = -infinity, clamped to 0: no blend. AMD's Windows driver may build `-a + b` as
`-(a - b)` (allowed: Vulkan doesn't keep the sign of a zero), which is -0, and -x / -0 = +infinity, clamped to 1:
every pixel took the next, coarser cascade's shadow, which drew the bars. The game's own program computes it in
another instruction order, so it was spared.

## Decision

Blend only where the region has a width: `span > 0 ? clamp(... / span, 0, 1) : 0`. The result is the same as
before wherever the cascades overlap, and what NVIDIA always gave where they don't.

## Measured (the 780M against the 4070, the late-game save, still camera)

| | Mean difference of the mask | Pixels differing by more than 0.1 |
|---|---|---|
| Before | 0.0645 | 15% |
| After | 0.0137 | 2% |
| The 4070 against itself, two runs | 0.0116 | |

What remains is run-to-run noise (idle animations). The 4070's result doesn't change.

## Also found, not fixed here

AMD's Windows driver supports no 3-component 8- or 16-bit vertex format (R8G8B8_*, R16G16B16_*), which NVIDIA
supports and Mesa emulates; the game's formats are passed to Vulkan unchecked (gpu_draw.c `vertex_format`). Not
this bug (the shadow maps were right), but undefined on AMD wherever the game uses one. Measured the same day with
a check that logs each unsupported format once (`gpu_vertex_format_ok`, `yw4: gpu: vertex format N not supported
for vertex input`), on the 780M: the town walk, the bamboo forest, the Yo-kai World, a battle and the opening hedge
cutscene use none. Left as the check: a fix (the 4-component format and the fourth value ignored) only if a log
ever shows one.
