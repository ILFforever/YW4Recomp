# Known issues

What is known to be missing or wrong, roughly in the order it is likely to matter. Fixed items move to
[progress.md](progress.md).

## Graphics

- **PCSS (F7) only reaches what the shadow mask covers.** It is in the game's screen-space shadow mask shader, which
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
  (the town is about 300); the game's own CPU work is about 17 ms. Not the shadow pass (no change with F9). Which
  pass is heavy is not measured yet: needs per-pass GPU timing.

## Game and system

- **The English translation mod** (LayeredFS) is not supported yet (Phase 5).
- **C++ exceptions are not supported**: nothing in `main` throws ([decision 0012](decisions/0012-musl-startup-tls-exceptions-libc.md)).
- Two character-following lights have zero colour in the kitchen; probably intended, not confirmed.

## Tools

- `tests/test_kernel.py::test_threads` fails now and then with "cannot reserve the heap and thread-local
  regions" (seen once on 2026-10-22, then 4 passes in a row): the test program reserves fixed addresses, which
  something else in the process can occasionally occupy first. Rerun before suspecting a change.

- `YW4_REPLAY` routes can drift: about one run in three ends somewhere else (for example stuck on the save-load
  screen). Check the screenshot before trusting a run. Since 2026-10-25 the town walk replay drifts every time,
  even between two runs of the game's own shadows, so replay screenshots can't be compared.
- The baseline benchmark against Eden ([baseline.md](baseline.md)) still lacks the opening-scene frame-time spread.
