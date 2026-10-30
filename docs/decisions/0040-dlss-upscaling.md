# 0040 — Upscaling with NVIDIA DLSS

Status: accepted, first version (2026-10-29). Added after FSR (decision 0039) because FSR turns fine patterns
smaller than a render pixel (the schoolyard's perforated fence, far goal nets) into grain at performance; a
machine-learned upscaler handles those better. The owner's GPU (RTX 4070 laptop) runs DLSS.

## Context

The upscaling plumbing from 0039 (two resolutions, the scene image named by address after the anti-aliasing
point, the depth stretch, the texture bias) takes any upscaler with FSR's inputs. DLSS takes the same: the smaller
3D picture, its depth, motion and the per-frame shift.

Two questions before building: the licence, and whether NVIDIA's library links with our compiler.

- **Licence** (NVIDIA RTX SDKs licence, `LICENSE.txt` in the SDK): proprietary. The SDK may be distributed only
  inside an application in object code, not reverse engineered, and not made subject to an open-source licence:
  so nothing of it is committed to this MIT repository; `tools/yw4/deps.py` downloads the needed files at a pinned
  tag, each checked against its SHA-256, and whoever builds accepts NVIDIA's terms. Applications using DLSS must
  credit NVIDIA (the settings panel's option text and the README do). Notifying NVIDIA is required only before a
  commercial release.
- **Linking**: the NGX library (`nvsdk_ngx_d.lib`) is built with MSVC; llvm-mingw has neither MSVC's runtime nor
  its C++ library. Linking a test showed it needs only twelve runtime pieces: `runtime/src/msvc_shim.c` supplies
  them (MSVC-named new/delete, `_fltused`, the /GS cookie and check, two GS exception filters, two STL error
  functions, thread-safe local statics); empty stand-ins satisfy its default-library requests. It throws no C++
  exceptions.

## Decision

1. **DLSS as anti-aliasing mode 5** (`YW4_AA=5`, panel ANTI-ALIASING: DLSS), next to FSR, from the same inputs
   (`runtime/src/gpu_dlss.c`). The colour goes in through the same reversible curve (DLSS told it is not HDR) and
   DLSS works out its own exposure. Jitter: Halton 2, 3 with 8 × ratio² positions, as NVIDIA suggests. Upscaling
   modes map to DLSS's: none = DLAA, quality, balanced, performance. When DLSS can't run (no NVIDIA RTX GPU, an old
   driver, a build without the SDK), FSR takes over, then TAA.
2. **Vulkan**: on NVIDIA GPUs, `window.c` enables the device extensions NGX lists (`VK_NVX_binary_import`,
   `VK_NVX_image_view_handle`, `VK_KHR_push_descriptor`; the old `VK_EXT_buffer_device_address` is left out where
   the core feature exists, because NGX allocates with the core feature's flag). Also enabled while there:
   `shaderDemoteToHelperInvocation`, which shaders compiled for Vulkan 1.3 need for `discard` (the validation
   layer flagged it).
3. **The model**: DLSS's defaults are its transformer models (K for DLAA/quality/balanced, M for performance).
   On this GPU at 5120×2160 output they cost 5.3 and 7.4 ms. The CNN model (preset E, marked deprecated in the
   SDK but working in the pinned DLL) costs 2.6 ms, less than FSR (3.7-4.5), and looks nearly as clean on the
   fine patterns. So a panel row DLSS MODEL: FAST (preset E, default) or BEST (the transformers);
   `YW4_DLSS_PRESET=<letter>` forces one for tests.
   **Changed 2026-10-31: the transformers only, the row removed.** The CNN makes shadow edges flicker: the
   character's shadow, standing still in the forest, has 4-6% of its edge pixels flickering over 16 frames with
   it and 0% with the transformers; the same in the build that brought DLSS in (02120ba), so not a regression.
   The owner saw it too and chose the clean picture over the CNN's speed. `YW4_DLSS_MODEL=0` keeps the CNN for
   tests.

## Results (bamboo forest, 5120×2160 21:9, owner's settings, one run switching modes)

| Mode | DLSS fast | DLSS best | FSR 3.1 |
|---|---|---|---|
| none (DLAA / FSR native) | ~53 | - | ~46 |
| quality | ~81 | ~66 | ~75 |
| balanced | ~93 | ~73 | ~86 |
| performance | ~112 | ~71 (M) / ~88 (K) | ~104 |

Native with TAA: ~59. Schoolyard walk, 1:1 crops: where FSR leaves the perforated fence as grain, both DLSS
models show a clean, regular pattern (not as round as native's holes); the goal net stays more structured.
No Vulkan validation errors.

## Not done yet

- Linux: NVIDIA ships `libnvidia-ngx-dlss.so`; the build fetches only the Windows files so far.
- The transformer models' cost may suit a desktop GPU better; not measured on one.
- The jitter and motion signs were taken over from FSR and look right in still and moving shots; switches to
  flip them for tests: `YW4_DLSS_JITTER_SIGN`, `YW4_DLSS_MV_SIGN`.
