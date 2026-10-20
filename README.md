# YW4 Recomp

A fan static-recompilation project to run Yo-kai Watch 4++ (Nintendo Switch, final update) natively on Windows and Linux, in the style of N64Recomp.

This repo contains only tools, runtime code, and patches. It contains no game data. Provide your own dump of the final 4++ update.

Status: Phase 2 (recompiler). All of `main` translates to C and compiles; nothing runs yet.

## Setup

Requirements: Python 3.11+, Git. CMake 3.24+ and a C++20 compiler come later (Phase 1+).

0. Install the game-data guard as a pre-commit hook: `python -m tools.yw4.guard --install`.
1. Copy `config.example.toml` to `config.local.toml`.
2. Fill in `exefs_dir` and `romfs_dir` with the folders of your already-extracted ExeFS and RomFS.
   If you have your base and update NSPs instead, also set `base_nsp`, `update_nsp`, `hactool_exe`
   ([hactool](https://github.com/SciresM/hactool) 1.4.0) and `keys_dir` (your console's `prod.keys` and
   `title.keys`), point `exefs_dir`/`romfs_dir` at empty folders, and run `python -m tools.yw4.extract`.
3. Keep `work_dir` outside any cloud-synced folder.
4. Check it:

   ```
   python tools/yw4/config.py
   ```

   It prints the resolved paths, or a clear error saying what is missing.

5. Decompress and verify the ExeFS modules:

   ```
   python -m tools.yw4.ingest
   ```

   It refuses to continue unless `main` matches the build pinned in `target.toml`. Outputs go to `<work_dir>/exefs/`.

6. Measure the binary and assets (tables go to `<work_dir>/recon/`, summary in `docs/recon.md`):

   ```
   python -m tools.yw4.recon
   ```

Tests (synthetic data only): `python -m unittest discover -s tests -t .`

## Layout

| Path | What |
|---|---|
| `tools/` | Python analysis and input-pipeline scripts |
| `runtime/` | Native runtime (Phase 1+) |
| `recomp/` | AArch64 → C recompiler (Phase 2+) |
| `docs/` | Recon report, baseline, decision records |
| `work/`, `build/` | Generated output (gitignored) |

See `docs/decisions/` for why things are set up this way.
