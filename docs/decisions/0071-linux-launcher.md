# 0071 — The launcher on Linux: one source, the distribution's tools, an AppImage

<!-- decision
state: accepted
date: 2026-11-14
replaces: none
code: launcher/src-tauri/src/platform.rs, launcher/src-tauri/tauri.linux.conf.json, launcher/package.py, launcher/ui/app.js, tools/yw4/system.py, tools/yw4/build_install.py, recomp/build.py
-->

Status: accepted. Built and checked in WSL (Ubuntu 24.04, software rendering); since then players use the AppImage on
their own Linux desktops (owner, 2026-11-17).

## Context

The launcher (decision 0050) was Windows only, though nothing in Tauri is: built in WSL as it was, it compiled, its
tests passed and its screens drew, with no change. What did not work was around it: about 40 places named a Windows
program or folder (`explorer`, `powershell`, `taskkill`, `tasklist`, `%APPDATA%`, `System32\curl.exe` and `tar.exe`,
`yw4.exe`, `python`), and the download (decision 0052) ships Windows programs: an embeddable Python, glslang,
hactool and the llvm-mingw compiler it fetches.

## Decision

**One source, the differences in one file.** `launcher/src-tauri/src/platform.rs` holds every Windows / Linux
difference; the rest of the launcher calls it and names no system program itself. On Windows each function does
what the launcher did before, so the Windows launcher behaves the same.

| | Windows | Linux |
|---|---|---|
| The launcher's records, "appdata" saves | `%APPDATA%\YW4 Recomp` | `$XDG_DATA_HOME/YW4Recomp` (`~/.local/share/YW4Recomp`), the game's own default |
| Links, folders | `explorer` | `xdg-open` |
| Cancel a build | `taskkill /T` | the step's own process group, killed |
| Graphics card names | `Win32_VideoController` | `vulkaninfo --summary`, else `lspci` |
| Shortcuts | `.lnk` on the desktop and in the Start menu | `.desktop` in the desktop folder and `~/.local/share/applications` |
| Free space | `GetDiskFreeSpaceExW` | `df` |
| Unpack a game update | `tar.exe` (bsdtar) | Python's `zipfile` (GNU tar reads no zip) |
| A newer launcher | its installer, run | its release page, as for the portable zip |

**The distribution's build tools, checked first.** On Linux the download ships no Python and no compiler. A build
uses the system's Python 3.11+, Clang 19+ (or GCC 15+), glslang and pkg-config, as README.md's "Building on Linux"
already asks. `tools/yw4/system.py` runs as the build's first step ("Checking the build tools"). It picks the newest
compiler that has `#embed` and stops with one message naming everything missing and the install line for Arch,
Fedora and Debian/Ubuntu. That check is in the game's code, not the launcher's, so a game update can change it.

Why not ship them: a compiler or Python built for one distribution's C library breaks on others, and every
distribution packages these four. The Windows reason for shipping them (decision 0052: nothing to install) is met
on Linux by one package-manager line.

**Shipped anyway: hactool and SDL3.** hactool has no Linux release, so `package.py` builds it from its pinned
1.4.0 source (it links only libc). SDL3 is missing from Ubuntu 24.04, the most used distribution. The download
carries an SDL3 build that loads its X11, Wayland and audio parts at run time, so one build works across
distributions, the way Steam ships it. The build uses the system's SDL3 when there is one; `recomp/build.py` copies
an SDL3 from outside the system folders next to `yw4` and links with run path `$ORIGIN`.

**An AppImage.** One file that runs on any distribution with WebKitGTK's libraries bundled. Tauri builds it
(`tauri.linux.conf.json`). That file also turns the window's see-through Mica backdrop off, which only Windows has.
`package.py` on Linux builds with cargo's Tauri CLI into a target folder of its own, never the repo's `target/` or
`node_modules`, which hold the Windows build's files.

**The screens.** The page sees Linux in WebKitGTK's user agent. It leaves out what only Windows has (the FSR and
DLSS options, their mentions, the DLSS slide), says "App menu entry" and "In your home folder", and suggests
`~/Games/YW4 Recomp`. Since [decision 0073](0073-dlss-fsr-on-linux.md) the DLSS and FSR options are back on Linux,
as upscaling only; only frame generation is left out.

## Found on the way

The launcher's "Find it" (an existing install) wrote `%APPDATA%/YW4 Recomp/save` into a new `game.ini` for
"appdata" saves. The game expands no `%APPDATA%` (`runtime/src/game_ini.c`), so it read a folder literally named
`%APPDATA%`. It now writes the real folder, on both systems.

## Issue #13: the AppImage's environment reached the build (2026-11-15)

A player on AOSC OS built the launcher. Two problems, one after the other:

1. **Bundling:** `cargo tauri build` stopped with only "failed to run linuxdeploy". Their `cargo tauri` was 2.10.1,
   older than the 2.12.4 we use (pinned in `package.json` for Windows; for Linux only a docstring said so). With 2.12.4
   it bundled. `package.py` now checks `cargo tauri --version` against `package.json`'s pin before it starts, and stops
   with the `cargo install tauri-cli --version 2.12.4 --locked` line instead.
2. **Running:** in the AppImage, BUILD failed at 0%: the system's `python3` stopped with "No module named 'encodings'",
   `PYTHONHOME` and `PYTHONPATH` pointing into the AppImage's mount. The binary run on its own worked. The AppImage's
   AppRun (AppImageKit's, which Tauri bundles) sets those for the launcher, with PATH, LD_LIBRARY_PATH, XDG_DATA_DIRS
   and the GTK / GStreamer / Qt module paths, and every program the launcher started inherited them: Python first, but
   also the compiler, the game, curl and xdg-open (the player's browser), which could load the AppImage's libraries.

   Fix: `platform::command`, used for every program the launcher starts (no `Command::new` left elsewhere). Inside an
   AppImage (APPDIR set) it gives the child the launcher's environment with every entry inside the mount left out of
   its variable's `:` list, the variable removed when nothing else is left, and AppRun's `PYTHONDONTWRITEBYTECODE`
   removed. The launcher keeps its own (its web view needs it). Nothing the builder starts needed the AppImage's
   libraries: hactool links only libc, and SDL3 is found next to the game through its run path. A Rust test feeds it
   the variables from the player's log.

   Never caught before: the AppImage was built in WSL but never started (Not done, below).

## Not done

- Tried only in WSL with software rendering: no real GPU, desktop, file manager or controller.
- hactool and SDL3 are built against the packager's glibc (Ubuntu 24.04: 2.39); distributions older than that would
  need them built on an older system.
- A Linux release (publishing the AppImage) is the owner's call.
