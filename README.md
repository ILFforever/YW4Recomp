<h1 align="center">YW4 Recomp</h1>

<p align="center">
  Play <b>Yo-kai Watch 4++</b> as a real PC program, from your own copy of the game: no Switch and no emulator.
  <br>
  <sub>A fan static recompilation of the Nintendo Switch game (update 2.2.0), in the style of <a href="https://github.com/N64Recomp/N64Recomp">N64Recomp</a>: the game's code is translated into a native Windows program.</sub>
</p>

<p align="center">
  <img alt="Phase 4: renderer" src="https://img.shields.io/badge/phase-4%20·%20renderer-6f42c1">
  <img alt="Platform: Windows, Linux planned" src="https://img.shields.io/badge/platform-Windows%20(Linux%20planned)-informational">
  <img alt="Graphics" src="https://img.shields.io/badge/graphics-Vulkan-ac162c">
  <img alt="Playable: house and town" src="https://img.shields.io/badge/playable-house%20%26%20town-success">
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

Early, but playable. The game runs from boot through a new game: logos, title menu, name entry, the opening
cutscenes with dialogue, free movement in the Amano house and town, the Yo-kai Pad menus, the diary and autosave, at
the game's 30 frames/s (some scenes still dip below it). Expect missing effects and crashes in places nobody has
reached yet.

- How it got here, step by step: [docs/progress.md](docs/progress.md)
- What is known to be missing or wrong: [docs/known-issues.md](docs/known-issues.md)

## Why not just use an emulator?

Switch emulators already run *Yo-kai Watch 4++* at full speed on a good PC. This project aims further:
**better rendering, better graphics and better performance** than the Switch version (see [Goals](#goals)).

An emulator can't do most of this. It imitates the console and plays the game's GPU commands as they are, so the
game still decides its shadow quality, its 16:9 UI, its 30 frames/s and its effects. Here the game's renderer is
being replaced with our own, so those become ours to change: bigger shadow maps and soft shadows are already in,
higher resolution, ultrawide, higher frame rates and better anti-aliasing are next.

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
| 🗓️ | **Ultrawide and other aspect ratios** | A wider view of the scene, with the UI laid out correctly |
| 🗓️ | **Higher frame rates** | Above the game's 30 frames/s; also needs the game's timing patched |
| 🗓️ | **Better anti-aliasing** | Smooth edges applied before the UI is drawn, so text stays crisp; then temporal AA |
| 🗓️ | **Mod support** | Load replaced game files from a mod folder without touching your dump (first use: the English translation mod) |
| 🗓️ | **Input remapping** | Keyboard, mouse and controller bindings of your choice with corresponding hints on screen |
| 🗓️ | **Linux** | The runtime is written to be portable; not built or tested there yet |

✅ = Done, 🔧 = In progress, 🗓️ = Planned

### Our own renderer

Replaying the game's draw calls ties every frame to the Switch's choices: its resolution, its 16:9 layout, its
shadow settings. A renderer of our own draws the same scene from the game's data, so those choices become ours to
change ([decision 0023](docs/decisions/0023-custom-renderer-plan.md)). 

| Status | Pass | Where it stands |
|---|---|---|
| Done, off by default (F9) | Shadow maps | Our own pass ([decision 0024](docs/decisions/0024-shadow-pass.md)) |
| Done, off by default (F6) | G-buffer | Our draws, our shaders for the main surfaces and characters, identical to the game's ([decision 0026](docs/decisions/0026-gbuffer-pass.md)) |
| In progress, off by default (F6) | Lit material pass | Our draws; our shaders for most surfaces, identical to the game's |
| Planned | Post-processing, UI | The game's |

Still the game's everywhere: the choice of what to draw, textures and character bones.

## Current Graphical Improvements

Improvements over the original game. Details, comparisons and measurements: [docs/graphics.md](docs/graphics.md).

Press **F1** in game for the settings panel: every option with all its choices, the current one boxed.

![The settings panel](docs/screenshots/22-settings-panel.png)

| Option | Key | What it does |
|---|---|---|
| Settings panel | F1 | All the options below in one list; arrow keys to choose and change |
| 3D view | F4 | 1600×900 (the game's), 1920×1080, 2560×1440 or 3840×2160; the UI is drawn at the same size |
| Texture filtering | F2 | The game's, or anisotropic ×2 to ×16: distant and slanted ground, roads and walls stay sharp. Default ×16 |
| Distance blur | panel | The game's, half or off: the game blurs everything far away. Default half |
| Shadow map size | F8 | 1024 (the game's), 2048 or 4096: sharper shadow edges. Recommended: 4096 |
| PCSS (soft shadows) | F7 | Off, ×1, ×2, ×4. Recommended: ×2 |
| Native shadow pass | F9 | Our own renderer draws the shadow casters from the game's data; slightly faster than the game's |
| Performance HUD | F3 | Frame time, CPU and GPU cost and memory, in the window's corner |

**Shadow map size**: a bigger shadow map gives sharper, more detailed shadow edges, most visibly on shadows far from
the camera. The bamboo forest, PCSS off:

| 1024 (the game's) | 2048 | 4096 (recommended) |
|---|---|---|
| ![1024 shadow map](docs/screenshots/12-shadow-map-1024.jpg) | ![2048 shadow map](docs/screenshots/13-shadow-map-2048.jpg) | ![4096 shadow map](docs/screenshots/14-shadow-map-4096.jpg) |

**PCSS** (percentage-closer soft shadows) makes shadows behave more like real ones:

- **Close shadows stay sharp**: where a shadow touches its caster (feet, poles, the base of a wall).
- **Farther shadows get softer**: the farther the caster is from the ground, the softer the edge, so tree canopies
  become soft dappled shade instead of a field of hard dots.
- **Less shimmering**: hard edges flicker as the camera moves; softer ones spread that out, so distant shadows
  shimmer much less.

| PCSS off | PCSS ×2 (recommended) |
|---|---|
| ![PCSS off: hard, speckled leaf shadows](docs/screenshots/16-pcss-off.jpg) | ![PCSS x2](docs/screenshots/18-pcss-x2.jpg) |

The game starts with the recommended 4096 map, PCSS ×2, ×16 texture filtering and half distance blur, at the game's
1600×900. For the game's original look, set all of them back to their first choice in the panel.

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
shadows here are our own pass with no graphical improvements applied ("CUSTOM", switched on with F9).

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
| `work/`, `build/` | Generated output from your own game files (gitignored, never committed) |

## Credits

- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** - the model this project follows: ship the tools, let
  every player recompile their own copy.
- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by peiikai - a static recompilation of *Link's Awakening*
  for Switch; its approach to the Switch SDK and its README layout inspired this project's.
- **[musl](https://musl.libc.org/)** - the math library (`third_party/musl`, MIT).
- **[SDL3](https://www.libsdl.org/)** and the **[Vulkan](https://www.vulkan.org/)** headers.
- **Eden** (a Switch emulator) - used only as a visual and performance reference; none of its code is used.
- **LEVEL-5** for Yo-kai Watch 4. This project is not affiliated with or endorsed by LEVEL-5 or Nintendo.

## License

[MIT](LICENSE), for the code and docs in this repository only: not the game, its files, or anything built from
your copy of it. Third-party code keeps its own license (`third_party/musl`: MIT).
