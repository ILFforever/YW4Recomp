# Android Port Plan (and Desktop Stepping Stones)

Status: draft plan. Grounded in static reading of the current runtime (`runtime/src/`, `recomp/build.py`).
No runtime code has been changed yet.

## Summary

Porting YW4 Recomp to Android (AArch64) does **not** require a rewrite. The recompiler already emits portable C11,
the runtime already builds and runs on Linux (POSIX `pthread`, `mmap`, `clock_gettime`, bundled `third_party/musl`
math), and windowing/audio/controllers already sit on SDL3 + Vulkan.

The two biggest technical blockers for mobile GPUs are **also** useful desktop portability improvements and can be
built and verified on Windows/Linux first behind environment-variable toggles:

1. **Phase 1:** Fallback path when `VK_EXT_external_memory_host` is unavailable (`YW4_HOST_IMPORT=0`).
2. **Phase 2:** BC1–BC7 texture decompression fallback when `textureCompressionBC` is unavailable (`YW4_NO_BC=1`).
3. **Phase 3:** AArch64 runtime cleanups and NDK cross-compilation.
4. **Phase 4:** Android bring-up (VA layout, mobile Vulkan/TBDR quirks, SDL3 lifecycle, PC-side APK packaging).

---

## Current State (Verified in Codebase)

- **Recompiler (`recomp/build.py`):** Emits portable C11 (`work/recomp/*.c`). Cross-compiling with Android NDK Clang
  for `aarch64-linux-android` keeps every `recomp/overrides.toml` hook, custom renderer pass, 60 fps patch,
  widescreen patch, and mod loader intact.
- **x86-specific code in `runtime/`:** Very small surface area:
  - `runtime/include/yw4cpu.h` (lines 170–210) and `runtime/include/yw4fp.h` (lines 25–30) already have portable
    `#else` fallbacks outside `#if defined(__x86_64__)`.
  - `runtime/src/host.c` (lines 494–498) already has `#elif defined(__aarch64__)` for crash-handler PC extraction.
  - `runtime/src/bones.c` (lines 85–161) uses SSE intrinsics (`__m128`); needs a NEON or scalar path.
  - `runtime/src/sysinfo.c` (line 111) has an `#if defined(__x86_64__)` check.
- **Desktop-only extras:** DLSS (`gpu_dlss.c`), XeSS (`gpu_xess.c`), FSR/Streamline (`gpu_fsr.c`, `gpu_fg.c`), and
  Discord Rich Presence (`discord*.c`) are already optional and can be compiled out on Android.
- **Hard Vulkan requirements today (`runtime/src/window.c`):**
  - `VK_EXT_external_memory_host`: required for the draw path (`window.c:507`, `gpu.c:156`).
  - Core Vulkan features (`window.c:629`): `dynamicRendering` (1.3), `timelineSemaphore` (1.2), and
    `shaderDrawParameters` (1.1).
  - Texture compression: `gpu.c:949-962` maps NVN formats `0x41`–`0x50` directly to `VK_FORMAT_BC1`..`VK_FORMAT_BC7`
    using `textureCompressionBC` (`window.c:573`).

---

## Phase 1: `VK_EXT_external_memory_host` Fallback (`YW4_HOST_IMPORT=0`)

### Why
Stock Android Vulkan drivers (Qualcomm Adreno, ARM Mali, Imagination PowerVR, Samsung Xclipse) almost never expose
`VK_EXT_external_memory_host`. Today, `gpu_init()` returns immediately when `!vk.host_import` (`gpu.c:156`), leaving
no draw path.

### Design
1. **Toggle (`runtime/src/vk.h`, `runtime/src/window.c`):**
   - Allow `YW4_HOST_IMPORT=0` to disable `VK_EXT_external_memory_host` on desktop so the fallback path can be tested
     and benchmarked directly on Windows/Linux.
   - Remove the early `if (!vk.host_import) return;` guard in `gpu_init()` (`gpu.c:156`).
2. **Shadow pool buffers (`runtime/src/gpu.c`):**
   - Add `uint8_t *mapped;` to `GpuPool`.
   - In `pool_import(NvnPool *p)`:
     - When `vk.host_import` is enabled: keep the existing `VK_EXT_external_memory_host` import path unchanged.
     - When `!vk.host_import`: create a regular `VkBuffer` of size `p->size` (no `VkExternalMemoryBufferCreateInfo`),
       allocate `HOST_VISIBLE | HOST_COHERENT` memory (preferring `HOST_CACHED` if available, and `DEVICE_LOCAL` on
       unified-memory GPUs), bind it, and persistently `vkMapMemory` it into `g->mapped`.
     - In `gpu_pool_finalize()`: unmap `g->mapped` (if non-NULL) before freeing `g->mem`.
3. **Syncing CPU writes into the shadow buffer (`gpu.c`, `gpu_query.c`, `gpu_internal.h`):**
   - **Reads by GPU (`gpu_buffer_at()`):** Used by `gpu_draw.c` (UBOs, SSBOs, dynamic VBs/IBs) and `gpu_watch.c`.
     When `!vk.host_import`, copy `[addr, addr + size)` from guest RAM into `g->mapped + off` before returning
     `(g->buf, off)`.
     - *Watch-out:* `gpu_buffer_at()` is also called by `gpu_draw.c` (lines 2179, 2199, 2204) as a bounds/existence
       check; add a read-only check or avoid redundant `memcpy` when only validating addresses.
     - *Watch-out:* On `POOL_CPU_NO_ACCESS` pools, never copy from guest RAM (the GPU owns the contents).
   - **Static geometry copies (`gpu_geometry_copy()`):** Copies `[addr, addr + size)` into `geo_buf` (`gpu.c:829`).
     When `!vk.host_import`, sync `[addr, addr + size)` into `g->mapped + (addr - p->storage)` right before
     `vkCmdCopyBuffer` (or upload via staging). Since static geometry is copied once into `geo_buf`, this adds no
     per-frame cost after the first draw.
   - **GPU detiling (`detile()`):** Reads swizzled texture bytes from `((GpuPool *)p->gpu)->buf` (`gpu.c:1098`).
     When `!vk.host_import`, either sync `[start, start + span)` into `g->mapped` before dispatching `detile.comp`,
     or fall back to CPU `tegra_read_region()` (which reads directly from `t->addr`).
   - **GPU-written occlusion queries (`runtime/src/gpu_query.c`):** `gpu_query.c` writes 8-byte query results into the
     pool via `vkCmdCopyQueryPoolResults` / `vkCmdFillBuffer` (lines 143–153) and reads them on the GPU via
     `VK_EXT_conditional_rendering` (line 277). Add a `gpu_buffer_at_nosync()` (or `sync` flag) for `gpu_query.c` so
     we **never** overwrite GPU-written query results with stale guest RAM.

### Files Touched
- `runtime/src/vk.h`
- `runtime/src/window.c`
- `runtime/src/gpu.c`
- `runtime/src/gpu_internal.h`
- `runtime/src/gpu_query.c`
- `docs/debugging.md` (document `YW4_HOST_IMPORT=0`)
- `docs/decisions/NNNN-*.md` (a decision record, the next free number)

### Verification
- Build with `python -m recomp.build`.
- Run via `python -m tools.dev.run` with `YW4_SETTINGS_FILE=work/test_nohost.ini` and `YW4_HOST_IMPORT=0` across town,
  battle, cutscene, and menu saves.
- Compare screenshots against default (`YW4_HOST_IMPORT=1`) — must match pixel-for-pixel.
- Record frame-time delta and confirm occlusion queries and dynamic UI/minimap/bike buffers update cleanly.

---

## Phase 2: BC1–BC7 Texture Decompression Fallback (`YW4_NO_BC=1`)

### Why
Mobile GPUs (Adreno, Mali, PowerVR) do not support `textureCompressionBC` (BC1–BC7); they support ETC2 and ASTC.
Without a fallback, every BCn texture creation fails or produces undefined rendering on Android.

### Format Mapping (`runtime/src/gpu.c:949-962`)

| NVN format | Native Vulkan format | Fallback Vulkan format (when `!vk.bc_textures`) | Output bytes/texel |
|---|---|---|---|
| `0x41`, `0x42` (BC1 UNORM) | `VK_FORMAT_BC1_RGBA_UNORM_BLOCK` | `VK_FORMAT_R8G8B8A8_UNORM` | 4 |
| `0x45`, `0x46` (BC1 SRGB) | `VK_FORMAT_BC1_RGBA_SRGB_BLOCK` | `VK_FORMAT_R8G8B8A8_SRGB` | 4 |
| `0x43` (BC2 UNORM) | `VK_FORMAT_BC2_UNORM_BLOCK` | `VK_FORMAT_R8G8B8A8_UNORM` | 4 |
| `0x47` (BC2 SRGB) | `VK_FORMAT_BC2_SRGB_BLOCK` | `VK_FORMAT_R8G8B8A8_SRGB` | 4 |
| `0x44` (BC3 UNORM) | `VK_FORMAT_BC3_UNORM_BLOCK` | `VK_FORMAT_R8G8B8A8_UNORM` | 4 |
| `0x48` (BC3 SRGB) | `VK_FORMAT_BC3_SRGB_BLOCK` | `VK_FORMAT_R8G8B8A8_SRGB` | 4 |
| `0x49` (BC4 UNORM) | `VK_FORMAT_BC4_UNORM_BLOCK` | `VK_FORMAT_R8_UNORM` | 1 |
| `0x4A` (BC4 SNORM) | `VK_FORMAT_BC4_SNORM_BLOCK` | `VK_FORMAT_R8_SNORM` | 1 |
| `0x4B` (BC5 UNORM) | `VK_FORMAT_BC5_UNORM_BLOCK` | `VK_FORMAT_R8G8_UNORM` | 2 |
| `0x4C` (BC5 SNORM) | `VK_FORMAT_BC5_SNORM_BLOCK` | `VK_FORMAT_R8G8_SNORM` | 2 |
| `0x4D` (BC7 UNORM) | `VK_FORMAT_BC7_UNORM_BLOCK` | `VK_FORMAT_R8G8B8A8_UNORM` | 4 |
| `0x4E` (BC7 SRGB) | `VK_FORMAT_BC7_SRGB_BLOCK` | `VK_FORMAT_R8G8B8A8_SRGB` | 4 |
| `0x4F` (BC6H SFLOAT) | `VK_FORMAT_BC6H_SFLOAT_BLOCK` | `VK_FORMAT_R16G16B16A16_SFLOAT` | 8 |
| `0x50` (BC6H UFLOAT) | `VK_FORMAT_BC6H_UFLOAT_BLOCK` | `VK_FORMAT_R16G16B16A16_SFLOAT` | 8 |

### Design
1. **Capability flag (`vk.h`, `window.c`):**
   - Set `vk.bc_textures = h->textureCompressionBC && !(getenv("YW4_NO_BC") && atoi(getenv("YW4_NO_BC")));`.
   - Pass `.textureCompressionBC = vk.bc_textures` in `VkPhysicalDeviceFeatures`.
2. **Format lookup (`gpu.c`):**
   - In `gpu_format(uint32_t f)`, return the fallback uncompressed `VkFormat` above when `!vk.bc_textures`.
   - Check texture view format compatibility (`gpu.c:2136, 2167`) so views onto BCn textures also resolve through
     `gpu_format()`.
3. **Self-contained CPU decoder (`runtime/src/bcn.c`, `runtime/src/bcn.h`):**
   - Zero external dependencies (MIT-compatible, written in C11).
   - Decodes a linear buffer of 4×4 BCn blocks (`wb * hb` blocks) into tightly packed uncompressed rows (`w * h * bpp`),
     clamping partial edge blocks when `w` or `h` is not a multiple of 4 (small mip levels).
   - Start by logging which BCn formats Yo-kai Watch 4++ actually uploads across town/battle/cutscenes to prioritize
     verification, while implementing all of BC1–BC7 so no format traps.
4. **Upload path (`gpu.c`):**
   - In `detile()`, return `0` for BCn textures when `!vk.bc_textures` so they go through `upload_texels()`.
   - In `upload_texels()`, when `!vk.bc_textures` and `f->kind == FMT_COMPRESSED`:
     - Detile each level/layer with `tegra_read_region()` into a scratch buffer of linear blocks,
     - Decode into the staging buffer using `bcn_decode(...)`,
     - Record `vkCmdCopyBufferToImage` with the uncompressed `bufferOffset` and `imageExtent = {lv.w, lv.h, lv.d}`.
   - *Future optimization (if needed on Android):* If CPU BC7 decoding causes noticeable area-load hitching on mobile
     CPUs, move BCn decoding into a Vulkan compute shader chained after `detile.comp`. Keep the CPU decoder as the
     reference implementation.

### Memory Impact Note
Uncompressing BC1/BC7 to RGBA8 expands compressed textures by 4×–8× in VRAM/RAM. On desktop this is negligible; on
6–8 GB Android phones, measure peak memory after Phase 2 and consider ASTC transcoding or mip-bias/texture-budget
limits only if real devices run low on memory.

### Files Touched
- `runtime/src/bcn.c`, `runtime/src/bcn.h` (new)
- `runtime/src/vk.h`
- `runtime/src/window.c`
- `runtime/src/gpu.c`
- `docs/debugging.md` (document `YW4_NO_BC=1`)
- `docs/decisions/NNNN-*.md` (a decision record, the next free number)

### Verification
- Build with `python -m recomp.build`.
- Run via `python -m tools.dev.run` with `YW4_SETTINGS_FILE=work/test_nobc.ini` and `YW4_NO_BC=1`.
- Diff screenshots against native BCn across town, battle, UI, and cutscenes (confirm sRGB gamma, alpha cutouts,
  normal maps in BC5, and any BC6H/BC7 assets match).
- Log upload time (`STALL_UPLOAD`) and VRAM usage (`gpu_vram`) with and without `YW4_NO_BC=1`.

---

## Phase 3: AArch64 Runtime & NDK Cross-Compilation

1. **Port x86-only C code:**
   - `runtime/src/bones.c`: add `#if defined(__aarch64__)` NEON path (or portable scalar fallback) alongside the
     existing SSE path.
   - `runtime/src/sysinfo.c`: guard x86-only CPUID/info reads for `__aarch64__`.
   - Gate desktop-only files (`gpu_dlss.c`, `gpu_xess.c`, `gpu_fsr.c`, `gpu_fg.c`, `discord*.c`) cleanly under
     `__ANDROID__` / build target flags.
2. **Bionic libc audit:**
   - Verify POSIX calls currently enabled by `-D_GNU_SOURCE` (`recomp/build.py:41`) compile cleanly against Android
     NDK API 31+ (Bionic).
3. **NDK build target (`recomp/build.py`):**
   - Add a `--target android-arm64` mode that compiles `work/recomp/*.c`, `runtime/src/*.c`, and `third_party/musl`
     into a shared library `libmain.so` (loaded by SDL3's `SDLActivity`) using NDK Clang and links against
     Android SDL3 + `libvulkan` + `liblog` + `libandroid`.

---

## Phase 4: Android Bring-up & Packaging

1. **Fixed 32 GiB guest VA range (`runtime/include/yw4load.h:65-79`):**
   - Currently reserves `0x7000000000`–`0x7800000000` (inside 39-bit VA).
   - Many Android AArch64 kernels use 39-bit user VA (`0x0`–`0x7FFFFFFFFF`), where ART/zygote/shared libraries can
     collide with `0x7000000000+` under ASLR.
   - Test on real Android hardware; if collisions occur, lower `YW4_GUEST_LO` or allow dynamic relocation of the
     guest window.
2. **Mobile Vulkan feature & TBDR audit:**
   - Target Android 13+/14+ devices with Vulkan 1.3 (`dynamicRendering`, `timelineSemaphore`, `shaderDrawParameters`).
   - Audit optional desktop features in `window.c:566-574` (`geometryShader`, `depthClamp`, `dualSrcBlend`, `logicOp`,
     `fillModeNonSolid`, `occlusionQueryPrecise`) on real Adreno and Mali drivers; add fallbacks where translated
     shaders actually require them.
   - Profile TBDR bandwidth: `gpu.c` currently keeps images in `VK_IMAGE_LAYOUT_GENERAL` with full barriers between
     passes. Tighten render-pass layouts and barriers if mobile frame times are bandwidth-bound.
3. **Android app lifecycle, storage, and input:**
   - Handle `SDL_EVENT_WILL_ENTER_BACKGROUND` / `SDL_EVENT_DID_ENTER_FOREGROUND` (teardown/recreate Vulkan swapchain
     when `ANativeWindow` is lost).
   - Storage: point RomFS, saves, and mods to Android app-external storage / SAF-picked directory.
   - Input: Bluetooth/USB controllers work via SDL3; add optional on-screen touch overlay later if needed.
4. **Legal packaging (Rule 1):**
   - Never distribute an APK containing `libmain.so` (derived from the game binary).
   - Extend the PC launcher (or a CLI script) to take the owner's dump, cross-compile `libmain.so` with the Android
     NDK, package and sign an APK locally, and let the owner sideload it.

---

## Open Questions / Unverified Items

- **Exact BCn format mix in YW4++:** Needs a runtime log (`gpu_format()` counter) to confirm whether BC6H/BC7 are
  used in practice and how many MiB uncompressed textures take in a late-game town scene.
- **CPU `memcpy` cost of `YW4_HOST_IMPORT=0`:** Needs measurement on Windows to see whether copying on every
  `gpu_buffer_at()` is negligible or if dirty-range/frame caching is needed for UBOs/dynamic VBs.
- **Android 39-bit VA collisions at `0x7000000000`:** Unverified until tested on real Android devices (Adreno/Mali).
- **Translated shader reliance on optional desktop Vulkan features** (`geometryShader`, `dualSrcBlend`, `logicOp`,
  `depthClamp`): Unverified on mobile drivers until tested.
