# 0059 — AMD Vulkan depth format fallback (D24S8 to D32S8)

<!-- decision
state: accepted
date: 2026-11-10
replaces: none
code: runtime/src/gpu.c, runtime/src/gpu_ab.c, docs/debugging.md
-->

Status: accepted (2026-11-10). Falls back to `VK_FORMAT_D32_SFLOAT_S8_UINT` when `VK_FORMAT_D24_UNORM_S8_UINT` is not supported as a depth/stencil attachment.

## Context

On clean installations running on AMD GPUs (e.g. AMD Ryzen integrated Radeon graphics and discrete Radeon GPUs),
the 3D game world failed to render, blowing out to a blinding white screen, and character models appeared inverted
with their facial features and mouth interiors rendered across the backs of their heads.

The opening prologue scene in Nate's room exhibited:
1. Missing walls, tatami floor, and furniture, entirely overwritten by pure white.
2. Whisper's mouth and eyes drawn directly on the back of his body mesh.
3. Jibanyan's mouth interior and skull geometry drawn over his front facial features.

## Findings

- **AMD 24-bit depth support**:
  NVN format `0x35` (`NVN_FORMAT_DEPTH24_STENCIL8`) was mapped to `VK_FORMAT_D24_UNORM_S8_UINT`. On AMD's Vulkan
  driver, `VK_FORMAT_D24_UNORM_S8_UINT` does not support `VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT`.
- **Attachment bit dropped**:
  In `runtime/src/gpu.c:1404-1406`, format properties are queried from the physical device:
  ```c
  if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
      usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  ```
  Because the driver lacks attachment support for D24S8, the primary scene depth buffer (`0x7269ff3000`) was
  allocated without `VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`, disabling depth testing and depth writes.
- **Why symptoms appeared**:
  - Without depth testing, rendering defaults to order-of-submission (painter's algorithm). Character details
    (facial decals, mouth interiors) drawn after base body meshes overwrote front faces and back heads alike.
  - The scene geometry is drawn in the G-buffer pass. Later, `sky_fog.frag` draws a full-screen quad at far-plane
    depth (`1.0`) with `LESS_OR_EQUAL` depth comparison. With depth comparison non-functional, the quad overwrote
    the entire room geometry with white sky/fog.

## Decision

1. **Dynamic format fallback**:
   In `runtime/src/gpu.c`, format `0x35` queries physical device capabilities via `d24s8_format()`. If
   `VK_FORMAT_D24_UNORM_S8_UINT` supports depth attachment and sampled image operations, it is preserved. If not
   (as on AMD Radeon), it dynamically falls back to `VK_FORMAT_D32_SFLOAT_S8_UINT`.
2. **Test override**:
   `YW4_FORCE_D32S8=1` forces the D32S8 mapping on any GPU (including NVIDIA) for verification.
3. **A/B comparison support**:
   `runtime/src/gpu_ab.c` recognises `VK_FORMAT_D32_SFLOAT_S8_UINT` alongside `VK_FORMAT_D32_SFLOAT` for floating-point
   depth comparisons.

## Tested

- 2026-11-10 (owner): an ASUS Zenbook with a Ryzen 5 7530U (integrated Radeon graphics), clean install through the
  launcher: the picture is right, about 40 frames/s at native resolution, where the Eden emulator gets 25-30.
- On NVIDIA (RTX 4070 laptop), `YW4_FORCE_D32S8=1` runs the fallback without problems; the D24S8 path is unchanged.
