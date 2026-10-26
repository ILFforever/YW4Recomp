# Development tools

Tools for testing changes to the runtime and the renderer: running the game in a repeatable way, comparing
two settings, and timing shader builds. Paths come from `config.local.toml`; the game's environment variables
and keys are in [docs/debugging.md](../../docs/debugging.md). Nothing here contains game data.

| Tool | What it does |
|---|---|
| `python -m tools.dev.run` | One run of the built game from a **fresh copy of a save**, with a replay and settings; prints the log's summaries |
| `python -m tools.dev.perf_compare` | Two settings, alternating runs of one replay; averages of game work, GPU time, CPU |
| `python -m tools.dev.shot_compare` | Side by side and difference of the same frames from two screenshot folders |
| `python -m tools.dev.frame_ab` | Compares two renderings **within one run** from image dumps of alternating frames |
| `python -m tools.dev.pipebench` | Builds `pipebench.c` and times **cold** Vulkan pipeline builds of translated shaders |
| `python -m tools.dev.spv_salt` | Makes a compiled shader new to the driver's cache, to test a cold start in the real game |
| `python -m tools.yw4.mainview` | Groups a traced frame into passes with the engine functions that draw them ([frame.md](../../docs/frame.md)) |

## Local files these use (in `work/`, not committed)

- `work/inputs/*.txt`: input replays. `load_save.txt` presses through the title into whatever save is given;
  `walk.txt` walks through the town; your own recordings are `rec_<time>.txt` (every run records one).
- `work/testsaves/save_base_*`: saves to start from (copied to `save_run` before each run, because the game
  autosaves). The owner's: `save_base_walk` (town), `save_base_lategame` (bamboo forest, late game),
  `save_base_forest` (a forest cutscene), `save_school` (the schoolyard, daytime, a running NPC).
- A replay file can press keys: a line `k <tick> F6` (F6, F9-F12) at that replay tick (60 per second).

## Methods that worked

**Check mode before drawing.** Before a pass of ours draws anything, a check mode compares what it would use
with what the game binds, on every draw, and logs a summary (`YW4_SHADOWS=check`, `YW4_GBUFFER=check`). Then
the owner plays for a while in that mode: variety a scripted replay never has (moving NPCs, many areas). An hour
of play gave 4.9 million checked G-buffer draws (decision 0026).

**Compare within one run, not across runs.** Replays drift: two runs of the same settings already differ on
hundreds of thousands of pixels. To show that a change is exact, switch it on and off on alternating frames of
one run and dump the images:

    python -m tools.dev.run --save work/testsaves/save_school --seconds 150 \
        YW4_GBUFFER_ALTERNATE=1800 YW4_DUMP_IMAGE=7268f13000,7269783000 \
        YW4_DUMP_IMAGE_AT=1801,1802,1803,1804,1805,1806
    python -m tools.dev.frame_ab --addr 7269783000 --a 1801 1803 1805 --b 1802 1804 --noise 7268f13000

Things that move still differ between frames, so compare with an image the change does not touch (`--noise`):
the change is exact when its count is no larger. Drive such tests by frame number (an environment variable read
by the runtime), not by replay ticks: dumps are slow and the replay runs on in real time.

**Compare in the same frame when things move.** Alternating frames can't see a fault on a moving object (a
running character differs between frames anyway). `YW4_AB_AT=<frames>` runs the pass twice in those frames,
ours and the game's, from the same starting images, and compares every target texel by texel:

    python -m tools.dev.run --save work/testsaves/save_school --seconds 90 YW4_GBUFFER=native \
        YW4_AB_AT=1800,1850 YW4_AB_PASS=material

Each `yw4: ab:` line must say 0 texels differ. Check the tool before trusting a 0: `YW4_GBUFFER=check` (both
runs the game's) gives 0, and a deliberately wrong value (one matrix element x 1.0001) gives hundreds of
thousands. 24-bit depth is compared without its undefined top 8 bits.

**Time shader builds cold.** A shader change that is cheap at run time can still be slow to *compile*: the
driver compiles pipelines when a scene first draws them, so the game freezes on loading (the PCSS helper in all
~200 shadow shaders: up to 25 s per shader, decision 0025). `pipebench` builds a shader's pipeline with a random
string added so the driver's cache can't answer:

    python -m tools.dev.pipebench 06d03ad703b4bee5 56517c1a34837483

The game's own shaders take ~60-75 ms. For a real cold start in the game, salt the file with `spv_salt` and
look for `pipeline took` in the log (every build over 100 ms is logged).

**Find a value's source from the stack.** When the engine's data for a draw doesn't match a guess, search the
calling function's stack at the hook for a pointer whose memory holds the value the game bound: that found the
renderer's per-draw world matrix (`[sp+0x30]+0x40`, `runtime/src/gbuffer.c`) after the object's own matrix was
wrong for posed parts and moving objects.

## Pitfalls

- Close every `yw4.exe` before measuring or building: a second game spoils timings, and the build can't replace
  the exe while it runs (`run.py` refuses to start then).
- Writing C with a Python heredoc turns `"\n"` in C strings into a real line break and breaks the build: edit C
  files with an editor, or write the script to a file first.
- Find a run's log by the path `run.py` prints, never by "the newest file in `work/logs`": `ls -t` in Git Bash
  listed a 2026-10-23 log first on 2026-10-27, and a grep on it reported "no such line" for a line that was there.
  `dev_run_*.log` (what `run.py` captures) and `yw4_*.log` (the runtime's own copy, `runtime/src/log.c`) hold the
  same lines.
- `YW4_PCSS_DEBUG` changes nothing you can judge on screen: its value replaces the shadow mask's lookup, which the
  lighting then uses. Read it from the mask itself: `YW4_CAPTURE_FS=06d03ad7 YW4_DUMP_IMAGE_AT=<frame>`, decoded
  with `tools.yw4.capview.decode` (the mask is VkFormat 122, B10G11R11 float, not RGBA8: decoding it as bytes
  gives coloured bands); the value is in red. Mode 2 (radius) returns the game's own lookup (0 or 1, lit or not)
  where the search finds no caster, not a radius, so compare it against a capture with `YW4_PCSS=0`.
- A capture can be missing after a run (one of four on 2026-10-27, cause unknown): check the file exists before
  reading it.
- The owner can reach any place in the game faster than a replay can, and is a good judge of flicker: for
  visual checks, launch the game for them with the right settings and ask.
