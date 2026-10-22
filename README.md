# YW4 Recomp

> [!IMPORTANT]
> **This project was made almost exclusively with AI.** Nearly all of the code, tooling, reverse engineering and
> documentation in this repository was written by an AI (Anthropic's Claude, through Claude Code), directed, tested
> and reviewed by a human. As a fan of this game, I made this for myself. Sharing it is a courtesy. If you don't agree
> with the use of AI or don't trust code written by AI, that is your right; you may simply ignore this project.
> As with anything on the internet, you run the code at your own discretion.

**YW4 Recomp** is a fan static recompilation of *Yo-kai Watch 4++* (Nintendo Switch, update 2.2.0) into a native
application, in the style of [N64Recomp](https://github.com/N64Recomp/N64Recomp).

> [!NOTE]
> Platforms:
> - Windows - x86-64, Vulkan graphics backend (this is what is built and tested)
> - Linux - x86-64, Vulkan: planned; the runtime is written to be portable but has not been tested yet

![The Amano house kitchen, recompiled and running natively on Windows](docs/screenshots/07-kitchen.jpg)

> [!NOTE]
> **No game files are included.** YW4 Recomp needs your own copy of the extracted game files of *Yo-kai Watch 4++*
> at update **2.2.0** (the final 4++ update). The tools check the build and refuse any other. Instructions on how to
> obtain these files will **NOT** be provided. This repository contains only tools, runtime code and patches; the
> recompiled code is generated on your machine from your own files and never committed.

## Status

Early, but playable. The game runs from boot through a new game: logos, title menu, name entry, the opening
cutscenes with dialogue, free movement in the Amano house and town, the Yo-kai Pad menus, the diary and autosave, at
a locked 30 frames/s. Expect missing effects and crashes in places nobody has reached yet.

- How it got here, step by step: [docs/progress.md](docs/progress.md)
- What is known to be missing or wrong: [docs/known-issues.md](docs/known-issues.md)

## How it works

- **This is not emulation.** Every ARM64 function in the game's `main` and `sdk` modules (73,844 functions) is
  translated to C ahead of time ([recomp/](recomp/README.md)), compiled with Clang and linked into one native program.
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
| ![Title screen](docs/screenshots/05-title-screen.jpg) | ![Opening scene](docs/screenshots/06-opening-scene.jpg) |
| Title screen | Opening scene |
| ![LEVEL-5 logo](docs/screenshots/02-level5-logo.jpg) | ![Opening movie](docs/screenshots/04-opening-movie.jpg) |
| LEVEL-5 logo | Opening movie |

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
| `docs/` | Setup, debugging, progress, known issues, decision records, recon report, Eden baseline |
| `third_party/musl/` | musl 1.1.19's math functions (MIT) |
| `work/`, `build/` | Generated output from your own game files (gitignored, never committed) |

## Credits

- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** - the model this project follows: ship the tools, let
  every player recompile their own copy.
- **[musl](https://musl.libc.org/)** - the math library (`third_party/musl`, MIT).
- **[SDL3](https://www.libsdl.org/)** and the **[Vulkan](https://www.vulkan.org/)** headers.
- **Eden** (a Switch emulator) - used only as a visual and performance reference; none of its code is used.
- **LEVEL-5** for Yo-kai Watch 4. This project is not affiliated with or endorsed by LEVEL-5 or Nintendo.
