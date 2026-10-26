# 0031 — Resolution scaling of the 3D view

Date: 2026-10-27
Status: accepted (owner: resolution scaling next, before shimmer and before finishing the takeover); off by default
(`YW4_RES=900`), F4 cycles 900 → 1080 → 1440 → 2160 lines

## The question

The game renders its 3D view at 1600×900 and the window shows that picture stretched. How do we render it sharper:
1920×1080, 2560×1440, 3840×2160? And does that need the takeover (0030) at 100% first?

## What we measured (docs/frame.md, "What depends on the screen size")

- **The images.** Everything sized from the screen is a GPU-only render target: 1600×900 (G-buffer, lit pass,
  effects, and the UI, drawn into the same colour image), copies at 1/2, 1/3, 1/4, 1/8, ~1/13 of it, and the
  1920×1080 window image, into whose top-left 1600×900 the last pass copies the picture by sampling; the game
  presents that crop. At start the engine makes its targets 1920×1080 first (with copies at 960×540, 639×359,
  480×270), then 1600×900 at the same addresses: it can render at either size.
- **The shaders are resolution-independent.** They read screen images in 0..1 coordinates from interpolated
  positions. The pixel position is used only to tile a 16×16 dither texture (the screen-door fade) or as depth;
  whole-pixel reads hit the same dither texture; size queries return the real size. One constant row is in pixels:
  the camera block's row 32 (1.5/1600, 1.5/900, 0.75/533, 0.75/300), tap offsets of the lit pass's upsample of the
  half-size lighting (215 shaders).
- **No copy commands.** The engine shrinks images by drawing them into smaller ones. Pixel rectangles appear only
  as viewports and scissors (clears are whole-image).

## Decision

Scale at the image level, in the runtime, the way the shadow maps are scaled (F8):

- An image's key carries a ratio (`scale_num / scale_den`; it was a whole number for the shadow maps). Screen-sized
  GPU-only 2D targets (a list of sizes, both chains) are made `height / 900` times larger, rounded to whole pixels.
  One factor for all of them, so the ratios between them (a half-size image read at half the position) hold.
- Draws into a scaled target get their viewport and scissor scaled (from the depth target, or the first colour target
  when there is none: the bloom and UI passes have no depth). Scissors are scaled outward so they cover what they did.
- The window image is scaled too, and the presented crop with it: otherwise the sharper picture is copied into a
  1600×900 corner and squeezed back down.
- Shaders that read camera row 32 get the camera block from a copy with that row times `900 / height` (the same copy
  that scales the shadow filter spread for F8).

**The takeover is not a prerequisite.** Scaling is per image, so the game's own draws scale as ours do. Finishing
the takeover matters for step 2 (our own selection), done pass by pass.

## Results (town walk, RTX 4070 laptop)

| 3D view | GPU per frame | Frames/s |
|---|---|---|
| 1600×900 (the game's) | 6.4 ms | 30 |
| 1920×1080 | 7.4 ms | 30 |
| 2560×1440 | 8.1 ms | 30 |
| 3840×2160 | 9.6 ms | 30 |

The cost grows much slower than the pixel count: the passes are dominated by vertex work (decision 0028). At 900 the
picture is the game's (the code path is the old one with a ratio of 1; screenshots differ between runs only as much
as two runs of the same build do).

## Open

- Battles, menus and cutscenes: other screen-sized targets (GPU-only 2D targets of sizes not in the list are logged
  once). The 1920×1080 chain's bloom sizes are not known yet (its 1/8 would be 240×135, the size of the occlusion
  boxes' depth image, which stays unscaled).
- Three shaders hold odd small constants: `4f5854cdab65c4b5`, `5ef1a937fe5a9908`, the compute shader
  `ff9fe0a375532914`.
- The same-frame A/B tool (`YW4_AB_AT`) copies targets texel by texel: not tried with scaled targets.

## Alternatives

- **Emulator-style: scale every render target.** Simpler rule, but it would also scale the shadow maps' own setting,
  the occlusion boxes (culling reads them), the ambient cube and the minimap, for no gain.
- **Our own renderer at a higher resolution, pass by pass.** Needs every pass ours first; the images-level rule gets
  the whole frame now and stays compatible with passes we replace later.
