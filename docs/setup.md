# Setup and building

Everything here needs your own dump of **Yo-kai Watch 4++, update 2.2.0** (the final 4++ update). The repo contains
no game data and no recompiler output; the tools refuse to run on any other build (`target.toml` pins the SHA-256 of
the decompressed `main`).

## Requirements

- Python 3.11+ and Git.
- A C11 compiler: **Clang 19 or newer** preferred ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw) on Windows;
  set `[tools] cc` in your config), or GCC 15 or newer. Older ones stop at `#embed` (the built-in fonts,
  `runtime/src/ui.c`: "invalid preprocessing directive"). The host CPU needs FMA3 (any x86-64 CPU since 2013).
- A GPU with Vulkan 1.3. The [Vulkan SDK](https://vulkan.lunarg.com/) provides `glslangValidator` for the shader
  step and the validation layer for debugging.
- SDL3 for the window: `recomp.build` fetches it on Windows (`python -m tools.yw4.deps`, pinned and hash-checked);
  on Linux install it from your distribution (`pkg-config sdl3` must find it). Where there is no package (Ubuntu
  24.04), build [SDL3](https://github.com/libsdl-org/SDL/releases) from source with CMake into a folder of your
  own, then build and run with `PKG_CONFIG_PATH=<folder>/lib/pkgconfig` and `LD_LIBRARY_PATH=<folder>/lib`.
- Linux is new (first built and started 2026-11-12, Ubuntu 24.04 under WSL with Clang 19, software rendering): it
  compiles, links and draws the first screens. Not yet tried there: a real GPU, audio, controllers. AMD FSR,
  NVIDIA DLSS, frame generation and Reflex are Windows only: their rows in the settings are locked with "Not
  supported on Linux yet", and anti-aliasing offers FXAA, SMAA and TAA.
- On Windows the first build also downloads AMD FSR (upscaling, frame generation), NVIDIA DLSS and NVIDIA
  Streamline (DLSS frame generation, Reflex): about 550 MB in all, once, each checked against a pinned SHA-256
  and kept in `work/deps/`. Later builds are offline. Without internet, download the archive yourself and put it
  where the build looks for it, for example `work/deps/fsr-2.4.0-dl/FidelityFX-SDK-v2.4.0.zip` or
  `work/deps/streamline-2.14.16-dl/streamline-sdk-v2.14.16.zip` (URLs and hashes in `tools/yw4/deps.py`); the
  hash is checked either way. NVIDIA's files are under NVIDIA's own licence, so they are never committed.
- Disk space: an install takes ~11.3 GB uncleaned, or ~9.3 GB after "Free up space" (reclaims ~2.0 GB). Peak disk
  space during building is ~11.4 GB when copying from folders, or ~20.7 GB when extracting from NSP/XCI (due to
  temporary NCAs). Building the launcher itself from source takes ~10.3 GB in `launcher/src-tauri/target/`. Details
  and folder breakdown in [disk-usage.md](disk-usage.md).

## One-time setup

0. Install the game-data guard as a pre-commit hook: `python -m tools.yw4.guard --install`.
1. Copy `config.example.toml` to `config.local.toml` (gitignored).
2. Fill in `exefs_dir` and `romfs_dir` with the folders of your already-extracted ExeFS and RomFS.
   If you have your base game and update as files instead, also set `base_nsp`, `update_nsp`, `hactool_exe`
   ([hactool](https://github.com/SciresM/hactool) 1.4.0) and `keys_dir`, point `exefs_dir`/`romfs_dir` at empty
   folders, and run `python -m tools.yw4.extract`. `base_nsp` may be an NSP or an XCI (a game-card image); leave
   `update_nsp` empty when your XCI already holds the 2.2.0 update. An XCI alone needs only `prod.keys`; an NSP
   also needs `title.keys`.
   Add-on content (DLC): with `dlc_dir` set, the same run unpacks each recognised add-on into
   `<romfs_dir>/../dlc-romfs/<index>/`. To unpack add-ons again later (new DLC files, or after an update that
   recognises more of them), run `python -m tools.yw4.extract --dlc-only`: it leaves the base game alone, needs no
   emptied folders, and skips add-ons already unpacked. A plain re-run stops when `exefs_dir` or `romfs_dir` holds
   files.
3. Keep `work_dir` outside any cloud-synced folder.
4. Check it: `python tools/yw4/config.py` prints the resolved paths, or a clear error saying what is missing.
5. Decompress and verify the ExeFS modules: `python -m tools.yw4.ingest`. It refuses to continue unless `main`
   matches the build pinned in `target.toml`. Outputs go to `<work_dir>/exefs/`.
6. Optional, Windows: the game's icon for the exe, `python -m tools.yw4.icon` (needs `base_nsp` or `update_nsp`,
   `hactool_exe` and `keys_dir`; `tools.yw4.extract` already does it). Extracted folders alone hold no icon.
7. Optional: measure the binary and assets with `python -m tools.yw4.recon` (tables in `<work_dir>/recon/`, summary
   in [recon.md](recon.md)).

## Build and run

```
python -m recomp.translate      # main and sdk -> <work_dir>/recomp/ (C, never committed; ~30 s on 16 threads)
python -m tools.yw4.shaders     # pull the game's shader assembly out -> <work_dir>/shaders/asm/
python -m recomp.shader --check # translate to GLSL, compile to SPIR-V (needs [tools] glslang)
python -m recomp.build --run    # build <work_dir>/build/yw4.exe and run it
```

A full build after `translate` takes a few minutes; later builds only recompile what changed (seconds). The build
cannot relink while `yw4.exe` is running. Details on the recompiler are in [recomp/README.md](../recomp/README.md).

For the launcher ([decision 0050](decisions/0050-launcher-and-install-layout.md)): with `YW4_PROGRESS=1` the
steps (`extract`, `ingest`, `translate`, `shader`, `build`) also print `@progress <step> <done> <total> <what>`
lines as they work: bytes unpacked, bytes extracted (measured against `target.toml` `[extracted]`, as hactool
itself says nothing while it works), functions translated, shaders, files compiled; ingest, icon and link report
`0 1` at their start and `1 1` at their end. See `tools/yw4/progress.py`.

### Build options

On Windows, `python -m recomp.build` takes `--no-fsr` and `--no-dlss` to leave out the upscaling and frame
generation libraries. `--no-fsr` leaves out AMD FSR (upscaling and frame generation); `--no-dlss` leaves out NVIDIA
DLSS (upscaling) and NVIDIA Streamline (DLSS frame generation and Reflex). The build then skips their download, leaves
their DLLs out of `work/build/`, and the settings panel greys out what needs them ("NOT IN THIS BUILD"). Switching
the flags recompiles only the runtime's own files.

Run from the repo root: shaders are looked up under `work/shaders/`. Directly:

```
./work/build/yw4.exe work/exefs <your romfs folder> work/save
```

Saves go to the folder given last (`work/save` above). Keyboard and gamepad both work; F12 saves a screenshot.
Runtime options and debugging tools are in [debugging.md](debugging.md).

## Tests

Synthetic data only, no game files needed: `python -m pytest tests -q` (or `python -m unittest discover -s tests -t .`).

`python -m recomp.ci` compiles everything that needs no game files (the runtime, musl's math, our shaders) with the
build's own flags, without a config: a quick "does it still compile here" before a push. GitHub runs it and the
tests on Linux and on Windows, and builds the launcher, on every push, tag and pull request
(`.github/workflows/build.yml`). It can't check the game itself: translating, linking and running it needs your
own game files, which are never on GitHub.
Instruction forms of the recompiler are checked against a real ARM64 CPU (a Raspberry Pi 5,
[decision 0004](decisions/0004-pi5-as-arm64-reference.md)).
