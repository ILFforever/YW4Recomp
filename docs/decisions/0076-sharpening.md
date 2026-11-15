# 0076 — SHARPENING: one setting after every anti-aliasing

<!-- decision
state: accepted
date: 2026-11-15
replaces: none
code: runtime/src/gpu_aa.c, runtime/shaders/taa_output.frag, runtime/src/settings.c, runtime/src/gpu_draw.c, runtime/src/gpu_post.c
-->

Status: accepted (owner, 2026-11-15, through the relay session). Measured in the game on every anti-aliasing
(below); not yet seen by the owner on screen.

## Context

GitHub issue #11: a player found the game a little soft even with DLSS at native resolution (DLAA), and compared it
with an emulator using FSR's sharpening. What the port did before this:

- **TAA**: a fixed sharpen in its last pass (`taa_output.frag`, 0.5, only `YW4_TAA_SHARPEN` changed it).
- **FSR**: FSR's own sharpener (RCAS) at 0.5, only `YW4_FSR_SHARPNESS` changed it.
- **DLSS**: none. NVIDIA took the sharpening out of DLSS (2.5.1 and later), and our pass after it ran with 0. So the
  best-looking mode was the softest one.
- **OFF / FXAA / SMAA**: none.

## Decision

A **SHARPENING** row in the settings (ANTI-ALIASING AND UPSCALING, under UPSCALING): **OFF / LOW / MEDIUM / HIGH /
MAX** = 0 / 0.25 / 0.5 / 0.75 / 1.0, default **MEDIUM**. It is a list of options rather than a slider (the owner's
choice: five steps are easy to tell apart and to talk about). It is never greyed out: the same choice applies to every
anti-aliasing, OFF included. Saved as `YW4_SHARPEN=0..4`.

Where it happens:

| Anti-aliasing | Sharpening |
|---|---|
| TAA | its last pass (`taa_output.frag`), as before, now with the setting's amount |
| DLSS | the same pass, which already copied DLSS's output back: nothing extra to run |
| FSR | FSR's own RCAS, given the setting's amount (0 turns it off). Not our pass too: sharpening twice brings rings and grain |
| OFF | the scene encoded (as for the others) and our pass: two full-screen passes |
| FXAA / SMAA | their result encoded again and our pass |

With OFF, the two places that start the anti-aliasing (`gpu_draw.c`, `gpu_post.c`) used to skip it. They now always
call `gpu_aa_apply`, which returns at once when nothing (anti-aliasing, upscaling, sharpening) is on.

The sharpen in `taa_output.frag` became **contrast-adaptive**, after the method AMD published as FidelityFX CAS (our
code, from the idea). The old one pushed every pixel away from its four neighbours' average by the same amount. The
new one scales that push by `sqrt(darkest / brightest)` of the 3x3 neighbourhood, in the perceptual curve (`aa.glsl`,
values 0..1). Soft, flat detail (textures) gets the full push, and edges with strong contrast get little, so they
don't ring. As before, the result is clamped to the four neighbours' range, so it adds no bright or dark rims. The
push is 2 x the amount where the area is flat (MEDIUM 1.0, MAX 2.0), so MEDIUM sharpens textures about as much as
the old fixed 0.5 did, and edges less.

Why the clamp and not CAS's own way of limiting: CAS limits its weight so the result can't leave 0..1. Our curve
already keeps it in range, and the clamp is what kept TAA free of rims before. It costs nothing extra (the min and
max are already there).

`YW4_TAA_SHARPEN` (TAA, DLSS, OFF, FXAA, SMAA: our pass) and `YW4_FSR_SHARPNESS` (FSR) still override the setting
with any value, for testing.

## Alternatives

- **A slider** (0..1 in steps of 0.05): the first plan. The owner chose five fixed steps.
- **Only for TAA / FSR / DLSS, greyed out otherwise**: also in the first plan. The owner wanted it everywhere: OFF and
  FXAA are soft too (FXAA blurs textures a little), and a greyed-out row needs explaining.
- **NVIDIA's sharpening**: DLSS no longer has one, and NVIDIA's NIS is another library for the same job as a few
  lines of shader.
- **Our pass after FSR as well**: FSR already has a sharpener made for its output. Running both would sharpen twice.
- **Default 0 for DLSS** (no change for DLSS players): the owner picked MEDIUM for all. DLSS was the softest mode.

## Measurements

The school scene (`tools.dev.scene school`, the schoolyard by day), 3840x2160, one run per anti-aliasing. In each run
the setting changes at fixed frames (`YW4_SET_AT`), so MEDIUM, OFF and MAX are pictures of the same place a few
seconds apart. "Detail" is the mean absolute Laplacian of the brightness over a brick wall (higher = crisper; the
character and the camera move a little between the shots, the wall doesn't change).

| Anti-aliasing | Detail OFF | MEDIUM | MAX | GPU time of the sharpening at 4K |
|---|---|---|---|---|
| OFF | 5.47 | 7.68 (+40%) | 8.95 (+64%) | 0.65-0.7 ms (encode + our pass) |
| FXAA | 5.28 | 7.39 (+40%) | 8.60 (+63%) | ~0.65 ms (encoded again + our pass) |
| SMAA | 5.37 | 7.59 (+41%) | 8.79 (+64%) | ~0.6 ms |
| TAA | 3.75 | 5.16 (+38%) | 6.15 (+64%) | inside TAA's last pass, as before |
| FSR | 4.74 | 6.24 (+32%) | 11.47 (+142%) | inside FSR (its RCAS), as before |
| DLSS (DLAA) | 3.80 | 4.94 (+30%) | 5.71 (+50%) | the write-back pass 0.16 -> 0.30 ms |
| DLSS Quality | 3.60 | 5.02 (+39%) | 5.82 (+62%) | 0.30 ms |

- MEDIUM adds about the same everywhere (+30-40%). MAX differs: FSR's RCAS keeps getting stronger (its amount is in
  stops, and nothing holds it to the neighbours), while ours is held to the neighbours' range, so it levels off. FSR at
  MAX looks crunchy (dark specks along shadow edges); ours at MAX has no rims. If players want a stronger MAX with
  TAA / DLSS, the clamp would have to loosen at MAX: a choice for later, not made here.
- The cost is a fraction of a millisecond at 4K: about a quarter of that at 1080p.
- What also makes the background soft is the game's own DISTANCE BLUR (on, at HALF, by default). Sharpening doesn't
  undo it; DISTANCE BLUR set to OFF does. Worth telling a player who finds the picture soft.
- The full test suite passes (`pytest tests`: 66 tests), with a test for the row (`tests/test_settings_texts.py`).
