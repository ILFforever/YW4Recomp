# 0021 — The draw path: NVN command lists on Vulkan

Date: 2026-10-21
Status: accepted (owner: "go for it" on the proposal; this file is the proposal)

## The question

NVN commands are recorded (0017) and the shaders are translated (0020). To draw, the game's memory has to reach the GPU: vertex, index and uniform buffers, textures, render targets. How?

## Measured (YW4_NVN_STATS, title menu)

- **Six memory pools, all 4 KiB-aligned**, 1.4 GB in total:

  | Flags | Meaning (NVN memory pool flags) | Size | Probable use |
  |---|---|---|---|
  | `0xa1` | CPU no access, GPU cached, compressible | 298 MiB | render targets |
  | `0x24` ×2 | CPU cached, GPU cached | 900 + 32 MiB | textures, meshes |
  | `0x22` | CPU uncached, GPU cached | 64 MiB | |
  | `0x12` | CPU uncached, GPU uncached | 64 MiB | per-frame data (uniforms) |
  | `0x64` | CPU cached, GPU cached, shader code | 32 MiB | shaders |

- Texture data: 42 MB through `WriteTexels` at start-up, then none (the menu).
- Per frame: 25 draws (none indexed), 2 compute dispatches, 16 programs, 33 shaders loaded so far, all of them translated.
- The GPU (RTX 4070 Laptop, Vulkan 1.4) has `VK_EXT_external_memory_host`, dynamic rendering, extended dynamic state 3, vertex input dynamic state, custom border colours.

## Decision

1. **Buffers: game memory used in place.** Each memory pool the CPU can reach is imported into Vulkan with `VK_EXT_external_memory_host` (guest pointers are host pointers, pools are page-aligned). A buffer is (pool, offset): vertex, index, uniform, storage and indirect buffers need no copies and no change tracking, and what the GPU writes (compute, counters) lands where the game reads it.
2. **Textures: Vulkan images, uploaded when their data changes.** One `VkImage` per NVN texture, created at first use. Images in the CPU-no-access pool are **render targets and never uploaded or read back**: the CPU cannot see them on the Switch either. Other textures are uploaded (de-swizzled with `tegra.c`) at first use and after `WriteTexels`/copies into them. Change detection for textures the CPU writes directly is added only if a measurement shows the game doing it.
3. **Execution at submit.** `QueueSubmitCommands` walks the recorded list and records a Vulkan command buffer: dynamic rendering, pipelines built on demand from (program, state, target formats) and cached, descriptors from the bound buffers and textures. A timeline semaphore numbers submissions; NVN syncs wait on those numbers. Simple global barriers between passes first (correct, maybe slow); finer later if profiling says so.
4. **Present on the GPU.** The window texture is an image; present blits it to the swapchain. The CPU clears and read-back of 0019 go.
5. **Shaders** are loaded as SPIR-V from `work/shaders/glsl` (`YW4_SHADER_DIR`) for now; embedding comes with packaging.

Requirements: Vulkan 1.3 and `VK_EXT_external_memory_host` (NVIDIA, AMD, Intel on Windows; Mesa RADV/ANV on Linux). No new library (no VMA): the few allocations are done by hand.

## Alternatives

- **Copy buffers to GPU memory with change tracking** (what LAS Recomp does for most data). Faster GPU reads, but needs write tracking for 1.4 GB of game memory and gets stale data wrong silently. Imported memory is read over PCIe, slower for big meshes; that is a performance question for later (Phase 4c owns the renderer anyway), and copying hot buffers can be added then.
- **Adapt LAS Recomp's Vulkan backend** (MIT, ~300 KB). Built for another game and mirrors its Metal backend; too big to own line by line. It stays a reference.
- **Keep rendering on the CPU.** No.

## Steps

1. **Done:** Vulkan 1.3 device with the features the shaders need, all six pools imported (1.4 GB, no copies), an image per texture, clears and present on the GPU, NVN syncs on a timeline semaphore (`runtime/src/gpu.c`, `vk.h`). The game runs at 30 frames/s on it; the window is still black (nothing draws yet), and screenshots now come from the GPU image. Headless (`YW4_HEADLESS=1`) keeps the CPU path.
2. **Done:** pipelines and draws (`runtime/src/gpu_draw.c`). The translated SPIR-V and a small interface file per shader (`<key>.iface`) are loaded on first use; descriptor sets per stage from bound uniform buffers, storage buffers and textures (texture handle = id | sampler id << 20 in the bound pools); pipelines cached per program on formats, blending, channel masks, polygon mode, topology and vertex input, everything else dynamic. Textures in CPU-visible pools are uploaded at first use and after `WriteTexels`. Quads (the 2D passes use them) become triangles through a fixed index buffer. Measured: the game sets the **upper-left window origin and near-at-0 depth** (Direct3D conventions), so the viewport is flipped and no depth fix-up is needed. Present honours **`nvnWindowSetCrop`**: the game renders at 1600×900 into the 1920×1080 window texture and crops (dynamic resolution). Result: the LEVEL-5 logo and the autosave notice, at 30 frames/s. `YW4_GPU_TRACE=<n>` describes the first n draws (programs, targets, viewport, textures).
3. Compute dispatches, the rest of the state, then scenes beyond the menu.
