# Setup and building

Everything here needs your own dump of **Yo-kai Watch 4++, update 2.2.0** (the final 4++ update). The repo contains
no game data and no recompiler output; the tools refuse to run on any other build (`target.toml` pins the SHA-256 of
the decompressed `main`).

## Requirements

- Python 3.11+ and Git.
- A C11 compiler: Clang preferred ([llvm-mingw](https://github.com/mstorsjo/llvm-mingw) on Windows; set `[tools] cc`
  in your config), GCC works too. The host CPU needs FMA3 (any x86-64 CPU since 2013).
- A GPU with Vulkan 1.3. The [Vulkan SDK](https://vulkan.lunarg.com/) provides `glslangValidator` for the shader
  step and the validation layer for debugging.
- SDL3 for the window: `recomp.build` fetches it on Windows (`python -m tools.yw4.deps`, pinned and hash-checked);
  on Linux install it from your distribution (`pkg-config sdl3`).
- On Windows the first build also downloads AMD FSR (upscaling, frame generation), NVIDIA DLSS and NVIDIA
  Streamline (DLSS frame generation, Reflex): about 550 MB in all, once, each checked against a pinned SHA-256
  and kept in `work/deps/`. Later builds are offline. Without internet, download the archive yourself and put it
  where the build looks for it, for example `work/deps/fsr-2.4.0-dl/FidelityFX-SDK-v2.4.0.zip` or
  `work/deps/streamline-2.14.16-dl/streamline-sdk-v2.14.16.zip` (URLs and hashes in `tools/yw4/deps.py`); the
  hash is checked either way. NVIDIA's files are under NVIDIA's own licence, so they are never committed.

## One-time setup

0. Install the game-data guard as a pre-commit hook: `python -m tools.yw4.guard --install`.
1. Copy `config.example.toml` to `config.local.toml` (gitignored).
2. Fill in `exefs_dir` and `romfs_dir` with the folders of your already-extracted ExeFS and RomFS.
   If you have your base and update NSPs instead, also set `base_nsp`, `update_nsp`, `hactool_exe`
   ([hactool](https://github.com/SciresM/hactool) 1.4.0) and `keys_dir`, point `exefs_dir`/`romfs_dir` at empty
   folders, and run `python -m tools.yw4.extract`.
3. Keep `work_dir` outside any cloud-synced folder.
4. Check it: `python tools/yw4/config.py` prints the resolved paths, or a clear error saying what is missing.
5. Decompress and verify the ExeFS modules: `python -m tools.yw4.ingest`. It refuses to continue unless `main`
   matches the build pinned in `target.toml`. Outputs go to `<work_dir>/exefs/`.
6. Optional: measure the binary and assets with `python -m tools.yw4.recon` (tables in `<work_dir>/recon/`, summary
   in [recon.md](recon.md)).

## Build and run

```
python -m recomp.translate      # main and sdk -> <work_dir>/recomp/ (C, never committed; ~1 min)
python -m tools.yw4.shaders     # pull the game's shader assembly out -> <work_dir>/shaders/asm/
python -m recomp.shader --check # translate to GLSL, compile to SPIR-V (needs [tools] glslang)
python -m recomp.build --run    # build <work_dir>/build/yw4.exe and run it
```

A full build after `translate` takes a few minutes; later builds only recompile what changed (seconds). The build
cannot relink while `yw4.exe` is running. Details on the recompiler are in [recomp/README.md](../recomp/README.md).

Run from the repo root: shaders are looked up under `work/shaders/`. Directly:

```
./work/build/yw4.exe work/exefs <your romfs folder> work/save
```

Saves go to the folder given last (`work/save` above). Keyboard and gamepad both work; F12 saves a screenshot.
Runtime options and debugging tools are in [debugging.md](debugging.md).

## Tests

Synthetic data only, no game files needed: `python -m pytest tests -q` (or `python -m unittest discover -s tests -t .`).
Instruction forms of the recompiler are checked against a real ARM64 CPU (a Raspberry Pi 5,
[decision 0004](decisions/0004-pi5-as-arm64-reference.md)).
