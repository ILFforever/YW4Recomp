# Known issues

What is known to be missing or wrong (checked 2026-11-28), roughly in the order it is likely to matter. Fixed items
move to [progress.md](progress.md).

## Graphics

- **The CPU caps busy scenes at about 70 frames/s** (measured 2026-10-29). With FRAME RATE unlimited, upscaling
  stops helping there: the GPU sits idle part of each frame. Busy late-game spot (`save_base_busy`, ~410 draws), DLSS
  Performance: game work 12.9 ms per frame, GPU 4.6 ms. The limit is the game's own code on the main thread and its
  two job threads (`main+0x97ad10` is the job queue). Next: profile both there (`YW4_PROFILE=1`, threads 6 and 7).
- **FSR frame generation: arms and legs doubled while the player runs** (owner, 2026-11-22, 4K, FSR
  anti-aliasing; DLSS frame generation doesn't show it). In the generated frame (`tools.dev.scene walk`, frame 3600,
  `YW4_FG_SHOT_GEN=2`). Gone with half the motion per frame (`YW4_REPLAY_HZ=120`), so mostly FSR's own limit with
  large steps between frames; real play at ~40 game frames/s has larger steps than the replays (1/60 s). Ruled out:
  late motion (`YW4_FG_MOTION_LOG=1`: always this frame's), the HUD-less copy, depth inverted / raw / placeholder
  camera (no change). The trail behind the whole player and Whisper, and the jitter with it, were the camera-only
  motion: fixed 2026-11-28 (OBJECT MOTION, [decision 0041](decisions/0041-frame-generation.md)).
- **A short shadow flash after unpausing from the game's own pause menu** (the Plus button), only in the bamboo
  forest and not after closing our overlay (owner, 2026-11-06 and 2026-11-13). Minor; not looked into.
- **PCSS (soft shadows) only reaches what the shadow mask covers.** Objects the game draws forward (6 other shadow
  shaders seen so far) keep the game's filter. Its strengths are tuned by eye in the town and the bamboo forest only.
- **Occlusion culling needs a GPU extension.** The game hides objects on the GPU from its occlusion queries
  (`runtime/src/gpu_query.c`). Without `VK_EXT_conditional_rendering` every such object is drawn, as before
  2026-11-14: the picture is the same, the GPU does more work. `SetRenderEnable(false)` is not done either (the
  game has not been seen to call it).
- **The one compute shader that writes an image has never been seen to run.** It is a plain texture copy
  (`2f3ceeb96f58c522`, the only one of the game's 5396 shaders with a storage image) and is supported since
  2026-11-14, but untested: no log so far (1278 of them) shows it. The log says `writes a storage image (first use`
  when it does.

## Linux

- **DLSS frame generation and Reflex are Windows only** (they come through Streamline). AMD FSR (upscaling and
  frame generation) and NVIDIA DLSS upscaling work on Linux ([decision 0073](decisions/0073-dlss-fsr-on-linux.md)).

## Game and system

- **C++ exceptions are not supported**: nothing in `main` throws
  ([decision 0012](decisions/0012-musl-startup-tls-exceptions-libc.md)).

## Input and menus

- **A DualSense over Bluetooth adds about 2.5 s to the start** (first frame at 7.4-7.9 s, against 5.2-5.7 s on its
  USB cable). Two waits inside SDL while it reads the pad over the wireless link. A fix would open the pad off the
  window's thread; until then, use the USB cable.
- **Yo-kai Pad mouse support ([decision 0057](decisions/0057-mouse-menu-lists.md))**: open issues from real-mouse
  testing (owner, 2026-11-08; the rest of the Pad works):
  - **Watch skills**: characters cannot be clicked.
  - **Organize Members → Switch moves**: not working.
- **An empty Diary slot 1 can show a character** in the Diary's list (Continue, and saving in the game) when the save
  is in another slot (owner, 2026-11-20: only slot 5 used). Row 1 is the game's own row; its row code puts the loaded
  save's little character on it at every refresh. Hiding the row's parts (`_dmy_dot01`, its shadow, `_pos_dot_01`) does
  not remove it: the character is a separate picture attached there (`runtime/src/savefs.c` diary_row_icons). Next:
  clear row 1's character pointer (menu + 0x460, which f_75e810 hands to f_70f150) for an empty slot. Cosmetic only:
  the row still reads "Empty Diary" and loads nothing.

## Tools

- The baseline benchmark against Eden ([baseline.md](baseline.md)) still lacks the opening scene's frame-time spread.
