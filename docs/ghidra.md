# Ghidra setup

How we load `main` in Ghidra, both by hand (GUI) and from scripts (headless).

## Versions

| Tool | Version | Why this one |
|---|---|---|
| Ghidra | **12.1.2** | The Switch loader is built for exactly this version. Ghidra refuses extensions built for any other version, so don't upgrade Ghidra until the loader has a matching release. |
| [Ghidra-Switch-Loader](https://github.com/Adubbz/Ghidra-Switch-Loader) | 1.6.1-c600d05 (for Ghidra 12.1.2) | Loads NSO/NRO/KIP directly: decompresses segments, applies relocations, imports `.dynsym` names. |
| JDK | 21 | Ghidra 12 needs a full JDK 21, not just a JRE (`javac -version` must work). |

Both are user-installed tools (Apache 2.0 and ISC licenses). We only run them and never copy their code.

## Install

1. Unzip Ghidra 12.1.2 anywhere outside the repo.
2. Unzip the Switch loader release into `<ghidra>/Ghidra/Extensions/`, so you get `<ghidra>/Ghidra/Extensions/SwitchLoader/extension.properties`. Installing there (not through the GUI's per-user extension manager) makes it available to headless runs too.
3. Set `[tools] ghidra_dir` in `config.local.toml` to the Ghidra folder.

## Headless analysis (scripted)

```
python -m tools.yw4.ghidra            # import + full auto-analysis + export (first run is long)
python -m tools.yw4.ghidra --export   # only re-export from the existing project
```

- Refuses to run unless `main` matches `target.toml`.
- Project: `<work_dir>/ghidra/yw4.gpr`. Log: `<work_dir>/ghidra/analyze.log`.
- Heap defaults to 12 GB. Set `GHIDRA_HEADLESS_MAXMEM` to change it. Ghidra's own default of 2 GB is too small for a 17 MB `.text`.
- Output: `<work_dir>/ghidra/main.functions.json`, from `tools/ghidra/ExportFunctions.java`. One entry per function: `entry` and `size` as offsets from the image base (they match offsets in `work/exefs/main.bin`), `name`, `thunk`, and `ranges` when the body isn't contiguous.

**The project database contains the game's code.** It lives in `work/` (gitignored) and must never be committed or shared (CLAUDE.md rule 1). `.gitignore` also blocks `*.gpr` / `*.rep` anywhere.

## Opening it in the GUI

1. Run `<ghidra>/ghidraRun.bat`.
2. **File → Open Project →** `<work_dir>/ghidra/yw4.gpr`, then open `main`. Analysis is already done.

To start fresh instead: create a project, **File → Import File →** your ExeFS `main`. The format should show as "Nintendo Switch Binary". Accept the default analyzers.

## Cross-checking Ghidra against our own parsers

`docs/recon.md` already has two independent function lists: 39,094 starts from `.eh_frame_hdr`, and 17,853 address-taken targets from relocations. Ghidra's function list should contain almost all of both. Big gaps mean either Ghidra missed functions (common after indirect jumps) or our parsing is wrong, so the comparison is worth running whenever the export changes.
