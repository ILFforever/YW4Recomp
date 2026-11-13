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
- **Switching upscaling modes many times in a row once crashed an 8 GB card** ("device lost", video memory full).
  Made less likely on 2026-11-10 and not seen since, but not proven gone. If it comes back, the log is wanted.
- **A short shadow flash after unpausing from the game's own pause menu** (the Plus button), only in the bamboo
  forest and not after closing our overlay (owner, 2026-11-06 and 2026-11-13). Minor; not looked into.
- **Battles with special moves can be heavy at 4K ultrawide.** The bloom composite pass took 7–11 ms instead of ~2
  ms in some battles (2026-10-28, maybe a special move's effects); not traced. The freezes at a battle's first
  special move are fixed (2026-11-12), and a spike check at 4K with DLSS and frame generation found three single
  slow frames in 11 minutes ([baseline.md](baseline.md)).
- **PCSS (soft shadows) only reaches what the shadow mask covers.** Objects the game draws forward (6 other shadow
  shaders seen so far) keep the game's filter. Its strengths are tuned by eye in the town and the bamboo forest only.
- **Shadow strength not yet checked against Eden** after the ambient-light fix; indoor shadows may still be a little
  stronger.
- **Occlusion queries always report 0 samples.** The game issues them in pairs from the house onward; answering
  "visible" changed nothing on screen so far, but the answer is wrong.
- **Compute shaders that use storage images are skipped** (log line "uses storage images").
- **The bridge in the snowy past-world town is heavy** (about 1,450 draws against the town's 300). It holds 30
  frames/s; above 30, looking at the bridge costs about 10 frames/s. That is the game's content, not a port fault.

## Linux

- **No launcher yet**: the game is built from source ([Building on Linux](../README.md#building-on-linux)).
- **FSR, DLSS, frame generation and Reflex are Windows only.**
- **A crash on exit** (segfault after "exit game") was seen once on 2026-11-12; later logs exit cleanly. Maybe gone,
  not confirmed by the tester.

## Game and system

- **C++ exceptions are not supported**: nothing in `main` throws
  ([decision 0012](decisions/0012-musl-startup-tls-exceptions-libc.md)).
- **The window stays blank for about 3.7 s at start**, while Vulkan, DLSS and the game's boot set up. Planned: a
  black screen with the game logo (`gamelogo.c`) until the first frame.
- **Odd letter spacing ("N A T E") seen once in English text** (owner, 2026-11-12). Not located yet: our name box or
  the game's own text.

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

- `tests/test_kernel.py::test_threads` fails now and then with "cannot reserve the heap and thread-local regions":
  the test program reserves fixed addresses that something else in the process can take first. Rerun before
  suspecting a change. (The game itself reserves its memory first and retries once with a new layout,
  `YW4_ASLR_RETRY`.)
- The baseline benchmark against Eden ([baseline.md](baseline.md)) still lacks the opening scene's frame-time spread.
