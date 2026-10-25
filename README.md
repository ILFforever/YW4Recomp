<h1 align="center">YW4 Recomp</h1>

<p align="center">
  A fan static recompilation of <b>Yo-kai Watch 4++</b> (Nintendo Switch, update 2.2.0) into a native application, in the style of <a href="https://github.com/N64Recomp/N64Recomp">N64Recomp</a>.
</p>

<p align="center">
  <img alt="Phase 4: renderer" src="https://img.shields.io/badge/phase-4%20·%20renderer-6f42c1">
  <img alt="Platforms" src="https://img.shields.io/badge/platforms-Windows%20%7C%20Linux-informational">
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
> **Bring your own game.** The repository holds tools, runtime code and patches, nothing from the game itself. To use
> it you need the ExeFS and RomFS of your own *Yo-kai Watch 4++* at update **2.2.0**, the last 4++ release; the tools
> check the exact build by hash and stop on anything else. Getting those files is up to you and is not covered here.
> The recompiled C code is produced on your machine from your copy and stays there.

## Status

Early, but playable. The game runs from boot through a new game: logos, title menu, name entry, the opening
cutscenes with dialogue, free movement in the Amano house and town, the Yo-kai Pad menus, the diary and autosave, at
a locked 30 frames/s. Expect missing effects and crashes in places nobody has reached yet.

- How it got here, step by step: [docs/progress.md](docs/progress.md)
- What is known to be missing or wrong: [docs/known-issues.md](docs/known-issues.md)

## Goals

| | Goal | Where it stands |
|---|---|---|
| ✅ | **Native performance** | The game runs as a native Windows program: about 10 ms of CPU work per frame against the 33 ms its 30 frames/s allow, under one core |
| 🚧 | **Our own renderer** | Replace the game's renderer pass by pass, so the improvements below become possible (see below) |
| 🚧 | **Better shadows** | Bigger shadow maps and PCSS soft shadows done; contact shadows and steadier cascades next |
| ⬜ | **Higher resolution** | Render the 3D scene above the game's 1600×900, sharper on 1080p, 1440p and 4K screens |
| ⬜ | **Ultrawide and other aspect ratios** | A wider view of the scene, with the UI laid out correctly |
| ⬜ | **Higher frame rates** | Above the game's 30 frames/s; also needs the game's timing patched |
| ⬜ | **Better anti-aliasing** | And other effects the Switch had no budget for |
| ⬜ | **Mod support** | Load replaced game files from a mod folder without touching your dump (first use: the English translation mod) |
| ⬜ | **Input remapping** | Keyboard, mouse and controller bindings of your choice |
| ⬜ | **Linux** | The runtime is written to be portable; not built or tested there yet |

✅ done · 🚧 in progress · ⬜ planned

### Our own renderer

Replaying the game's draw calls ties every frame to the Switch's choices: its resolution, its 16:9 layout, its
shadow settings. A renderer of our own draws the same scene from the game's data, so those choices become ours to
change ([decision 0023](docs/decisions/0023-custom-renderer-plan.md)). Each replaced pass first has to look exactly
like the game's (checked pixel by pixel) before anything is changed.

| | Pass | Where it stands |
|---|---|---|
| ✅ | Shadow maps | Our own pass ([decision 0024](docs/decisions/0024-shadow-pass.md)) |
| ✅ | G-buffer | Our draws, our shaders for the main surfaces and characters, identical to the game's ([decision 0026](docs/decisions/0026-gbuffer-pass.md)) |
| 🚧 | Lit material pass | Our draws; our shaders for most surfaces, identical to the game's |
| ⬜ | Post-processing, UI | The game's |

Still the game's everywhere: the choice of what to draw, textures and character bones.

## How it works

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
shadows here are our own pass ("CUSTOM").

<p align="center">
  <img src="docs/screenshots/11-hud-in-town.jpg" alt="In town with the performance HUD: frame time, work, GPU, CPU, memory and a frame-time graph" width="820">
</p>

## Graphics options

Improvements over the original game. Details, comparisons and measurements: [docs/graphics.md](docs/graphics.md).

| Option | Key | What it does |
|---|---|---|
| Performance HUD | F3 | Frame time, CPU and GPU cost, memory and the settings below, in the window's corner |
| Native shadow pass | F9 | Our own renderer draws the shadow casters from the game's data; slightly faster than the game's |
| Shadow map size | F8 | 1024 (the game's), 2048 or 4096: sharper shadow edges. Recommended: 4096 |
| PCSS (soft shadows) | F7 | Off, ×1, ×2, ×4. Recommended: ×2 |

**PCSS** (percentage-closer soft shadows) makes shadows behave more like real ones:

- **Close shadows stay sharp**: where a shadow touches its caster (feet, poles, the base of a wall).
- **Farther shadows get softer**: the farther the caster is from the ground, the softer the edge, so tree canopies
  become soft dappled shade instead of a field of hard dots.
- **Less shimmering**: hard edges flicker as the camera moves; softer ones spread that out, so distant shadows
  shimmer much less.

| PCSS off | PCSS ×2 (recommended) |
|---|---|
| ![PCSS off: hard, speckled leaf shadows](docs/screenshots/16-pcss-off.jpg) | ![PCSS x2](docs/screenshots/18-pcss-x2.jpg) |

The game starts with the recommended 4096 map and PCSS ×2. For the game's original shadows, start with
`YW4_SHADOW_RES=1024 YW4_PCSS=0` (or press F8 and F7).

## Building from source

Requirements: Python 3.11+, Git, Clang (or GCC), a GPU with Vulkan 1.3 and the Vulkan SDK; SDL3 is fetched
automatically on Windows. Full steps in [docs/setup.md](docs/setup.md).

1. Copy `config.example.toml` to `config.local.toml` and point it at your extracted ExeFS and RomFS.
2. Verify and decompress the game code: `python -m tools.yw4.ingest`.
3. Recompile and build: `python -m recomp.translate`, `python -m tools.yw4.shaders`, `python -m recomp.shader --check`,
   then `python -m recomp.build --run`.

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
