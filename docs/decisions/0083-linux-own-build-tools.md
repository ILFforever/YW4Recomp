# 0083 — Linux: our own compiler and glslang where the system has none

<!-- decision
state: accepted
date: 2026-11-19
replaces: none
code: tools/yw4/system.py, tools/yw4/deps.py, tools/yw4/build_install.py, tools/yw4/fsr_linux.py, recomp/build.py
-->

Status: accepted. Changes one part of [decision 0071](0071-linux-launcher.md): "the distribution's build tools".
Built in WSL (Ubuntu 24.04) and in a bare Fedora 45 container (below); not yet tried on a real Bazzite or SteamOS
PC.

## Context

Decision 0071 left the compiler, glslang, pkg-config and SDL3 to the distribution: one package-manager line, which
`tools/yw4/system.py` printed when something was missing. A player on Bazzite (2026-11-19, NVIDIA edition) got
that message and could not follow it: Bazzite, like SteamOS, keeps the system read-only, so `dnf install` doesn't
work, and the workarounds (`rpm-ostree install` and a reboot, or a distrobox) are not what a launcher's one Build
button promises. These are the distributions gamers use most, so 0071's reason for not shipping the tools ("met on
Linux by one package-manager line") doesn't hold for them.

## Decision

**What the system lacks is downloaded, as on Windows (decision 0052).** The build's first step
(`system.check`) uses the system's compiler and glslang when it has them, exactly as before, and downloads only
what is missing into the build's `deps/linux-tools/` (which the launcher's Free up space deletes):

| Tool | Where from | Why |
|---|---|---|
| C/C++ compiler | Zig 0.17.1 (`ziglang.org`, pinned SHA-256; MIT) | One archive with Clang 22 and the C library's headers for every glibc version: it builds on a system with no compiler and no headers installed. 57 MB download |
| glslang | Khronos' 16.6.0 Linux release (pinned SHA-256) | The version the Windows download already ships. Needs only libstdc++.so.6 |
| SDL3 | The launcher's own copy (decision 0071), now read directly | No pkg-config needed |
| FSR's patches | Applied in Python (`fsr_linux.apply_patch`) | No `git` or `patch` needed. Output byte-identical to `git apply` (checked on the SDK's source) |
| Python | The system's | Every desktop distribution has it, Bazzite and SteamOS included |

Everything lives in the game's code, not the launcher's, so it reaches existing Linux launchers as a game update.

**Zig behind two scripts.** `bin/zig-clang` and `bin/zig-clang++` run `zig cc` / `zig c++`, so the build sees
an ordinary Clang. They also:
- translate `-march=` CPU names: Clang's `x86-64` is Zig's `x86_64`, and every file failed without it;
- pass `-g0`: Zig adds debug info by default (yw4 came out at 883 MB instead of ~160 MB; one big file compiled in
  7.0 s instead of 5.4 s; with `-g0` it matches the system's Clang 19, 5.9 s, and the code is the same size);
- keep Zig's caches in the tools folder, not the player's home.

The scripts are rewritten on every build, so a game update's fix to them takes effect at once.

**DLSS's C++ runtime.** NVIDIA's static NGX library was built with GCC and needs ten functions of GCC's C++
runtime (libstdc++). Zig brings LLVM's libc++, a different ABI. With our compiler the build therefore links the
system's `libstdc++.so.6` by its path. Every desktop has it (Steam and KDE need it), even without the development
package that `-lstdc++` needs. If it isn't found, the game is built without DLSS and the log says why.

**FSR's library with Zig.** Zig refuses `--exclude-libs`, which hid the bundled C++ runtime's names. A version
script does the same job: the library exports only `ffx*` and `yw4*`.

`YW4_OWN_TOOLS=1` uses the downloaded tools even where the system has its own, to test them.

## Checked (2026-11-19)

- WSL, Ubuntu 24.04, `YW4_OWN_TOOLS=1`, no `cc` or `glslang` in the config, SDL3 from a launcher-style folder (no
  pkg-config file for it): the whole build through `tools.yw4.build_install`, as the launcher runs it. The tools step
  took about 10 s (Zig 57 MB in 6 s, glslang 8 MB in 1 s); every file compiled and linked, with DLSS against the
  system's `libstdc++.so.6`. The game booted on Lavapipe to the Level-5 logo (frame 450) and exited cleanly.
- A bare Fedora 45 container (python3 added, nothing else: no compiler, no C headers, no git, patch or
  pkg-config), the same build: tools in 7 s, everything compiled in 563 s on 16 threads, 856 s in all, FSR and DLSS
  included. yw4 159 MB (the system Clang's build: 159 MB), needing libSDL3.so.0 (next to it, run path `$ORIGIN`),
  libstdc++.so.6, libm, libc.
- FSR's library with Zig: 62 s, exports exactly the six functions, needs only libc and libm.
- The Python patcher against `git apply` on the SDK's real source: identical files.

## Why not the alternatives

- **Always our compiler, as on Windows.** That would give every player the same compiler, but it would change
  working builds on Arch, Fedora and Debian for no gain to them. Kept for later if a distribution's compiler causes
  trouble.
- **LLVM's official Clang release.** About 1 GB, and it brings no C library headers: a bare system still couldn't
  compile.
- **Shipping the tools inside the AppImage.** That needs a new launcher release for every change. A game update
  can't change them.

## Not done

- Not tried on a real Bazzite or SteamOS PC yet: the container has the same missing pieces, but not the same GPU
  drivers or desktop.
- Our copies on the `deps-mirror` release (tried first, as for the other downloads) are not uploaded yet, so the
  official addresses are used.
- A system without `libstdc++.so.6` builds without DLSS. None is known.
