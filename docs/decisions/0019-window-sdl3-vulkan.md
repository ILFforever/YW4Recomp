# 0019 — The window: SDL3 and Vulkan; CPU clears as the first pixels

Date: 2026-10-21
Status: accepted (follows the owner's choices in 0017: SDL3, vendored Vulkan headers)

## Dependencies

- **SDL3 3.4.18** (zlib licence), the official MinGW development release, fetched by `python -m tools.yw4.deps` into `<work_dir>/deps/` (pinned by version and SHA-256; `recomp/build.py` calls it). No binary is committed. Linux uses the system's SDL3 through `pkg-config`. `SDL3.dll` is copied next to `yw4.exe`.
- **Vulkan headers** `vulkan-sdk-1.4.357.0` (Apache-2.0 or MIT), committed in `third_party/vulkan/`. Vulkan itself is loaded at run time through `SDL_Vulkan_GetVkGetInstanceProcAddr`; no Vulkan library is linked. `YW4_VULKAN_VALIDATION=1` enables the Khronos validation layer once the owner has installed the LunarG SDK.

## Threads

The process's main thread owns the window and pumps its events. The main guest thread moved to its own host thread (64 MiB host stack, reserved only), and `yw4_kernel_init` runs there, because the kernel binds the main guest thread to the calling host thread. Closing the window ends the process. `YW4_HEADLESS=1` (or no usable window) runs everything as before, without a window.

## What is shown, for now

Nothing draws yet: draws need the game's shaders translated (Phase 4a proper). So that the window shows the game's real state from the start:
- **Colour clears run on the CPU** (`nvn_cmdbuf.c`): `SetRenderTargets` and `ClearColor` write the clear colour into the texture's storage in the game's memory. If every texel is equal and the texel size divides 16, the layout doesn't matter and the whole level is filled. Clear colours are encoded per format in `pixels.c` (8-bit unorm and sRGB, BGRA, half and single floats, RGB10A2, R11G11B10F). Other formats, and partial channel masks, are reported once and skipped.
- **Present reads the window texture back** from the game's memory (`tegra_read_region`, 16-byte runs where possible), converts it to RGBA8 and gives it to the window. Vulkan copies it into an image and scales it into the swapchain, letterboxed to keep its shape. FIFO presentation; the game already paces itself to 30 frames per second (0017).
- **Screenshots:** F12, or `YW4_SCREENSHOTS=<frame>,<frame>,...`, as BMP files in `YW4_SCREENSHOT_DIR` (`build.py --run`: `<work_dir>/screenshots/`, never committed: they are pictures of the game).

## What the first frames show

The window is black, and that is correct for now. Logging the clears shows how the game renders. It clears a **1600×900 set of render targets** every frame (RGBA8 to (0,0,1,0), R11G11B10F to grey 0.298, RGBA8 to black, RG16F): a deferred renderer's G-buffer at 900p. It also clears 533×300 targets (a third of 1600×900: bloom or blur buffers). The very first frame used 1920×1080 targets, so the internal resolution changes at run time. The 1920×1080 window texture is never cleared: the final picture is *drawn* into it, and drawing doesn't exist yet. These numbers are the first real input for the renderer design (Phase 4b).

## Testing

`tests/test_nvn.py` adds region round trips: writing a whole level equals the swizzle, and reading whole and partial regions back (aligned and unaligned starts; 1-, 4-, 8- and 16-byte texels) returns the source. With the window open the game still runs at 30.0 frames per second.
