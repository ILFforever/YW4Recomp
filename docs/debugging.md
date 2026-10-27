# Run options and debugging

Environment variables and keys read by the runtime. The game runs fine with none of them set. Tools that run the
game with them and compare results: [tools/dev](../tools/dev/README.md).

## Everyday options

| Variable | What it does |
|---|---|
| `YW4_SETTINGS_FILE=<file>` (`none`) | Where the settings panel's choices are kept (default `settings.ini` next to `yw4.exe`): read at start, written on every change in the panel, one `YW4_...=value` line per setting. A variable set in the environment wins over its line. `tools.dev.run` sets `none`, so test runs neither read nor change the owner's file |
| `YW4_SCREENSHOTS=600,2500` | Save a screenshot at these frame numbers (also F12 at any time) |
| `YW4_SCREENSHOT_DIR=path` | Where screenshots go (default `<work_dir>/screenshots/`) |
| `YW4_PERF=0` | Turn off the performance log (on by default: `work/perf/perf_<time>.csv` per frame and a `.txt` summary: frame interval, game work, GPU busy/span from timestamp queries, time in command-list translation and GPU waits, CPU, memory; every 600 frames stderr also lists each busy guest thread's share of a core; `YW4_PERF_DIR` moves it) |
| `YW4_PROFILE=1` | Sample where the presenting (main) thread is every millisecond (Windows; `YW4_PROFILE=submit`: the threads that submit GPU work), from frame `YW4_PROFILE_FROM` if set; `work/perf/profile_<time>.txt`, named with `python -m tools.yw4.profile` |
| `YW4_PROFILE_THREAD=<n>` | With `YW4_PROFILE=submit`: sample only guest thread n (the number in the performance report's thread lines; the busiest render thread is usually 20) |
| `YW4_LOG=0` | Turn off the log file (on by default: everything printed on stderr is also saved in `work/logs/yw4_<time>.log`; `YW4_LOG_DIR` moves it) |
| `YW4_AUDIO=0` | No sound (the game's audio still runs, timed at 48 kHz) |
| `YW4_AUDIO_DUMP=<file>` | Also save the sound as played: raw PCM16, 48 kHz, the game's channel count (6 = 5.1) |
| `YW4_HEADLESS=1` | No window, no Vulkan (nothing is drawn) |
| `YW4_VULKAN_VALIDATION=1` | Turn on the Vulkan validation layer |
| `YW4_SHADOWS=native` | Start with our own shadow-caster pass on ([decision 0024](decisions/0024-shadow-pass.md)); work in progress, the default is the game's. The settings panel (F1, SHADOW PASS) switches while playing |
| `YW4_GBUFFER=check` | Our G-buffer pass ([decision 0026](decisions/0026-gbuffer-pass.md)), step 1: compares what it would use with the game's G-buffer draws (buffers, matrices) and logs a summary every 300 frames; nothing is drawn |
| `YW4_GBUFFER=native` | Our G-buffer pass, step 2b: our own draws (our vertex and index buffers and object constants; the game's material constants, textures and targets) with our readable programs (`runtime/shaders/gbuffer_*`) in place of the game's main G-buffer draws; logs a summary every 300 frames; the settings panel (F1, G-BUFFER) switches while playing |
| `YW4_GBUFFER=programs` | Step 2a only: our readable programs in the game's own draws |
| `YW4_GPU_THREAD=0` | Translate the game's command lists to Vulkan inside its submit call, on its own thread, as before decision 0029, instead of on our GPU worker thread (the default) |
| `YW4_VRAM_GEOMETRY=0` | The game's vertex and index data read from its memory pools in system memory, as before decision 0028, instead of copies in video memory (the default; about 3x less GPU time in busy towns) |
| `YW4_GPU_PASSES=1` | Per-pass GPU timing from the start (F5 switches it): every 300 frames the log lists the most expensive passes in ms per frame, with their targets, draws, render pass restarts, texture uploads, vertices and first programs (`runtime/src/gpu_passes.c`) |
| `YW4_SHADOW_PROGRAMS=0` | With `YW4_GBUFFER=native`, the shadow layers' draws our shadow pass leaves to the game (programs it has no shader for) stay the game's; by default they are ours, drawn in place with the game's program and our matrices |
| `YW4_GBUFFER_SKIP=<keys>` | Native mode leaves these vertex programs (hex keys, comma separated) to the game, to narrow a fault down |
| `YW4_MATERIAL=0` | Leave the lit material pass (layer kind 0, keys 0..2) to the game; by default it follows `YW4_GBUFFER` (our draws there too in `native` mode) |
| `YW4_STAGES=0` | Leave the main view's other model layers (kind 0, other keys: the named stages) to the game; by default they follow `YW4_GBUFFER` too |
| `YW4_TAKEOVER=1` | With `YW4_GBUFFER=native`: the takeover ([decision 0030](decisions/0030-takeover.md)): G-buffer draws whose inputs are all decoded are made by us at the submesh-draw hook and the engine's state work and draw for them are skipped; `yw4: takeover:` lines every 300 frames count ours and why the rest stay the game's |
| `YW4_TAKEOVER_ALTERNATE=1` | The takeover on odd presented frames only, the game's on even ones: for `tools.dev.frame_ab` comparisons within one run (the same-frame A/B can't replay a draw the game never recorded) |
| `YW4_CAPTURE_LAYER_AT=<GPU frame>[,...]` | With our G-buffer pass on: the colour target as the material pass left it, in those frames, to `work/dump/layer_material_f<frame>_t0.raw` (for comparisons within one run, with `YW4_GBUFFER_ALTERNATE`) |
| `YW4_AB_AT=<GPU frame>[,...]` `YW4_AB_PASS=gbuffer\|material\|shadow` | **Same-frame A/B** (`runtime/src/gpu_ab.c`), with our pass on (`YW4_GBUFFER=native`): in those frames the pass's commands run twice from the same images, with our draws and with the game's, and every target is compared texel by texel (`yw4: ab:` lines; 0 = identical). Nothing moves between the two runs. `YW4_GBUFFER=check` is the control (both runs the game's: must give 0). `shadow`: the first shadow layer of the frame; needs `YW4_SHADOW_RES=1024` (bigger maps are too big to copy and are skipped) |
| `YW4_GBUFFER_ALTERNATE=<frame>` | For 16 GPU frames from that one, odd frames use our G-buffer pass (the mode's: `native` or `programs`) and even frames the game's: compares the two in one run (with `YW4_DUMP_IMAGE`; a frame dumped as F was drawn as F-1) |
| `YW4_HUD=1` | Start with the performance HUD shown (F3 toggles it) |
| `YW4_SHADOW_RES=1024` (`2048`, `4096`) | Shadow map size at start (default 4096; 1024 is the game's; the settings panel changes it): the shadow maps that size instead of the game's 1024, and the shadow filter's spread scaled down to match, so edges get sharper, mostly in the far cascades. Costs GPU time and memory. Works with either shadow pass |
| `YW4_RES=900` (`1080`, `1440`, `2160`) | Resolution of the 3D view at start (default 900, the game's; the settings panel changes it): the game's screen-sized targets (1600×900 and its smaller copies, the 1920×1080 window image and its copies) made height/900 times larger, with viewports, scissors, the presented crop and the camera block's tap offsets scaled to match ([decision 0031](decisions/0031-resolution-scaling.md)). Other GPU-only target sizes are logged once (`kept at the game's size`) |
| `YW4_ANISO=16` (`0`, `2`, `4`, `8`) | Texture filtering at start (default 16; 0 = the game's; the settings panel changes it): every sampler the game makes has anisotropic filtering off and picks one mip level per texel, so distant and slanted surfaces go blurry early, in steps. On, mipmapped samplers that don't compare (not the shadow maps) blend between levels and filter anisotropically with up to this many taps (`gpu_sampler` in `runtime/src/gpu.c`). No measurable GPU cost in town |
| `YW4_DOF=1` (`2`, `0`) | Distance blur (depth of field) at start: half (default, the owner's pick), the game's, off. The composite shader `27b63a2bf65deaf0` blends a blurred half-size image over the scene by depth; half halves its amount (cbuf 6 row 0 `.w`, in a copy), off skips the draw (`gpu_dof` in `runtime/src/gpu_draw.c`). Its blur is measured in pixels of 800×450 images, so a larger 3D view (`YW4_RES`) also weakens it. In the settings panel (F1) |
| `YW4_EFFECTS=0` (`1`) | Effects resolution at start: 0 the game's (half), 1 full. The game draws its shadow mask, lamp lights, ambient occlusion and effects into half-size targets (800×450; 960×540 in the 1080p chain); full makes them the 3D view's size: sharper, less blocky shadow edges. Bamboo forest at 1600×900: GPU 6.9 → 8.6 ms. The distance blur's own images keep the game's ratio (named from its draws, logged `keeps the game's size ratio`), so its setting keeps its meaning. In the settings panel (F1) |
| `YW4_AA=0` (`1`, `2`, `3`) | Anti-aliasing at start: off (default), FXAA, SMAA, TAA (`runtime/src/gpu_aa.c`; SMAA from `third_party/smaa`; TAA: [decision 0032](decisions/0032-taa.md), 3D draws shifted per frame, blended with the game's own motion vectors). Runs on the scene image right after the bloom composite (fragment shader `58b87f4410e7bc5d`), before the UI is drawn into it; the HDR scene goes through a reversible perceptual curve first. In the settings panel (F1) |
| `YW4_EFFECTS_KEEP=<hex addr>[,...]` | With `YW4_EFFECTS=1`: these half-size targets keep the game's ratio too. For bisecting an effect that breaks at full size: the enlarged ones are logged (`format ... made 1600x900 (the game's is 800x450)`); keep one at a time until the picture is right (found the distance blur's depth mismatch this way, 2026-10-27) |
| `YW4_AA_DEBUG=1` (`2`, `3`) | With `YW4_AA`: 1 shows SMAA's detected edges, 2 its blend weights (×4) in place of the picture; 3 replaces the scene with shallow hard-edged stripes (5° and 20°) and anti-aliases those, a self-test: SMAA turns their steps into ramps, FXAA only softens the step corners (2026-10-27); with TAA: 4 shows the game's motion (x red, y green, ×50), 5 how far last frame's moved result is from this frame (×4, lower is better) |
| `YW4_TAA_SHARPEN=0.5` | TAA's sharpening after the blend (0 none; 1 stronger), clamped to the neighbours' range so edges get no rims |
| `YW4_TAA_SIGN=x,y` | Testing: overrides the signs TAA applies to the game's motion (default `1,-1` with the upper-left window origin) |
| `YW4_TAA_LOG=1` | Every 300 frames: how many draws TAA shifted and how many after it (the UI) it left alone |
| `YW4_SAMPLER_LOG=1` | Log each new sampler as the game set it (filters, wrap, LOD range and bias, anisotropy, compare) |
| `YW4_PCSS=0` (`1`, `2`, `3`) | PCSS (soft contact shadows) at start: off (x1, x2, x4); default x2; the settings panel changes it. `YW4_SHADOW_RES=1024 YW4_PCSS=0` gives the game's original shadows. Only the game's screen-space shadow mask shader has it (`PCSS_SHADERS` in recomp/shader.py) |
| `YW4_PCSS_SCALE=a,b,c,d` / `YW4_PCSS_MAX=<texels>` / `YW4_PCSS_EPS=<depth>` | PCSS tuning: blur radius per unit of depth gap for cascades 0-3 (default 0.01,0.005,0.0025,0.0025), largest radius and search area (default 6 texels of a 1024 map), smallest depth gap that counts as a caster |
| `YW4_PCSS_DEBUG=1`..`6` | PCSS debug views in place of the shadow: casters found, blur radius, the game's lookup, raw depth, a hand-made compare, the reference depth. Not visible as such on screen: the value goes into the shadow mask; capture the mask to read it ([tools/dev/README.md](../tools/dev/README.md), pitfalls) |
| `YW4_PREBUILD=0` | Don't build the listed pipelines (the PCSS shadow mask) on a background thread at start-up |

Saved screenshots get the same sRGB decode as the window, so their colours match what is on screen.

## Capture keys

| Key | What it does |
|---|---|
| F12 | Screenshot of the next frame |
| F11 | Describe the draws of the next frame in the log: targets, shaders, textures, the vertex stage's matrices (`YW4_GPU_TRACE=<n>` with `YW4_GPU_TRACE_AT=<frame>` does the same from a start frame) |
| F10 | Dump multi-layer depth images (shadow maps) to `work/dump/*.pgm` |
| F1 | Settings panel, the only place for the graphics settings (the other keys here are debugging tools): the graphics settings in groups (picture, effects, shadows, renderer), every option shown, the current one on a chip, and a box explaining the chosen setting and option; scaled up with the window by whole steps; Up/Down choose, Left/Right change, F1 or Escape close. On a gamepad: Home, or Minus and Plus together, opens and closes it; the D-pad or left stick choose and change; B closes. While it is open neither the keyboard nor the gamepad reaches the game, and after it closes they stay held back until everything is let go (so the B or Escape that closed it doesn't reach the game too). F2 opens the CONTROLS page instead (F1 and F2 switch between the pages, the same key again closes; on a gamepad L/R switch) (`runtime/src/bindings.c`): per Switch button and stick direction two keys and one gamepad input (a button, or a stick or trigger direction; sticks stay analog); Enter (A) waits five seconds for the new key or input, Delete/Backspace (X) clears, the last row resets all; a key or input that presses two Switch inputs shows orange. The panel's own keys stay fixed. Choices and controls are saved (`YW4_SETTINGS_FILE`). In replays: `k <tick> F1`, then `UP`, `DOWN`, `LEFT`, `RIGHT`, `ENTER`, `DELETE`, `F2`, `PAGE` (a binding itself can't be captured from a replay) (`runtime/src/settings.c`) |
| F5 | Per-pass GPU timing on or off (`YW4_GPU_PASSES`): where the GPU's time goes, in the log every 300 frames |
| F3 | Show or hide the performance HUD in the window's top-left corner: frames per second, frame interval, the game's work per frame, GPU busy time, draws, process CPU, memory; the settings are in the settings panel (F1). Averages over the last 30 frames, from the perf log's numbers (`YW4_PERF=0` leaves it empty). F12 screenshots include it while it is shown |

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
