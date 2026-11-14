# Known issues

What is known to be missing or wrong (checked 2026-11-13), roughly in the order it is likely to matter. Fixed items
move to [progress.md](progress.md).

## Graphics

- **The CPU caps busy scenes at about 70 frames/s** (measured 2026-10-29). With FRAME RATE unlimited, upscaling
  stops helping there: the GPU sits idle part of each frame. Busy late-game spot (`save_base_busy`, ~410 draws), DLSS
  Performance: game work 12.9 ms per frame, GPU 4.6 ms. The limit is the game's own code on the main thread and its
  two job threads (`main+0x97ad10` is the job queue). Next: profile both there (`YW4_PROFILE=1`, threads 6 and 7).
- **FSR frame generation with DLSS at 32:9 4K is slow for a few seconds after changing the aspect ratio** (~80
  frames of 200-660 ms "present"). The other upscaling and frame-generation choices recover at once (2026-11-10).
- **A short shadow flash after unpausing from the game's own pause menu** (the Plus button), only in the bamboo
  forest and not after closing our overlay (owner, 2026-11-06 and 2026-11-13). Minor; not looked into.
- **The first battle after loading a save stutters for its first few seconds**: about 25 frames of 70-80 ms
  ("window present" in the log) over ~4 s; the second battle in the same run had none (owner, 2026-11-13, late-game
  save, 4K 21:9, DLSS, DLSS frame generation). Cause not found. During the battles themselves frames were steady:
  ~38 game frames/s, GPU ~19-20 ms, no heavier than the bamboo forest around them.
- **PCSS (soft shadows) only reaches what the shadow mask covers.** Objects the game draws forward (6 other shadow
  shaders seen so far) keep the game's filter. Its strengths are tuned by eye in the town and the bamboo forest only.
- **Occlusion culling needs a GPU extension.** The game hides objects on the GPU from its occlusion queries
  (`runtime/src/gpu_query.c`). Without `VK_EXT_conditional_rendering` every such object is drawn, as before
  2026-11-14: the picture is the same, the GPU does more work. `SetRenderEnable(false)` is not done either (the
  game has not been seen to call it).
- **Compute shaders that use storage images are skipped** (log line "uses storage images").

## Linux

- **No launcher download yet**: the launcher builds as an AppImage ([decision 0071](decisions/0071-linux-launcher.md)),
  but it was checked only in WSL with software rendering, never on a real Linux desktop, and none is published.
  Until then the game is built from source ([Building on Linux](../README.md#building-on-linux)).
- **Frame generation and Reflex are Windows only.** DLSS and FSR upscaling are built on Linux too
  ([decision 0073](decisions/0073-dlss-fsr-on-linux.md)), but have not run on a real Linux GPU yet: FSR was checked on
  software Vulkan only, DLSS only builds.
- **A crash on exit** (segfault after "exit game") was seen once on 2026-11-12; later logs exit cleanly. Maybe gone,
  not confirmed by the tester.

## Game and system

- **C++ exceptions are not supported**: nothing in `main` throws
  ([decision 0012](decisions/0012-musl-startup-tls-exceptions-libc.md)).
- **The window stays blank for about 3.7 s at start**, while Vulkan, DLSS and the game's boot set up. Planned: a
  black screen with the game logo (`gamelogo.c`) until the first frame.

## Input and menus

- **A DualSense over Bluetooth adds about 2.5 s to the start** (first frame at 7.4-7.9 s, against 5.2-5.7 s on its
  USB cable). Two waits inside SDL while it reads the pad over the wireless link. A fix would open the pad off the
  window's thread; until then, use the USB cable.
- **Yo-kai Pad mouse support ([decision 0057](decisions/0057-mouse-menu-lists.md))**: open issues from real-mouse
  testing (owner, 2026-11-08; the rest of the Pad works):
  - **Watch skills**: characters cannot be clicked.
  - **Soulmate**: page is buggy.
  - **Slimey marks (Yo-kai Trophies)**: page not working.
  - **Treasure photos**: only up and down work.
  - **Organize Members → Switch moves**: not working.

## Tools

- The baseline benchmark against Eden ([baseline.md](baseline.md)) still lacks the opening scene's frame-time spread.
