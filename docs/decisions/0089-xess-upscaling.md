# 0089 — Upscaling with Intel XeSS (Windows)

<!-- decision
state: accepted
date: 2026-11-24
replaces: none
code: runtime/src/gpu_xess.c, runtime/src/gpu_aa.c, runtime/src/window.c, runtime/src/settings.c, tools/yw4/deps.py, recomp/build.py
-->

Status: accepted, first version (2026-11-24). Owner: "add it to Windows".

## Context

Players asked for XeSS next to FSR and DLSS. XeSS is a machine-learned upscaler that runs on any GPU: on Intel Arc
with its matrix units (XMX), elsewhere on a slower general path (DP4a). That gives AMD players a learned upscaler
too (FSR on AMD shakes at the edges, #20).

## What Intel ships (checked 2026-11-24, XeSS SDK 3.0.2, github.com/intel/xess)

- **Licence:** the headers are MIT (an SPDX line in each). `libxess.dll` is under Intel's Simplified Software
  Licence: use and redistribution allowed, binary unchanged, with Intel's notice and terms alongside; no reverse
  engineering. MIT project-compatible (decision 0027) as long as we only load the DLL. Nothing is committed:
  `tools/yw4/deps.py` downloads the release archive from Intel's address only (pinned SHA-256), and
  `recomp/build.py` copies `libxess.dll` and its licence (`libxess.license.txt`) next to `yw4.exe`.
- **Windows only.** No Linux library. The xess-xmx-linux project runs XeSS for Windows games under Proton; it can't
  serve a native Linux program. Linux keeps FSR, DLSS and TAA.
- **Upscaling only.** XeSS frame generation (`libxess_fg.dll`) is Direct3D 12 only.
- `libxess.dll` (77 MB) needs the Visual C++ runtime, as FSR's DLLs already do: no new requirement for players.

## Decision

1. **XeSS as anti-aliasing mode 6** (`YW4_AA=6`, panel ANTI-ALIASING: XeSS), after DLSS, from the same inputs as DLSS
   (decision 0040): the encoded scene (told LDR: the curve is 0..1), the game's motion with DLSS's scale, the jitter
   from the same Halton sequence (8 × ratio² positions), a depth-only copy of the depth. Falls back to FSR, then TAA.
   Our sharpening runs after it (XeSS has none of its own), at our own steps.
2. **The UPSCALING modes map to XeSS's** quality, balanced and performance with Intel's older scale factors
   (`xessForceLegacyScaleFactors`: 1.5 / 1.7 / 2.0, like FSR's and DLSS's); OFF is XeSS's anti-aliasing mode
   (1.0). If the render size falls outside the chosen setting's range, the nearest setting that takes it is used.
3. **Device setup:** unlike FSR, XeSS needs instance and device extensions and features of its own. The DLL is
   loaded before the Vulkan instance is made; its extensions are added where the GPU has them, and it fills its
   features into our feature chain. `YW4_XESS=0` leaves all of that out (the device is made as before XeSS).
4. **Layouts:** XeSS wants its inputs in SHADER_READ_ONLY_OPTIMAL; ours live in GENERAL, so they move there and back
   around its pass. That is why the depth always goes in as a depth-only copy (a depth with stencil would need both
   aspects moved).
5. Its shaders are built in the background at start (`xessVKBuildPipelines`, non-blocking); `xessVKInit` for a size
   waits for them, the first time.
6. **A launcher switch** (Options: Intel XeSS, Windows only, on by default, ~77 MB), like FSR's and DLSS's: off
   passes `--no-xess` through `tools.yw4.build_install` to `recomp.build`. No protocol change: older game code
   ignores the option (`parse_known_args`), and older launchers never pass it (their builds include XeSS). Installs
   from before the switch count as built with it.

## Results (RTX 4070 laptop, late-game bamboo forest, 3840x2160 output, quality: 3D at 2560x1440, 2026-11-24)

One run switching modes (`YW4_SET_AT`), GPU time from `YW4_GPU_PASSES`, sharpness = Laplacian variance of the
screenshot, sharpening at MEDIUM (each upscaler's own steps):

| Mode | the upscaler itself | GPU per frame | sharpness |
|---|---|---|---|
| XeSS (DP4a on NVIDIA) | ~5.0 ms | ~11.8 ms | 69 |
| FSR 3.1.6 | ~4.7 ms | ~11.6 ms | 137 |
| DLSS (model K) | ~4.8 ms | ~11.6 ms | 162 |
| native TAA, no upscaling | 1.4 ms (TAA) | ~12.0 ms | 133 |

XeSS works and costs about what FSR and DLSS do, but on this NVIDIA GPU its picture is clearly softer (leaves and
water smeared at 1:1). XeSS without upscaling (4K in, 4K out) scores 90, also below TAA. Not a sign error: separate
runs to the same frame with the jitter flipped in x, y or both, or the motion flipped, scored 62-73 against 71. So
far this looks like XeSS's general (DP4a) path; Intel Arc's XMX path is said to be sharper, not checkable here.

## Not checked yet

- Whether stronger sharpening for XeSS (DLSS's steps) closes the gap; the 780M.
- Intel Arc itself (no Arc GPU here).
- Battles' zoom-in (TAA stands in, as for FSR and DLSS), menus, cutscenes, 21:9, frame generation on top.
- The Vulkan validation layer with XeSS on.
- Whether a first-start default should pick XeSS on Intel GPUs.
