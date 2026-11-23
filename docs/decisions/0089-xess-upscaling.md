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
6. The launcher has no XeSS switch: every Windows build includes it (~77 MB more to download), `--no-xess` leaves it
   out.

## Not checked yet

- Image quality against FSR and DLSS (same-run, same spot), and its cost on the RTX 4070 laptop and the 780M.
- Intel Arc itself (no Arc GPU here).
- Battles' zoom-in (TAA stands in, as for FSR and DLSS), menus, cutscenes, 21:9, frame generation on top.
- The Vulkan validation layer with XeSS on.
- Whether a first-start default should pick XeSS on Intel GPUs.
