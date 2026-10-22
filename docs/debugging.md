# Run options and debugging

Environment variables and keys read by the runtime. The game runs fine with none of them set.

## Everyday options

| Variable | What it does |
|---|---|
| `YW4_SCREENSHOTS=600,2500` | Save a screenshot at these frame numbers (also F12 at any time) |
| `YW4_SCREENSHOT_DIR=path` | Where screenshots go (default `<work_dir>/screenshots/`) |
| `YW4_PERF=0` | Turn off the performance log (on by default: `work/perf/perf_<time>.csv` per frame and a `.txt` summary: frame interval, game work, GPU busy/span from timestamp queries, CPU, memory; `YW4_PERF_DIR` moves it) |
| `YW4_LOG=0` | Turn off the log file (on by default: everything printed on stderr is also saved in `work/logs/yw4_<time>.log`; `YW4_LOG_DIR` moves it) |
| `YW4_HEADLESS=1` | No window, no Vulkan (nothing is drawn) |
| `YW4_VULKAN_VALIDATION=1` | Turn on the Vulkan validation layer |

Saved screenshots are currently paler than the live window (see [known issues](known-issues.md)); judge colours from
the window, or decode the BMP's sRGB once.

## Capture keys

| Key | What it does |
|---|---|
| F12 | Screenshot of the next frame |
| F11 | Describe the draws of the next frame in the log: targets, shaders, textures, the vertex stage's matrices (`YW4_GPU_TRACE=<n>` with `YW4_GPU_TRACE_AT=<frame>` does the same from a start frame) |
| F10 | Dump multi-layer depth images (shadow maps) to `work/dump/*.pgm` |

## Recording and replaying input

| Variable | What it does |
|---|---|
| `YW4_RECORD=0` | Every input change and capture key is recorded in `work/inputs/rec_<time>.txt` (on by default); this turns it off |
| `YW4_REPLAY=<file>` | Feed a recording back at the same ticks, so a route can be run without a person. Long routes can drift (about one run in three ends somewhere else, e.g. stuck on the save-load screen); check the screenshot |
| `YW4_INPUT=2400:1:10` | Scripted presses, `tick:buttons(hex):length`, comma separated; `1` is A |

## Looking inside a frame

| Variable | What it does |
|---|---|
| `YW4_DUMP_IMAGE=<hex addr>[,<hex addr>...]` `YW4_DUMP_IMAGE_AT=<frame>` | Read back the images at those guest addresses at that frame (depth, RGBA8, RGBA16F, R11G11B10F): per-layer min/max/cleared share, `work/dump/image_*.pgm`, and raw floats in `image_*.rgbaf` (colour) or `image_*.depthf` (depth). This is the image's last use in the frame |
| `YW4_CAPTURE_FS=<fs key prefix>[,...]` with `YW4_DUMP_IMAGE_AT=<frame>` | Copy the colour targets right after each draw with that fragment shader in that frame, to `work/dump/cap_*.raw`; shows a pass's output before later passes reuse the target. `python -m tools.yw4.capview` makes a contact sheet |
| `YW4_COMPUTE_TRACE=<n>` | Every n frames, the inputs and results of the last dispatch of each compute shader |

Image addresses and shader keys come from the F11 log. Shaders are named by the FNV-1a hash of their assembly:
`work/shaders/asm/<key>.<stage>.asm` is the original, `work/shaders/glsl/<key>.<stage>` the translation (one GLSL
statement per instruction, with the source as a comment).

## Tracing and probes

`YW4_TRACE_*`, `YW4_PROBE*`, `YW4_WATCH*` (guest write watch with a call stack; one per process) and
`YW4_GPU_TRACE*` are described where they are read in `runtime/src/`. To name a guest address in a log, pipe it
through `python -m tools.yw4.where -`.
