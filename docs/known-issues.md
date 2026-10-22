# Known issues

What is known to be missing or wrong, roughly in the order it is likely to matter. Fixed items move to
[progress.md](progress.md).

## Graphics

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

## Game and system

- **The English translation mod** (LayeredFS) is not supported yet (Phase 5).
- **C++ exceptions are not supported**: nothing in `main` throws ([decision 0012](decisions/0012-musl-startup-tls-exceptions-libc.md)).
- Two character-following lights have zero colour in the kitchen; probably intended, not confirmed.

## Tools

- `YW4_REPLAY` routes can drift: about one run in three ends somewhere else (for example stuck on the save-load
  screen). Check the screenshot before trusting a run.
- The baseline benchmark against Eden ([baseline.md](baseline.md)) still lacks the opening-scene frame-time spread.
