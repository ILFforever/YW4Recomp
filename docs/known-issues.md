# Known issues

What is known to be missing or wrong, roughly in the order it is likely to matter. Fixed items move to
[progress.md](progress.md).

## Graphics

- **CPU limit at ~70 frames/s in busy scenes; optimize the CPU further** (owner, 2026-10-29). With FRAME RATE
  unlimited, upscaling (DLSS/FSR) no longer helps there: the GPU is idle part of each frame. Busy late-game spot
  (`save_base_busy`, ~410 draws), DLSS Performance: game work 12.9 ms per frame, GPU 4.6 ms. The short-sleep fix
  (`YW4_SHORT_SLEEP`, 61 → 77 frames/s) took most of the main thread's waiting away; left: ~1.8 ms per frame of
  waits (`YW4_WAITPROF=1`: an SDK mutex taken ~140 times per frame from `main+0x97ae14`, WaitForAddress) and the
  game code itself. The mutex is the game's job queue (`main+0x97ad10`: lock, take the next job, unlock, run it;
  the main thread helps the two worker threads while it waits for them). Spinning 5 us before sleeping in
  ArbitrateLock cut its wait 0.35 → 0.07 ms per frame but gave no frames (70 → 70 frames/s, 2 runs each): the main
  thread then waits for the same jobs elsewhere, so it was removed. The limit is the jobs' and the main thread's
  game code: next, profile both in `save_base_busy` (`YW4_PROFILE=1`, and the workers, threads 6 and 7).
- **Lag after switching ASPECT RATIO from ultra-ultrawide back to 21:9** (owner, 2026-10-29, late-game save): ~7
  frames/s with the GPU busy ~400 ms per frame for a while, then it recovered. Likely cause: each aspect ratio's set
  of screen-sized targets (7680×2160 at 32:9) stayed in video memory until 300 frames unused, so cycling through all
  of them overflowed it. Fix in testing: the old size's targets are freed 8 frames after a size setting changes.
- **Crash (GPU device lost) after switching the upscaling mode several times in a row** (owner, 2026-10-29, DLSS,
  4K 21:9, right after the aspect-ratio cycling above): `vkWaitSemaphores failed (VkResult -4)`. Memory peaked at
  5.0 GB against ~3.3 GB normally, so video memory running out is a suspect. Not reproduced yet.
- **Stutters at 4K ultrawide; to optimize** (owner, 2026-10-28; 4K height at 21:9 = 5120×2160, TAA, full-size effects). From the
  owner's session logs: bamboo forest (late-game save) GPU 23.2 ms per frame on average, 36.4 ms at the 99th
  percentile (budget 33.3), 419 of 27,382 frames over 50 ms; school save GPU 18.2 / 27.6 ms, 134 of 5,620 over
  50 ms. Two sources: the GPU near its limit in heavy scenes, and rare long spikes in the game's own CPU work (up to
  ~400 ms). Not yet split by pass (F5 per-pass GPU timing) or by cause of the CPU spikes.
  **Battles, 16:9 against 21:9** (owner, 2026-10-28, late-game save, 4K, TAA, full-size effects, one session each,
  not the same moments): slow frames (over 40 ms) 3.1% → 6.9%; most are the game's CPU work, not the GPU (GPU busy
  over 30 ms in 5 → 62 of them). The game's own code on the main thread 5.9 → 9.0 ms per frame on average; its two
  hottest functions, the skeleton animation math `f_a79c00` and `f_a77390`, about doubled (a wider view animates
  more characters). GPU in battle 14.2 → 20.7 ms per frame; most of the extra in two 300-frame windows where the
  bloom composite's pass (with ~12 more draws, maybe a special move's effects) took 7–11 ms against ~2 ms
  otherwise; not traced yet. A 1.8 s freeze seen once at a battle's start did not come back (no shader builds
  logged then).
- **PCSS (soft shadows) only reaches what the shadow mask covers.** It is in the game's screen-space shadow mask shader, which
  shadows everything drawn into the G-buffer. Objects the game draws forward (6 other shadow shaders seen so far) keep
  the game's filter. Its strengths are tuned by eye in the town and the bamboo forest only.
- **To do: contact shadows.** Small gaps where things touch the ground (shoes, bench legs, props against walls)
  are thinner than a shadow-map texel, so they get no shadow. A screen-space version in the game's shadow mask shader
  was tried and removed (2026-10-25): that pass only has half-resolution depth, so it found no real contact gaps and
  drew a ring of dots around character outlines, even under Whisper floating in the air. Redo it in our own 3D
  renderer with full-resolution depth ([decision 0025](decisions/0025-pcss-soft-shadows.md)).
- **Shadow strength not yet checked against Eden** after the ambient-light fix; indoor shadows may still be a little
  stronger.
- **Occlusion queries always report 0 samples.** The game issues them in pairs from the house onward; answering
  "visible" changed nothing on screen so far, but the answer is wrong.
- **Compute shaders that use storage images are skipped** (log line "uses storage images").
- **Alpha test is ignored** (log line "alpha test ... ignored").
- **Indirect and transform-feedback draws are skipped.**
- **Streaks in movies.**
- **The "4++" logo variant** does not show (add-on ownership).
- GPU time is about 1-2× Eden's: full barriers between passes, one general image layout, textures re-uploaded, no
  pipeline cache. The Vulkan validation layer reports barriers recorded inside dynamic rendering.
- **The bridge in the snowy past-world town is a heavy scene** (late game). It used to drop to about 25 frames/s
  (2026-10-25: about 1,450 draws against the town's 300). Since the later GPU work it holds the game's 30
  (owner, 2026-10-29), but above 30 (the FRAME RATE setting) looking at the bridge costs about 10 frames/s: the
  game's own content is that heavy there, not a fault of the port.

## Game and system

- **The English translation mod** (LayeredFS) is not supported yet (Phase 5).
- **C++ exceptions are not supported**: nothing in `main` throws ([decision 0012](decisions/0012-musl-startup-tls-exceptions-libc.md)).
- Two character-following lights have zero colour in the kitchen; probably intended, not confirmed.

## Tools

- **To do: a build option with or without FSR / DLSS** (owner, 2026-10-29). The Windows build always downloads and
  links AMD FSR, NVIDIA DLSS and Streamline (`tools/yw4/deps.py`). Later, a flag to leave out one or both (no
  download, the settings hidden), for builds shipped without NVIDIA's files or built without internet.

- `tests/test_kernel.py::test_threads` fails now and then with "cannot reserve the heap and thread-local
  regions" (seen once on 2026-10-22, then 4 passes in a row): the test program reserves fixed addresses, which
  something else in the process can occasionally occupy first. Rerun before suspecting a change. The game itself
  failed the same way once at start (2026-10-29, the run straight after worked): it reserves the guest's fixed
  addresses too.

- `YW4_REPLAY` routes can drift: about one run in three ends somewhere else (for example stuck on the save-load
  screen). Check the screenshot before trusting a run. Since 2026-10-25 the town walk replay drifts every time,
  even between two runs of the game's own shadows, so replay screenshots can't be compared.
- The baseline benchmark against Eden ([baseline.md](baseline.md)) still lacks the opening-scene frame-time spread.
