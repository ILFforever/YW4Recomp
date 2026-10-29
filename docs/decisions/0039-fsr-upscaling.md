# 0039 — Upscaling with AMD FSR

Status: accepted, first version (2026-10-29). The owner chose FSR (option A below) for higher frame rates at 4K,
after 60 frames/s and no limit came in (the game moves by measured time, `runtime/src/framerate.c`).

## Context

At the owner's settings (5120×2160, 21:9, PCSS, effects full) the RTX 4070 laptop is 97-98% busy at ~125 W: the
GPU limits the frame rate (~59 frames/s with TAA). Most of that time scales with the pixel count of the 3D
(soft shadows 29%, the game's surfaces and lighting, full-size effects). Drawing the 3D smaller and rebuilding
a full-size picture is the largest single lever. The UI must stay full size and sharp.

The game draws the 3D at 1600×900 and the UI into the same image, then copies it 1:1 into the window image
(decision 0031): there is no stretch of its own to replace.

## Options considered

| Option | For | Against |
|---|---|---|
| **A. AMD FSR SDK 2.4.0** (signed DLLs, Vulkan) | Newest, no shader build of our own; FSR 4 on the AMD GPUs it supports; frame generation later from the same package | Windows only (DLLs); released the day before |
| B. FidelityFX SDK 1.1.4 | Mature, full source | No FSR 4; a Windows-oriented build of its own |
| C. FSR 2 from source (GLSL) | Builds everywhere like our shaders | Older quality, no frame generation |
| Our own TAA upscaler | Cheap (~1 ms), no dependency | Softer than FSR; still to do |

Licence (checked in the archive, `Kits/FidelityFX/docs/license.md`): files not listed there fall under AMD's
binary licence (no reverse engineering), but the listed ones are MIT, and the list names everything we use: the
API and upscaler headers and `amd_fidelityfx_loader_vk.dll` / `amd_fidelityfx_upscaler_vk.dll`. MIT fits the
project (decision 0027). Nothing of the SDK is committed: `tools/yw4/deps.py` downloads the source archive
(pinned SHA-256), extracts only those headers and DLLs into `work/deps/fsr-2.4.0/`, and `recomp/build.py` copies
the DLLs next to `yw4.exe`.

## Decision

1. **FSR as an anti-aliasing mode** (`YW4_AA=4`, panel ANTI-ALIASING: FSR), at TAA's place: after the bloom
   composite, before the UI. Same inputs as TAA (decision 0032): the 3D draws shifted per frame (FSR's own
   jitter sequence, applied as a viewport shift), the game's motion copied when the G-buffer pass ends, plus the
   scene's depth. The colour goes in through the anti-aliasing's reversible curve (perceptual, 0..1, so FSR is
   told it is non-linear) and comes back through its inverse. `runtime/src/gpu_fsr.c` loads the DLLs on first
   use, makes a context per size pair, and falls back to TAA when FSR can't run.
2. **Upscaling as two resolutions** (`YW4_UPSCALE`, panel UPSCALING: off / quality 1.5× / balanced 1.7× /
   performance 2×, greyed out unless the anti-aliasing is FSR). `gpu.c` gives the 3D's screen-sized images the
   render height (output / ratio) and two kinds the output height: the presented window images, and the scene's
   colour and depth images from the anti-aliasing point until the present. Those two are named by address: at
   the anti-aliasing point the address starts to name a second, output-sized image, so the game's UI draws and
   its final copy land at full size without knowing. All the game's shaders read screen images in 0..1, so the
   3D needs no change; the camera row of tap offsets uses the render height.
3. **The depth is stretched too** (`aa_depth_upscale.frag`): the draws after the anti-aliasing point include world
   markers (the "!" above people) that the game hides behind what stands in front. Without depth they showed
   through walls and from afar (owner, 2026-10-29).
4. **Texture bias** while upscaling: drawn smaller, the 3D would pick smaller mip levels where fine patterns (the
   schoolyard's goal net, the perforated high fence) average out to a flat colour. Mipmapped, non-comparing
   samplers of the draws before the anti-aliasing point get log2(render / output) − 1 (AMD's recommendation;
   quality −1.58, performance −2).
5. **Device features**: 16-bit floats, 16-bit storage, 8-bit ints and subgroup size control are enabled where the
   GPU has them, for FSR's fast shader variants.
6. **Version 3.1.6** (the DLL's default on NVIDIA). It also offers 2.3.4 (`YW4_FSR_VERSION=2.3.4`): ~20% cheaper,
   but it turns the high fence into wavy moiré where 3.1.6 leaves fine grain.

## Results (late-game save, bamboo forest, 5120×2160 21:9, owner's settings, one run switching modes)

| Mode | 3D drawn at | frames/s | FSR's own cost |
|---|---|---|---|
| native + TAA | 5120×2160 | ~59 | (TAA 1.4 ms) |
| FSR, no upscaling | 5120×2160 | ~46 | ~6.6 ms |
| FSR quality | 3413×1440 | ~75 | ~4.5 ms |
| FSR balanced | 3013×1271 | ~86 | ~4.0 ms |
| FSR performance | 2560×1080 | ~104 | ~3.7 ms |

FSR's cost follows the output's size, so it shrinks little with the mode. Checked with 1:1 crops: the UI is as
sharp as native in every mode; a running character stays sharp (a faint 1-2 pixel edge on a moving leg, no
trail); objects close to the camera, which the game fades with a dot pattern, become smooth transparency.
Fine high-contrast patterns below a render pixel (the perforated fence at performance) come out grainy:
ruled out as causes were history resets (1 in 900 frames), motion (FSR's debug view), the jitter's signs (all
four alike), missing jitter (320-560 draws shifted per frame) and the camera distances given to FSR. This is the
known limit of the hand-written FSR versions; machine-learned upscalers do better there.

## Not done yet

- Our own TAA upscaler (the cheap option), and DLSS for NVIDIA RTX GPUs (licence and linking to check first).
- Linux: no FSR DLLs there; TAA is used.
- Per-object motion (decision 0032's open item) would help FSR with characters that move on their own.
- The camera's real near/far and field of view for FSR (placeholders 0.1 / 1000 / 45°; no visible effect).
- Whether the fence's own draws get the per-frame shift (decode one frame's draws).
- Testing switches: `YW4_FSR_DEBUG`, `YW4_FSR_DEBUG_VIEW`, `YW4_FSR_LOG`, `YW4_FSR_TEST_RATIO`, `YW4_FSR_MV_SIGN`,
  `YW4_FSR_JITTER_SIGN`, `YW4_FSR_NEAR` / `YW4_FSR_FAR`, `YW4_FSR_SHARPNESS`, `YW4_FSR_MIP_BIAS` (docs/debugging.md).
