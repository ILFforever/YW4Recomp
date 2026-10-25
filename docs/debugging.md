# Run options and debugging

Environment variables and keys read by the runtime. The game runs fine with none of them set. Tools that run the
game with them and compare results: [tools/dev](../tools/dev/README.md).

## Everyday options

| Variable | What it does |
|---|---|
| `YW4_SCREENSHOTS=600,2500` | Save a screenshot at these frame numbers (also F12 at any time) |
| `YW4_SCREENSHOT_DIR=path` | Where screenshots go (default `<work_dir>/screenshots/`) |
| `YW4_PERF=0` | Turn off the performance log (on by default: `work/perf/perf_<time>.csv` per frame and a `.txt` summary: frame interval, game work, GPU busy/span from timestamp queries, time in command-list translation and GPU waits, CPU, memory; every 600 frames stderr also lists each busy guest thread's share of a core; `YW4_PERF_DIR` moves it) |
| `YW4_PROFILE=1` | Sample where the presenting (main) thread is every millisecond (Windows; `YW4_PROFILE=submit`: the threads that submit GPU work), from frame `YW4_PROFILE_FROM` if set; `work/perf/profile_<time>.txt`, named with `python -m tools.yw4.profile` |
| `YW4_LOG=0` | Turn off the log file (on by default: everything printed on stderr is also saved in `work/logs/yw4_<time>.log`; `YW4_LOG_DIR` moves it) |
| `YW4_AUDIO=0` | No sound (the game's audio still runs, timed at 48 kHz) |
| `YW4_AUDIO_DUMP=<file>` | Also save the sound as played: raw PCM16, 48 kHz, the game's channel count (6 = 5.1) |
| `YW4_HEADLESS=1` | No window, no Vulkan (nothing is drawn) |
| `YW4_VULKAN_VALIDATION=1` | Turn on the Vulkan validation layer |
| `YW4_SHADOWS=native` | Start with our own shadow-caster pass on ([decision 0024](decisions/0024-shadow-pass.md)); work in progress, the default is the game's. F9 switches while playing |
| `YW4_GBUFFER=check` | Our G-buffer pass ([decision 0026](decisions/0026-gbuffer-pass.md)), step 1: compares what it would use with the game's G-buffer draws (buffers, matrices) and logs a summary every 300 frames; nothing is drawn |
| `YW4_GBUFFER=native` | Our G-buffer pass, step 2b: our own draws (our vertex and index buffers and object constants; the game's material constants, textures and targets) with our readable programs (`runtime/shaders/gbuffer_*`) in place of the game's main G-buffer draws; logs a summary every 300 frames; F6 switches while playing (the HUD's G-BUFFER line) |
| `YW4_GBUFFER=programs` | Step 2a only: our readable programs in the game's own draws |
| `YW4_GBUFFER_LAYOUTS=<hex mask>` | Native mode draws only the programs of these entries of the table in `runtime/src/gbuffer.c` (all by default), to narrow a fault down |
| `YW4_MATERIAL=0` | Leave the lit material pass (layer kind 0, keys 0..2) to the game; by default it follows `YW4_GBUFFER` (our draws there too in `native` mode) |
| `YW4_STAGES=0` | Leave the main view's other model layers (kind 0, other keys: the named stages) to the game; by default they follow `YW4_GBUFFER` too |
| `YW4_CAPTURE_LAYER_AT=<GPU frame>[,...]` | With our G-buffer pass on: the colour target as the material pass left it, in those frames, to `work/dump/layer_material_f<frame>_t0.raw` (for comparisons within one run, with `YW4_GBUFFER_ALTERNATE`) |
| `YW4_GBUFFER_ALTERNATE=<frame>` | For 16 GPU frames from that one, odd frames use our G-buffer pass (the mode's: `native` or `programs`) and even frames the game's: compares the two in one run (with `YW4_DUMP_IMAGE`; a frame dumped as F was drawn as F-1) |
| `YW4_HUD=1` | Start with the performance HUD shown (F3 toggles it) |
| `YW4_SHADOW_RES=1024` (`2048`, `4096`) | Shadow map size at start (default 4096; 1024 is the game's; F8 cycles it): the shadow maps that size instead of the game's 1024, and the shadow filter's spread scaled down to match, so edges get sharper, mostly in the far cascades. Costs GPU time and memory. Works with either shadow pass |
| `YW4_PCSS=0` (`1`, `2`, `3`) | PCSS (soft contact shadows) at start: off (x1, x2, x4); default x2; F7 cycles it. `YW4_SHADOW_RES=1024 YW4_PCSS=0` gives the game's original shadows. Only the game's screen-space shadow mask shader has it (`PCSS_SHADERS` in recomp/shader.py) |
| `YW4_PCSS_SCALE=a,b,c,d` / `YW4_PCSS_MAX=<texels>` / `YW4_PCSS_EPS=<depth>` | PCSS tuning: blur radius per unit of depth gap for cascades 0-3 (default 0.01,0.005,0.0025,0.0025), largest radius and search area (default 6 texels of a 1024 map), smallest depth gap that counts as a caster |
| `YW4_PCSS_DEBUG=1`..`6` | PCSS debug views in place of the shadow: casters found, blur radius, the game's lookup, raw depth, a hand-made compare, the reference depth |
| `YW4_PREBUILD=0` | Don't build the listed pipelines (the PCSS shadow mask) on a background thread at start-up |

Saved screenshots get the same sRGB decode as the window, so their colours match what is on screen.

## Capture keys

| Key | What it does |
|---|---|
| F12 | Screenshot of the next frame |
| F11 | Describe the draws of the next frame in the log: targets, shaders, textures, the vertex stage's matrices (`YW4_GPU_TRACE=<n>` with `YW4_GPU_TRACE_AT=<frame>` does the same from a start frame) |
| F10 | Dump multi-layer depth images (shadow maps) to `work/dump/*.pgm` |
| F9 | Switch the shadow casters between the game's and our pass ([decision 0024](decisions/0024-shadow-pass.md)); the log says which, and how many submeshes our pass drew since the last switch. Recorded and replayed like F10-F12 |
| F8 | Shadow quality: cycles the shadow maps 1024 (the game's) → 2048 → 4096 (the default), with the filter spread scaled to match; the HUD's SHADOWS line shows the size. The frame of the switch can show wrong shadows once |
| F6 | The G-buffer: the game's programs or ours ([decision 0026](decisions/0026-gbuffer-pass.md)); recorded and replayed like F9 |
| F7 | PCSS (soft contact shadows): off → x1 → x2 (the default) → x4; the HUD's PCSS line shows it |
| F3 | Show or hide the performance HUD in the window's top-left corner: frames per second, frame interval, the game's work per frame, GPU busy time, draws, process CPU, memory, whose shadow casters are drawn. Averages over the last 30 frames, from the perf log's numbers (`YW4_PERF=0` leaves it empty). F12 screenshots include it while it is shown |

## Recording and replaying input

| Variable | What it does |
|---|---|
| `YW4_RECORD=0` | Every input change and capture key is recorded in `work/inputs/rec_<time>.txt` (on by default); this turns it off |
| `YW4_REPLAY=<file>` | Feed a recording back at the same ticks, so a route can be run without a person. When the replay ends, the keyboard and gamepad work again, so a short recording can just load the save. Long routes can drift (about one run in three ends somewhere else, e.g. stuck on the save-load screen); check the screenshot |
| `YW4_INPUT=2400:1:10` | Scripted presses, `tick:buttons(hex):length`, comma separated; `1` is A |

## Looking inside a frame

| Variable | What it does |
|---|---|
| `YW4_DUMP_IMAGE=<hex addr>[,<hex addr>...]` `YW4_DUMP_IMAGE_AT=<frame>[,<frame>...]` | Read back the images at those guest addresses at that frame (several frames: the file names get `_f<frame>`) (depth, RGBA8, RGBA16F, R11G11B10F): per-layer min/max/cleared share, `work/dump/image_*.pgm`, and raw floats in `image_*.rgbaf` (colour) or `image_*.depthf` (depth). This is the image's last use in the frame |
| `YW4_CAPTURE_FS=<fs key prefix>[,...]` with `YW4_DUMP_IMAGE_AT=<frame>` | Copy the colour targets right after each draw with that fragment shader in that frame, to `work/dump/cap_*.raw`; shows a pass's output before later passes reuse the target. `python -m tools.yw4.capview` makes a contact sheet |
| `YW4_COMPUTE_TRACE=<n>` | Every n frames, the inputs and results of the last dispatch of each compute shader |

Image addresses and shader keys come from the F11 log. Shaders are named by the FNV-1a hash of their assembly:
`work/shaders/asm/<key>.<stage>.asm` is the original, `work/shaders/glsl/<key>.<stage>` the translation (one GLSL
statement per instruction, with the source as a comment).

## Tracing and probes

`YW4_TRACE_*`, `YW4_PROBE*`, `YW4_WATCH*` (guest write watch with a call stack; one per process) and
`YW4_GPU_TRACE*` are described where they are read in `runtime/src/`. To name a guest address in a log, pipe it
through `python -m tools.yw4.where -`.

Two of them map the engine's frame ([frame.md](frame.md)):

| Variable | What it does |
|---|---|
| `YW4_TRACE_LAYERS=<frame>` | Every layer the model renderer (`f_a45890`) draws while frames `<frame>` to `<frame>+2` are presented: thread, layer kind, key range, mask and submeshes drawn. Maps layer kinds to passes ([frame.md](frame.md)) |
| `YW4_TRACE_RENDER_JOBS=<frame>` | Every job the render threads run while frames `<frame>` to `<frame>+2` are presented: function, its two arguments, the thread. Slows the game a little (a check at every function entry) |
| `YW4_TRACE_DRAW_CALLERS=1` | Each different call stack that records a draw, once per render-target size and program, with the shaders; `python -m tools.yw4.mainview <log>` groups such a log (with `YW4_GPU_TRACE`) into passes with the drawing functions |
| `YW4_TRACE_DRAWLIST=<frame>` | From that frame, the model renderer's draws (`YW4_TRACE_DRAWLIST_MAX`, default 24): what was bound, the submesh record, and where in the draw object, geometry and submesh records the bound addresses and uniform-buffer contents are found (pointer search two levels deep), the engine's texture slots, the model record's texture table and a texture view's words. `YW4_TRACE_DRAWLIST_COLOURS=3` keeps only draws into 3 colour targets (the G-buffer). Slow while it prints |
