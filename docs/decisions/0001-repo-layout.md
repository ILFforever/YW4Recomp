# 0001 — Repo layout and keeping game data out

Date: 2026-10-14
Status: accepted

## Decision

- `tools/` — Python analysis and input-pipeline scripts (Phase 0). Python because these are one-shot binary-parsing jobs where iteration speed matters more than speed of execution. Stdlib only for now (`tomllib` needs Python 3.11+).
- `runtime/`, `recomp/` — C/C++ built with CMake. Empty until Phases 1–2.
- `docs/` — recon reports, baseline numbers, and these decision records.
- `work/`, `build/` — all generated output. Gitignored.
- `config.local.toml` — the only place paths to user-supplied files live. Gitignored. `config.example.toml` is the committed template.

## Why

- Rule 1 (no game data) is enforced by `.gitignore`, not by memory. It blocks dump/container formats, NSO/ELF, the extensionless ExeFS module names (`main`, `rtld`, `sdk`, `subsdk*`), keys, firmware, and Ghidra/IDA databases. Reverse-engineering databases are on the list because they embed the game's code.
- The ExeFS names are ignored everywhere, not just in `exefs/`. Cost: a source folder can't be called `sdk` or `main` without a `!` exception. That is cheaper than one accidental commit of `main`.
- `**/generated/` and `work/` cover recompiler output, which is derived from the game's code.
- In-source CMake builds are refused so build output can't mix with tracked files.

## Known risk

The repo path is under `OneDrive\Desktop`, but OneDrive sync is turned off on the owner's machine (confirmed 2026-10-14), so nothing is uploaded. If sync is ever turned back on, `work/` (decompressed ELF, RomFS extracts, generated C) would be uploaded. `tools/yw4/config.py` prints a warning when `work_dir` is under a cloud-folder path as a reminder; it can't tell whether sync is actually on.
