# Known issues

What is known to be missing or wrong, roughly in the order it is likely to matter. Fixed items move to
[progress.md](progress.md).

## Graphics

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
- **Frame rate drops to about 25 frames/s on the bridge in the snowy past-world town** (late game; only on that
  bridge). The GPU is the limit: about 40 ms of GPU work per frame against the 33 ms budget, with about 1,450 draws
  (the town is about 300); the game's own CPU work is about 17 ms. Not the shadow pass (no change when switching it). Which
  pass is heavy is not measured yet: needs per-pass GPU timing.

## Game and system

- **The English translation mod** (LayeredFS) is not supported yet (Phase 5).
- **C++ exceptions are not supported**: nothing in `main` throws ([decision 0012](decisions/0012-musl-startup-tls-exceptions-libc.md)).
- Two character-following lights have zero colour in the kitchen; probably intended, not confirmed.

## Tools

- `tests/test_kernel.py::test_threads` fails now and then with "cannot reserve the heap and thread-local
  regions" (seen once on 2026-10-22, then 4 passes in a row): the test program reserves fixed addresses, which
  something else in the process can occasionally occupy first. Rerun before suspecting a change. The game itself
  failed the same way once at start (2026-10-29, the run straight after worked): it reserves the guest's fixed
  addresses too.

- `YW4_REPLAY` routes can drift: about one run in three ends somewhere else (for example stuck on the save-load
  screen). Check the screenshot before trusting a run. Since 2026-10-25 the town walk replay drifts every time,
  even between two runs of the game's own shadows, so replay screenshots can't be compared.
- The baseline benchmark against Eden ([baseline.md](baseline.md)) still lacks the opening-scene frame-time spread.
