# YW4 Recomp

A fan static-recompilation project to run Yo-kai Watch 4++ (Nintendo Switch, final update) natively on Windows and Linux, in the style of N64Recomp.

This repo contains only tools, runtime code, and patches. It contains no game data. Provide your own dump of the final 4++ update.

Status: Phase 0 (recon and tooling). Nothing runs yet.

## Setup

Requirements: Python 3.11+, Git. CMake 3.24+ and a C++20 compiler come later (Phase 1+).

1. Copy `config.example.toml` to `config.local.toml`.
2. Fill in `exefs_dir` and `romfs_dir` with the folders of your already-extracted ExeFS and RomFS.
3. Keep `work_dir` outside any cloud-synced folder.
4. Check it:

   ```
   python tools/yw4/config.py
   ```

   It prints the resolved paths, or a clear error saying what is missing.

## Layout

| Path | What |
|---|---|
| `tools/` | Python analysis and input-pipeline scripts |
| `runtime/` | Native runtime (Phase 1+) |
| `recomp/` | AArch64 → C recompiler (Phase 2+) |
| `docs/` | Recon report, baseline, decision records |
| `work/`, `build/` | Generated output (gitignored) |

See `docs/decisions/` for why things are set up this way.
