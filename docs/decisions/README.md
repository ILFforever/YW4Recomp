# Decisions

Why the project is built the way it is: one record per choice, numbered in the order they were made. This
index is generated from the records' headers by `python -m tools.dev.decisions` (do not edit it by hand);
`--check` also checks the docs' links and the code's `decision NNNN` mentions.

States: **accepted** (in use), **plan** (agreed, not built yet), **proposed**, **rejected**. *Replaced by*
lists later records that replace a record, or part of it.

| # | Decision | State | Date | Replaced by | Code |
|---|---|---|---|---|---|
| 0001 | [Repo layout and keeping game data out](0001-repo-layout.md) | accepted | 2026-10-14 | [0006 (part)](0006-recompiler-design.md) | `.gitignore`, `config.example.toml`, `tools/yw4/config.py` |
| 0002 | [NSO input pipeline and version lock](0002-nso-pipeline-and-version-lock.md) | accepted | 2026-10-20 |  | `tools/yw4/nso.py`, `tools/yw4/lz4_block.py`, `target.toml` |
| 0003 | [Extracting ExeFS/RomFS from NSPs with hactool](0003-nsp-extraction-with-hactool.md) | accepted | 2026-10-20 |  | `tools/yw4/extract.py`, `tools/yw4/pfs0.py` |
| 0004 | [Raspberry Pi 5 as the ARM64 test reference; x86-64 is the first target](0004-pi5-as-arm64-reference.md) | accepted | 2026-10-20 |  |  |
| 0005 | [Phase 1 runtime strategy](0005-phase1-runtime-strategy.md) | accepted | 2026-10-20 | [0014 (part)](0014-recompile-the-sdk-too.md) |  |
| 0006 | [Recompiler design (first version)](0006-recompiler-design.md) | accepted | 2026-10-20 |  | `recomp/golden.py`, `recomp/check.py`, `recomp/pi` |
| 0007 | [Whole-function translation](0007-whole-function-translation.md) | accepted | 2026-10-20 |  | `recomp/function.py`, `runtime/src/dispatch.c`, `tests/functest` |
| 0008 | [Floating-point and SIMD semantics](0008-floating-point-semantics.md) | accepted | 2026-10-21 |  | `runtime/include/yw4fp.h` |
| 0009 | [Atomics: exclusives, acquire/release](0009-atomics.md) | accepted | 2026-10-21 |  | `tests/functest/atomics.S`, `tests/test_kernel.py` |
| 0010 | [Loader and import stubs (Phase 3 skeleton)](0010-loader-and-import-stubs.md) | accepted | 2026-10-21 | [0014 (part)](0014-recompile-the-sdk-too.md) | `recomp/translate.py`, `recomp/build.py`, `runtime/src/sha256.c`, `runtime/src/hle.c` |
| 0011 | [First HLE: memory start-up, diag logging, StandardAllocator](0011-first-hle-memory-and-diag.md) | accepted | 2026-10-21 |  | `runtime/src/hle.h`, `runtime/src/hle.c`, `runtime/src/hle_os.c`, `runtime/src/hle_diag.c`, `runtime/src/hle_mem.c`, `runtime/src/heap.c` |
| 0012 | [musl start-up, TLS, C++ exceptions, first C library functions](0012-musl-startup-tls-exceptions-libc.md) | accepted | 2026-10-21 |  | `runtime/src/hle_string.c` |
| 0013 | [libm from musl 1.1.19, checked against the sdk's own code](0013-libm-from-musl-checked-against-the-sdk.md) | accepted | 2026-10-21 |  | `runtime/src/hle_math.c`, `third_party/musl`, `tools/yw4/sdkref.py`, `tools/yw4/pi/sdkrun.c` |
| 0014 | [Recompile the sdk too; native code at the kernel and service level](0014-recompile-the-sdk-too.md) | accepted | 2026-10-21 |  | `runtime/src/kernel.c`, `runtime/src/ipc.c`, `runtime/src/romfs.c`, `runtime/src/crypto.c`, `recomp/function.py`, `tools/yw4/sdkref.py` |
| 0015 | [Guest threads are host threads; one kernel lock](0015-guest-threads.md) | accepted | 2026-10-21 | [0018 (part)](0018-setjmp-longjmp.md) | `runtime/src/host.c` |
| 0016 | [Native overrides by symbol; NVN starts at nvnBootstrapLoader](0016-native-overrides-and-the-nvn-entry.md) | accepted | 2026-10-21 |  | `recomp/overrides.toml`, `runtime/src/nvn.c` |
| 0017 | [NVN on the host: headless first; SDL3 and Vulkan next](0017-nvn-headless-first.md) | accepted | 2026-10-21 |  | `runtime/src/nvn.c`, `runtime/src/nvn_objects.c`, `runtime/src/tegra.c`, `runtime/src/vi.c`, `runtime/src/svc_am.c`, `runtime/src/svc_acc.c` |
| 0018 | [setjmp and longjmp in recompiled code](0018-setjmp-longjmp.md) | accepted | 2026-10-21 |  | `runtime/src/setjmp.c`, `recomp/translate.py`, `recomp/function.py` |
| 0019 | [The window: SDL3 and Vulkan; CPU clears as the first pixels](0019-window-sdl3-vulkan.md) | accepted | 2026-10-21 | [0021 (part)](0021-draw-path.md) | `runtime/src/window.c`, `recomp/build.py`, `tools/yw4/deps.py`, `runtime/src/nvn_cmdbuf.c`, `third_party/vulkan` |
| 0020 | [Shaders: translate the shipped assembly, not the GPU code](0020-shaders-from-the-assembly.md) | accepted | 2026-10-21 |  | `tools/yw4/shaders.py`, `recomp/shader.py` |
| 0021 | [The draw path: NVN command lists on Vulkan](0021-draw-path.md) | accepted | 2026-10-21 |  | `runtime/src/gpu.c`, `runtime/src/vk.h`, `runtime/src/gpu_draw.c` |
| 0022 | [From the title menu into a new game](0022-new-game-start.md) | accepted | 2026-10-22 |  | `runtime/src/swkbd.c`, `recomp/function.py`, `runtime/src/svc_time.c`, `runtime/src/svc_am.c`, `runtime/src/gpu_draw.c` |
| 0023 | [Plan for the custom renderer (Phase 4b/4c)](0023-custom-renderer-plan.md) | accepted | 2026-10-22 |  |  |
| 0024 | [The first replaced pass: shadow casters](0024-shadow-pass.md) | accepted | 2026-10-22 |  | `runtime/src/shadow.c`, `runtime/src/gpu_shadow.c`, `runtime/shaders/shadow_static.vert`, `recomp/overrides.toml`, `recomp/build.py` |
| 0025 | [Soft contact shadows (PCSS) in the shadow mask shader](0025-pcss-soft-shadows.md) | accepted | 2026-10-25 |  | `recomp/shader.py` |
| 0026 | [The second replaced pass: the G-buffer, hybrid first](0026-gbuffer-pass.md) | accepted | 2026-10-25 |  | `runtime/src/gbuffer.c`, `runtime/src/gpu_ab.c`, `runtime/shaders/gbuffer_main.vert`, `runtime/shaders/gbuffer_main.frag`, `runtime/shaders/gbuffer_frag.glsl`, `recomp/shader.py` |
| 0027 | [The project's license: MIT](0027-mit-license.md) | accepted | 2026-10-26 |  | `LICENSE` |
| 0028 | [The game's geometry in video memory](0028-geometry-in-video-memory.md) | accepted | 2026-10-26 |  | `runtime/src/gpu.c`, `runtime/src/gpu_passes.c` |
| 0029 | [Translating command lists on a worker thread](0029-gpu-worker-thread.md) | accepted | 2026-10-26 |  | `runtime/src/gpu_queue.c` |
| 0030 | [The takeover: our renderer does the per-draw work, then the selection](0030-takeover.md) | accepted | 2026-10-26 |  | `runtime/src/gbuffer.c`, `runtime/src/gpu_draw.c`, `runtime/src/nvn_objects.c` |
| 0031 | [Resolution scaling of the 3D view](0031-resolution-scaling.md) | accepted | 2026-10-27 |  |  |
| 0032 | [Temporal anti-aliasing from the game's own motion vectors](0032-taa.md) | accepted | 2026-10-27 |  | `runtime/src/gpu_aa.c`, `runtime/shaders/taa_resolve.frag`, `runtime/shaders/taa_output.frag` |
| 0033 | [Widescreen: a wider camera, wider screen images, the UI kept 16:9 in the middle](0033-widescreen.md) | accepted | 2026-10-27 |  | `runtime/src/gpu.c`, `runtime/src/gpu_draw.c` |
| 0034 | [Host CPU target: a setting, the 2003 baseline by default](0034-host-cpu-target.md) | accepted | 2026-10-28 |  | `recomp/check.py` |
| 0035 | [Guest registers in local variables: tried, off](0035-register-caching.md) | rejected | 2026-10-28 |  | `recomp/function.py` |
| 0036 | [Native skeleton matrices](0036-native-bone-matrices.md) | accepted | 2026-10-28 |  | `runtime/src/bones.c`, `recomp/overrides.toml` |
| 0037 | [Our own bloom pass (first post-processing pass)](0037-bloom-pass.md) | accepted | 2026-10-28 |  | `runtime/src/bloom.c`, `runtime/src/gpu_post.c`, `runtime/shaders/bloom_bright.frag`, `runtime/shaders/bloom_blur.frag`, `runtime/shaders/bloom_composite.frag` |
| 0038 | [Our own distance blur pass](0038-distance-blur-pass.md) | accepted | 2026-10-28 |  | `runtime/src/dof.c`, `runtime/src/gpu_post.c`, `runtime/shaders/dof_copy.frag`, `runtime/shaders/dof_blur.frag`, `runtime/shaders/dof_composite.frag` |
| 0039 | [Upscaling with AMD FSR](0039-fsr-upscaling.md) | accepted | 2026-10-29 |  | `runtime/src/gpu_fsr.c`, `runtime/src/gpu.c`, `runtime/src/gpu_aa.c`, `runtime/shaders/aa_depth_upscale.frag`, `tools/yw4/deps.py`, `recomp/build.py` |
| 0040 | [Upscaling with NVIDIA DLSS](0040-dlss-upscaling.md) | accepted | 2026-10-29 |  | `runtime/src/gpu_dlss.c`, `runtime/src/msvc_shim.c`, `runtime/src/window.c`, `tools/yw4/deps.py` |
| 0041 | [Frame generation](0041-frame-generation.md) | accepted | 2026-10-29 |  | `runtime/src/gpu_fg.c`, `runtime/src/gpu_dlssg.cpp`, `runtime/src/gpu_aa.c`, `runtime/src/gpu.c`, `runtime/src/window.c` |
| 0042 | [Button icons that follow the device, and the mouse](0042-button-icons-and-mouse.md) | accepted | 2026-10-30 |  | `runtime/src/bindings.c`, `runtime/src/window.c` |
| 0043 | [Mods: a mod's romfs laid over the game's (LayeredFS)](0043-mods-layeredfs.md) | accepted | 2026-10-30 |  | `runtime/src/romfs.c` |
| 0044 | [Our own shadow mask program](0044-shadow-mask-pass.md) | accepted | 2026-10-30 |  | `runtime/shaders/shadow_mask.frag`, `runtime/src/shadow_mask.c`, `runtime/src/gpu_draw.c`, `runtime/src/gpu_ab.c` |
| 0045 | [Contact shadows](0045-contact-shadows.md) | accepted | 2026-11-01 |  | `runtime/shaders/shadow_mask.frag` |
| 0046 | [Our programs in the engine's screen passes](0046-screen-passes.md) | accepted | 2026-11-02 |  | `runtime/shaders`, `runtime/src/gpu_draw.c` |
| 0047 | [Versioning: one number for the project, one for the launcher](0047-versioning.md) | accepted | 2026-11-03 |  | `VERSION`, `recomp/build.py`, `runtime/src/version.h` |
| 0048 | [The exe finds its own files from anywhere; the game's icon from the user's files](0048-home-folder-and-icon.md) | accepted | 2026-11-03 |  | `tools/yw4/icon.py`, `recomp/build.py` |
| 0049 | [DLC: the game finds and checks the owner's add-ons on boot](0049-dlc-on-boot.md) | accepted | 2026-11-03 |  | `target.toml`, `recomp/build.py`, `runtime/src/svc_misc.c` |
| 0050 | [The launcher (Tauri) and the install folder](0050-launcher-and-install-layout.md) | accepted | 2026-11-03 |  | `launcher` |
| 0051 | [Function starts without Ghidra: 597 addresses in the repo](0051-function-starts-without-ghidra.md) | accepted | 2026-11-03 |  | `recomp/main_extra_functions.txt`, `recomp/translate.py` |
| 0052 | [Packaging the launcher: what the download holds and what it fetches](0052-launcher-packaging.md) | accepted | 2026-11-04 | [0054 (part)](0054-game-and-launcher-updates.md) | `launcher/src-tauri/src/buildrun.rs` |
| 0053 | [The in-game overlay: the game's own pause, a side panel drawn on the CPU from its design](0053-in-game-overlay.md) | accepted | 2026-11-05 |  | `runtime/src/pause.c`, `runtime/src/ui.c`, `runtime/src/gamemenu.c`, `runtime/src/gamelogo.c`, `third_party/fonts`, `third_party/nanosvg` |
| 0054 | [Game updates and launcher updates, each on its own](0054-game-and-launcher-updates.md) | accepted | 2026-11-05 |  | `launcher/VERSION`, `launcher/package.py`, `launcher/src-tauri/src/buildrun.rs`, `launcher/src-tauri/src/update.rs`, `tools/yw4/build_install.py` |
| 0055 | [Overlay and HUD themes: the launcher's theme files, read when the game starts](0055-overlay-themes.md) | accepted | 2026-11-06 |  | `runtime/src/theme.c`, `runtime/src/theme.h`, `runtime/src/gamemenu.c`, `runtime/src/hud_style.c`, `runtime/src/settings.c`, `launcher/src-tauri/src/ready.rs` |
| 0056 | [DLC content: extracting add-on RomFS files and serving them on `fsp-srv` 202](0056-dlc-content.md) | accepted | 2026-11-07 |  | `target.toml`, `runtime/src/svc_misc.c`, `runtime/src/svc_fs.c`, `runtime/src/romfs.c`, `tools/yw4/extract.py`, `tools/yw4/build_install.py`, `launcher/src-tauri/src/check.rs`, `launcher/src-tauri/src/buildrun.rs` |
| 0057 | [Mouse support in Yo-kai Pad pages and scrolling lists](0057-mouse-menu-lists.md) | accepted | 2026-11-08 |  | `runtime/src/mousemenu.c`, `runtime/src/mousemenu.h`, `runtime/src/window.c`, `docs/debugging.md` |
| 0058 | [Controller rumble and haptic feedback](0058-controller-rumble.md) | accepted | 2026-11-09 |  | `runtime/src/svc_hid.c`, `runtime/src/window.c`, `runtime/src/window.h`, `runtime/src/bindings.c`, `docs/debugging.md` |
| 0059 | [AMD Vulkan depth format fallback (D24S8 to D32S8)](0059-amd-depth-format-fallback.md) | accepted | 2026-11-10 |  | `runtime/src/gpu.c`, `runtime/src/gpu_ab.c`, `docs/debugging.md` |
| 0060 | [The launcher's icon: the owner's own Whisper silhouette](0060-launcher-icon.md) | accepted | 2026-11-10 |  | `launcher/icon-source.png`, `launcher/src-tauri/icons` |
| 0061 | [The game's "time was changed" check is answered "not changed"](0061-time-change-check.md) | accepted | 2026-11-11 |  | `runtime/src/svc_time.c`, `recomp/overrides.toml` |
| 0062 | [First start: graphics picked for the GPU, two levels](0062-first-start-graphics.md) | accepted | 2026-11-11 |  | `runtime/src/settings.c`, `runtime/src/window.c` |
| 0063 | [Pipeline cache and journal on disk, and when they are thrown away](0063-pipeline-cache.md) | accepted | 2026-11-12 |  | `runtime/src/gpu_pcache.c`, `runtime/src/gpu_draw.c` |
| 0064 | [Holding 30 and 60 frames/s: the vsync tick, or the display's refresh](0064-pacing-tick-or-refresh.md) | accepted | 2026-11-12 |  | `runtime/src/framerate.c`, `runtime/src/vi.c`, `runtime/src/perf.c` |
| 0065 | [Linux: textures and geometry follow the game's events, not page tracking](0065-linux-upload-events.md) | accepted | 2026-11-13 |  | `runtime/src/gpu.c`, `runtime/src/nvn_objects.c`, `runtime/src/gpu.h`, `docs/debugging.md` |
| 0066 | [Typed names keep their plain letters](0066-typed-names-half-width.md) | accepted | 2026-11-14 |  | `runtime/src/swkbd.c`, `recomp/overrides.toml` |
| 0067 | [Several mods' cpk_list files are merged](0067-mods-merge-cpk-list.md) | accepted | 2026-11-14 |  | `runtime/src/romfs.c`, `launcher/src-tauri/src/ready.rs` |
| 0068 | [Mods' copies of one data file are merged entry by entry](0068-mods-merge-data-files.md) | accepted | 2026-11-14 |  | `runtime/src/modmerge.c`, `runtime/src/romfs.c`, `runtime/src/main.c`, `launcher/src-tauri/src/ready.rs`, `launcher/ui/app.js` |
| 0069 | [Above 30 frames/s a motion's time counts at least 1/30 s for its least time](0069-motion-least-time.md) | accepted | 2026-11-14 |  | `runtime/src/framerate.c`, `recomp/overrides.toml`, `runtime/src/kernel.c`, `runtime/src/dispatch.c` |
| 0070 | [The pipeline cache and journal are kept when only the shaders change](0070-pipeline-cache-kept-on-shader-change.md) | accepted | 2026-11-14 |  | `runtime/src/gpu_pcache.c` |
