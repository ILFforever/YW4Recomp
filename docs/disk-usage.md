# Disk space usage and build sizes

Measured disk usage for the launcher, the game installation, and intermediate build artifacts (measured 2026-11-10
during a clean build to drive `W:\YW4Clean` with `YW4 Recomp.exe` 0.6.0 on Windows 11, AMD Ryzen 9 7940HS).

## Summary

| Build stage / mode | Disk space | Notes |
|---|---|---|
| **Clean install (after build, folders mode)** | **11.28 GB** (11.35 GB drive delta) | Copied RomFS (8.46 GB), compiler (0.70 GB), dependencies (0.65 GB), generated C (0.49 GB), build output (0.43 GB), DLC (0.44 GB) |
| **Peak during build (folders mode)** | **11.35 GB** | Peak reached near the end of compiling and DLC extraction |
| **Peak during build (NSP/XCI mode)** | **~20.7 GB** | During extraction, `work/nca/` holds 9.40 GB of temporary NCAs until RomFS is fully extracted |
| **After "Free up space"** | **9.27 GB** (2.00 GB freed) | Deletes `toolchain`, `recomp/`, `build/obj/`, `deps/`, and `shaders/asm/` |
| **Building the launcher itself from source** | **~10.3 GB** | Cargo build in `launcher/src-tauri/target/` (10.24 GB) + `node_modules` (15 MB) |

## Breakdown of the game install folder (`<install>/`)

Measured after a complete clean build into `W:\YW4Clean` before "Free up space":

| Folder / file | Size | Kept after "Free up space"? | Purpose |
|---|---|---|---|
| `data/romfs/` | 8.458 GB (9,081.5 MB) | Yes | The game's RomFS (update 2.2.0 applied) |
| `builder/toolchain/` | 0.699 GB (750.1 MB) | No (deleted) | Downloaded llvm-mingw compiler (`clang.exe`, libraries) |
| `work/deps/` | 0.654 GB (702.6 MB) | No (deleted) | Downloaded SDL3, FSR, DLSS, and Streamline SDK archives and unpacked headers/libs |
| `work/recomp/` | 0.489 GB (525.2 MB) | No (deleted) | Recompiled C source code generated from `main` and `sdk` |
| `work/build/` | 0.430 GB (461.7 MB) | Yes (except `obj/`) | `yw4.exe`, runtime DLLs, and `obj/` (153 MB of compiled object files) |
| `data/dlc/` | 0.219 GB (234.7 MB) | Yes | Copied add-on NSP files |
| `data/dlc-romfs/` | 0.216 GB (232.2 MB) | Yes | Extracted RomFS for the 4++ Upgrade Pack (index 100) |
| `work/shaders/` | 0.094 GB (101.2 MB) | Yes (except `asm/`) | Translated GLSL 4.60 shaders and compiled SPIR-V binaries |
| `data/exefs/` | 0.017 GB (17.9 MB) | Yes | Extracted ExeFS binaries (`main`, `sdk`, `rtld`) |
| `app/` (launcher app itself) | 0.039 GB (41.4 MB) | Yes | The launcher executable, embedded Python, and bundled tools |
| `work/pycache/` | < 1 MB | Yes | Python bytecode cache |
| **Total** | **11.276 GB** (12,107.5 MB) | **9.273 GB** after cleanup | Total installation size |

## Reclaimable disk space ("Free up space")

The Ready screen and Settings offer "Free up space" (`ready::free_space`), which deletes folders that only a full rebuild needs (`reclaim_dirs`):

* `builder/toolchain/`: 750.1 MB
* `work/deps/`: 702.6 MB
* `work/recomp/`: 525.2 MB
* `work/build/obj/`: 153.2 MB
* `work/shaders/asm/`: ~19 MB
* `work/nca/`: cleaned during extraction unless `--keep-nca` was used

**Total space reclaimed:** **2.003 GB** (`2,150.5 MB`).
The game remains fully playable from `<install>/work/build/yw4.exe`, taking **9.27 GB**. Future updates will re-download the compiler and dependencies as needed.

## Building from NSP vs Extracted Folders

The launcher supports two input modes:

1. **Folders mode (`mode: folders`)**:
   Copies already-extracted RomFS (8.46 GB) and ExeFS (16.6 MB).
   * Peak disk usage equals the final uncleaned build size: **11.35 GB**.
   * Build duration on a 16-thread Ryzen 9 7940HS: **~8.8 minutes**.

2. **Files mode (`mode: files`)**:
   Extracts `base_nsp` and `update_nsp` via `hactool.exe`.
   * Unpacks NCAs into `work/nca/`: the base program NCA (5.56 GB) and update NCA (3.84 GB), taking **9.40 GB**.
   * `hactool` extracts `data/romfs/` (8.46 GB) while both NCAs are still on disk.
   * **Peak disk space during extraction:** **~20.7 GB** (9.40 GB NCAs + 8.46 GB RomFS + ~2.8 GB toolchain/build/DLC).
   * Once extraction completes, `work/nca/` is deleted, dropping disk usage back to 11.28 GB.

## Developing and building the launcher from source

For developers working on `launcher/`:
* `launcher/src-tauri/target/`: **10.24 GB** (Rust debug and release builds, intermediate object files and dependencies).
* `launcher/node_modules/`: **15.4 MB** (Tauri CLI npm package).
* Output packages (`work/package/dist/`):
  * `YW4-Recomp-Launcher-0.6.0-setup.exe`: **14.5 MB**
  * `YW4-Recomp-Launcher-0.6.0-portable.zip`: **17.8 MB**
