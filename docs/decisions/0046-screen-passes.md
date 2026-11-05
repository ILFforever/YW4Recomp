# 0046 — Our programs in the engine's screen passes
<!-- decision
state: accepted
date: 2026-11-02
replaces: none
code: runtime/shaders, runtime/src/gpu_draw.c
-->

Status: accepted, on by default (2026-11-02); the settings panel's SCREEN PASSES row (F1, DEVELOPER OPTIONS), F7 or
`YW4_SCREEN_PASSES=game` switches back to the game's programs.

## Context

After the shadow mask (decision 0044), the rest of the lighting stage and the passes up to the UI were still the
game's: depth copies, the lighting inputs, lamp lights, an edge-aware upsample, sky and aerial perspective, tone
mapping, the game's own FXAA and a few copies and fills (docs/frame.md "Lighting stage"). Two ways were weighed:

- **The whole pass ours** (bloom's way, decisions 0037, 0038): hook the engine's function, skip it, draw in its
  place. The half-size copy `f_80d270` showed what that costs here: its target and textures come from a GPU command
  chunk the engine records once and replays (the target description at `renderer +0x3720` points into one), and
  it leaves state behind that later passes need (record 44 bound to fragment texture 13). Each pass would need its
  own decoding of those chunks and side effects: days per pass, and a new hook (a regenerated translation) each.
- **Our programs in the engine's draws** (the shadow mask's way, 0044): the engine still issues the draw; our
  readable program runs in place of the game's, with the same inputs and targets. About an hour per program, no
  hooks, no engine state to copy.

The owner chose the second for these passes (2026-11-02): all their shader code becomes ours quickly, and the
heavier way stays for passes we want to change or speed up, where owning the draw pays off.

## Decision

Readable GLSL in `runtime/shaders`, each with the translated program's interface (bindings, inputs, outputs),
swapped in for the game's fragment program by `gpu_draw.c` (the `replacements` table, as for 0026 and 0044). The
screen passes have one switch of their own (`screen_fs`, `YW4_SCREEN_PASSES`). Every program does the game's
operations in the game's order (its fused multiply-adds, reciprocals, F16 constants, odd steps such as
`(v + 1) - 1`), so the results can be bit-exact; names and comments say what each step means.

| Game program | Ours | What |
|---|---|---|
| `95ea1b` | `depth_copy.frag` | colour and depth copy, shrunk (half and third size; `f_80d270`, `f_8050d0`, `main+0x80f988`) |
| `6b4575` | `depth_only_copy.frag` | depth-only copy |
| `d2aaa3` | `vertex_colour.frag` | fill with the quad's colour |
| `2571d0` | `clamp_copy.frag` | copy, negatives to 0 |
| `2d68c8` | `tinted_copy.frag` | copy times the quad's colour |
| `790774` | `lighting_inputs.frag` | the lighting inputs (`f_7f4c90`): shadow mask and two factors, normals in -1..1 |
| `42759b` | `lamp_light.frag` | lamp lights: point-light volumes at a third of the size |
| `3c94bd` | `edge_upsample.frag` | edge-aware upsample of a small image |
| `627287` | `sky_fog.frag` | sky and aerial perspective (Rayleigh and Mie scattering, height fog) |
| `22d015` | `tonemap.frag` | tone mapping (extended Reinhard with eye adaptation) |
| `3f9fc9` | `game_fxaa.frag` | the game's own FXAA (the console FXAA 3) |

The vertex programs stay the game's for now (pass-through quads and the lamps' volumes).

## Checking

A same-frame A/B per draw, with no hook: `YW4_AB_PASS=screen` with `YW4_AB_FS=<program>` (and `YW4_AB_DRAW=<n>`
for the frame's n-th such draw) makes the executor put the A/B markers (gpu_ab.c) right around that draw, so it
runs with ours, then again from the same images with the game's program, and the targets are compared texel by
texel. `YW4_AB_FS=all` compares the frame's (frame % 32 + 1)-th screen draw, so 32 frames in a row cover each.

Result: on the town save (frames 2500 and 2600, each draw of each program) 0 texels differ, colour and depth, for
all eleven. On all 13 test saves (`YW4_AB_FS=all`, frames 2500-2531: 420 compares) 418 are exact. The two that
are not, checked again over 32 frames: the sky and fog on the save `save_base_lag`, 3-6 of 1.44 million texels a
frame, and the lamp lights on `save_base_yokaiworld`, 1 texel in 3 of 32 frames. Each is the lowest bit of one
channel of the packed R11G11B10 colour (format 122), e.g. 0x6918d329 against 0x6918db29: the same driver-level
difference as the shadow mask's (0044), where the same operations in the same order still compile a little
differently. Accepted as there: one step of one channel at a few scattered pixels is not visible.
