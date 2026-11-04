# 0052 — Packaging the launcher: what the download holds and what it fetches

Status: **accepted** (2026-11-04, the owner's answers below); nothing of it is built yet. Follows decision 0050 (one
download, nothing else to install, the build stays this repo's Python tools run by an embedded Python).

## Context

Today the launcher borrows everything from the repo it was built in (`buildrun.rs`): `python` from the PATH, the
repo's `tools/`, `recomp/` and `runtime/`, and the compiler, `glslang` and `hactool` paths from the owner's
`config.local.toml`. On another PC none of that exists. Measured on the owner's PC (2026-11-04):

| Needed by the build | Size | Licence | Used for |
|---|---|---|---|
| Python 3.11+ (3.13 here) | embeddable zip ~10 MB | PSF | every build step |
| this repo's `tools/`, `recomp/`, `runtime/` | ~3 MB | MIT (ours) | the steps themselves |
| llvm-mingw (clang, lld, mingw-w64) | 737 MB unpacked; the x86-64 parts only (bin, include, lib, x86_64-w64-mingw32) ~480 MB | Apache-2.0 with LLVM exception; mingw-w64 permissive | compiling the game |
| glslang (`glslang.exe` only) | 4.9 MB | BSD-3 / MIT / Apache-2.0 mix | the shaders |
| hactool | 1.6 MB | ISC | unpacking NSP/XCI (not for extracted folders) |
| SDL3, FSR, DLSS, Streamline | ~565 MB download (`tools/yw4/deps.py`, already pinned by SHA-256) | zlib, MIT, NVIDIA RTX SDK | already fetched by the build |

The build path uses only Python's standard library (`numpy` and `PIL` appear only in dev tools: `capview.py`,
`gaiji.py`).

## Proposal

**The installer carries the small things; the build fetches the big one.**

1. **In the download** (Tauri's NSIS installer, about 25 MB): the launcher, the embeddable Python (unpacked next
   to it, never on the PATH), the repo's `tools/`, `recomp/`, `runtime/` at the release tag, `glslang.exe`,
   `hactool.exe`, and the licence files of each. The launcher runs `<install>\builder\python\python.exe` with
   the builder folder as the working directory instead of the repo.
2. **Fetched on the first build**, as `deps.py` already does for SDL/FSR/DLSS: llvm-mingw's official release zip
   at a pinned version and SHA-256, unpacked into `<install>\builder\toolchain\`. The Build screen already shows
   a download line ("About 565 MB"), which grows by the toolchain's size. **Decided (owner):** the player can
   delete it afterwards to save space: "Free up space" (the Ready screen's popup and the Options row "Free up
   space after building") also deletes `builder\toolchain\`, and says so; the next build (an update, Build
   again) downloads it again, checked against the same pinned hash. The game never needs it to run.
3. **`config.toml` for the install** (`write_config`) names the bundled tools instead of the repo's `[tools]`.
4. **Version lock stays as it is**: the main NSO hash check refuses any other game version (rule 3).

Why fetch the compiler instead of bundling it: it is 20 times the rest together, and its official zip is already
pinned and checked the same way as the other downloads, so nothing new is trusted. Why bundle Python and the
two small tools: without them the launcher can't even start the download step, and they are small.

## Alternatives

- **Bundle everything** (~250-300 MB installer): works offline, but every launcher update re-downloads the
  compiler, and the download page carries an LLVM toolchain.
- **Prune llvm-mingw to x86-64** (~480 MB → maybe ~250 MB after dropping unused tools from `bin`): smaller, but
  a hand-made subset is fragile across llvm-mingw releases. Only worth it if the download size matters.
- **No Python: port the build steps to Rust**: rejected in 0050 (rewrites working, tested code).

## Open questions for the owner

1. ~~Fetch the compiler on first build or bundle it?~~ Fetch on the first build, deletable after (owner, 2026-11-04).
2. ~~NSIS installer or a plain zip?~~ **Both** (owner): Tauri's NSIS installer (Start menu entry, uninstaller) and a
   portable zip of the same files, side by side on each release.
3. ~~Where releases live, update check?~~ **GitHub Releases on this repo, and the launcher checks for a newer
   version** (owner) and offers it; the check reads the repo's latest release (GitHub's API, no account needed) and
   is the launcher's only network call besides the build's pinned downloads.
4. Pin llvm-mingw at the version used here (20261006, ucrt, x86_64) so every player builds with the compiler the
   owner tested.

## Then

Implement, then the clean-PC (VM) test from the shipping plan: a fresh Windows with nothing installed, the
installer, the owner's own files, Build, Play.
