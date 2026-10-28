# 0037 — Our own bloom pass (first post-processing pass)

Status: accepted, on by default (2026-10-28); the settings panel's BLOOM row (F1) or `YW4_BLOOM=game` switches back.

## Context

The renderer plan (0023) replaces the game's passes one by one. After the shadow maps (0024) and the G-buffer and
lit pass (0026, 0030), the owner picked post-processing next. The game's post-processing is two engine functions
(docs/frame.md "Post-processing"): the distance blur `f_8007d0` and the bloom `f_802e40`. Bloom is the smaller and
self-contained one: 8 full-screen draws, inputs and settings all in the renderer object.

## Decision

A hook on `f_802e40(device, renderer)` (runtime/src/bloom.c) reads, from the renderer object, the 9 images the
bloom uses (records 0, 11, 9, 19-24 of its 0x28-byte image table at `+0x2e28`; record `+0x10` is a view whose first
word + 0x18 is the guest NVNtexture) and its settings (`+0x7c0` blur steps, `+0x7d0..+0x7e8` composite,
`+0x7f0..+0x80f` thresholds, `+0x388d` flag), and records one CMD_NATIVE in the device's command buffer. With
`YW4_BLOOM=native` the hook then skips the engine's function: none of its CPU work or draws happen.

The executor (runtime/src/gpu_bloom.c) draws the same 8 passes with our own shaders (runtime/shaders/bloom*),
written from what the game's programs do, with the same operations in the same order (`fma` where the game
multiplies and adds in one step), the same quad (its corners and texture coordinates as the engine's vertex
buffer holds them, split into triangles like the runtime splits the game's quads), the same viewport (scaled and
flipped as `gpu_dynamic_state` does for the game's draws), sampler and blend (`one`, `one minus source alpha`;
alpha kept). Then anti-aliasing runs as it does after the game's composite.

Not copied: the translated game shaders (game-derived output, never committed). Ours are hand-written.

## How it is checked

`YW4_BLOOM=check`: the engine draws its bloom as usual; our pass, recorded just before, draws the same into
copies of its 7 images and of the scene. After the game's composite (before anti-aliasing) both versions of
all 8 images are copied out and compared texel by texel when the GPU is done; a `yw4: bloom: check frame N`
line per checked frame (every 300th, or `YW4_BLOOM_CHECK_AT`). It must say 0 differ on every image, on every
test save, before ours becomes the default.

## The one thing that differed

The first check matched on most frames, but on some about 20 texels of the smallest level (and a few in the
scene) were one low bit off. Inputs and constants were equal (logged). The cause was the sampler: the game's
allows mip levels (min filter 3, levels 0-15), so the runtime's texture filtering setting (`YW4_ANISO`, default
16) turns on anisotropic filtering and blending between levels for it, and our plain linear sampler didn't
have that. With `YW4_ANISO=0` both matched exactly; now our pass makes its sampler from the game's settings
through `gpu_sampler`, the way the game's draws get theirs, and matches with any setting.

## What skipping the engine's function leaves out

Most of what `f_802e40` changes is the engine's record of what is bound (texture slots, constant buffers, the
program, the render state stack, pushed and popped). It is deliberately not recreated: none of the engine's bloom
commands are sent either, so its record and the game's command stream still agree, as they were before bloom;
setting the record alone would make the engine skip bindings the GPU never got. What it changes that is not a
binding is done by the hook: the device flag `+0x45da` bit 0, the bits in `+0x45ac` for every image it reads
(0x12 or 0x10 by the record's `+0x22`/`+0x23`), and its draw statistics (`+0x4654`, `+0x4658`).

## Results

`YW4_BLOOM=check` on all nine test saves at 4K 21:9 (the owner's settings), four frames each in the scene
(1800-2700): 0 of 564 million texels differ. Native runs load and play like the game's; the game's own draws drop
by its 8 bloom draws. (A run's draw count also depends on where the replay ends up: ~165 or ~300 in the Yo-kai
World replay, either setting.)

## Open

- Then the distance blur the same way; then improvements as options (more levels at 4K, a smoother bloom).
