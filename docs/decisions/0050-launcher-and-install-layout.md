# 0050 — The launcher (Tauri) and the install folder

Status: accepted as a plan (2026-11-03); nothing of the launcher is written yet. The screens were designed first as
a clickable mockup (Home, game files, check, check failed, options, building, ready to play).

## Context

Other players get the game by option A (2026-11-03): no game code is shipped; a launcher takes the player's own
dump, checks it, builds the game on their PC (about 6 minutes, once) and starts it. It must be one download that
needs nothing else installed, and the game it builds must not depend on it afterwards.

## Decision

**One app, two jobs.** Setup and build on the first run; after that a play screen (Play, mods, DLC, downloads,
rebuild). The game itself runs without the launcher: `yw4.exe` reads `game.ini` next to it (the folders below,
as paths relative to it) when started with no arguments, finds mods and DLC on boot (decisions 0043, 0049) and
its own files from its home folder (0048). The launcher is only needed to build, update or change downloads.

**Tauri** (MIT/Apache-2.0): the screens are HTML/CSS/JS ported from the mockup, shown in Windows' own WebView2;
Rust handles files, processes, the GPU check and starting the game (with `YW4_HOME`). The build steps stay the
Python tools of this repo, run by a small embedded Python the launcher brings; nothing that works is rewritten.
Its code goes in `launcher/`. Chosen over pywebview (one language, but the owner prefers Tauri), Qt (large, LGPL,
restyling) and plain C + SDL (the look by hand).

**Install folder** (default `C:\Games\YW4 Recomp`, the player can change it; not under Program Files, which the
game can't write to):

```
<install>\
  YW4 Recomp.exe     the launcher
  game\              yw4.exe, its DLLs, game.ini, shaders\
  data\              exefs\, romfs\, dlc\: from the player's dump, checked
  mods\  screenshots\  logs\
  tools\             compiler, Python, glslang, hactool, FSR/DLSS SDKs (~1.5 GB): only for builds
  build\             generated C and objects (~0.7 GB): kept so updates rebuild faster
```

- **Game files are always copied** (or extracted) into `data\`, never moved or hard-linked: the install owns its
  files, whatever happens to the player's originals. NSP/XCI are extracted; DLC is checked, then copied; keys are
  used during extraction only and never copied. The copied RomFS is checked against one fingerprint of the clean
  2.2.0 RomFS (a hash, kept in `target.toml`).
- **Deleting the originals** is offered once after a successful build, with a confirmation popup listing the
  files and sizes; never the keys.
- **Saves and settings**: the player's choice, in the install folder (default: portable, one folder) or in
  `%AppData%\YW4 Recomp` (survives deleting the install). Changing it moves them, after a confirmation.
- **Free up space**: a button deletes `tools\` and `build\` (~2.2 GB); an option does it after every build. The
  next update then downloads and rebuilds everything.
- Confirmations are popups.

## Consequences

- Install size: ~11 GB with the RomFS copy, plus mods; ~8.8 GB after freeing up space.
- The order of work: `game.ini` in `yw4.exe`; progress lines in the build steps; the launcher screens with real
  logic, one at a time; packaging, tested on a clean PC.
