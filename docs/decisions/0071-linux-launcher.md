# 0071 — The launcher on Linux: one source, the distribution's tools, an AppImage

<!-- decision
state: accepted
date: 2026-11-14
replaces: none
code: launcher/src-tauri/src/platform.rs, launcher/src-tauri/tauri.linux.conf.json, launcher/package.py, launcher/ui/app.js, tools/yw4/system.py, tools/yw4/build_install.py, recomp/build.py
-->

Status: accepted. Built and checked in WSL (Ubuntu 24.04, software rendering); not yet on a real Linux desktop.

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
`~/Games/YW4 Recomp`.

## Found on the way

The launcher's "Find it" (an existing install) wrote `%APPDATA%/YW4 Recomp/save` into a new `game.ini` for
"appdata" saves. The game expands no `%APPDATA%` (`runtime/src/game_ini.c`), so it read a folder literally named
`%APPDATA%`. It now writes the real folder, on both systems.

## Not done

- Tried only in WSL with software rendering: no real GPU, desktop, file manager or controller.
- hactool and SDL3 are built against the packager's glibc (Ubuntu 24.04: 2.39); distributions older than that would
  need them built on an older system.
- A Linux release (publishing the AppImage) is the owner's call.
