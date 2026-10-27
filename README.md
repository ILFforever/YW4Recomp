<h1 align="center">YW4 Recomp</h1>

<p align="center">
  <b>Yo-kai Watch 4++, recompiled for PC.</b>
  <br>
  4K · Ultrawide 21:9 / 32:9 · PCSS soft shadows · TAA / SMAA · ×16 texture filtering · Xbox, PlayStation and Nintendo pads
  <br>
  <sub>A fan static recompilation of the Nintendo Switch game (update 2.2.0), in the style of <a href="https://github.com/N64Recomp/N64Recomp">N64Recomp</a>: the game's code is translated into a native Windows program.</sub>
</p>

<p align="center">
  <img alt="Phase 4: renderer" src="https://img.shields.io/badge/phase-4%20·%20renderer-6f42c1">
  <img alt="Platform: Windows, Linux planned" src="https://img.shields.io/badge/platform-Windows%20(Linux%20planned)-informational">
  <img alt="Graphics" src="https://img.shields.io/badge/graphics-Vulkan-ac162c">
  <img alt="Status: playable" src="https://img.shields.io/badge/status-playable-success">
</p>

<p align="center">
  <img src="docs/screenshots/05-title-screen.jpg" alt="The title screen, running recompiled on Windows" width="820">
  <br>
  <sub>The title screen, recompiled and running natively on Windows (Vulkan, 30 frames/s).</sub>
</p>

> [!IMPORTANT]
> **Built with AI.** Claude (Anthropic's model, used through Claude Code) wrote most of this repository: the
> recompiler, the runtime, the reverse engineering notes and these docs. A human decides what to work on, plays and
> tests every build, compares it against the real game and reviews the changes. It is a personal fan project that
> happens to be public. If AI-written code is not something you want to use or read, feel free to pass on it; if you
> do run it, that is your call.

> [!NOTE]
> Platforms:
> - Windows - x86-64, Vulkan graphics backend (this is what is built and tested)
> - Linux - x86-64, Vulkan: planned; the runtime is written to be portable but has not been tested yet

> [!NOTE]
> **Bring your own game.** Nothing from the game is in this repository. You need files from your own copy of
> *Yo-kai Watch 4++* with the last update, **2.2.0**. Getting those files is up to you and is not covered here.
>

## Status

Playable. The game runs from boot through a new game (logos, title menu, name entry, the opening
cutscenes, the Amano house and town, the Yo-kai Pad menus, the diary and autosave) and from late-game saves: towns at
night, the bamboo forest, the Yo-kai World. It holds the game's 30 frames/s in most places tested (see the known issues). Expect missing
effects and crashes in places nobody has reached yet.

- How it got here, step by step: [docs/progress.md](docs/progress.md)
- What is known to be missing or wrong: [docs/known-issues.md](docs/known-issues.md)

## Why not just use an emulator?

Switch emulators already run *Yo-kai Watch 4++* at full speed on a good PC. This project aims further:
**better rendering, better graphics and better performance** than the Switch version (see [Goals](#goals)).

An emulator can't do most of this. It imitates the console and plays the game's GPU commands as they are, so the
game still decides its shadow quality, its 16:9 UI, its 30 frames/s and its effects. Here the game's renderer is
being replaced with our own, so those become ours to change: higher resolution, sharper textures, bigger and soft
shadows, anti-aliasing that leaves the UI sharp (TAA included) and ultrawide are already in; higher frame rates are
next.

A town street, same spot: the game's shadows (1024 map, no PCSS) against ours (4096 map, PCSS ×2). The
characters' shadows keep their shape, and the tree shade on the pavement is soft:

| Stock (the game's) | Improved (ours) |
|---|---|
| ![Stock shadows: blurry character shadows, hard tree shade](docs/screenshots/20-shadows-stock.jpg) | ![Improved shadows: defined character shadows, soft tree shade](docs/screenshots/21-shadows-improved.jpg) |

It is also lighter to run. The same busy street on the same laptop ([docs/baseline.md](docs/baseline.md),
2026-10-22):

| | Emulator (Eden) | This project |
|---|---|---|
| CPU | 210% of one core | 111% |
| Memory | 15.7 GB | 3.9 GB |
| GPU | 30% | 33% |

## Goals

| Status | Goal | Where it stands |
|---|---|---|
| 🔧 | **Native performance** | Runs as a native program, no emulation; a steady 30 frames/s everywhere is not there yet (some scenes still dip) |
| 🔧 | **Our own renderer** | Replace the game's renderer pass by pass, so the improvements below become possible (see below) |
| 🔧 | **Better shadows** | Bigger shadow maps and PCSS soft shadows done; contact shadows and steadier cascades next |
| ✅ | **Sharper distance** | Anisotropic texture filtering and a softer distance blur: distant ground, roads and buildings stay sharp without 4K |
| ✅ | **Higher resolution** | The 3D view at 1080p, 1440p or 4K instead of the game's 1600×900 (30 frames/s at 4K on an RTX 4070 laptop) |
| ✅ | **Ultrawide and other aspect ratios** | 16:10, 21:9 and 32:9 in the panel: a wider view of the world with the UI kept 16:9 in the middle; exploring, battles and cutscenes work; dialog dimming and the loading screen cover the full width |
| 🗓️ | **Higher frame rates** | Above the game's 30 frames/s; also needs the game's timing patched |
| 🔧 | **Better anti-aliasing** | FXAA, SMAA and now TAA (steady thin edges: fences, nets, leaves) on the 3D scene before the UI is drawn, so text stays crisp; per-object motion for TAA next |
| 🗓️ | **Mod support** | Load replaced game files from a mod folder without touching your dump (first use: the English translation mod) |
| 🔧 | **Input remapping** | Keyboard and controller bindings on the panel's CONTROLS page, saved, with your controller's own button names; tested with a DualSense. Mouse and matching on-screen button hints next |
| 🗓️ | **Linux** | The runtime is written to be portable; not built or tested there yet |

✅ = Done, 🔧 = In progress, 🗓️ = Planned

### Our own renderer

Replaying the game's draw calls ties every frame to the Switch's choices: its resolution, its 16:9 layout, its
shadow settings. A renderer of our own draws the same scene from the game's data, so those choices become ours to
change ([decision 0023](docs/decisions/0023-custom-renderer-plan.md)). 

| Status | Pass | Where it stands |
|---|---|---|
| Done, off by default (panel: SHADOW PASS) | Shadow maps | Our own pass ([decision 0024](docs/decisions/0024-shadow-pass.md)) |
| Done, off by default (panel: G-BUFFER) | G-buffer | Our draws, our shaders for the main surfaces and characters, identical to the game's ([decision 0026](docs/decisions/0026-gbuffer-pass.md)) |
| In progress (`YW4_TAKEOVER=1`) | Lit material pass | With the takeover, 85-93% of the G-buffer's and the lit pass's draws are made from the game's data by us, skipping the engine's own draw setup ([decision 0030](docs/decisions/0030-takeover.md)) |
| Partly ours | Post-processing | The game's, with our anti-aliasing between the 3D scene and the UI |
| Planned | UI | The game's |

Still the game's: the choice of what to draw (which objects, in which order) and the engine's own draw walk.

## Current Graphical Improvements

Improvements over the original game. Details, comparisons and measurements: [docs/graphics.md](docs/graphics.md).

<details>
<summary><b>Aspect ratio</b>: 16:10, 21:9 and 32:9, the UI kept 16:9 in the middle</summary>

16:9 (the Switch's), 16:10, 21:9 and 32:9. Below, 21:9 in the schoolyard: the world goes on at
the sides; the minimap and button hints stay where they are on the Switch.

![21:9 ultrawide: a wider schoolyard with the UI in the middle](docs/screenshots/23-ultrawide-21-9.jpg)

Exploring, cutscenes and battles all work at the wider ratios. 21:9 at 4K height (5120×2160), from play:

| Town | Mansion |
|---|---|
| ![21:9 town street](docs/screenshots/24-ultrawide-town.jpg) | ![21:9 mansion hall](docs/screenshots/25-ultrawide-mansion.jpg) |
| **Cutscene: arriving at Mount Oomori** | **Cutscene: dialogue** |
| ![21:9 cutscene with the location title](docs/screenshots/26-ultrawide-cutscene-title.jpg) | ![21:9 cutscene with Jibanyan and Whisper](docs/screenshots/27-ultrawide-cutscene-dialogue.jpg) |

Layers over the whole screen, like the dimming behind a dialog and the loading screen's black, cover the full
width; pictures made for 16:9 (the copyright notice, movies, the dungeon map's backdrop) stay 16:9 in the middle
with black bars, and the town's fast-travel map shows more of the town at the sides. At 4K ultrawide there are
occasional stutters in heavy scenes; more optimization is planned ([known issues](docs/known-issues.md)).

Menus, maps and battles at 21:9 (the dialog at 2133×900, the rest at 5120×2160):

| Yo-kai Pad | Fast-travel map: more of the town at the sides |
|---|---|
| ![21:9 Yo-kai Pad menu](docs/screenshots/55-ultrawide-yokai-pad.jpg) | ![21:9 town fast-travel map reaching the edges](docs/screenshots/56-ultrawide-town-map.jpg) |
| **Dungeon map: a 16:9 picture with black bars** | **Dialog: the dimming covers the full width** |
| ![21:9 bamboo forest map with black side bars](docs/screenshots/57-ultrawide-dungeon-map.jpg) | ![21:9 dialog with the whole screen dimmed](docs/screenshots/58-ultrawide-dialog.jpg) |
| **Into a battle** | **Battle start** |
| ![21:9 swirl into a battle](docs/screenshots/59-ultrawide-battle-swirl.jpg) | ![21:9 battle start with the enemy yo-kai](docs/screenshots/60-ultrawide-battle-start.jpg) |
| **Battle** | **Battle results** |
| ![21:9 battle with the command wheel](docs/screenshots/61-ultrawide-battle.jpg) | ![21:9 battle results over the blurred field](docs/screenshots/62-ultrawide-battle-results.jpg) |

</details>

<details>
<summary><b>Shadow map size</b>: up to 4096, for sharper shadow edges</summary>

A bigger shadow map gives sharper, more detailed shadow edges, most visibly on shadows far from
the camera. The bamboo forest, PCSS off:

| 1024 (the game's) | 2048 | 4096 (recommended) |
|---|---|---|
| ![1024 shadow map](docs/screenshots/12-shadow-map-1024.jpg) | ![2048 shadow map](docs/screenshots/13-shadow-map-2048.jpg) | ![4096 shadow map](docs/screenshots/14-shadow-map-4096.jpg) |

</details>

<details>
<summary><b>Soft shadows (PCSS)</b>: sharp where a shadow touches its caster, softer farther away</summary>

PCSS (percentage-closer soft shadows) makes shadows behave more like real ones:

- **Close shadows stay sharp**: where a shadow touches its caster (feet, poles, the base of a wall).
- **Farther shadows get softer**: the farther the caster is from the ground, the softer the edge, so tree canopies
  become soft dappled shade instead of a field of hard dots.
- **Less shimmering**: hard edges flicker as the camera moves; softer ones spread that out, so distant shadows
  shimmer much less.

| PCSS off | PCSS ×2 (recommended) |
|---|---|
| ![PCSS off: hard, speckled leaf shadows](docs/screenshots/16-pcss-off.jpg) | ![PCSS x2](docs/screenshots/18-pcss-x2.jpg) |

</details>

<details>
<summary><b>Anti-aliasing</b>: FXAA, SMAA and TAA, with the UI kept sharp</summary>

The Switch version has none. Three kinds are in, all applied to the 3D scene before the UI is
drawn, so text and menus stay sharp:

- **FXAA**: fast, smooths jagged edges in one frame.
- **SMAA**: sharper edge smoothing than FXAA, also one frame at a time.
- **TAA**: also fixes shimmer. Thin things (fences, the goal net, wires, leaves) fall between pixels differently
  every frame, so they crawl while the camera moves. TAA shifts the 3D scene by a fraction of a pixel each frame and
  blends the frames together, using the motion the game already writes for every pixel. In the schoolyard the goal
  net breaks into dots and dashes without AA; with TAA it is a continuous mesh. A light sharpen keeps textures crisp
  ([decision 0032](docs/decisions/0032-taa.md)).

Yo-kai World with TAA, 21:9 at 4K height (5120×2160):

![Yo-kai World street with TAA](docs/screenshots/29-aa-taa-yokai-world.jpg)

The same frame zoomed in 3×: the red railing, the fence by the lamp post and the road line.

| Off | SMAA | TAA |
|---|---|---|
| ![Railing, no AA: stair-stepped edges](docs/screenshots/30-aa-rail-off.png) | ![Railing, SMAA](docs/screenshots/31-aa-rail-smaa.png) | ![Railing, TAA: smooth edges](docs/screenshots/32-aa-rail-taa.png) |
| ![Fence, no AA: sparkling bars](docs/screenshots/33-aa-fence-off.png) | ![Fence, SMAA](docs/screenshots/34-aa-fence-smaa.png) | ![Fence, TAA: steady bars](docs/screenshots/35-aa-fence-taa.png) |
| ![Road line, no AA](docs/screenshots/36-aa-road-off.png) | ![Road line, SMAA](docs/screenshots/37-aa-road-smaa.png) | ![Road line, TAA](docs/screenshots/38-aa-road-taa.png) |

</details>

<details>
<summary><b>Texture filtering</b>: anisotropic up to ×16, sharp ground into the distance</summary>

The Switch version uses none, so the ground blurs a short way ahead of the camera. Anisotropic
filtering keeps the road, pavement and walls sharp into the distance, even at a low angle. Yo-kai World with ×16:

![Yo-kai World street with ×16 texture filtering](docs/screenshots/39-filtering-x16-yokai-world.jpg)

The same spot zoomed in 2×: the crosswalk far ahead, the pavement on the right and the road surface.

| Off (the Switch's) | ×2 | ×4 | ×8 | ×16 |
|---|---|---|---|---|
| ![Crosswalk, off: faded, blurred stripes](docs/screenshots/40-filtering-crosswalk-off.png) | ![Crosswalk, x2](docs/screenshots/41-filtering-crosswalk-x2.png) | ![Crosswalk, x4](docs/screenshots/42-filtering-crosswalk-x4.png) | ![Crosswalk, x8](docs/screenshots/43-filtering-crosswalk-x8.png) | ![Crosswalk, x16: crisp white stripes](docs/screenshots/44-filtering-crosswalk-x16.png) |
| ![Pavement, off: smooth blur](docs/screenshots/45-filtering-pavement-off.png) | ![Pavement, x2](docs/screenshots/46-filtering-pavement-x2.png) | ![Pavement, x4](docs/screenshots/47-filtering-pavement-x4.png) | ![Pavement, x8](docs/screenshots/48-filtering-pavement-x8.png) | ![Pavement, x16: brick pattern visible](docs/screenshots/49-filtering-pavement-x16.png) |
| ![Road, off: smeared](docs/screenshots/50-filtering-road-off.png) | ![Road, x2](docs/screenshots/51-filtering-road-x2.png) | ![Road, x4](docs/screenshots/52-filtering-road-x4.png) | ![Road, x8](docs/screenshots/53-filtering-road-x8.png) | ![Road, x16: grainy asphalt](docs/screenshots/54-filtering-road-x16.png) |

</details>

Press **F1** in game for the settings panel: every option with all its choices, and what each one does (arrow keys or
D-pad to choose and change). **F2** opens the **CONTROLS** page: two keys and one gamepad button for every Switch
button and stick direction, changed by pressing the new one. On a gamepad the DualSense's **mic button** (Share on
Xbox pads) opens the settings and **Create + Options** (Minus + Plus) the controls; L/R or the triggers switch
pages. The DualSense's **touchpad** shows the performance HUD (F3). Your choices are saved in
`settings.ini` next to `yw4.exe` and kept for the next start. Settings are only in the panel; the other function keys
are debugging tools ([docs/debugging.md](docs/debugging.md)).

**Settings (F1)**

![The settings panel](docs/screenshots/22-settings-panel.png)

**Controls (F2)**, with a DualSense

![The controls page with a DualSense: cross, circle, square, triangle, L1 to R3](docs/screenshots/28-controls-dualsense.png)

**Controllers:** Xbox, PlayStation and Nintendo pads work, and the controls page names the buttons as printed on
yours (A B X Y and LB/RT on Xbox; × ○ □ △ and L1-R3 on PlayStation; ZL/ZR on Nintendo). By default the game's
confirm is your controller's own confirm: × on a DualSense, A on Xbox. Tested by playing with a **DualSense (PS5)**,
battles included, and with the controls page.

| Setting | What it does |
|---|---|
| Resolution | 1600×900 (the Switch's), 1920×1080, 2560×1440 or 3840×2160; the UI is drawn at the same size |
| Aspect ratio | 16:9 (the Switch's), 16:10, 21:9 or 32:9: the world shows more at the sides, the UI stays 16:9 in the middle |
| Anti-aliasing | Off (default), FXAA, SMAA or TAA, applied to the 3D scene before the UI is drawn, so text stays sharp. TAA also steadies thin things that shimmer: fences, nets, leaves |
| Texture filtering | Off (the Switch's) or anisotropic ×2 to ×16: distant and slanted ground, roads and walls stay sharp. Default ×16 |
| Distance blur | Full (the Switch's), half or off: the game blurs everything far away. Default half |
| Effects resolution | Half (the Switch's) or full: the shadow mask, lamp lights and ambient occlusion at the 3D view's size, for sharper shadow edges. Costs ~2 ms of GPU at 1600×900 |
| Shadow map | 1024 (the Switch's), 2048 or 4096: sharper shadow edges. Default 4096 |
| Soft shadows (PCSS) | Off (the Switch's), ×1, ×2, ×4. Default ×2 |
| Shadow pass | The game's or our custom renderer draws the shadow casters; the same picture, ours slightly faster |
| G-buffer | The game's or our custom renderer draws the world's surfaces; the same picture |

F3 shows a performance HUD in the window's corner: frame time, CPU and GPU cost and memory.

The game starts at the Switch's 1600×900 and 16:9 with a 4096 shadow map, PCSS ×2, ×16 texture filtering, half
distance blur, half-size effects and no anti-aliasing. For the game's original look, set every setting to its first choice in the
panel.

## Screenshots

All captured from the recompiled build on Windows.

| | |
|---|---|
| ![Amano house kitchen](docs/screenshots/07-kitchen.jpg) | ![Opening scene](docs/screenshots/06-opening-scene.jpg) |
| The Amano house kitchen | Opening scene |
| ![LEVEL-5 logo](docs/screenshots/02-level5-logo.jpg) | ![Opening movie](docs/screenshots/04-opening-movie.jpg) |
| LEVEL-5 logo | Opening movie |

In town with the performance HUD on (F3). The game is a native Windows program: the frame takes about 10 ms of
work on the CPU and under one core of a 16-core laptop, against the 33 ms the game's 30 frames/s allow; the
shadows here are our own pass with no graphical improvements applied (SHADOW PASS: CUSTOM in the panel).

<p align="center">
  <img src="docs/screenshots/11-hud-in-town.jpg" alt="In town with the performance HUD: frame time, work, GPU, CPU, memory and a frame-time graph" width="820">
</p>



## Behind the scenes

- **Translated ahead of time, not interpreted.** Every ARM64 function in the game's `main` and `sdk` modules (73,844
  functions) becomes C before the game runs ([recomp/](recomp/README.md)); Clang compiles it into one native program.
- A native runtime ([runtime/](runtime/)) stands in for the Switch: the kernel (threads, memory, synchronisation),
  system services (filesystem with your RomFS read in place, input, audio, time, saves, applets) and the NVN graphics
  API on top of Vulkan.
- The game's own shaders are translated from the NV_gpu_program5 assembly they ship with into GLSL and SPIR-V
  ([decision 0020](docs/decisions/0020-shaders-from-the-assembly.md)).
- No emulator code is linked ([decision 0005](docs/decisions/0005-phase1-runtime-strategy.md)). Each instruction form
  of the recompiler is checked against a real ARM64 CPU (a Raspberry Pi 5).
- Why things are built this way is written down in [docs/decisions/](docs/decisions/).

## Building from source

Requirements: Python 3.11+, Git, a C compiler (Clang recommended:
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) on Windows; GCC also works), a GPU with Vulkan 1.3, and
`glslangValidator` (from the [Vulkan SDK](https://vulkan.lunarg.com/), or a standalone glslang). SDL3 is fetched
automatically on Windows. Full steps in [docs/setup.md](docs/setup.md).

1. If you will commit: install the guard that keeps game data out of commits, `python -m tools.yw4.guard --install`.
2. Copy `config.example.toml` to `config.local.toml`. Point `exefs_dir` and `romfs_dir` at your extracted ExeFS and
   RomFS; set `[tools] cc` to your Clang (the default is gcc) and `[tools] glslang` if it isn't on your PATH.
3. Check the config: `python tools/yw4/config.py` prints the resolved paths or says what is missing.
4. Verify and decompress the game code: `python -m tools.yw4.ingest`.
5. Recompile and build: `python -m recomp.translate`, `python -m tools.yw4.shaders`, `python -m recomp.shader --check`,
   then `python -m recomp.build --run`.

Runtime options are environment variables, for example `YW4_SHADOW_RES=1024`: in PowerShell
`$env:YW4_SHADOW_RES = "1024"`, in cmd `set YW4_SHADOW_RES=1024`, before starting the game.

Runtime options, capture keys, input replay and the frame-inspection tools are in
[docs/debugging.md](docs/debugging.md).

## Project layout

| Path | What |
|---|---|
| `recomp/` | AArch64 → C recompiler and the shader translator |
| `runtime/` | Native runtime: kernel, services, NVN on Vulkan |
| `tools/` | Python input pipeline and analysis tools |
| `tests/` | Tests on synthetic data (no game files) |
| `docs/` | Setup, debugging, graphics options, progress, known issues, decision records, recon report, Eden baseline |
| `third_party/musl/` | musl 1.1.19's math functions (MIT) |
| `third_party/smaa/` | SMAA's shader and lookup textures (MIT) |
| `work/`, `build/` | Generated output from your own game files (gitignored, never committed) |

## Credits

- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** - the model this project follows: ship the tools, let
  every player recompile their own copy.
- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by peiikai - a static recompilation of *Link's Awakening*
  for Switch; its approach to the Switch SDK and its README layout inspired this project's.
- **[musl](https://musl.libc.org/)** - the math library (`third_party/musl`, MIT).
- **[SMAA](https://github.com/iryoku/smaa)** by Jorge Jimenez et al. - the SMAA anti-aliasing shader and lookup textures (`third_party/smaa`, MIT).
- **[SDL3](https://www.libsdl.org/)** and the **[Vulkan](https://www.vulkan.org/)** headers.
- **Eden** (a Switch emulator) - used only as a visual and performance reference; none of its code is used.
- **LEVEL-5** for Yo-kai Watch 4. This project is not affiliated with or endorsed by LEVEL-5 or Nintendo.

## License

[MIT](LICENSE), for the code and docs in this repository only: not the game, its files, or anything built from
your copy of it. Third-party code keeps its own license (`third_party/musl`, `third_party/smaa`: MIT).
