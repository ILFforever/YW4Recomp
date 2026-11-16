# 0076 — SHARPENING: one setting after every anti-aliasing

<!-- decision
state: accepted
date: 2026-11-15
replaces: none
code: runtime/src/gpu_aa.c, runtime/shaders/sharpen.glsl, runtime/shaders/taa_output.frag, runtime/shaders/ui_sharpen.frag, runtime/src/gpu.c, runtime/src/settings.c, runtime/src/gpu_draw.c, runtime/src/gpu_post.c
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
| FSR | FSR's own RCAS, with its own table of amounts (below; 0 turns it off). Not our pass too: sharpening twice brings rings and grain |
| OFF | the scene encoded (as for the others) and our pass: two full-screen passes |
| FXAA / SMAA | their result encoded again and our pass |

With OFF, the two places that start the anti-aliasing (`gpu_draw.c`, `gpu_post.c`) used to skip it. They now always
call `gpu_aa_apply`, which returns at once when nothing (anti-aliasing, upscaling, sharpening) is on.

The sharpen (`sharpen.glsl`, used by `taa_output.frag`) became **contrast-adaptive**, after the method AMD published as FidelityFX CAS (our
code, from the idea). The old one pushed every pixel away from its four neighbours' average by the same amount. The
new one scales that push by `sqrt(darkest / brightest)` of the 3x3 neighbourhood, in the perceptual curve (`aa.glsl`,
values 0..1). Soft, flat detail (textures) gets the full push, and edges with strong contrast get little, so they
don't ring. As before, the result is clamped to the four neighbours' range, so it adds no bright or dark rims. The
push is 2 x the amount where the area is flat (MEDIUM 1.0, MAX 2.0), so MEDIUM sharpens textures about as much as
the old fixed 0.5 did, and edges less.

Why the clamp and not CAS's own way of limiting: CAS limits its weight so the result can't leave 0..1. Our curve
already keeps it in range, and the clamp is what kept TAA free of rims before. It costs nothing extra (the min and
max are already there).

### FSR's own table

FSR's RCAS grows much faster with its amount than our pass: its amount is in stops of 2x, and nothing holds the result
to the neighbours' range. With the plain 0.25 a step, FSR's MAX (1.0) added 142% detail where DLSS's added 50-64%, and
looked crunchy. FSR now gets **LOW 0.25 / MEDIUM 0.5 / HIGH 0.6 / MAX 0.7**: measured to add about as much at each step
as DLSS and TAA (table below). `YW4_FSR_SHARPEN_MAP=<low>,<medium>,<high>,<max>` replaces it, for measuring.

### SHARPEN TEXT AND MENUS

Where things are drawn: the game draws the 3D into its scene image, our anti-aliasing (and with it every sharpening
above, FSR's RCAS included) runs on that image right after the bloom composite, then the game draws its UI (HUD,
menus, text) into the same image and copies the whole into the window image that is presented. So until this record,
**nothing of the UI was sharpened**, in any mode. Our own overlay (settings, controls, F3 HUD) is drawn later still,
onto the window (`window.c`).

Some players at low resolutions want the text sharpened too. A row **SHARPEN TEXT AND MENUS < OFF / ON >**, right
under SHARPENING, default **OFF**, saved as `YW4_SHARPEN_UI=0/1`, greyed out while SHARPENING is OFF. With ON:

- At the anti-aliasing point, the 3D as it is there (already sharpened) is copied into an image like the window image,
  where the window shows it: the same copy frame generation makes as its HUD-less image (`gpu_fg.c`).
- At the present (`gpu_present`, before the captures, screenshots and frame generation), `ui_sharpen.frag` sharpens the
  window image with the same filter and amount, in its own values, **only where it differs from that copy** (by more
  than 1.5/255): the pixels the UI drew. The 3D is left as it was, so it isn't sharpened twice, and frame generation's
  HUD-less image still matches the 3D of the picture it gets.
- A frame with no 3D (the title, full-screen menus, videos) has no copy: it is sharpened whole.

Why not one pass over the finished picture instead of two places: the 3D would then be sharpened by our filter after
the UI, FSR's RCAS would have to step aside, and the HUD-less image frame generation compares with would differ from
the picture everywhere (FSR's frame generation reads such differences as UI). Why not the 3D again with the UI: twice
sharpened is grainy.

`YW4_TAA_SHARPEN` (our pass: TAA, DLSS, OFF, FXAA, SMAA, text and menus) and `YW4_FSR_SHARPNESS` (FSR) still override
the amount with any value, for testing.

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

- That first table had FSR at 0.25 a step. MEDIUM added about the same everywhere (+30-40%), but FSR's MAX added 142%
  and looked crunchy (dark specks along shadow edges): hence FSR's own table.
- The cost is a fraction of a millisecond at 4K: about a quarter of that at 1080p.
- What also makes the background soft is the game's own DISTANCE BLUR (on, at HALF, by default). Sharpening doesn't
  undo it; DISTANCE BLUR set to OFF does. Worth telling a player who finds the picture soft.
- The full test suite passes (`pytest tests`: 66 tests), with a test for the row (`tests/test_settings_texts.py`).

### Every level, FSR on its own table (second round)

Same scene and method, the setting stepped OFF, LOW, MEDIUM, HIGH, MAX every 40 frames in one run; detail gained
over OFF, averaged over three areas (a brick wall, the ground, a house):

| | LOW | MEDIUM | HIGH | MAX |
|---|---|---|---|---|
| DLSS (DLAA) | +17% | +29% | +39% | +49% |
| TAA | +20% | +36% | +50% | +61% |
| FSR, 0.25 / 0.5 / 0.6 / 0.7 (chosen) | +21% | +35% | +43% | +58% |
| FSR, 0.35 / 0.55 / 0.65 / 0.8 (tried) | +24% | +39% | +46% | +73% |

The chosen table falls between DLSS and TAA at every step; FSR at MAX no longer looks crunchy.

### SHARPEN TEXT AND MENUS, OFF against ON (MEDIUM, TAA)

| 3D view | 3D (brick wall) | Minimap | Objective text | GPU time of the pass |
|---|---|---|---|---|
| 1600x900 | 7.42 -> 7.34 (same) | +18% | +18% | not timed |
| 1920x1080 | 7.19 -> 7.15 (same) | +13% | +18% | 0.31-0.34 ms |
| 3840x2160 | 5.19 -> 5.20 (same) | +25% | +20% | 0.80 ms |

The 3D measures the same with ON (not sharpened twice); the UI is crisper. The copy at the anti-aliasing point adds a
blit of the 3D (counted in the anti-aliasing's own line). A 720p window was not run: 900 lines is the smallest picture
the game draws, and a smaller window only shows it scaled down after the sharpening, so it should look like the
900-line row made smaller. Seen at 4x zoom: the filter works per colour channel (as
CAS does), so hard UI edges can get a faint coloured edge; at normal size it doesn't show.

## Follow-up 2026-11-16: DLSS's own steps, and FSR's method for our pass

The player on #11 found FSR sharper after this record but DLSS "kinda the same". Measured at 1080p (school scene,
upscaling off, same method; wall / boy / ground / house): DLSS on MAX (10.0 / 11.2 / 4.7 / 6.5) was about FSR on
MEDIUM (10.3 / 10.1 / 5.7 / 7.0). Both add about the same share of detail per step; DLSS's picture simply starts
15-30% softer than FSR's (OFF: 6.6 / 7.5 / 3.2 / 4.4 against 7.8 / 8.0 / 4.2 / 5.4), so matching steps by the share
gained left DLSS a step behind everywhere.

- **Our pass now uses RCAS's method** (`sharpen.glsl`: FSR's own sharpener, AMD FidelityFX, MIT): one weight for the
  three colours, as large as keeps the result in 0..1, at most 0.1875, scaled by FSR's sharpness in stops. The clamped
  push from above measured about the same at a given step, but pushed further it went blocky (tree trunks at 2.5x MAX)
  and still stayed under FSR on soft ground; RCAS reaches FSR's look cleanly. Every mode now takes FSR's amounts:
  **LOW 0.25 / MEDIUM 0.5 / HIGH 0.6 / MAX 0.7** (`YW4_SHARPEN_MAP`).
- **DLSS gets its own steps: 0.6 / 0.75 / 0.8 / 0.85** (`YW4_DLSS_SHARPEN_MAP`), measured so each looks about as sharp
  as FSR at the same step. 0.85 is still clean; 1.0 puts bright rims on edges.

| 1080p | wall | boy | ground | house |
|---|---|---|---|---|
| DLSS OFF | 6.64 | 7.49 | 3.18 | 4.40 |
| DLSS LOW (0.6) | 9.43 | 10.73 | 4.54 | 6.13 |
| DLSS MEDIUM (0.75) | 10.93 | 12.47 | 5.28 | 7.06 |
| DLSS HIGH (0.8) | 11.62 | 13.31 | 5.67 | 7.55 |
| DLSS MAX (0.85) | 12.62 | 14.47 | 6.15 | 8.13 |
| FSR LOW | 9.28 | 9.42 | 5.00 | 6.35 |
| FSR MEDIUM | 10.26 | 10.21 | 5.69 | 7.05 |
| FSR MAX | 11.92 | 11.43 | 6.63 | 8.15 |

TAA stays on the common steps (MEDIUM 7.95 / 8.91 / 4.28 / 5.66): softer than FSR and DLSS, as TAA is; nobody asked.
SHARPEN TEXT AND MENUS with RCAS: objective text +8% at MEDIUM over OFF (1080p), no rims seen at 3x zoom. Tests: 71 pass.

## Follow-up 2026-11-16: a slider at the five levels

The same player asked for a slider. The owner, who had picked five steps over a slider for this record, agreed, and
picked style B of a mockup (`work/mockups/sharpen_slider`): a track with the five levels marked on it. A first build
moved in 5% steps and showed the percent with the nearest level's name; the owner then wanted "only level": the
slider **stops only at OFF / LOW / MEDIUM / HIGH / MAX and shows the level's name, no number**.

- Kept in percent inside (0 / 25 / 50 / 75 / 100), MEDIUM the default, with exactly the amounts above (DLSS on its
  own). `sharpen_amount` still blends between two levels, unused while the slider snaps, should finer steps return.
- **Saved as `YW4_SHARPEN=0.50`** (the slider's fraction, as the HUD's sliders are saved). A file from before the
  slider (`YW4_SHARPEN=0..4`) is read as its level: HIGH comes back as HIGH. Any value snaps to the nearest level
  (a 5%-step build's 0.65 reads as HIGH). `YW4_SET_AT` takes either form.
- **The overlay** draws the slider where a row's arrows and value are (settings page and the preview's bar); Left /
  Right (and A / Enter) move it one level, a click or a drag on the track sets the nearest level. A slider with named
  points shows the name as its value (`slider_text`), so the old F1 panel's slider row says MEDIUM too.
- Checked in the game (school scene, DLSS, 1080p, a replay through the overlay): `3` started at HIGH, `0.65` at HIGH,
  each Left / Right one level with the picture following; screenshots of the settings page and the preview bar. The
  owner tried the mouse on the test window during the run. 71 tests pass.
